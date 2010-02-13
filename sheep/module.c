/*
 * sheep/module.c
 *
 * Copyright (c) 2009 Johannes Weiner <hannes@cmpxchg.org>
 */
#include <sheep/compile.h>
#include <sheep/config.h>
#include <sheep/object.h>
#include <sheep/string.h>
#include <sheep/vector.h>
#include <sheep/eval.h>
#include <sheep/read.h>
#include <sheep/util.h>
#include <sheep/map.h>
#include <sheep/vm.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>

#include <sheep/module.h>

static void do_free_module(struct sheep_module *mod)
{
	sheep_free(mod->name);
	sheep_map_drain(&mod->env);
	if (mod->handle)
		dlclose(mod->handle);
	sheep_free(mod);
}

static void free_module(struct sheep_vm *vm, sheep_t sheep)
{
	do_free_module(sheep_data(sheep));
}

static void format_module(sheep_t sheep, char **bufp, size_t *posp, int repr)
{
	struct sheep_module *mod = sheep_data(sheep);

	sheep_addprintf(bufp, posp, "#<module '%s'>", mod->name);
}

const struct sheep_type sheep_module_type = {
	.free = free_module,
	.format = format_module,
};

enum {
	LOAD_OK,
	LOAD_SKIP,
	LOAD_FAIL,
};

static unsigned int load_so(struct sheep_vm *vm, const char *path,
			struct sheep_module *mod)
{
	int (*init)(struct sheep_vm *, struct sheep_module *);
	void *handle;

	handle = dlopen(path, RTLD_NOW);
	if (!handle) {
		fprintf(stderr, "load: dlopen(%s) failed?!\n", path);
		return LOAD_SKIP;
	}

	init = dlsym(handle, "init");
	if (!init) {
		fprintf(stderr, "load: %s has no init()\n", path);
		goto err;
	}

	if (init(vm, mod))
		goto err;

	mod->handle = handle;
	return LOAD_OK;
err:
	dlclose(handle);
	return LOAD_FAIL;
}

static unsigned int load_sheep(struct sheep_vm *vm, const char *path,
			struct sheep_module *mod)
{
	struct sheep_reader reader;
	FILE *fp;
	int ret;

	fp = fopen(path, "r");
	if (!fp)
		return LOAD_SKIP;

	ret = LOAD_FAIL;

	sheep_reader_init(&reader, path, fp);
	while (1) {
		struct sheep_expr *expr;
		sheep_t fun;

		expr = sheep_read(&reader, vm);
		if (!expr)
			goto out_file;
		if (expr->object == &sheep_eof) {
			sheep_free_expr(expr);
			break;
		}
		fun = __sheep_compile(vm, mod, expr);
		sheep_free_expr(expr);
		if (!fun)
			goto out_file;
		if (!sheep_eval(vm, fun))
			goto out_file;
	}
	ret = LOAD_OK;
out_file:
	fclose(fp);
	return ret;
}

static unsigned int module_load(struct sheep_vm *vm, const char *path,
				const char *name, struct sheep_module *mod)
{
	char filename[1024];
	int ret = LOAD_SKIP;

	snprintf(filename, sizeof(filename), "%s/%s.so", path, name);
	if (!access(filename, R_OK)) {
		ret = load_so(vm, filename, mod);
		if (ret != LOAD_SKIP)
			return ret;
	}

	snprintf(filename, sizeof(filename), "%s/%s.sheep", path, name);
	if (!access(filename, R_OK))
		ret = load_sheep(vm, filename, mod);
	return ret;
}

static unsigned int load_path;

sheep_t sheep_module_load(struct sheep_vm *vm, const char *name)
{
	struct sheep_module *mod;
	struct sheep_list *paths;
	sheep_t paths_;

	mod = sheep_zalloc(sizeof(struct sheep_module));
	mod->name = sheep_strdup(name);
	sheep_module_shared(vm, mod, "module", sheep_make_string(vm, name));

	paths_ = vm->globals.items[load_path];
	if (sheep_type(paths_) != &sheep_list_type) {
		fprintf(stderr, "load: load-path is not a list\n");
		goto err;
	}

	paths = sheep_list(paths_);
	while (paths->head) {
		const char *path;

		if (sheep_type(paths->head) != &sheep_string_type) {
			fprintf(stderr, "load: bogus load-path contents\n");
			goto err;
		}

		path = sheep_rawstring(paths->head);
		switch (module_load(vm, path, name, mod)) {
		case LOAD_OK:
			goto found;
		case LOAD_FAIL:
			goto err;
		case LOAD_SKIP:
		default:
			paths = sheep_list(paths->tail);
		}
	}

	fprintf(stderr, "load: %s not found\n", name);
err:		
	do_free_module(mod);
	return NULL;
found:
	return sheep_make_object(vm, &sheep_module_type, mod);
}

/**
 * sheep_module_shared - register a shared global slot
 * @vm: runtime
 * @module: binding environment
 * @name: name to associate the slot with
 * @sheep: initial slot value
 *
 * Allocates a global slot, stores @sheep it in and binds the slot to
 * @name in @module.
 *
 * The slot can be accessed and modified from sheep code by @name and
 * from C code through the returned slot index into vm->globals.items.
 */
unsigned int sheep_module_shared(struct sheep_vm *vm,
				struct sheep_module *module,
				const char *name, sheep_t sheep)
{
	unsigned int slot;

	slot = sheep_vector_push(&vm->globals, sheep);
	sheep_map_set(&module->env, name, (void *)(unsigned long)slot);
	return slot;
}

static sheep_t builtin_load_path(struct sheep_vm *vm)
{
	return sheep_make_list(vm, 2,
			sheep_make_string(vm, "."),
			sheep_make_string(vm, SHEEP_MODDIR));
}

void sheep_module_builtins(struct sheep_vm *vm)
{
	load_path = sheep_module_shared(vm, &vm->main,
					"load-path", builtin_load_path(vm));

	sheep_vm_variable(vm, "module", &sheep_nil);
}
