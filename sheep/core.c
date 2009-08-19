#include <sheep/function.h>
#include <sheep/compile.h>
#include <sheep/config.h>
#include <sheep/module.h>
#include <sheep/bool.h>
#include <sheep/code.h>
#include <sheep/list.h>
#include <sheep/name.h>
#include <sheep/util.h>
#include <sheep/map.h>
#include <sheep/vm.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>

#include <sheep/core.h>

struct unpack_map {
	char control;
	const struct sheep_type *type;
	const char *type_name;
};

static struct unpack_map unpack_table[] = {
	{ 'o',	NULL,			"object" },
	{ 'a',	&sheep_name_type,	"name" },
	{ 'l',	&sheep_list_type,	"list" },
	{ 0,	NULL,			NULL },
};

static struct unpack_map *map_control(char control)
{
	struct unpack_map *map;

	for (map = unpack_table; map->control; map++)
		if (map->control == control)
			break;
	sheep_bug_on(!map->control);
	return map;
}

static struct unpack_map *map_type(const struct sheep_type *type)
{
	struct unpack_map *map;

	for (map = unpack_table; map->control; map++)
		if (map->type == type)
			break;
	sheep_bug_on(!map->control);
	return map;
}

static int verify(const char *caller, char control, sheep_t object)
{
	struct unpack_map *want;

	want = map_control(control);
	if (want->type && object->type != want->type) {
		struct unpack_map *got = map_type(object->type);

		fprintf(stderr, "%s: expected %s, got %s\n",
			caller, want->type_name, got->type_name);
		return -1;
	}
	return 0;
}	

static int unpack(const char *caller, struct sheep_list *list,
		const char *items, ...)
{
	struct sheep_object *object;
	int ret = -1;
	va_list ap;

	va_start(ap, items);
	while (*items) {
		if (*items == 'r') {
			/*
			 * "r!" makes sure the rest is non-empty
			 */
			if (items[1] == '!' && !list)
				break;
			*va_arg(ap, struct sheep_list **) = list;
			ret = 0;
			goto out;
		}

		if (!list)
			break;

		object = list->head;
		if (verify(caller, tolower(*items), object))
			goto out;
		if (isupper(*items))
			*va_arg(ap, void **) = sheep_data(object);
		else
			*va_arg(ap, sheep_t *) = object;

		items++;
		list = list->tail;
	}

	if (*items)
		fprintf(stderr, "%s: too few arguments\n", caller);
	else if (list)
		fprintf(stderr, "%s: too many arguments\n", caller);
	else
		ret = 0;
out:
	va_end(ap);
	return ret;
}

/* (quote expr) */
static int compile_quote(struct sheep_vm *vm, struct sheep_context *context,
			struct sheep_list *args)
{
	unsigned int slot;
	sheep_t obj;

	if (unpack("quote", args, "o", &obj))
		return -1;

	slot = sheep_slot_constant(vm, obj);
	sheep_emit(context->code, SHEEP_GLOBAL, slot);
	return 0;
}

static int do_compile_block(struct sheep_vm *vm, struct sheep_code *code,
			struct sheep_function *function, struct sheep_map *env,
			struct sheep_context *parent, struct sheep_list *args)
{
	struct sheep_context context = {
		.code = code,
		.function = function,
		.env = env,
		.parent = parent,
	};

	while (args) {
		sheep_t value = args->head;

		if (value->type->compile(vm, &context, value))
			return -1;

		if (!args->tail)
			break;
		/*
		 * The last value is the value of the whole block,
		 * drop everything in between to keep the stack
		 * balanced.
		 */
		sheep_emit(code, SHEEP_DROP, 0);
		args = args->tail;
	}
	return 0;
}

/* (block expr*) */
static int compile_block(struct sheep_vm *vm, struct sheep_context *context,
			struct sheep_list *args)
{
	SHEEP_DEFINE_MAP(env);
	int ret;

	/* Just make sure the block is not empty */
	if (unpack("block", args, "r!", &args))
		return -1;

	ret = do_compile_block(vm, context->code, context->function,
			&env, context, args);

	sheep_map_drain(&env);
	return ret;
}

/* (with ([name expr]*) expr*) */
static int compile_with(struct sheep_vm *vm, struct sheep_context *context,
			struct sheep_list *args)
{
	struct sheep_list *bindings, *body;
	SHEEP_DEFINE_MAP(env);
	int ret = -1;

	if (unpack("with", args, "Lr!", &bindings, &body))
		return -1;

	while (bindings) {
		unsigned int slot;
		const char *name;
		sheep_t value;

		if (unpack("with", bindings, "Aor", &name, &value, &bindings))
			goto out;

		if (value->type->compile(vm, context, value))
			goto out;

		if (context->function) {
			slot = sheep_slot_local(context);
			sheep_emit(context->code, SHEEP_SET_LOCAL, slot);
		} else {
			slot = sheep_slot_global(vm);
			sheep_emit(context->code, SHEEP_SET_GLOBAL, slot);
		}
		sheep_map_set(&env, name, (void *)(unsigned long)slot);
	}

	ret = do_compile_block(vm, context->code, context->function,
			&env, context, body);
out:
	sheep_map_drain(&env);
	return ret;
}

/* (variable name expr) */
static int compile_variable(struct sheep_vm *vm, struct sheep_context *context,
			struct sheep_list *args)
{
	unsigned int slot;
	const char *name;
	sheep_t value;

	if (unpack("variable", args, "Ao", &name, &value))
		return -1;

	if (value->type->compile(vm, context, value))
		return -1;

	if (context->function) {
		slot = sheep_slot_local(context);
		sheep_emit(context->code, SHEEP_SET_LOCAL, slot);
		sheep_emit(context->code, SHEEP_LOCAL, slot);
	} else {
		slot = sheep_slot_global(vm);
		sheep_emit(context->code, SHEEP_SET_GLOBAL, slot);
		sheep_emit(context->code, SHEEP_GLOBAL, slot);
	}
	sheep_map_set(context->env, name, (void *)(unsigned long)slot);
	return 0;
}

/* (function name? (arg*) expr*) */
static int compile_function(struct sheep_vm *vm, struct sheep_context *context,
			struct sheep_list *args)
{
	unsigned int cslot, bslot = 0xf00;
	struct sheep_function *function;
	struct sheep_list *parms, *body;
	SHEEP_DEFINE_CODE(code);
	SHEEP_DEFINE_MAP(env);
	const char *name;
	sheep_t sheep;
	int ret = -1;

	if (args && args->head->type == &sheep_name_type) {
		name = sheep_cname(args->head);
		args = args->tail;
	} else
		name = NULL;

	if (unpack("function", args, "Lr!", &parms, &body))
		return -1;

	sheep = sheep_native_function(vm);
	function = sheep_data(sheep);

	while (parms) {
		unsigned int slot;
		const char *parm;

		if (unpack("function", parms, "Ar", &parm, &parms))
			goto out;
		slot = function->function.native->nr_locals++;
		sheep_map_set(&env, parm, (void *)(unsigned long)slot);
		function->nr_parms++;
	}

	cslot = sheep_slot_constant(vm, sheep);
	if (name) {
		if (context->function)
			bslot = sheep_slot_local(context);
		else
			bslot = sheep_slot_global(vm);
		sheep_map_set(context->env, name, (void *)(unsigned long)bslot);
	}

	sheep_protect(vm, sheep);
	ret = do_compile_block(vm, &code, function, &env, context, body);
	sheep_unprotect(vm, sheep);

	sheep_emit(&code, SHEEP_RET, 0);
	function->function.native->offset = vm->code.code.nr_items;
	sheep_vector_concat(&vm->code.code, &code.code);

	sheep_emit(context->code, SHEEP_CLOSURE, cslot);
	if (name) {
		if (context->function) {
			sheep_emit(context->code, SHEEP_SET_LOCAL, bslot);
			sheep_emit(context->code, SHEEP_LOCAL, bslot);
		} else {
			sheep_emit(context->code, SHEEP_SET_GLOBAL, bslot);
			sheep_emit(context->code, SHEEP_GLOBAL, bslot);
		}
	}
out:
	sheep_free(code.code.items);
	sheep_map_drain(&env);
	return ret;
}

static int eval_ddump(struct sheep_vm *vm)
{
	sheep_ddump(vm->stack.items[vm->stack.nr_items - 1]);
	return 0;
}

void sheep_core_init(struct sheep_vm *vm)
{
	sheep_map_set(&vm->specials, "quote", compile_quote);
	sheep_map_set(&vm->specials, "block", compile_block);
	sheep_map_set(&vm->specials, "with", compile_with);
	sheep_map_set(&vm->specials, "variable", compile_variable);
	sheep_map_set(&vm->specials, "function", compile_function);

	sheep_module_shared(vm, &vm->main, "true", &sheep_true);
	sheep_module_shared(vm, &vm->main, "false", &sheep_false);

	sheep_module_shared(vm, &vm->main, "ddump",
			sheep_foreign_function(vm, eval_ddump, 1));
}

void sheep_core_exit(struct sheep_vm *vm)
{
	sheep_map_drain(&vm->specials);
}