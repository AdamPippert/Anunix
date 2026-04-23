/*
 * js_compile.h — AST → flat bytecode + constant pool.
 *
 * Bytecode is a sequence of uint8_t opcodes with inline operands.
 * Constants (strings, doubles) are stored in a separate pool; opcodes
 * reference them by uint16_t index.
 *
 * Variable scoping: single flat frame (no closures yet).  Variables are
 * looked up by name in a compile-time symbol table; local slots are
 * addressed by uint8_t index.
 */

#ifndef ANX_JS_COMPILE_H
#define ANX_JS_COMPILE_H

#include <anx/types.h>
#include "js_heap.h"
#include "js_val.h"
#include "js_parse.h"

/* ── Opcodes ──────────────────────────────────────────────────────── */

typedef enum {
	/* Stack */
	OP_PUSH_CONST,    /* u16: const_pool index → push val */
	OP_PUSH_INT,      /* i16: small integer inline → push int32 */
	OP_PUSH_TRUE,
	OP_PUSH_FALSE,
	OP_PUSH_NULL,
	OP_PUSH_UNDEF,
	OP_POP,
	OP_DUP,           /* duplicate top */
	OP_SWAP,          /* swap top two */

	/* Variables */
	OP_LOAD_LOCAL,    /* u8: slot → push */
	OP_STORE_LOCAL,   /* u8: slot ← pop */
	OP_LOAD_GLOBAL,   /* u16: name const → push from global obj */
	OP_STORE_GLOBAL,  /* u16: name const ← pop */

	/* Object / property */
	OP_GET_PROP,      /* u16: name const; obj=pop → push obj[name] */
	OP_SET_PROP,      /* u16: name const; val=pop,obj=pop → obj[name]=val */
	OP_GET_INDEX,     /* key=pop,obj=pop → push obj[key] */
	OP_SET_INDEX,     /* val=pop,key=pop,obj=pop → obj[key]=val */
	OP_DEL_PROP,      /* u16: name const; obj=pop → delete */
	OP_NEW_OBJECT,    /* → push {} */
	OP_NEW_ARRAY,     /* u16: n elements (top-n..top popped) → push [] */
	OP_INIT_PROP,     /* u16: name; val=pop,obj=top → obj.name=val */

	/* Arithmetic */
	OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
	OP_NEG, OP_POS,
	OP_BITAND, OP_BITOR, OP_BITXOR, OP_BITNOT,
	OP_SHL, OP_SHR, OP_USHR,
	OP_INC, OP_DEC,   /* in-place on top (no operand) */

	/* Comparison */
	OP_EQ, OP_NEQ, OP_STRICT_EQ, OP_STRICT_NEQ,
	OP_LT, OP_GT, OP_LE, OP_GE,
	OP_IN, OP_INSTANCEOF,

	/* Logic */
	OP_NOT,
	OP_AND,           /* short-circuit: i16 jump offset if top falsy */
	OP_OR,            /* short-circuit: i16 jump offset if top truthy */

	/* Control flow */
	OP_JUMP,          /* i16: relative offset from next instr */
	OP_JUMP_IF_FALSE, /* i16: relative; pop condition */
	OP_JUMP_IF_TRUE,  /* i16: relative; pop condition */

	/* Functions */
	OP_CALL,          /* u8: argc; args+callee popped → push result */
	OP_CALL_METHOD,   /* u16:name, u8:argc; args+obj popped → push */
	OP_RETURN,        /* pop return value */
	OP_MAKE_FUNC,     /* u16: func const index → push function obj */

	/* Misc */
	OP_TYPEOF,        /* pop → push typeof string */
	OP_VOID,          /* pop → push undefined */
	OP_THROW,         /* pop, throw */
	OP_ENTER_TRY,     /* u16: catch handler offset */
	OP_LEAVE_TRY,
	OP_THIS,          /* push 'this' */

	OP_NOP,
	OP_HALT,

	OP_COUNT
} js_opcode;

/* ── Constant pool entry ──────────────────────────────────────────── */

#define JS_CONST_POOL_MAX  512
#define JS_CODE_MAX        8192  /* bytecode bytes per function */
#define JS_LOCAL_MAX       64
#define JS_FUNC_MAX        32    /* nested functions per script */

struct js_func_proto {
	uint8_t  code[JS_CODE_MAX];
	uint32_t code_len;

	js_val   consts[JS_CONST_POOL_MAX];
	uint16_t n_consts;

	uint8_t  n_params;
	uint8_t  n_locals;

	/* Name for debugging */
	char     name[32];
};

/* ── Compiler state ───────────────────────────────────────────────── */

#define JS_SCOPE_DEPTH_MAX  8

struct js_scope {
	char     names[JS_LOCAL_MAX][32];
	uint8_t  n_locals;
};

struct js_compiler {
	struct js_parser    *parser;
	struct js_heap      *heap;
	struct js_func_proto funcs[JS_FUNC_MAX];
	uint32_t             n_funcs;
	uint32_t             cur_func;   /* index into funcs[] */
	struct js_scope      scopes[JS_SCOPE_DEPTH_MAX];
	uint32_t             scope_depth;
	bool                 error;
	const char          *error_msg;
};

/* Compile a parsed program (root node index) into func 0 (the script body).
 * Returns false on compile error. */
bool js_compile(struct js_compiler *c, struct js_parser *p,
                struct js_heap *h, uint16_t root);

#endif /* ANX_JS_COMPILE_H */
