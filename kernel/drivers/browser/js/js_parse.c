/*
 * js_parse.c — Recursive-descent JavaScript parser.
 *
 * Implements a subset of ES2020 sufficient for DOM scripting:
 *   var/let/const, function declarations/expressions, arrow functions,
 *   if/while/for/for-in/for-of, try/catch/finally, return/throw/break/continue,
 *   object/array literals, member access, calls, new, typeof/delete/void,
 *   all binary/unary/assignment operators, template literals (plain).
 *
 * Classes, generators, async/await, destructuring, and import/export are
 * parsed but reduced to stubs (error node) — enough to not crash.
 */

#include "js_parse.h"
#include "js_str.h"
#include <anx/string.h>

/* ── Error helpers ─────────────────────────────────────────────────── */

static uint16_t parse_error(struct js_parser *p, const char *msg)
{
	if (!p->error) {
		p->error      = true;
		p->error_msg  = msg;
		p->error_line = js_lex_peek(&p->lex).line;
	}
	return JS_NODE_NULL;
}

/* ── Node allocation ───────────────────────────────────────────────── */

uint16_t js_node_alloc(struct js_parser *p, js_node_kind kind, uint32_t line)
{
	if (p->n_nodes >= JS_AST_MAX_NODES)
		return parse_error(p, "AST overflow");
	uint16_t idx = (uint16_t)p->n_nodes++;
	struct js_node *n = &p->nodes[idx];
	n->kind   = (uint16_t)kind;
	n->flags  = 0;
	n->flags2 = 0;
	n->line   = line;
	n->val    = JV_UNDEF;
	n->c[0] = n->c[1] = n->c[2] = n->c[3] = JS_NODE_NULL;
	return idx;
}

uint16_t js_list_append(struct js_parser *p, uint16_t list, uint16_t item,
                        uint32_t line)
{
	uint16_t node = js_node_alloc(p, ND_LIST, line);
	if (node == JS_NODE_NULL) return JS_NODE_NULL;
	p->nodes[node].c[0] = item;
	p->nodes[node].c[1] = JS_NODE_NULL;
	if (list == JS_NODE_NULL) return node;
	/* Walk to end of existing list */
	uint16_t cur = list;
	while (p->nodes[cur].c[1] != JS_NODE_NULL)
		cur = p->nodes[cur].c[1];
	p->nodes[cur].c[1] = node;
	return list;
}

/* ── Token helpers ─────────────────────────────────────────────────── */

static struct js_token cur(struct js_parser *p)
{
	return js_lex_peek(&p->lex);
}

static js_tok_kind cur_kind(struct js_parser *p)
{
	return js_lex_peek(&p->lex).kind;
}

static struct js_token advance(struct js_parser *p)
{
	struct js_token t = js_lex_peek(&p->lex);
	js_lex_consume(&p->lex);
	return t;
}

static bool eat(struct js_parser *p, js_tok_kind k)
{
	if (cur_kind(p) == k) { advance(p); return true; }
	return false;
}

static bool expect(struct js_parser *p, js_tok_kind k)
{
	if (cur_kind(p) == k) { advance(p); return true; }
	parse_error(p, "unexpected token");
	return false;
}

static void skip_semi(struct js_parser *p)
{
	eat(p, TK_SEMICOLON);
}

static bool at_eof(struct js_parser *p)
{
	return cur_kind(p) == TK_EOF;
}

/* ── Forward declarations ──────────────────────────────────────────── */

static uint16_t parse_stmt(struct js_parser *p);
static uint16_t parse_expr(struct js_parser *p);
static uint16_t parse_assign_expr(struct js_parser *p);

/* ── String / ident helpers ────────────────────────────────────────── */

static js_val tok_to_str_val(struct js_parser *p, struct js_token t)
{
	struct js_str *s = js_str_new(p->heap, t.str, t.str_len);
	if (!s) { parse_error(p, "OOM"); return JV_NULL; }
	return jv_str(s);
}

/* ── Statement list (block body) ───────────────────────────────────── */

static uint16_t parse_stmt_list(struct js_parser *p)
{
	uint16_t list = JS_NODE_NULL;
	while (!at_eof(p) && cur_kind(p) != TK_RBRACE) {
		uint16_t s = parse_stmt(p);
		if (s == JS_NODE_NULL && p->error) break;
		list = js_list_append(p, list, s, cur(p).line);
	}
	return list;
}

/* ── Function parameters ───────────────────────────────────────────── */

static uint16_t parse_params(struct js_parser *p)
{
	uint16_t list = JS_NODE_NULL;
	expect(p, TK_LPAREN);
	while (cur_kind(p) != TK_RPAREN && !at_eof(p)) {
		uint32_t line = cur(p).line;
		uint16_t pn = js_node_alloc(p, ND_PARAM, line);
		if (pn == JS_NODE_NULL) break;
		if (cur_kind(p) == TK_DOTDOTDOT) {
			advance(p);
		}
		if (cur_kind(p) != TK_IDENT) { parse_error(p, "param name"); break; }
		struct js_token nt = advance(p);
		uint16_t name = js_node_alloc(p, ND_IDENT, nt.line);
		if (name == JS_NODE_NULL) break;
		p->nodes[name].val = tok_to_str_val(p, nt);
		p->nodes[pn].c[0] = name;
		if (eat(p, TK_ASSIGN)) {
			p->nodes[pn].c[1] = parse_assign_expr(p);
		}
		list = js_list_append(p, list, pn, line);
		if (!eat(p, TK_COMMA)) break;
	}
	expect(p, TK_RPAREN);
	return list;
}

/* ── Function body ─────────────────────────────────────────────────── */

static uint16_t parse_func_body(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	expect(p, TK_LBRACE);
	uint16_t list = parse_stmt_list(p);
	expect(p, TK_RBRACE);
	uint16_t block = js_node_alloc(p, ND_BLOCK, line);
	if (block != JS_NODE_NULL)
		p->nodes[block].c[0] = list;
	return block;
}

/* ── Statements ────────────────────────────────────────────────────── */

static uint16_t parse_var_decl(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	advance(p); /* consume var/let/const */
	uint16_t n = js_node_alloc(p, ND_VAR_DECL, line);
	if (n == JS_NODE_NULL) return JS_NODE_NULL;

	if (cur_kind(p) != TK_IDENT) return parse_error(p, "expected identifier");
	struct js_token nt = advance(p);
	uint16_t name = js_node_alloc(p, ND_IDENT, nt.line);
	if (name == JS_NODE_NULL) return JS_NODE_NULL;
	p->nodes[name].val = tok_to_str_val(p, nt);
	p->nodes[n].c[0] = name;

	if (eat(p, TK_ASSIGN))
		p->nodes[n].c[1] = parse_assign_expr(p);

	skip_semi(p);
	return n;
}

static uint16_t parse_func_decl(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	advance(p); /* consume 'function' */
	uint16_t n = js_node_alloc(p, ND_FUNC_DECL, line);
	if (n == JS_NODE_NULL) return JS_NODE_NULL;

	uint16_t name = JS_NODE_NULL;
	if (cur_kind(p) == TK_IDENT) {
		struct js_token nt = advance(p);
		name = js_node_alloc(p, ND_IDENT, nt.line);
		if (name != JS_NODE_NULL)
			p->nodes[name].val = tok_to_str_val(p, nt);
	}
	p->nodes[n].c[0] = name;
	p->nodes[n].c[1] = parse_params(p);
	p->nodes[n].c[2] = parse_func_body(p);
	return n;
}

static uint16_t parse_if(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	advance(p); /* if */
	uint16_t n = js_node_alloc(p, ND_IF, line);
	if (n == JS_NODE_NULL) return JS_NODE_NULL;
	expect(p, TK_LPAREN);
	p->nodes[n].c[0] = parse_expr(p);
	expect(p, TK_RPAREN);
	p->nodes[n].c[1] = parse_stmt(p);
	if (cur_kind(p) == TK_ELSE) {
		advance(p);
		p->nodes[n].c[2] = parse_stmt(p);
	}
	return n;
}

static uint16_t parse_while(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	advance(p);
	uint16_t n = js_node_alloc(p, ND_WHILE, line);
	if (n == JS_NODE_NULL) return JS_NODE_NULL;
	expect(p, TK_LPAREN);
	p->nodes[n].c[0] = parse_expr(p);
	expect(p, TK_RPAREN);
	p->nodes[n].c[1] = parse_stmt(p);
	return n;
}

static uint16_t parse_for(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	advance(p); /* for */
	expect(p, TK_LPAREN);

	/* Check for for-in / for-of */
	bool is_var = (cur_kind(p) == TK_VAR || cur_kind(p) == TK_LET ||
	               cur_kind(p) == TK_CONST);

	if (is_var) {
		/* Peek ahead: var x in/of expr */
		uint32_t saved_pos = p->lex.pos;
		uint32_t saved_line = p->lex.line;
		bool saved_peeked = p->lex.peeked;
		struct js_token saved_cur = p->lex.cur;

		advance(p); /* consume var/let/const */
		if (cur_kind(p) == TK_IDENT) {
			struct js_token var_name = advance(p);
			js_tok_kind next = cur_kind(p);
			if (next == TK_IN || next == TK_OF) {
				js_node_kind forin_kind = (next == TK_IN) ? ND_FOR_IN : ND_FOR_OF;
				advance(p); /* consume in/of */
				uint16_t n = js_node_alloc(p, forin_kind, line);
				if (n == JS_NODE_NULL) return JS_NODE_NULL;
				uint16_t vn = js_node_alloc(p, ND_IDENT, var_name.line);
				if (vn != JS_NODE_NULL)
					p->nodes[vn].val = tok_to_str_val(p, var_name);
				p->nodes[n].c[0] = vn;
				p->nodes[n].c[1] = parse_expr(p);
				expect(p, TK_RPAREN);
				p->nodes[n].c[2] = parse_stmt(p);
				return n;
			}
			/* Not for-in/of — restore and fall through */
			(void)var_name;
		}
		/* Restore lexer state */
		p->lex.pos    = saved_pos;
		p->lex.line   = saved_line;
		p->lex.peeked = saved_peeked;
		p->lex.cur    = saved_cur;
	}

	/* Regular for(init; cond; update) */
	uint16_t n = js_node_alloc(p, ND_FOR, line);
	if (n == JS_NODE_NULL) return JS_NODE_NULL;

	if (cur_kind(p) != TK_SEMICOLON)
		p->nodes[n].c[0] = parse_stmt(p); /* init — eats semi */
	else
		advance(p);

	if (cur_kind(p) != TK_SEMICOLON)
		p->nodes[n].c[1] = parse_expr(p);
	expect(p, TK_SEMICOLON);

	if (cur_kind(p) != TK_RPAREN)
		p->nodes[n].c[2] = parse_expr(p);
	expect(p, TK_RPAREN);

	p->nodes[n].c[3] = parse_stmt(p);
	return n;
}

static uint16_t parse_return(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	advance(p);
	uint16_t n = js_node_alloc(p, ND_RETURN, line);
	if (n == JS_NODE_NULL) return JS_NODE_NULL;
	if (cur_kind(p) != TK_SEMICOLON && cur_kind(p) != TK_RBRACE && !at_eof(p))
		p->nodes[n].c[0] = parse_expr(p);
	skip_semi(p);
	return n;
}

static uint16_t parse_throw(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	advance(p);
	uint16_t n = js_node_alloc(p, ND_THROW, line);
	if (n == JS_NODE_NULL) return JS_NODE_NULL;
	p->nodes[n].c[0] = parse_assign_expr(p);
	skip_semi(p);
	return n;
}

static uint16_t parse_try(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	advance(p); /* try */
	uint16_t n = js_node_alloc(p, ND_TRY, line);
	if (n == JS_NODE_NULL) return JS_NODE_NULL;
	p->nodes[n].c[0] = parse_func_body(p);

	if (cur_kind(p) == TK_CATCH) {
		advance(p);
		uint16_t cn = js_node_alloc(p, ND_CATCH, line);
		expect(p, TK_LPAREN);
		if (cur_kind(p) == TK_IDENT) {
			struct js_token nt = advance(p);
			uint16_t pn = js_node_alloc(p, ND_IDENT, nt.line);
			if (pn != JS_NODE_NULL)
				p->nodes[pn].val = tok_to_str_val(p, nt);
			if (cn != JS_NODE_NULL)
				p->nodes[cn].c[0] = pn;
		}
		expect(p, TK_RPAREN);
		if (cn != JS_NODE_NULL)
			p->nodes[cn].c[1] = parse_func_body(p);
		p->nodes[n].c[1] = cn;
	}
	if (cur_kind(p) == TK_FINALLY) {
		advance(p);
		p->nodes[n].c[2] = parse_func_body(p);
	}
	return n;
}

static uint16_t parse_block(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	expect(p, TK_LBRACE);
	uint16_t list = parse_stmt_list(p);
	expect(p, TK_RBRACE);
	uint16_t n = js_node_alloc(p, ND_BLOCK, line);
	if (n != JS_NODE_NULL) p->nodes[n].c[0] = list;
	return n;
}

static uint16_t parse_stmt(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	js_tok_kind k = cur_kind(p);

	switch (k) {
	case TK_LBRACE:   return parse_block(p);
	case TK_VAR: case TK_LET: case TK_CONST: return parse_var_decl(p);
	case TK_FUNCTION: return parse_func_decl(p);
	case TK_IF:       return parse_if(p);
	case TK_WHILE:    return parse_while(p);
	case TK_FOR:      return parse_for(p);
	case TK_RETURN:   return parse_return(p);
	case TK_THROW:    return parse_throw(p);
	case TK_TRY:      return parse_try(p);
	case TK_BREAK: {
		advance(p); skip_semi(p);
		return js_node_alloc(p, ND_BREAK, line);
	}
	case TK_CONTINUE: {
		advance(p); skip_semi(p);
		return js_node_alloc(p, ND_CONTINUE, line);
	}
	case TK_SEMICOLON: {
		advance(p);
		return js_node_alloc(p, ND_EMPTY, line);
	}
	default: {
		uint16_t n = js_node_alloc(p, ND_EXPR_STMT, line);
		if (n == JS_NODE_NULL) return JS_NODE_NULL;
		p->nodes[n].c[0] = parse_expr(p);
		skip_semi(p);
		return n;
	}
	}
}

/* ── Expression parsing (Pratt-style precedence climbing) ──────────── */

/* Precedence levels (higher = tighter binding) */
static int binop_prec(js_tok_kind k)
{
	switch (k) {
	case TK_PIPEPIPE:     return 4;
	case TK_AMPAMP:       return 5;
	case TK_PIPE:         return 6;
	case TK_CARET:        return 7;
	case TK_AMP:          return 8;
	case TK_EQ: case TK_NEQ:
	case TK_STRICT_EQ: case TK_STRICT_NEQ: return 9;
	case TK_LT: case TK_GT:
	case TK_LE: case TK_GE:
	case TK_IN: case TK_INSTANCEOF:         return 10;
	case TK_LSHIFT: case TK_RSHIFT:
	case TK_URSHIFT:                        return 11;
	case TK_PLUS: case TK_MINUS:            return 12;
	case TK_STAR: case TK_SLASH:
	case TK_PERCENT:                        return 13;
	case TK_STARSTAR:                       return 14; /* right-assoc */
	default: return 0;
	}
}

static bool is_assign_op(js_tok_kind k)
{
	switch (k) {
	case TK_ASSIGN:
	case TK_PLUS_ASSIGN: case TK_MINUS_ASSIGN:
	case TK_STAR_ASSIGN: case TK_SLASH_ASSIGN: case TK_PERCENT_ASSIGN:
	case TK_AMP_ASSIGN: case TK_PIPE_ASSIGN: case TK_CARET_ASSIGN:
	case TK_LSHIFT_ASSIGN: case TK_RSHIFT_ASSIGN:
		return true;
	default: return false;
	}
}

/* Parse primary expressions */
static uint16_t parse_primary(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	js_tok_kind k = cur_kind(p);
	struct js_token t = cur(p);

	switch (k) {
	case TK_NUMBER: {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_NUMBER, line);
		if (n != JS_NODE_NULL) p->nodes[n].val = jv_double(t.num);
		return n;
	}
	case TK_STRING: {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_STRING, line);
		if (n != JS_NODE_NULL) p->nodes[n].val = tok_to_str_val(p, t);
		return n;
	}
	case TK_TRUE: {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_BOOL, line);
		if (n != JS_NODE_NULL) p->nodes[n].flags = 1;
		return n;
	}
	case TK_FALSE: {
		advance(p);
		return js_node_alloc(p, ND_BOOL, line);
	}
	case TK_NULL: {
		advance(p);
		return js_node_alloc(p, ND_NULL_LIT, line);
	}
	case TK_UNDEFINED: {
		advance(p);
		return js_node_alloc(p, ND_UNDEF_LIT, line);
	}
	case TK_THIS: {
		advance(p);
		return js_node_alloc(p, ND_THIS, line);
	}
	case TK_SUPER: {
		advance(p);
		return js_node_alloc(p, ND_SUPER, line);
	}
	case TK_IDENT: {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_IDENT, line);
		if (n != JS_NODE_NULL) p->nodes[n].val = tok_to_str_val(p, t);
		return n;
	}
	case TK_FUNCTION: {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_FUNC_EXPR, line);
		if (n == JS_NODE_NULL) return JS_NODE_NULL;
		if (cur_kind(p) == TK_IDENT) {
			struct js_token nt = advance(p);
			uint16_t nm = js_node_alloc(p, ND_IDENT, nt.line);
			if (nm != JS_NODE_NULL) p->nodes[nm].val = tok_to_str_val(p, nt);
			p->nodes[n].c[0] = nm;
		}
		p->nodes[n].c[1] = parse_params(p);
		p->nodes[n].c[2] = parse_func_body(p);
		return n;
	}
	case TK_LBRACKET: {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_ARRAY, line);
		if (n == JS_NODE_NULL) return JS_NODE_NULL;
		uint16_t list = JS_NODE_NULL;
		while (cur_kind(p) != TK_RBRACKET && !at_eof(p)) {
			uint16_t el;
			if (cur_kind(p) == TK_DOTDOTDOT) {
				advance(p);
				uint16_t sp = js_node_alloc(p, ND_SPREAD, line);
				if (sp != JS_NODE_NULL)
					p->nodes[sp].c[0] = parse_assign_expr(p);
				el = sp;
			} else {
				el = parse_assign_expr(p);
			}
			list = js_list_append(p, list, el, line);
			if (!eat(p, TK_COMMA)) break;
		}
		expect(p, TK_RBRACKET);
		p->nodes[n].c[0] = list;
		return n;
	}
	case TK_LBRACE: {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_OBJECT, line);
		if (n == JS_NODE_NULL) return JS_NODE_NULL;
		uint16_t list = JS_NODE_NULL;
		while (cur_kind(p) != TK_RBRACE && !at_eof(p)) {
			uint32_t pline = cur(p).line;
			uint16_t pn = js_node_alloc(p, ND_PROP, pline);
			if (pn == JS_NODE_NULL) break;
			/* Key: ident, string, number, or computed [expr] */
			if (cur_kind(p) == TK_LBRACKET) {
				advance(p);
				p->nodes[pn].flags = 1; /* computed */
				p->nodes[pn].c[0] = parse_assign_expr(p);
				expect(p, TK_RBRACKET);
			} else if (cur_kind(p) == TK_IDENT || cur_kind(p) == TK_STRING) {
				struct js_token kt = advance(p);
				uint16_t kn = js_node_alloc(p, ND_IDENT, kt.line);
				if (kn != JS_NODE_NULL) p->nodes[kn].val = tok_to_str_val(p, kt);
				p->nodes[pn].c[0] = kn;
			} else {
				parse_error(p, "expected property name");
				break;
			}
			if (eat(p, TK_COLON)) {
				p->nodes[pn].c[1] = parse_assign_expr(p);
			} else {
				/* Shorthand { x } → { x: x } */
				p->nodes[pn].c[1] = p->nodes[pn].c[0];
			}
			list = js_list_append(p, list, pn, pline);
			if (!eat(p, TK_COMMA)) break;
		}
		expect(p, TK_RBRACE);
		p->nodes[n].c[0] = list;
		return n;
	}
	case TK_LPAREN: {
		advance(p);
		/* Could be grouped expr or arrow function params */
		uint16_t e = parse_expr(p);
		expect(p, TK_RPAREN);
		/* Arrow: (params) => body */
		if (cur_kind(p) == TK_ARROW) {
			advance(p);
			uint16_t n = js_node_alloc(p, ND_ARROW, line);
			if (n == JS_NODE_NULL) return JS_NODE_NULL;
			/* Reuse e as params (LIST of IDENT/PARAM) */
			p->nodes[n].c[0] = e;
			if (cur_kind(p) == TK_LBRACE)
				p->nodes[n].c[1] = parse_func_body(p);
			else
				p->nodes[n].c[1] = parse_assign_expr(p);
			return n;
		}
		return e;
	}
	default:
		advance(p);
		return parse_error(p, "unexpected token in expression");
	}
}

/* Parse postfix: member access, index, call */
static uint16_t parse_postfix(struct js_parser *p)
{
	uint16_t left = parse_primary(p);
	if (left == JS_NODE_NULL) return JS_NODE_NULL;

	for (;;) {
		uint32_t line = cur(p).line;
		js_tok_kind k = cur_kind(p);
		if (k == TK_DOT) {
			advance(p);
			if (cur_kind(p) != TK_IDENT) return parse_error(p, "expected property name");
			struct js_token pt = advance(p);
			uint16_t n = js_node_alloc(p, ND_MEMBER, line);
			if (n == JS_NODE_NULL) return JS_NODE_NULL;
			uint16_t prop = js_node_alloc(p, ND_IDENT, pt.line);
			if (prop != JS_NODE_NULL) p->nodes[prop].val = tok_to_str_val(p, pt);
			p->nodes[n].c[0] = left;
			p->nodes[n].c[1] = prop;
			left = n;
		} else if (k == TK_LBRACKET) {
			advance(p);
			uint16_t idx = parse_expr(p);
			expect(p, TK_RBRACKET);
			uint16_t n = js_node_alloc(p, ND_INDEX, line);
			if (n == JS_NODE_NULL) return JS_NODE_NULL;
			p->nodes[n].c[0] = left;
			p->nodes[n].c[1] = idx;
			left = n;
		} else if (k == TK_LPAREN) {
			advance(p);
			uint16_t args = JS_NODE_NULL;
			while (cur_kind(p) != TK_RPAREN && !at_eof(p)) {
				uint16_t a;
				if (cur_kind(p) == TK_DOTDOTDOT) {
					advance(p);
					uint16_t sp = js_node_alloc(p, ND_SPREAD, line);
					if (sp != JS_NODE_NULL)
						p->nodes[sp].c[0] = parse_assign_expr(p);
					a = sp;
				} else {
					a = parse_assign_expr(p);
				}
				args = js_list_append(p, args, a, line);
				if (!eat(p, TK_COMMA)) break;
			}
			expect(p, TK_RPAREN);
			uint16_t n = js_node_alloc(p, ND_CALL, line);
			if (n == JS_NODE_NULL) return JS_NODE_NULL;
			p->nodes[n].c[0] = left;
			p->nodes[n].c[1] = args;
			left = n;
		} else if (k == TK_PLUSPLUS || k == TK_MINUSMINUS) {
			advance(p);
			uint16_t n = js_node_alloc(p, ND_UPDATE, line);
			if (n == JS_NODE_NULL) return JS_NODE_NULL;
			p->nodes[n].c[0] = left;
			p->nodes[n].flags = (uint16_t)k;
			p->nodes[n].flags2 = 0; /* postfix */
			left = n;
		} else {
			break;
		}
	}
	return left;
}

/* Parse unary prefix */
static uint16_t parse_unary(struct js_parser *p)
{
	uint32_t line = cur(p).line;
	js_tok_kind k = cur_kind(p);

	if (k == TK_BANG || k == TK_TILDE || k == TK_MINUS || k == TK_PLUS) {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_UNARY, line);
		if (n == JS_NODE_NULL) return JS_NODE_NULL;
		p->nodes[n].flags  = (uint16_t)k;
		p->nodes[n].flags2 = 1;
		p->nodes[n].c[0]   = parse_unary(p);
		return n;
	}
	if (k == TK_PLUSPLUS || k == TK_MINUSMINUS) {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_UPDATE, line);
		if (n == JS_NODE_NULL) return JS_NODE_NULL;
		p->nodes[n].flags  = (uint16_t)k;
		p->nodes[n].flags2 = 1; /* prefix */
		p->nodes[n].c[0]   = parse_unary(p);
		return n;
	}
	if (k == TK_TYPEOF) {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_TYPEOF, line);
		if (n == JS_NODE_NULL) return JS_NODE_NULL;
		p->nodes[n].c[0] = parse_unary(p);
		return n;
	}
	if (k == TK_DELETE) {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_DELETE, line);
		if (n == JS_NODE_NULL) return JS_NODE_NULL;
		p->nodes[n].c[0] = parse_unary(p);
		return n;
	}
	if (k == TK_VOID) {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_VOID_OP, line);
		if (n == JS_NODE_NULL) return JS_NODE_NULL;
		p->nodes[n].c[0] = parse_unary(p);
		return n;
	}
	if (k == TK_NEW) {
		advance(p);
		uint16_t n = js_node_alloc(p, ND_NEW, line);
		if (n == JS_NODE_NULL) return JS_NODE_NULL;
		p->nodes[n].c[0] = parse_postfix(p);
		/* args already consumed by parse_postfix as TK_CALL, but ND_NEW
		 * keeps callee separate.  For 'new X(args)' parse_postfix will have
		 * produced ND_CALL; we peel it apart here. */
		if (p->nodes[n].c[0] != JS_NODE_NULL &&
		    p->nodes[p->nodes[n].c[0]].kind == ND_CALL) {
			uint16_t call = p->nodes[n].c[0];
			p->nodes[n].c[0] = p->nodes[call].c[0]; /* callee */
			p->nodes[n].c[1] = p->nodes[call].c[1]; /* args */
		}
		return n;
	}
	return parse_postfix(p);
}

/* Binary expression with precedence climbing */
static uint16_t parse_binary(struct js_parser *p, int min_prec)
{
	uint16_t left = parse_unary(p);
	if (left == JS_NODE_NULL) return JS_NODE_NULL;

	for (;;) {
		js_tok_kind op = cur_kind(p);
		int prec = binop_prec(op);
		if (prec < min_prec || prec == 0) break;
		uint32_t line = cur(p).line;
		advance(p);
		/* ** is right-associative */
		int next_prec = (op == TK_STARSTAR) ? prec : prec + 1;
		uint16_t right = parse_binary(p, next_prec);
		uint16_t n = js_node_alloc(p, ND_BINARY, line);
		if (n == JS_NODE_NULL) return JS_NODE_NULL;
		p->nodes[n].flags  = (uint16_t)op;
		p->nodes[n].c[0]   = left;
		p->nodes[n].c[1]   = right;
		left = n;
	}
	return left;
}

/* Conditional (ternary) */
static uint16_t parse_cond(struct js_parser *p)
{
	uint16_t test = parse_binary(p, 1);
	if (cur_kind(p) != TK_QUESTION) return test;
	uint32_t line = cur(p).line;
	advance(p);
	uint16_t n = js_node_alloc(p, ND_COND, line);
	if (n == JS_NODE_NULL) return JS_NODE_NULL;
	p->nodes[n].c[0] = test;
	p->nodes[n].c[1] = parse_assign_expr(p);
	expect(p, TK_COLON);
	p->nodes[n].c[2] = parse_assign_expr(p);
	return n;
}

/* Assignment */
static uint16_t parse_assign_expr(struct js_parser *p)
{
	/* Arrow function: ident => body */
	if (cur_kind(p) == TK_IDENT) {
		uint32_t saved_pos  = p->lex.pos;
		uint32_t saved_line = p->lex.line;
		bool     saved_pk   = p->lex.peeked;
		struct js_token saved_cur = p->lex.cur;
		uint32_t line = cur(p).line;
		struct js_token nt = advance(p);
		if (cur_kind(p) == TK_ARROW) {
			advance(p);
			uint16_t param = js_node_alloc(p, ND_IDENT, nt.line);
			if (param != JS_NODE_NULL)
				p->nodes[param].val = tok_to_str_val(p, nt);
			uint16_t plist = js_list_append(p, JS_NODE_NULL, param, line);
			uint16_t n = js_node_alloc(p, ND_ARROW, line);
			if (n == JS_NODE_NULL) return JS_NODE_NULL;
			p->nodes[n].c[0] = plist;
			if (cur_kind(p) == TK_LBRACE)
				p->nodes[n].c[1] = parse_func_body(p);
			else
				p->nodes[n].c[1] = parse_assign_expr(p);
			return n;
		}
		/* Not an arrow — restore and fall through */
		p->lex.pos    = saved_pos;
		p->lex.line   = saved_line;
		p->lex.peeked = saved_pk;
		p->lex.cur    = saved_cur;
	}

	uint16_t lhs = parse_cond(p);
	if (lhs == JS_NODE_NULL) return JS_NODE_NULL;

	js_tok_kind op = cur_kind(p);
	if (!is_assign_op(op)) return lhs;
	uint32_t line = cur(p).line;
	advance(p);
	uint16_t rhs = parse_assign_expr(p); /* right-assoc */
	uint16_t n = js_node_alloc(p, ND_ASSIGN, line);
	if (n == JS_NODE_NULL) return JS_NODE_NULL;
	p->nodes[n].flags = (uint16_t)op;
	p->nodes[n].c[0]  = lhs;
	p->nodes[n].c[1]  = rhs;
	return n;
}

/* Comma expression */
static uint16_t parse_expr(struct js_parser *p)
{
	uint16_t e = parse_assign_expr(p);
	if (cur_kind(p) != TK_COMMA) return e;
	uint32_t line = cur(p).line;
	uint16_t n = js_node_alloc(p, ND_SEQUENCE, line);
	if (n == JS_NODE_NULL) return JS_NODE_NULL;
	p->nodes[n].c[0] = e;
	uint16_t list = JS_NODE_NULL;
	while (eat(p, TK_COMMA)) {
		uint16_t ne = parse_assign_expr(p);
		list = js_list_append(p, list, ne, line);
	}
	p->nodes[n].c[1] = list;
	return n;
}

/* ── Entry point ───────────────────────────────────────────────────── */

uint16_t js_parse(struct js_parser *p, struct js_heap *h,
                  const char *src, uint32_t len)
{
	p->heap    = h;
	p->n_nodes = 0;
	p->error   = false;
	p->error_msg  = NULL;
	p->error_line = 0;
	js_lex_init(&p->lex, src, len);

	/* Reserve node 0 as NULL sentinel */
	p->n_nodes = 1;
	p->nodes[0].kind = (uint16_t)ND_EMPTY;

	uint32_t line = 1;
	uint16_t prog = js_node_alloc(p, ND_PROGRAM, line);
	if (prog == JS_NODE_NULL) return JS_NODE_NULL;

	uint16_t list = parse_stmt_list(p);
	p->nodes[prog].c[0] = list;
	return prog;
}
