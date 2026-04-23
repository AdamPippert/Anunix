/*
 * js_compile.c — AST → bytecode compiler.
 */

#include "js_compile.h"
#include "js_str.h"
#include <anx/string.h>
#include <anx/kprintf.h>

/* ── Emit helpers ──────────────────────────────────────────────────── */

static struct js_func_proto *cur_fn(struct js_compiler *c)
{
	return &c->funcs[c->cur_func];
}

static bool emit1(struct js_compiler *c, uint8_t b)
{
	struct js_func_proto *fn = cur_fn(c);
	if (fn->code_len >= JS_CODE_MAX) {
		c->error = true; c->error_msg = "code overflow";
		return false;
	}
	fn->code[fn->code_len++] = b;
	return true;
}

static bool emit2(struct js_compiler *c, uint8_t a, uint8_t b)
{
	return emit1(c, a) && emit1(c, b);
}


/* Emit opcode + i16 operand (little-endian) */
static bool emit_i16(struct js_compiler *c, uint8_t op, int16_t val)
{
	return emit1(c, op) &&
	       emit1(c, (uint8_t)(val & 0xFF)) &&
	       emit1(c, (uint8_t)((val >> 8) & 0xFF));
}

/* Emit opcode + u16 operand */
static bool emit_u16(struct js_compiler *c, uint8_t op, uint16_t val)
{
	return emit1(c, op) &&
	       emit1(c, (uint8_t)(val & 0xFF)) &&
	       emit1(c, (uint8_t)(val >> 8));
}

/* Reserve 3 bytes for a jump; return offset of operand to patch */
static uint32_t emit_jump(struct js_compiler *c, uint8_t op)
{
	struct js_func_proto *fn = cur_fn(c);
	emit1(c, op);
	uint32_t patch = fn->code_len;
	emit1(c, 0); emit1(c, 0);
	return patch;
}

/* Patch a previously reserved jump target */
static void patch_jump(struct js_compiler *c, uint32_t patch_pos)
{
	struct js_func_proto *fn = cur_fn(c);
	uint32_t target = fn->code_len;
	int16_t offset = (int16_t)(target - (patch_pos + 2));
	fn->code[patch_pos]     = (uint8_t)(offset & 0xFF);
	fn->code[patch_pos + 1] = (uint8_t)((offset >> 8) & 0xFF);
}

/* ── Constant pool ─────────────────────────────────────────────────── */

static uint16_t add_const(struct js_compiler *c, js_val v)
{
	struct js_func_proto *fn = cur_fn(c);
	/* Deduplicate strings by hash+equality */
	if (jv_is_str(v)) {
		const struct js_str *s = jv_to_str(v);
		uint16_t i;
		for (i = 0; i < fn->n_consts; i++) {
			if (jv_is_str(fn->consts[i])) {
				const struct js_str *t = jv_to_str(fn->consts[i]);
				if (js_str_eq(s, t)) return i;
			}
		}
	}
	if (fn->n_consts >= JS_CONST_POOL_MAX) {
		c->error = true; c->error_msg = "const pool overflow";
		return 0;
	}
	uint16_t idx = fn->n_consts++;
	fn->consts[idx] = v;
	return idx;
}

static uint16_t add_str_const(struct js_compiler *c, const char *s, uint32_t len)
{
	struct js_str *str = js_str_new(c->heap, s, len);
	if (!str) { c->error = true; c->error_msg = "OOM"; return 0; }
	return add_const(c, jv_str(str));
}

/* ── Scope / variable lookup ───────────────────────────────────────── */

static struct js_scope *cur_scope(struct js_compiler *c)
{
	return &c->scopes[c->scope_depth];
}

static void push_scope(struct js_compiler *c)
{
	if (c->scope_depth + 1 < JS_SCOPE_DEPTH_MAX)
		c->scope_depth++;
	c->scopes[c->scope_depth].n_locals = c->scopes[c->scope_depth - 1].n_locals;
}

static void pop_scope(struct js_compiler *c)
{
	if (c->scope_depth > 0)
		c->scope_depth--;
}

/* Returns local slot index, or -1 if not found in any scope */
static int lookup_local(struct js_compiler *c, const char *name, uint32_t len)
{
	int d = (int)c->scope_depth;
	while (d >= 0) {
		struct js_scope *sc = &c->scopes[d];
		int i;
		for (i = (int)sc->n_locals - 1; i >= 0; i--) {
			if (anx_strncmp(sc->names[i], name, len) == 0 &&
			    sc->names[i][len] == '\0')
				return i;
		}
		d--;
	}
	return -1;
}

static int define_local(struct js_compiler *c, const char *name, uint32_t len)
{
	struct js_scope *sc = cur_scope(c);
	if (sc->n_locals >= JS_LOCAL_MAX) return -1;
	uint8_t slot = sc->n_locals++;
	uint32_t copy = len < 31 ? len : 31;
	anx_memcpy(sc->names[slot], name, copy);
	sc->names[slot][copy] = '\0';
	/* Keep func proto n_locals in sync */
	struct js_func_proto *fn = cur_fn(c);
	if (sc->n_locals > fn->n_locals)
		fn->n_locals = sc->n_locals;
	return slot;
}

/* ── Compile expressions ───────────────────────────────────────────── */

static void compile_node(struct js_compiler *c, uint16_t idx);

static void compile_expr(struct js_compiler *c, uint16_t idx)
{
	compile_node(c, idx);
}

static const char *ident_name(struct js_compiler *c, uint16_t idx, uint32_t *len_out)
{
	struct js_node *n = &c->parser->nodes[idx];
	if (!jv_is_str(n->val)) { *len_out = 0; return ""; }
	const struct js_str *s = jv_to_str(n->val);
	*len_out = s->len;
	return js_str_data(s);
}

static void compile_load_name(struct js_compiler *c, const char *name, uint32_t len)
{
	int slot = lookup_local(c, name, len);
	if (slot >= 0) {
		emit2(c, OP_LOAD_LOCAL, (uint8_t)slot);
	} else {
		uint16_t ci = add_str_const(c, name, len);
		emit_u16(c, OP_LOAD_GLOBAL, ci);
	}
}

static void compile_store_name(struct js_compiler *c, const char *name, uint32_t len)
{
	int slot = lookup_local(c, name, len);
	if (slot >= 0) {
		emit2(c, OP_STORE_LOCAL, (uint8_t)slot);
	} else {
		uint16_t ci = add_str_const(c, name, len);
		emit_u16(c, OP_STORE_GLOBAL, ci);
	}
}

/* Compile a list and return element count */
static uint32_t compile_list(struct js_compiler *c, uint16_t list)
{
	uint32_t count = 0;
	uint16_t cur = list;
	while (cur != JS_NODE_NULL) {
		struct js_node *n = &c->parser->nodes[cur];
		if (n->kind != ND_LIST) { compile_expr(c, cur); count++; break; }
		if (n->c[0] != JS_NODE_NULL) { compile_expr(c, n->c[0]); count++; }
		cur = n->c[1];
	}
	return count;
}

static void compile_node(struct js_compiler *c, uint16_t idx)
{
	if (c->error || idx == JS_NODE_NULL) return;
	struct js_node *n = &c->parser->nodes[idx];
	js_node_kind kind = (js_node_kind)n->kind;

	switch (kind) {

	/* ── Literals ── */
	case ND_NUMBER: {
		double v = jv_to_double(n->val);
		/* Small integers as inline OP_PUSH_INT */
		if (v == (int16_t)v) {
			emit_i16(c, OP_PUSH_INT, (int16_t)v);
		} else {
			uint16_t ci = add_const(c, n->val);
			emit_u16(c, OP_PUSH_CONST, ci);
		}
		break;
	}
	case ND_STRING: {
		uint16_t ci = add_const(c, n->val);
		emit_u16(c, OP_PUSH_CONST, ci);
		break;
	}
	case ND_BOOL:      emit1(c, n->flags ? OP_PUSH_TRUE : OP_PUSH_FALSE); break;
	case ND_NULL_LIT:  emit1(c, OP_PUSH_NULL); break;
	case ND_UNDEF_LIT: emit1(c, OP_PUSH_UNDEF); break;
	case ND_THIS:      emit1(c, OP_THIS); break;
	case ND_SUPER:     emit1(c, OP_PUSH_UNDEF); break; /* stub */

	/* ── Identifier ── */
	case ND_IDENT: {
		uint32_t len;
		const char *name = ident_name(c, idx, &len);
		compile_load_name(c, name, len);
		break;
	}

	/* ── Unary ── */
	case ND_UNARY: {
		compile_expr(c, n->c[0]);
		switch ((js_tok_kind)n->flags) {
		case TK_MINUS:  emit1(c, OP_NEG); break;
		case TK_PLUS:   emit1(c, OP_POS); break;
		case TK_BANG:   emit1(c, OP_NOT); break;
		case TK_TILDE:  emit1(c, OP_BITNOT); break;
		default: break;
		}
		break;
	}
	case ND_TYPEOF: compile_expr(c, n->c[0]); emit1(c, OP_TYPEOF); break;
	case ND_DELETE: compile_expr(c, n->c[0]); emit1(c, OP_POP); emit1(c, OP_PUSH_TRUE); break;
	case ND_VOID_OP: compile_expr(c, n->c[0]); emit1(c, OP_VOID); break;

	/* ── Update (++ --) ── */
	case ND_UPDATE: {
		uint16_t operand = n->c[0];
		bool prefix = (n->flags2 != 0);
		bool inc = (n->flags == TK_PLUSPLUS);

		/* Load current value */
		compile_expr(c, operand);
		if (!prefix) emit1(c, OP_DUP); /* postfix: save old value */
		emit1(c, inc ? OP_INC : OP_DEC);
		if (prefix) emit1(c, OP_DUP);

		/* Store back */
		struct js_node *op = &c->parser->nodes[operand];
		if (op->kind == ND_IDENT) {
			uint32_t len;
			const char *name = ident_name(c, operand, &len);
			compile_store_name(c, name, len);
		} else if (op->kind == ND_MEMBER) {
			/* obj.prop: need obj on stack before the incremented val */
			emit1(c, OP_SWAP);
			uint32_t nlen;
			const char *nm = ident_name(c, op->c[1], &nlen);
			uint16_t ci = add_str_const(c, nm, nlen);
			emit_u16(c, OP_SET_PROP, ci);
		}
		if (!prefix) {
			/* postfix: swap so original value is on top */
			emit1(c, OP_SWAP);
			emit1(c, OP_POP);
		}
		break;
	}

	/* ── Binary ── */
	case ND_BINARY: {
		compile_expr(c, n->c[0]);
		compile_expr(c, n->c[1]);
		switch ((js_tok_kind)n->flags) {
		case TK_PLUS:        emit1(c, OP_ADD); break;
		case TK_MINUS:       emit1(c, OP_SUB); break;
		case TK_STAR:        emit1(c, OP_MUL); break;
		case TK_SLASH:       emit1(c, OP_DIV); break;
		case TK_PERCENT:     emit1(c, OP_MOD); break;
		case TK_STARSTAR:    emit1(c, OP_POW); break;
		case TK_AMP:         emit1(c, OP_BITAND); break;
		case TK_PIPE:        emit1(c, OP_BITOR); break;
		case TK_CARET:       emit1(c, OP_BITXOR); break;
		case TK_LSHIFT:      emit1(c, OP_SHL); break;
		case TK_RSHIFT:      emit1(c, OP_SHR); break;
		case TK_URSHIFT:     emit1(c, OP_USHR); break;
		case TK_EQ:          emit1(c, OP_EQ); break;
		case TK_NEQ:         emit1(c, OP_NEQ); break;
		case TK_STRICT_EQ:   emit1(c, OP_STRICT_EQ); break;
		case TK_STRICT_NEQ:  emit1(c, OP_STRICT_NEQ); break;
		case TK_LT:          emit1(c, OP_LT); break;
		case TK_GT:          emit1(c, OP_GT); break;
		case TK_LE:          emit1(c, OP_LE); break;
		case TK_GE:          emit1(c, OP_GE); break;
		case TK_IN:          emit1(c, OP_IN); break;
		case TK_INSTANCEOF:  emit1(c, OP_INSTANCEOF); break;
		default: break;
		}
		break;
	}

	/* ── Logical && / || (short-circuit) ── */
	/* logical && / || handled as ND_BINARY with flag check below */

	/* ── Conditional ── */
	case ND_COND: {
		compile_expr(c, n->c[0]);
		uint32_t j_false = emit_jump(c, OP_JUMP_IF_FALSE);
		compile_expr(c, n->c[1]);
		uint32_t j_end = emit_jump(c, OP_JUMP);
		patch_jump(c, j_false);
		compile_expr(c, n->c[2]);
		patch_jump(c, j_end);
		break;
	}

	/* ── Assignment ── */
	case ND_ASSIGN: {
		struct js_node *lhs = &c->parser->nodes[n->c[0]];
		js_tok_kind op = (js_tok_kind)n->flags;

		if (op != TK_ASSIGN) {
			/* Compound: load lhs first */
			compile_expr(c, n->c[0]);
		}
		compile_expr(c, n->c[1]);
		if (op != TK_ASSIGN) {
			switch (op) {
			case TK_PLUS_ASSIGN:    emit1(c, OP_ADD); break;
			case TK_MINUS_ASSIGN:   emit1(c, OP_SUB); break;
			case TK_STAR_ASSIGN:    emit1(c, OP_MUL); break;
			case TK_SLASH_ASSIGN:   emit1(c, OP_DIV); break;
			case TK_PERCENT_ASSIGN: emit1(c, OP_MOD); break;
			case TK_AMP_ASSIGN:     emit1(c, OP_BITAND); break;
			case TK_PIPE_ASSIGN:    emit1(c, OP_BITOR); break;
			case TK_CARET_ASSIGN:   emit1(c, OP_BITXOR); break;
			case TK_LSHIFT_ASSIGN:  emit1(c, OP_SHL); break;
			case TK_RSHIFT_ASSIGN:  emit1(c, OP_SHR); break;
			default: break;
			}
		}
		emit1(c, OP_DUP); /* assignment is an expression — leave value */

		if ((js_node_kind)lhs->kind == ND_IDENT) {
			uint32_t len;
			const char *name = ident_name(c, n->c[0], &len);
			compile_store_name(c, name, len);
		} else if ((js_node_kind)lhs->kind == ND_MEMBER) {
			compile_expr(c, lhs->c[0]);
			emit1(c, OP_SWAP);
			uint32_t nlen;
			const char *nm = ident_name(c, lhs->c[1], &nlen);
			uint16_t ci = add_str_const(c, nm, nlen);
			emit_u16(c, OP_SET_PROP, ci);
		} else if ((js_node_kind)lhs->kind == ND_INDEX) {
			compile_expr(c, lhs->c[0]);
			compile_expr(c, lhs->c[1]);
			emit1(c, OP_SET_INDEX);
		}
		break;
	}

	/* ── Member access ── */
	case ND_MEMBER: {
		compile_expr(c, n->c[0]);
		uint32_t len;
		const char *nm = ident_name(c, n->c[1], &len);
		uint16_t ci = add_str_const(c, nm, len);
		emit_u16(c, OP_GET_PROP, ci);
		break;
	}
	case ND_INDEX: {
		compile_expr(c, n->c[0]);
		compile_expr(c, n->c[1]);
		emit1(c, OP_GET_INDEX);
		break;
	}

	/* ── Call ── */
	case ND_CALL: {
		struct js_node *callee = &c->parser->nodes[n->c[0]];
		uint32_t argc = 0;
		if ((js_node_kind)callee->kind == ND_MEMBER) {
			/* method call: push obj, then args, then OP_CALL_METHOD */
			compile_expr(c, callee->c[0]);
			argc = compile_list(c, n->c[1]);
			uint32_t nlen;
			const char *nm = ident_name(c, callee->c[1], &nlen);
			uint16_t ci = add_str_const(c, nm, nlen);
			emit1(c, OP_CALL_METHOD);
			emit1(c, (uint8_t)(ci & 0xFF));
			emit1(c, (uint8_t)(ci >> 8));
			emit1(c, (uint8_t)argc);
		} else {
			compile_expr(c, n->c[0]);
			argc = compile_list(c, n->c[1]);
			emit2(c, OP_CALL, (uint8_t)argc);
		}
		break;
	}

	/* ── New ── */
	case ND_NEW: {
		compile_expr(c, n->c[0]);
		uint32_t argc = compile_list(c, n->c[1]);
		emit2(c, OP_CALL, (uint8_t)argc); /* runtime treats new via THIS context */
		break;
	}

	/* ── Object / Array literals ── */
	case ND_OBJECT: {
		emit1(c, OP_NEW_OBJECT);
		uint16_t props = n->c[0];
		while (props != JS_NODE_NULL) {
			struct js_node *pn = &c->parser->nodes[props];
			if ((js_node_kind)pn->kind == ND_LIST) {
				uint16_t prop = pn->c[0];
				if (prop != JS_NODE_NULL) {
					struct js_node *pr = &c->parser->nodes[prop];
					if ((js_node_kind)pr->kind == ND_PROP) {
						compile_expr(c, pr->c[1]); /* value */
						uint32_t klen;
						const char *kname = ident_name(c, pr->c[0], &klen);
						uint16_t ci = add_str_const(c, kname, klen);
						emit_u16(c, OP_INIT_PROP, ci);
					}
				}
				props = pn->c[1];
			} else break;
		}
		break;
	}
	case ND_ARRAY: {
		uint32_t count = 0;
		uint16_t elems = n->c[0];
		uint16_t saved = elems;
		/* Count elements first, then emit in order */
		while (elems != JS_NODE_NULL) {
			struct js_node *en = &c->parser->nodes[elems];
			if ((js_node_kind)en->kind == ND_LIST) {
				if (en->c[0] != JS_NODE_NULL) count++;
				elems = en->c[1];
			} else { count++; break; }
		}
		elems = saved;
		while (elems != JS_NODE_NULL) {
			struct js_node *en = &c->parser->nodes[elems];
			if ((js_node_kind)en->kind == ND_LIST) {
				if (en->c[0] != JS_NODE_NULL) compile_expr(c, en->c[0]);
				elems = en->c[1];
			} else { compile_expr(c, elems); break; }
		}
		emit_u16(c, OP_NEW_ARRAY, (uint16_t)count);
		break;
	}

	/* ── Function expr / arrow ── */
	case ND_FUNC_EXPR:
	case ND_ARROW:
	case ND_FUNC_DECL: {
		if (c->n_funcs >= JS_FUNC_MAX) {
			c->error = true; c->error_msg = "too many functions";
			break;
		}
		uint32_t fn_idx = c->n_funcs++;
		struct js_func_proto *fn = &c->funcs[fn_idx];
		fn->code_len = 0;
		fn->n_consts = 0;
		fn->n_params = 0;
		fn->n_locals = 0;
		fn->name[0]  = '\0';

		/* Name */
		if (n->c[0] != JS_NODE_NULL) {
			uint32_t nlen;
			const char *nm = ident_name(c, n->c[0], &nlen);
			uint32_t cp = nlen < 31 ? nlen : 31;
			anx_memcpy(fn->name, nm, cp);
			fn->name[cp] = '\0';
		}

		uint32_t saved_func  = c->cur_func;
		uint32_t saved_depth = c->scope_depth;
		c->cur_func    = fn_idx;
		c->scope_depth = 0;
		c->scopes[0].n_locals = 0;

		/* Parameters */
		uint16_t params = (kind == ND_FUNC_EXPR || kind == ND_FUNC_DECL)
		                  ? n->c[1] : n->c[0];
		uint16_t body   = (kind == ND_FUNC_EXPR || kind == ND_FUNC_DECL)
		                  ? n->c[2] : n->c[1];
		uint16_t pl = params;
		while (pl != JS_NODE_NULL) {
			struct js_node *pn = &c->parser->nodes[pl];
			if ((js_node_kind)pn->kind == ND_LIST) {
				uint16_t param = pn->c[0];
				if (param != JS_NODE_NULL) {
					struct js_node *par = &c->parser->nodes[param];
					uint16_t name_node = ((js_node_kind)par->kind == ND_PARAM)
					                     ? par->c[0] : param;
					uint32_t nl;
					const char *nm = ident_name(c, name_node, &nl);
					define_local(c, nm, nl);
					fn->n_params++;
				}
				pl = pn->c[1];
			} else break;
		}

		compile_node(c, body);
		/* Ensure return */
		emit1(c, OP_PUSH_UNDEF);
		emit1(c, OP_RETURN);

		c->cur_func    = saved_func;
		c->scope_depth = saved_depth;

		/* Emit reference in parent function */
		uint16_t ci = add_const(c, jv_int((int32_t)fn_idx));
		emit_u16(c, OP_MAKE_FUNC, ci);

		/* For function declarations: store in scope */
		if (kind == ND_FUNC_DECL && n->c[0] != JS_NODE_NULL) {
			uint32_t nlen;
			const char *nm = ident_name(c, n->c[0], &nlen);
			int slot = define_local(c, nm, nlen);
			if (slot >= 0)
				emit2(c, OP_STORE_LOCAL, (uint8_t)slot);
			else {
				uint16_t gci = add_str_const(c, nm, nlen);
				emit_u16(c, OP_STORE_GLOBAL, gci);
			}
		}
		break;
	}

	/* ── Sequence ── */
	case ND_SEQUENCE: {
		compile_expr(c, n->c[0]);
		emit1(c, OP_POP);
		uint16_t rest = n->c[1];
		uint32_t cnt = 0;
		while (rest != JS_NODE_NULL) {
			struct js_node *rn = &c->parser->nodes[rest];
			if ((js_node_kind)rn->kind == ND_LIST) {
				if (cnt++ > 0) emit1(c, OP_POP);
				if (rn->c[0] != JS_NODE_NULL) compile_expr(c, rn->c[0]);
				rest = rn->c[1];
			} else { compile_expr(c, rest); break; }
		}
		break;
	}

	/* ── Statements ── */
	case ND_PROGRAM: {
		uint16_t list = n->c[0];
		while (list != JS_NODE_NULL) {
			struct js_node *ln = &c->parser->nodes[list];
			if ((js_node_kind)ln->kind == ND_LIST) {
				compile_node(c, ln->c[0]);
				list = ln->c[1];
			} else { compile_node(c, list); break; }
		}
		emit1(c, OP_HALT);
		break;
	}
	case ND_BLOCK: {
		push_scope(c);
		uint16_t list = n->c[0];
		while (list != JS_NODE_NULL) {
			struct js_node *ln = &c->parser->nodes[list];
			if ((js_node_kind)ln->kind == ND_LIST) {
				compile_node(c, ln->c[0]);
				list = ln->c[1];
			} else { compile_node(c, list); break; }
		}
		pop_scope(c);
		break;
	}
	case ND_EXPR_STMT: {
		compile_expr(c, n->c[0]);
		emit1(c, OP_POP);
		break;
	}
	case ND_VAR_DECL: {
		uint32_t nlen;
		const char *name = ident_name(c, n->c[0], &nlen);
		int slot = define_local(c, name, nlen);
		if (n->c[1] != JS_NODE_NULL) {
			compile_expr(c, n->c[1]);
			if (slot >= 0)
				emit2(c, OP_STORE_LOCAL, (uint8_t)slot);
			else {
				uint16_t ci = add_str_const(c, name, nlen);
				emit_u16(c, OP_STORE_GLOBAL, ci);
			}
		}
		break;
	}
	case ND_RETURN: {
		if (n->c[0] != JS_NODE_NULL)
			compile_expr(c, n->c[0]);
		else
			emit1(c, OP_PUSH_UNDEF);
		emit1(c, OP_RETURN);
		break;
	}
	case ND_IF: {
		compile_expr(c, n->c[0]);
		uint32_t j_else = emit_jump(c, OP_JUMP_IF_FALSE);
		compile_node(c, n->c[1]);
		if (n->c[2] != JS_NODE_NULL) {
			uint32_t j_end = emit_jump(c, OP_JUMP);
			patch_jump(c, j_else);
			compile_node(c, n->c[2]);
			patch_jump(c, j_end);
		} else {
			patch_jump(c, j_else);
		}
		break;
	}
	case ND_WHILE: {
		uint32_t loop_start = cur_fn(c)->code_len;
		compile_expr(c, n->c[0]);
		uint32_t j_end = emit_jump(c, OP_JUMP_IF_FALSE);
		compile_node(c, n->c[1]);
		/* Back-jump */
		emit1(c, OP_JUMP);
		uint32_t back_pos = cur_fn(c)->code_len;
		int16_t back = (int16_t)(loop_start - (back_pos + 2));
		emit1(c, (uint8_t)(back & 0xFF));
		emit1(c, (uint8_t)((back >> 8) & 0xFF));
		patch_jump(c, j_end);
		break;
	}
	case ND_FOR: {
		if (n->c[0] != JS_NODE_NULL) compile_node(c, n->c[0]);
		uint32_t loop_start = cur_fn(c)->code_len;
		uint32_t j_end = 0;
		bool has_cond = (n->c[1] != JS_NODE_NULL);
		if (has_cond) {
			compile_expr(c, n->c[1]);
			j_end = emit_jump(c, OP_JUMP_IF_FALSE);
		}
		compile_node(c, n->c[3]);
		if (n->c[2] != JS_NODE_NULL) {
			compile_expr(c, n->c[2]);
			emit1(c, OP_POP);
		}
		emit1(c, OP_JUMP);
		uint32_t back_pos = cur_fn(c)->code_len;
		int16_t back = (int16_t)(loop_start - (back_pos + 2));
		emit1(c, (uint8_t)(back & 0xFF));
		emit1(c, (uint8_t)((back >> 8) & 0xFF));
		if (has_cond) patch_jump(c, j_end);
		break;
	}
	case ND_THROW: {
		compile_expr(c, n->c[0]);
		emit1(c, OP_THROW);
		break;
	}
	case ND_TRY: {
		uint32_t try_enter = emit_jump(c, OP_ENTER_TRY);
		compile_node(c, n->c[0]);
		emit1(c, OP_LEAVE_TRY);
		uint32_t j_after = emit_jump(c, OP_JUMP);
		patch_jump(c, try_enter);
		if (n->c[1] != JS_NODE_NULL) {
			/* catch block: exception value is on stack */
			struct js_node *cn = &c->parser->nodes[n->c[1]];
			if (cn->c[0] != JS_NODE_NULL) {
				uint32_t el;
				const char *en = ident_name(c, cn->c[0], &el);
				int slot = define_local(c, en, el);
				if (slot >= 0) emit2(c, OP_STORE_LOCAL, (uint8_t)slot);
				else emit1(c, OP_POP);
			} else {
				emit1(c, OP_POP);
			}
			compile_node(c, cn->c[1]);
		}
		patch_jump(c, j_after);
		if (n->c[2] != JS_NODE_NULL)
			compile_node(c, n->c[2]);
		break;
	}
	case ND_BREAK:    emit_i16(c, OP_JUMP, 0); break; /* patched by loop — stub */
	case ND_CONTINUE: emit_i16(c, OP_JUMP, 0); break;
	case ND_EMPTY:    emit1(c, OP_NOP); break;
	case ND_LIST:     compile_node(c, n->c[0]); break;

	default:
		/* Unknown node — push undefined */
		emit1(c, OP_PUSH_UNDEF);
		break;
	}
}

/* ── Entry point ───────────────────────────────────────────────────── */

bool js_compile(struct js_compiler *c, struct js_parser *p,
                struct js_heap *h, uint16_t root)
{
	c->parser      = p;
	c->heap        = h;
	c->error       = false;
	c->error_msg   = NULL;
	c->n_funcs     = 1;   /* func 0 = script body */
	c->cur_func    = 0;
	c->scope_depth = 0;

	struct js_func_proto *fn = &c->funcs[0];
	fn->code_len = 0;
	fn->n_consts = 0;
	fn->n_params = 0;
	fn->n_locals = 0;
	anx_strlcpy(fn->name, "<script>", sizeof(fn->name));

	c->scopes[0].n_locals = 0;

	compile_node(c, root);

	if (c->error) {
		kprintf("js_compile: error: %s\n", c->error_msg);
		return false;
	}
	return true;
}
