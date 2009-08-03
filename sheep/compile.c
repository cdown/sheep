#include <sheep/block.h>
#include <sheep/code.h>
#include <sheep/list.h>
#include <sheep/name.h>
#include <sheep/util.h>
#include <sheep/map.h>
#include <sheep/vm.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sheep/compile.h>

struct sheep_context {
	struct sheep_code *code;
	struct sheep_vector *locals;
	struct sheep_vector *foreigns;
	struct sheep_map *env;
	struct sheep_context *parent;
};

static unsigned int constant_slot(struct sheep_compile *compile, void *data)
{
	return sheep_vector_push(&compile->vm->globals, data);
}

static unsigned int local_slot(struct sheep_context *context, void *data)
{
	return sheep_vector_push(context->locals, data);
}

static unsigned int foreign_slot(struct sheep_context *context, void **islot)
{
	return sheep_vector_push(context->foreigns, islot);
}

static void code_init(struct sheep_code *code)
{
	memset(&code->code, 0, sizeof(struct sheep_vector));
	code->code.blocksize = 64;
}

struct sheep_code *__sheep_compile(struct sheep_compile *compile, sheep_t expr)
{
	struct sheep_context context;
	struct sheep_code *code;

	code = sheep_malloc(sizeof(struct sheep_code));
	code_init(code);

	memset(&context, 0, sizeof(context));
	context.code = code;
	context.locals = &compile->vm->globals;
	context.env = &compile->vm->main.env;

	if (expr->type->compile(compile, &context, expr)) {
		sheep_free(code->code.items);
		sheep_free(code);
		return NULL;
	}

	sheep_emit(code, SHEEP_RET, 0);
	return code;
}

int sheep_compile_constant(struct sheep_compile *compile,
			struct sheep_context *context, sheep_t expr)
{
	unsigned int slot;

	slot = constant_slot(compile, expr);
	sheep_emit(context->code, SHEEP_GLOBAL, slot);
	return 0;
}

enum env_level {
	ENV_LOCAL,
	ENV_GLOBAL,
	ENV_FOREIGN,
};

static int lookup(struct sheep_context *context, const char *name,
		struct sheep_vector **slots, unsigned int *slot,
		enum env_level *env_level)
{
	struct sheep_context *current = context;
	void *entry;

	while (sheep_map_get(current->env, name, &entry)) {
		if (!current->parent)
			return -1;
		current = current->parent;
	}

	*slot = (unsigned long)entry;
	*slots = current->locals;

	if (!current->parent)
		*env_level = ENV_GLOBAL;
	else if (current == context)
		*env_level = ENV_LOCAL;
	else
		*env_level = ENV_FOREIGN;

	return 0;
}

int sheep_compile_name(struct sheep_compile *compile,
		struct sheep_context *context, sheep_t expr)
{
	const char *name = sheep_cname(expr);
	struct sheep_vector *foreigns;
	enum env_level level;
	unsigned int slot;

	if (lookup(context, name, &foreigns, &slot, &level)) {
		fprintf(stderr, "unbound name: %s\n", name);
		return -1;
	}

	switch (level) {
	case ENV_LOCAL:
		sheep_emit(context->code, SHEEP_LOCAL, slot);
		break;
	case ENV_GLOBAL:
		sheep_emit(context->code, SHEEP_GLOBAL, slot);
		break;
	case ENV_FOREIGN:
		slot = foreign_slot(context, &foreigns->items[slot]);
		sheep_emit(context->code, SHEEP_FOREIGN, slot);
		break;
	}

	return 0;
}

struct unpack_map {
	char control;
	const struct sheep_type *type;
	const char *type_name;
};

static struct unpack_map unpack_table[] = {
	{ 'o',	NULL,			"object" },
	{ 'c',	&sheep_name_type,	"name" },
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
	assert(map->control);
	return map;
}

static struct unpack_map *map_type(const struct sheep_type *type)
{
	struct unpack_map *map;

	for (map = unpack_table; map->control; map++)
		if (map->type == type)
			break;
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
	va_list ap;

	va_start(ap, items);
	while (*items) {
		if (*items == 'r') {
			*va_arg(ap, struct sheep_list **) = list;
			va_end(ap);
			return 0;
		}
		if (!list)
			break;
		object = list->head;
		if (verify(caller, *items, object) < 0) {
			va_end(ap);
			return -1;
		}
		if (*items == 'c')
			*va_arg(ap, const char **) = sheep_cname(object);
		else
			*va_arg(ap, sheep_t *) = object;
		items++;
		list = list->tail;
	}
	va_end(ap);
	if (*items) {
		fprintf(stderr, "%s: too few arguments\n", caller);
		return -1;
	}
	if (list) {
		fprintf(stderr, "%s: too many arguments\n", caller);
		return -1;
	}
	return 0;
}

/* (quote expr) */
static int compile_quote(struct sheep_compile *compile,
			struct sheep_context *context, struct sheep_list *args)
{
	unsigned int slot;
	sheep_t obj;

	if (unpack("quote", args, "o", &obj))
		return -1;
	slot = sheep_vector_push(&compile->vm->globals, obj);
	sheep_emit(context->code, SHEEP_GLOBAL, slot);
	return 0;
}

/* (block &rest expr) */
static int compile_block(struct sheep_compile *compile,
			struct sheep_context *context, struct sheep_list *args)
{
	while (args) {
		sheep_t value = args->head;

		if (value->type->compile(compile, context, value))
			return -1;
		if (!args->tail)
			break;
		sheep_emit(context->code, SHEEP_DROP, 0);
		args = args->tail;
	}
	return 0;
}

/* (with (name expr name expr ...) &rest expr) */
static int compile_with(struct sheep_compile *compile,
			struct sheep_context *context, struct sheep_list *args)
{
	struct sheep_list *bindings, *body;
	struct sheep_context wcontext;
	struct sheep_map wenv;
	sheep_t pairs, value;
	unsigned int slot;
	const char *name;
	int ret = -1;

	memset(&wenv, 0, sizeof(struct sheep_map));
	wcontext.code = context->code;
	wcontext.locals = context->locals;
	wcontext.foreigns = context->foreigns;
	wcontext.env = &wenv;
	wcontext.parent = context;

	if (unpack("with", args, "lr", &pairs, &body))
		return -1;
	bindings = sheep_data(pairs);
	do {
		if (unpack("with", bindings, "cor", &name, &value, &bindings))
			goto out;
		if (value->type->compile(compile, &wcontext, value))
			goto out;
		slot = local_slot(&wcontext, NULL);
		sheep_emit(context->code, SHEEP_SET_LOCAL, slot);
		sheep_map_set(wcontext.env, name, (void *)(unsigned long)slot);
	} while (bindings);
	ret = compile_block(compile, &wcontext, body);
out:
	sheep_map_drain(wcontext.env);
	return ret;
}

int sheep_compile_list(struct sheep_compile *compile,
		struct sheep_context *context, sheep_t expr)
{
	struct sheep_list *form = sheep_data(expr);
	const char *op;
	void *entry;

	if (unpack("function call", form, "cr", &op, &form))
		return -1;

	if (!sheep_map_get(&compile->vm->specials, op, &entry)) {
		int (*special)(struct sheep_compile *, struct sheep_context *,
			struct sheep_list *) = entry;

		return special(compile, context, form);
	}

	fprintf(stderr, "function calls not implemented\n");
	return -1;
}

void sheep_compiler_init(struct sheep_vm *vm)
{
	code_init(&vm->code);
	sheep_map_set(&vm->specials, "quote", compile_quote);
	sheep_map_set(&vm->specials, "block", compile_block);
	sheep_map_set(&vm->specials, "with", compile_with);
}

void sheep_compiler_exit(struct sheep_vm *vm)
{
	sheep_map_drain(&vm->specials);
}
