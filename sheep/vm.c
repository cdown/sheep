#include <sheep/compile.h>
#include <sheep/object.h>
#include <sheep/util.h>
#include <string.h>

#include <sheep/vm.h>

void sheep_vm_init(struct sheep_vm *vm)
{
	memset(vm, 0, sizeof(*vm));
	vm->protected.blocksize = 32;
	vm->globals.blocksize = 32;
	vm->code.code.blocksize = 64;
	vm->stack.blocksize = 32;
	vm->calls.blocksize = 16;
	sheep_compiler_init(vm);
}

void sheep_vm_exit(struct sheep_vm *vm)
{
	sheep_free(vm->calls.items);
	sheep_free(vm->stack.items);
	sheep_compiler_exit(vm);
	sheep_free(vm->globals.items);
	sheep_free(vm->code.code.items);
	sheep_objects_exit(vm);
}
