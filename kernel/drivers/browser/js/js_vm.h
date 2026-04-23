/*
 * js_vm.h — Stack-based bytecode interpreter.
 *
 * Each call frame has a value stack (256 slots) and a local variable
 * array (JS_LOCAL_MAX slots).  The interpreter runs until OP_HALT or
 * OP_RETURN, or until an uncaught exception.
 *
 * The global object (js_obj) holds all global variables.  It is created
 * once per js_context and populated by js_std_init() and js_dom_init().
 */

#ifndef ANX_JS_VM_H
#define ANX_JS_VM_H

#include <anx/types.h>
#include "js_heap.h"
#include "js_val.h"
#include "js_compile.h"

#define JS_STACK_MAX     256
#define JS_CALL_DEPTH    32
#define JS_TRY_MAX       16

struct js_try_frame {
	uint32_t catch_ip;   /* instruction pointer of catch handler */
	uint16_t stack_top;  /* stack height to restore on throw */
};

struct js_call_frame {
	const struct js_func_proto *fn;
	uint32_t                    ip;
	js_val                      locals[JS_LOCAL_MAX];
	uint16_t                    n_locals;
	js_val                      this_val;

	struct js_try_frame  try_stack[JS_TRY_MAX];
	uint8_t              try_depth;
};

/* Forward declaration */
struct js_obj;

struct js_vm {
	struct js_heap        *heap;
	struct js_obj         *globals;   /* global object */
	js_val                 stack[JS_STACK_MAX];
	uint16_t               sp;        /* stack pointer (index of next push) */
	struct js_call_frame   frames[JS_CALL_DEPTH];
	uint8_t                call_depth;
	js_val                 exception; /* JV_UNDEF = no exception */
	bool                   threw;
};

/* Initialize VM state.  globals must be pre-populated. */
void js_vm_init(struct js_vm *vm, struct js_heap *h, struct js_obj *globals);

/* Execute func_proto fn with 'this_val' and argc arguments on stack.
 * Returns result value (JV_UNDEF on error / void return).
 * Called recursively for nested calls. */
js_val js_vm_exec(struct js_vm *vm, const struct js_func_proto *fn,
                  const struct js_func_proto *all_fns, uint32_t n_fns,
                  js_val this_val, js_val *args, uint8_t argc);

/* Call a JS function value (js_obj wrapping a func index).
 * Used by the VM for indirect calls. */
js_val js_vm_call(struct js_vm *vm, js_val callee, js_val this_val,
                  js_val *args, uint8_t argc,
                  const struct js_func_proto *all_fns, uint32_t n_fns);

/* Push / pop helpers for embedding */
static inline void js_vm_push(struct js_vm *vm, js_val v)
{
	if (vm->sp < JS_STACK_MAX)
		vm->stack[vm->sp++] = v;
}

static inline js_val js_vm_pop(struct js_vm *vm)
{
	return vm->sp > 0 ? vm->stack[--vm->sp] : JV_UNDEF;
}

static inline js_val js_vm_peek(struct js_vm *vm)
{
	return vm->sp > 0 ? vm->stack[vm->sp - 1] : JV_UNDEF;
}

#endif /* ANX_JS_VM_H */
