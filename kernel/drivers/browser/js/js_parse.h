/*
 * js_parse.h — Recursive-descent parser: token stream → AST.
 *
 * The AST is a flat pool of js_node structs allocated from the GC heap.
 * Each node stores its kind plus up to 4 child node indices (uint16_t,
 * relative to the pool base).  String data (identifiers, literals) is
 * stored as js_str on the heap.
 *
 * Pool limit: JS_AST_MAX_NODES nodes per parse.  For DOM scripting this
 * is plenty; complex pages split JS into small inline scripts anyway.
 */

#ifndef ANX_JS_PARSE_H
#define ANX_JS_PARSE_H

#include <anx/types.h>
#include "js_heap.h"
#include "js_val.h"
#include "js_lex.h"

#define JS_AST_MAX_NODES  4096
#define JS_NODE_NULL      0xFFFFu   /* sentinel: no child */

/* ── Node kinds ───────────────────────────────────────────────────── */

typedef enum {
	/* Statements */
	ND_PROGRAM,        /* body = list of stmts */
	ND_BLOCK,          /* body = list of stmts */
	ND_VAR_DECL,       /* c[0]=name(IDENT), c[1]=init(expr|NULL) */
	ND_FUNC_DECL,      /* c[0]=name, c[1]=params(LIST), c[2]=body */
	ND_RETURN,         /* c[0]=expr|NULL */
	ND_IF,             /* c[0]=cond, c[1]=then, c[2]=else|NULL */
	ND_WHILE,          /* c[0]=cond, c[1]=body */
	ND_FOR,            /* c[0]=init, c[1]=cond, c[2]=update, c[3]=body */
	ND_FOR_IN,         /* c[0]=var, c[1]=obj, c[2]=body */
	ND_FOR_OF,         /* c[0]=var, c[1]=iter, c[2]=body */
	ND_BREAK,          /* no children */
	ND_CONTINUE,       /* no children */
	ND_THROW,          /* c[0]=expr */
	ND_TRY,            /* c[0]=try_body, c[1]=catch_body, c[2]=finally|NULL */
	ND_EXPR_STMT,      /* c[0]=expr */
	ND_EMPTY,          /* ; */

	/* Expressions */
	ND_ASSIGN,         /* c[0]=lhs, c[1]=rhs; op in flags */
	ND_COND,           /* c[0]=test, c[1]=cons, c[2]=alt */
	ND_BINARY,         /* c[0]=lhs, c[1]=rhs; op in flags */
	ND_UNARY,          /* c[0]=operand; op in flags, prefix in flags2 */
	ND_UPDATE,         /* c[0]=operand; op in flags (++ or --), prefix in flags2 */
	ND_CALL,           /* c[0]=callee, c[1]=args(LIST) */
	ND_NEW,            /* c[0]=callee, c[1]=args(LIST) */
	ND_MEMBER,         /* c[0]=obj, c[1]=prop(IDENT or expr); computed in flags */
	ND_INDEX,          /* c[0]=obj, c[1]=key */
	ND_FUNC_EXPR,      /* c[0]=name|NULL, c[1]=params, c[2]=body */
	ND_ARROW,          /* c[0]=params(LIST), c[1]=body(block or expr) */
	ND_ARRAY,          /* c[0]=elements(LIST) */
	ND_OBJECT,         /* c[0]=props(LIST of PROP) */
	ND_PROP,           /* c[0]=key(IDENT or expr), c[1]=val; computed in flags */
	ND_SEQUENCE,       /* c[0]=first, c[1]=rest(LIST) */
	ND_SPREAD,         /* c[0]=expr */
	ND_TYPEOF,         /* c[0]=expr */
	ND_DELETE,         /* c[0]=expr */
	ND_VOID_OP,        /* c[0]=expr */

	/* Leaves */
	ND_IDENT,          /* str = name */
	ND_NUMBER,         /* val = number */
	ND_STRING,         /* str = string content */
	ND_BOOL,           /* flags = 1 (true) or 0 (false) */
	ND_NULL_LIT,
	ND_UNDEF_LIT,
	ND_THIS,
	ND_SUPER,

	/* Helpers */
	ND_LIST,           /* linked list: c[0]=head, c[1]=next LIST|NULL */
	ND_CATCH,          /* c[0]=param(IDENT), c[1]=body */
	ND_PARAM,          /* c[0]=name, c[1]=default|NULL */

	ND_KIND_COUNT
} js_node_kind;

/* ── AST node ─────────────────────────────────────────────────────── */

struct js_node {
	uint16_t     kind;     /* js_node_kind */
	uint16_t     flags;    /* operator token kind or bool value */
	uint8_t      flags2;   /* prefix flag for unary/update */
	uint8_t      _pad[3];
	uint16_t     c[4];     /* child indices (JS_NODE_NULL = none) */
	js_val       val;      /* number literal or string js_val */
	uint32_t     line;
};

/* ── Parser state ─────────────────────────────────────────────────── */

struct js_parser {
	struct js_lexer  lex;
	struct js_heap  *heap;
	struct js_node   nodes[JS_AST_MAX_NODES];
	uint32_t         n_nodes;
	bool             error;
	uint32_t         error_line;
	const char      *error_msg;
};

/* Parse src[0..len-1].  Returns root node index or JS_NODE_NULL on error. */
uint16_t js_parse(struct js_parser *p, struct js_heap *h,
                  const char *src, uint32_t len);

/* Allocate a new node; returns JS_NODE_NULL on overflow. */
uint16_t js_node_alloc(struct js_parser *p, js_node_kind kind, uint32_t line);

/* Convenience: build a ND_LIST node. */
uint16_t js_list_append(struct js_parser *p, uint16_t list, uint16_t item,
                        uint32_t line);

#endif /* ANX_JS_PARSE_H */
