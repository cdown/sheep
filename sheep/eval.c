#include <sheep/function.h>
#include <sheep/object.h>
#include <sheep/alien.h>
#include <sheep/bool.h>
#include <sheep/code.h>
#include <sheep/util.h>
#include <sheep/map.h>
#include <sheep/vm.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

#include <sheep/eval.h>

static sheep_t closure(struct sheep_vm *vm, sheep_t sheep)
{
	struct sheep_function *old, *new;
	struct sheep_vector *foreigns;
	unsigned int i;

	sheep_bug_on(sheep->type != &sheep_function_type);
	old = sheep_data(sheep);

	if (!old->foreigns)
		return sheep;

	foreigns = sheep_malloc(sizeof(struct sheep_vector));
	sheep_vector_init(foreigns, 4);

	sheep_bug_on(old->foreigns->nr_items % 2);
	for (i = 0; i < old->foreigns->nr_items; i += 2) {
		unsigned long fbasep, offset;
		unsigned int dist, slot;
		void **foreignp;

		dist = (unsigned long)old->foreigns->items[i];
		slot = (unsigned long)old->foreigns->items[i + 1];
		/*
		 * Okay, we have the static function level distance to
		 * our foreign slot owner and the actual intex into
		 * the stack locals of that owner.
		 *
		 * Every active stack frame has 3 entries in
		 * vm->calls:
		 *
		 *     vm->calls = [... lastpc lastbasep lastfunction]
		 *
		 * Work out the base pointer of the owning frame and
		 * establish the foreign slot reference to the live
		 * stack slot.
		 */
		dist = (dist - 1) * 3 + 1;
		offset = vm->calls.nr_items - 1 - dist;
		fbasep = (unsigned long)vm->calls.items[offset];
		foreignp = vm->stack.items + fbasep;
		sheep_vector_push(foreigns, foreignp + slot);
	}

	sheep = sheep_function(vm);
	new = sheep_data(sheep);
	*new = *old;
	new->foreigns = foreigns;

	return sheep;
}

static sheep_t __sheep_eval(struct sheep_vm *vm, struct sheep_code *code,
			struct sheep_function *function, unsigned long pc,
			unsigned long basep)
{
	struct sheep_code *current = code;
	unsigned int nesting = 0;

	for (;;) {
		enum sheep_opcode op;
		unsigned int arg;
		sheep_t tmp;

		sheep_decode((unsigned long)current->code.items[pc], &op, &arg);
		sheep_code_dump(vm, function, basep, op, arg);

		switch (op) {
		case SHEEP_DROP:
			sheep_vector_pop(&vm->stack);
			break;
		case SHEEP_LOCAL:
			tmp = vm->stack.items[basep + arg];
			sheep_vector_push(&vm->stack, tmp);
			break;
		case SHEEP_SET_LOCAL:
			tmp = sheep_vector_pop(&vm->stack);
			vm->stack.items[basep + arg] = tmp;
			break;
		case SHEEP_FOREIGN:
			tmp = function->foreigns->items[arg];
			sheep_vector_push(&vm->stack, *(sheep_t *)tmp);
			break;
		case SHEEP_SET_FOREIGN:
			tmp = sheep_vector_pop(&vm->stack);
			*(sheep_t *)function->foreigns->items[arg] = tmp;
			break;
		case SHEEP_GLOBAL:
			tmp = vm->globals.items[arg];
			sheep_vector_push(&vm->stack, tmp);
			break;
		case SHEEP_SET_GLOBAL:
			tmp = sheep_vector_pop(&vm->stack);
			vm->globals.items[arg] = tmp;
			break;
		case SHEEP_CLOSURE:
			tmp = vm->globals.items[arg];
			tmp = closure(vm, tmp);
			sheep_vector_push(&vm->stack, tmp);
			break;
		case SHEEP_CALL:
			tmp = sheep_vector_pop(&vm->stack);

			if (tmp->type == &sheep_alien_type) {
				sheep_alien_t alien;

				alien = *(sheep_alien_t *)sheep_data(tmp);
				tmp = alien(vm, arg);
				if (!tmp)
					goto err;
				sheep_vector_push(&vm->stack, tmp);
				break;
			}

			sheep_bug_on(tmp->type != &sheep_function_type);

			/* Save the old context */
			sheep_vector_push(&vm->calls, (void *)pc);
			sheep_vector_push(&vm->calls, (void *)basep);
			sheep_vector_push(&vm->calls, function);

			function = sheep_data(tmp);
			if (function->nr_parms != arg) {
				fprintf(stderr, "wrong number of arguments\n");
				goto err;
			}

			if (function->nr_locals)
				sheep_vector_grow(&vm->stack, function->nr_locals - arg);

			basep = vm->stack.nr_items - function->nr_locals;
			pc = function->offset;
			current = &vm->code;
			nesting++;
			continue;
		case SHEEP_RET:
			sheep_bug_on(vm->stack.nr_items - basep -
				(function ? function->nr_locals : 0) != 1);

			/* Nip the locals */
			if (function && function->nr_locals) {
				vm->stack.items[basep] =
					vm->stack.items[basep +
							function->nr_locals];
				vm->stack.nr_items = basep + 1;
			}

			if (!nesting--)
				goto out;

			/* Restore the old context */
			function = sheep_vector_pop(&vm->calls);
			basep = (unsigned long)sheep_vector_pop(&vm->calls);
			pc = (unsigned long)sheep_vector_pop(&vm->calls);

			/* Switch back to toplevel code? */
			if (!function)
				current = code;
			break;
		case SHEEP_BRN:
			tmp = sheep_vector_pop(&vm->stack);
			if (sheep_test(tmp))
				break;
		case SHEEP_BR:
			pc += arg;
			continue;
		default:
			abort();
		}
		pc++;
	}
out:
	return sheep_vector_pop(&vm->stack);
err:
	/* Unwind the stack */
	vm->stack.nr_items = 0;
	vm->calls.nr_items = 0;
	return NULL;
}

sheep_t sheep_eval(struct sheep_vm *vm, struct sheep_code *code)
{
	return __sheep_eval(vm, code, NULL, 0, 0);
}

sheep_t sheep_call(struct sheep_vm *vm, sheep_t callable,
		unsigned int nr_args, ...)
{
	struct sheep_function *function;
	unsigned int nr = nr_args;
	unsigned long basep;
	va_list ap;

	va_start(ap, nr_args);
	while (nr--)
		sheep_vector_push(&vm->stack, va_arg(ap, sheep_t));
	va_end(ap);

	if (callable->type == &sheep_alien_type) {
		sheep_alien_t alien;

		alien = *(sheep_alien_t)sheep_data(callable);
		return alien(vm, nr_args);
	}

	sheep_bug_on(callable->type != &sheep_function_type);
	function = sheep_data(callable);
	if (function->nr_parms != nr_args) {
		fprintf(stderr, "wrong number of arguments\n");
		return NULL;
	}

	if (function->nr_locals)
		sheep_vector_grow(&vm->stack, function->nr_locals - nr_args);
	basep = vm->stack.nr_items - function->nr_locals;
	return __sheep_eval(vm, &vm->code, function, function->offset, basep);
}

void sheep_evaluator_init(struct sheep_vm *vm)
{
	vm->stack.blocksize = 32;
	vm->calls.blocksize = 16;
}

void sheep_evaluator_exit(struct sheep_vm *vm)
{
	sheep_free(vm->calls.items);
	sheep_free(vm->stack.items);
	sheep_map_drain(&vm->main.env);
}
