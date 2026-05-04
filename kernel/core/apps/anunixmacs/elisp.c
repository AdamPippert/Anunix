/*
 * elisp.c — Minimal eLISP interpreter for anunixmacs (RFC-0023).
 *
 * Lisp-1, lexical scope (chained env), reference-counted values.
 * Special forms: quote if and or cond when unless let let* setq
 *                lambda progn while.
 * Built-ins: arithmetic, predicates, cons/list, strings, princ/print,
 *            buffer ops, anx-* OID bindings.
 *
 * Not supported (out of v1 scope): macros, set-car!/set-cdr!, true GC
 * for cycles (the readable subset cannot construct them), tail-call
 * optimization, multibyte/UTF-8 source.
 */

#include <anx/anunixmacs.h>
#include <anx/types.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/state_object.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

static struct anx_lv g_nil = { .tag = ANX_LV_NIL, .rc = 0xffffffff };
static struct anx_lv g_t   = { .tag = ANX_LV_T,   .rc = 0xffffffff };

struct anx_lv *anx_lv_nil(void) { return &g_nil; }
struct anx_lv *anx_lv_t(void)   { return &g_t;   }

/* ------------------------------------------------------------------ */
/* Refcount                                                            */
/* ------------------------------------------------------------------ */

void anx_lv_retain(struct anx_lv *v)
{
	if (!v || v == &g_nil || v == &g_t) return;
	if (v->rc == 0xffffffff) return;
	v->rc++;
}

static void env_release(struct anx_lv_env *e);

void anx_lv_release(struct anx_lv *v)
{
	if (!v || v == &g_nil || v == &g_t) return;
	if (v->rc == 0xffffffff) return;
	if (v->rc == 0) return;
	if (--v->rc) return;
	switch (v->tag) {
	case ANX_LV_STR:
	case ANX_LV_SYM:
		if (v->u.s.bytes) anx_free(v->u.s.bytes);
		break;
	case ANX_LV_CONS:
		anx_lv_release(v->u.cons.car);
		anx_lv_release(v->u.cons.cdr);
		break;
	case ANX_LV_FN:
		anx_lv_release(v->u.fn.params);
		anx_lv_release(v->u.fn.body);
		env_release(v->u.fn.closure);
		break;
	default:
		break;
	}
	anx_free(v);
}

/* ------------------------------------------------------------------ */
/* Constructors                                                        */
/* ------------------------------------------------------------------ */

struct anx_lv *anx_lv_new(enum anx_lv_tag tag)
{
	struct anx_lv *v = (struct anx_lv *)anx_zalloc(sizeof(*v));
	if (!v) return NULL;
	v->tag = tag;
	v->rc  = 1;
	return v;
}

struct anx_lv *anx_lv_int(int64_t i)
{
	struct anx_lv *v = anx_lv_new(ANX_LV_INT);
	if (v) v->u.i = i;
	return v;
}

struct anx_lv *anx_lv_str(const char *s, uint32_t n)
{
	struct anx_lv *v = anx_lv_new(ANX_LV_STR);
	if (!v) return NULL;
	v->u.s.bytes = (char *)anx_alloc(n + 1);
	if (!v->u.s.bytes) { anx_free(v); return NULL; }
	if (n) anx_memcpy(v->u.s.bytes, s, n);
	v->u.s.bytes[n] = '\0';
	v->u.s.len = n;
	return v;
}

struct anx_lv *anx_lv_sym(const char *s, uint32_t n)
{
	struct anx_lv *v = anx_lv_new(ANX_LV_SYM);
	if (!v) return NULL;
	v->u.s.bytes = (char *)anx_alloc(n + 1);
	if (!v->u.s.bytes) { anx_free(v); return NULL; }
	if (n) anx_memcpy(v->u.s.bytes, s, n);
	v->u.s.bytes[n] = '\0';
	v->u.s.len = n;
	return v;
}

struct anx_lv *anx_lv_cons(struct anx_lv *car, struct anx_lv *cdr)
{
	struct anx_lv *v = anx_lv_new(ANX_LV_CONS);
	if (!v) return NULL;
	v->u.cons.car = car;
	v->u.cons.cdr = cdr;
	return v;
}

static struct anx_lv *lv_buf(uint32_t handle)
{
	struct anx_lv *v = anx_lv_new(ANX_LV_BUF);
	if (v) v->u.buf_handle = handle;
	return v;
}

static struct anx_lv *lv_builtin(const struct anx_lv_builtin *b)
{
	struct anx_lv *v = anx_lv_new(ANX_LV_BUILTIN);
	if (v) v->u.builtin = b;
	return v;
}

/* ------------------------------------------------------------------ */
/* Environment                                                         */
/* ------------------------------------------------------------------ */

static struct anx_lv_env *env_new(struct anx_lv_env *parent)
{
	struct anx_lv_env *e = (struct anx_lv_env *)anx_zalloc(sizeof(*e));
	if (!e) return NULL;
	e->bindings = &g_nil;
	e->parent = parent;
	e->rc = 1;
	return e;
}

static void env_retain(struct anx_lv_env *e) { if (e) e->rc++; }

static void env_release(struct anx_lv_env *e)
{
	if (!e) return;
	if (--e->rc) return;
	anx_lv_release(e->bindings);
	anx_free(e);
}

static struct anx_lv *env_lookup(struct anx_lv_env *e, struct anx_lv *sym)
{
	while (e) {
		struct anx_lv *cur = e->bindings;
		while (cur && cur->tag == ANX_LV_CONS) {
			struct anx_lv *pair = cur->u.cons.car;
			if (pair && pair->tag == ANX_LV_CONS &&
			    pair->u.cons.car && pair->u.cons.car->tag == ANX_LV_SYM &&
			    anx_strcmp(pair->u.cons.car->u.s.bytes,
				       sym->u.s.bytes) == 0) {
				return pair->u.cons.cdr;
			}
			cur = cur->u.cons.cdr;
		}
		e = e->parent;
	}
	return NULL;
}

static int env_set(struct anx_lv_env *e, struct anx_lv *sym, struct anx_lv *val)
{
	struct anx_lv_env *cur_e = e;
	struct anx_lv     *pair;

	/* Update existing binding first. */
	while (cur_e) {
		struct anx_lv *cur = cur_e->bindings;
		while (cur && cur->tag == ANX_LV_CONS) {
			pair = cur->u.cons.car;
			if (pair && pair->tag == ANX_LV_CONS &&
			    pair->u.cons.car &&
			    pair->u.cons.car->tag == ANX_LV_SYM &&
			    anx_strcmp(pair->u.cons.car->u.s.bytes,
				       sym->u.s.bytes) == 0) {
				anx_lv_retain(val);
				anx_lv_release(pair->u.cons.cdr);
				pair->u.cons.cdr = val;
				return ANX_OK;
			}
			cur = cur->u.cons.cdr;
		}
		cur_e = cur_e->parent;
	}
	/* Create at the topmost provided env. */
	anx_lv_retain(sym);
	anx_lv_retain(val);
	pair = anx_lv_cons(sym, val);
	if (!pair) return ANX_ENOMEM;
	struct anx_lv *new_head = anx_lv_cons(pair, e->bindings);
	if (!new_head) { anx_lv_release(pair); return ANX_ENOMEM; }
	e->bindings = new_head;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Reader                                                              */
/* ------------------------------------------------------------------ */

struct reader {
	const char *src;
	uint32_t    pos;
	uint32_t    len;
};

static void skip_ws(struct reader *r)
{
	while (r->pos < r->len) {
		char c = r->src[r->pos];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			r->pos++;
		} else if (c == ';') {
			while (r->pos < r->len && r->src[r->pos] != '\n') r->pos++;
		} else break;
	}
}

static int peek(struct reader *r) {
	skip_ws(r);
	return r->pos < r->len ? r->src[r->pos] : -1;
}

static struct anx_lv *read_form(struct reader *r);

static struct anx_lv *read_string(struct reader *r)
{
	uint32_t start;
	char    *out;
	uint32_t out_len = 0;
	struct anx_lv *v;

	r->pos++;	/* consume opening quote */
	start = r->pos;
	while (r->pos < r->len && r->src[r->pos] != '"') {
		if (r->src[r->pos] == '\\' && r->pos + 1 < r->len) r->pos++;
		r->pos++;
	}
	out = (char *)anx_alloc(r->pos - start + 1);
	if (!out) return NULL;
	uint32_t i = start;
	while (i < r->pos) {
		char c = r->src[i++];
		if (c == '\\' && i < r->pos) {
			char n = r->src[i++];
			switch (n) {
			case 'n':  c = '\n'; break;
			case 't':  c = '\t'; break;
			case 'r':  c = '\r'; break;
			case '"':  c = '"';  break;
			case '\\': c = '\\'; break;
			default:   c = n;
			}
		}
		out[out_len++] = c;
	}
	out[out_len] = '\0';
	if (r->pos < r->len) r->pos++;	/* consume closing quote */
	v = anx_lv_str(out, out_len);
	anx_free(out);
	return v;
}

static int is_atom_char(char c)
{
	if (c == '(' || c == ')' || c == '\'' || c == '"' || c == ';') return 0;
	if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0) return 0;
	return 1;
}

static struct anx_lv *read_atom(struct reader *r)
{
	uint32_t start = r->pos;
	while (r->pos < r->len && is_atom_char(r->src[r->pos])) r->pos++;
	uint32_t n = r->pos - start;
	if (n == 0) return &g_nil;
	const char *s = r->src + start;
	/* Try integer */
	int negative = 0;
	uint32_t i = 0;
	if (s[0] == '-' || s[0] == '+') {
		if (n > 1) {
			negative = s[0] == '-';
			i = 1;
		}
	}
	bool is_int = i < n;
	int64_t val = 0;
	for (; i < n; i++) {
		if (s[i] < '0' || s[i] > '9') { is_int = false; break; }
		val = val * 10 + (s[i] - '0');
	}
	if (is_int) return anx_lv_int(negative ? -val : val);
	/* nil and t */
	if (n == 3 && s[0]=='n' && s[1]=='i' && s[2]=='l') return &g_nil;
	if (n == 1 && s[0]=='t') return &g_t;
	return anx_lv_sym(s, n);
}

static struct anx_lv *read_list(struct reader *r)
{
	r->pos++;	/* consume '(' */
	struct anx_lv *head = &g_nil;
	struct anx_lv *tail = NULL;
	while (1) {
		int c = peek(r);
		if (c == -1) return head;	/* EOF — accept partial */
		if (c == ')') { r->pos++; return head; }
		struct anx_lv *elt = read_form(r);
		if (!elt) elt = &g_nil;
		struct anx_lv *cell = anx_lv_cons(elt, &g_nil);
		if (!cell) return &g_nil;
		if (head == &g_nil) {
			head = cell;
		} else {
			tail->u.cons.cdr = cell;
		}
		tail = cell;
	}
}

static struct anx_lv *read_form(struct reader *r)
{
	int c = peek(r);
	if (c == -1) return &g_nil;
	if (c == '(') return read_list(r);
	if (c == '\'') {
		r->pos++;
		struct anx_lv *q = read_form(r);
		struct anx_lv *qsym = anx_lv_sym("quote", 5);
		return anx_lv_cons(qsym, anx_lv_cons(q, &g_nil));
	}
	if (c == '"') return read_string(r);
	return read_atom(r);
}

/* ------------------------------------------------------------------ */
/* Printer                                                             */
/* ------------------------------------------------------------------ */

struct sb { char *buf; uint32_t cap; uint32_t len; };

static void sb_putc(struct sb *s, char c)
{
	if (s->len + 1 >= s->cap) return;
	s->buf[s->len++] = c;
}
static void sb_puts(struct sb *s, const char *str)
{
	while (*str) sb_putc(s, *str++);
}

static void print_to(struct sb *s, const struct anx_lv *v);

static void print_list(struct sb *s, const struct anx_lv *v)
{
	sb_putc(s, '(');
	bool first = true;
	while (v && v->tag == ANX_LV_CONS) {
		if (!first) sb_putc(s, ' ');
		first = false;
		print_to(s, v->u.cons.car);
		v = v->u.cons.cdr;
	}
	if (v && v->tag != ANX_LV_NIL) {
		sb_puts(s, " . ");
		print_to(s, v);
	}
	sb_putc(s, ')');
}

static void print_to(struct sb *s, const struct anx_lv *v)
{
	char tmp[32];
	if (!v || v == &g_nil) { sb_puts(s, "nil"); return; }
	if (v == &g_t)         { sb_putc(s, 't'); return; }
	switch (v->tag) {
	case ANX_LV_INT:
		anx_snprintf(tmp, sizeof(tmp), "%lld", (long long)v->u.i);
		sb_puts(s, tmp);
		break;
	case ANX_LV_STR:
		sb_putc(s, '"');
		for (uint32_t i = 0; i < v->u.s.len; i++) {
			char c = v->u.s.bytes[i];
			if (c == '"' || c == '\\') sb_putc(s, '\\');
			sb_putc(s, c);
		}
		sb_putc(s, '"');
		break;
	case ANX_LV_SYM:
		sb_puts(s, v->u.s.bytes);
		break;
	case ANX_LV_CONS:
		print_list(s, v);
		break;
	case ANX_LV_FN:
		sb_puts(s, "<lambda>");
		break;
	case ANX_LV_BUILTIN:
		sb_puts(s, "<builtin:");
		sb_puts(s, v->u.builtin->name);
		sb_putc(s, '>');
		break;
	case ANX_LV_BUF:
		anx_snprintf(tmp, sizeof(tmp), "<buffer:%u>",
			     v->u.buf_handle);
		sb_puts(s, tmp);
		break;
	default:
		sb_puts(s, "<?>");
	}
}

/* ------------------------------------------------------------------ */
/* Evaluator                                                           */
/* ------------------------------------------------------------------ */

static struct anx_lv *eval(struct anx_lv *form, struct anx_lv_env *env,
			   struct anx_ed_session *sess);

static uint32_t list_length(struct anx_lv *l)
{
	uint32_t n = 0;
	while (l && l->tag == ANX_LV_CONS) { n++; l = l->u.cons.cdr; }
	return n;
}

static struct anx_lv *nth(struct anx_lv *l, uint32_t n)
{
	while (l && l->tag == ANX_LV_CONS && n > 0) {
		l = l->u.cons.cdr; n--;
	}
	if (l && l->tag == ANX_LV_CONS) return l->u.cons.car;
	return &g_nil;
}

static int truthy(struct anx_lv *v)
{
	if (!v || v == &g_nil) return 0;
	return 1;
}

/* Evaluate each cdr element into a fresh list (refcount-correct). */
static struct anx_lv *eval_args(struct anx_lv *args, struct anx_lv_env *env,
				struct anx_ed_session *sess)
{
	struct anx_lv *head = &g_nil;
	struct anx_lv *tail = NULL;
	while (args && args->tag == ANX_LV_CONS) {
		struct anx_lv *v = eval(args->u.cons.car, env, sess);
		struct anx_lv *cell = anx_lv_cons(v, &g_nil);
		if (head == &g_nil) head = cell;
		else                tail->u.cons.cdr = cell;
		tail = cell;
		args = args->u.cons.cdr;
	}
	return head;
}

/* ------------------------------------------------------------------ */
/* Built-ins                                                           */
/* ------------------------------------------------------------------ */

#define BUILTIN(NAME, FN, MIN, MAX) \
	static struct anx_lv *bi_##FN(struct anx_lv *a, struct anx_lv_env *e, void *ctx); \
	static const struct anx_lv_builtin bd_##FN = { NAME, bi_##FN, (MIN), (MAX) }; \
	static struct anx_lv *bi_##FN(struct anx_lv *a, struct anx_lv_env *e, void *ctx)

#define UNUSED(x) (void)(x)

BUILTIN("+", add, 0, 0xff)
{
	UNUSED(e); UNUSED(ctx);
	int64_t s = 0;
	while (a && a->tag == ANX_LV_CONS) {
		struct anx_lv *v = a->u.cons.car;
		if (v && v->tag == ANX_LV_INT) s += v->u.i;
		a = a->u.cons.cdr;
	}
	return anx_lv_int(s);
}

BUILTIN("-", sub, 1, 0xff)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *first = a->u.cons.car;
	int64_t s = first && first->tag == ANX_LV_INT ? first->u.i : 0;
	a = a->u.cons.cdr;
	if (!a || a->tag == ANX_LV_NIL) return anx_lv_int(-s);
	while (a && a->tag == ANX_LV_CONS) {
		struct anx_lv *v = a->u.cons.car;
		if (v && v->tag == ANX_LV_INT) s -= v->u.i;
		a = a->u.cons.cdr;
	}
	return anx_lv_int(s);
}

BUILTIN("*", mul, 0, 0xff)
{
	UNUSED(e); UNUSED(ctx);
	int64_t s = 1;
	while (a && a->tag == ANX_LV_CONS) {
		struct anx_lv *v = a->u.cons.car;
		if (v && v->tag == ANX_LV_INT) s *= v->u.i;
		a = a->u.cons.cdr;
	}
	return anx_lv_int(s);
}

BUILTIN("/", div, 2, 2)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *x = a->u.cons.car;
	struct anx_lv *y = a->u.cons.cdr->u.cons.car;
	if (!x || x->tag != ANX_LV_INT || !y || y->tag != ANX_LV_INT)
		return anx_lv_int(0);
	if (y->u.i == 0) return anx_lv_int(0);
	return anx_lv_int(x->u.i / y->u.i);
}

BUILTIN("=", eq_int, 2, 2)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *x = a->u.cons.car;
	struct anx_lv *y = a->u.cons.cdr->u.cons.car;
	if (x && y && x->tag == ANX_LV_INT && y->tag == ANX_LV_INT &&
	    x->u.i == y->u.i)
		return &g_t;
	return &g_nil;
}

BUILTIN("<", lt, 2, 2)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *x = a->u.cons.car;
	struct anx_lv *y = a->u.cons.cdr->u.cons.car;
	if (x && y && x->tag == ANX_LV_INT && y->tag == ANX_LV_INT &&
	    x->u.i < y->u.i)
		return &g_t;
	return &g_nil;
}

BUILTIN(">", gt, 2, 2)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *x = a->u.cons.car;
	struct anx_lv *y = a->u.cons.cdr->u.cons.car;
	if (x && y && x->tag == ANX_LV_INT && y->tag == ANX_LV_INT &&
	    x->u.i > y->u.i)
		return &g_t;
	return &g_nil;
}

BUILTIN("cons", cons_bi, 2, 2)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *x = a->u.cons.car;
	struct anx_lv *y = a->u.cons.cdr->u.cons.car;
	anx_lv_retain(x); anx_lv_retain(y);
	return anx_lv_cons(x, y);
}

BUILTIN("car", car_bi, 1, 1)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *x = a->u.cons.car;
	if (x && x->tag == ANX_LV_CONS) {
		anx_lv_retain(x->u.cons.car);
		return x->u.cons.car;
	}
	return &g_nil;
}

BUILTIN("cdr", cdr_bi, 1, 1)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *x = a->u.cons.car;
	if (x && x->tag == ANX_LV_CONS) {
		anx_lv_retain(x->u.cons.cdr);
		return x->u.cons.cdr;
	}
	return &g_nil;
}

BUILTIN("list", list_bi, 0, 0xff)
{
	UNUSED(e); UNUSED(ctx);
	/* eval_args returns a fresh list; just forward. */
	anx_lv_retain(a);
	return a;
}

BUILTIN("length", length_bi, 1, 1)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *x = a->u.cons.car;
	if (x && x->tag == ANX_LV_STR) return anx_lv_int((int64_t)x->u.s.len);
	return anx_lv_int((int64_t)list_length(x));
}

BUILTIN("null?", nullp, 1, 1)
{
	UNUSED(e); UNUSED(ctx);
	return truthy(a->u.cons.car) ? &g_nil : &g_t;
}

BUILTIN("eq?", eqp, 2, 2)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *x = a->u.cons.car;
	struct anx_lv *y = a->u.cons.cdr->u.cons.car;
	if (x == y) return &g_t;
	if (x && y && x->tag == y->tag) {
		if (x->tag == ANX_LV_INT && x->u.i == y->u.i) return &g_t;
		if ((x->tag == ANX_LV_SYM || x->tag == ANX_LV_STR) &&
		    x->u.s.len == y->u.s.len &&
		    anx_memcmp(x->u.s.bytes, y->u.s.bytes, x->u.s.len) == 0)
			return &g_t;
	}
	return &g_nil;
}

BUILTIN("string-append", str_app, 1, 0xff)
{
	UNUSED(e); UNUSED(ctx);
	uint32_t total = 0;
	struct anx_lv *cur = a;
	while (cur && cur->tag == ANX_LV_CONS) {
		struct anx_lv *v = cur->u.cons.car;
		if (v && v->tag == ANX_LV_STR) total += v->u.s.len;
		cur = cur->u.cons.cdr;
	}
	char *buf = (char *)anx_alloc(total + 1);
	if (!buf) return &g_nil;
	uint32_t off = 0;
	cur = a;
	while (cur && cur->tag == ANX_LV_CONS) {
		struct anx_lv *v = cur->u.cons.car;
		if (v && v->tag == ANX_LV_STR) {
			anx_memcpy(buf + off, v->u.s.bytes, v->u.s.len);
			off += v->u.s.len;
		}
		cur = cur->u.cons.cdr;
	}
	buf[off] = '\0';
	struct anx_lv *r = anx_lv_str(buf, off);
	anx_free(buf);
	return r;
}

BUILTIN("princ", princ, 1, 1)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *v = a->u.cons.car;
	struct sb s = { (char *)anx_alloc(512), 512, 0 };
	if (!s.buf) return &g_nil;
	print_to(&s, v);
	s.buf[s.len] = '\0';
	kprintf("%s", s.buf);
	anx_free(s.buf);
	anx_lv_retain(v);
	return v;
}

/* ------------------------------------------------------------------ */
/* Buffer primitives                                                   */
/* ------------------------------------------------------------------ */

static int session_alloc_buf(struct anx_ed_session *sess,
			     struct anx_ed_buffer *buf)
{
	uint32_t i;
	for (i = 0; i < ANX_ED_MAX_BUFFERS; i++) {
		if (!sess->buffers[i]) { sess->buffers[i] = buf; return (int)i; }
	}
	return -1;
}

static struct anx_ed_buffer *session_get_buf(struct anx_ed_session *sess,
					     uint32_t handle)
{
	if (handle >= ANX_ED_MAX_BUFFERS) return NULL;
	return sess->buffers[handle];
}

BUILTIN("buffer-create", buf_create, 0, 0)
{
	UNUSED(a); UNUSED(e);
	struct anx_ed_session *sess = (struct anx_ed_session *)ctx;
	struct anx_ed_buffer *buf;
	if (anx_ed_buf_create(&buf) != ANX_OK) return &g_nil;
	int h = session_alloc_buf(sess, buf);
	if (h < 0) { anx_ed_buf_free(buf); return &g_nil; }
	return lv_buf((uint32_t)h);
}

BUILTIN("buffer-insert", buf_insert, 2, 2)
{
	UNUSED(e);
	struct anx_ed_session *sess = (struct anx_ed_session *)ctx;
	struct anx_lv *bh = a->u.cons.car;
	struct anx_lv *s  = a->u.cons.cdr->u.cons.car;
	if (!bh || bh->tag != ANX_LV_BUF) return &g_nil;
	if (!s  || s->tag  != ANX_LV_STR) return &g_nil;
	struct anx_ed_buffer *buf = session_get_buf(sess, bh->u.buf_handle);
	if (!buf) return &g_nil;
	if (anx_ed_buf_insert(buf, s->u.s.bytes, s->u.s.len) != ANX_OK)
		return &g_nil;
	return &g_t;
}

BUILTIN("buffer-delete", buf_delete, 2, 2)
{
	UNUSED(e);
	struct anx_ed_session *sess = (struct anx_ed_session *)ctx;
	struct anx_lv *bh = a->u.cons.car;
	struct anx_lv *n  = a->u.cons.cdr->u.cons.car;
	if (!bh || bh->tag != ANX_LV_BUF) return &g_nil;
	if (!n  || n->tag  != ANX_LV_INT) return &g_nil;
	struct anx_ed_buffer *buf = session_get_buf(sess, bh->u.buf_handle);
	if (!buf) return &g_nil;
	if (anx_ed_buf_delete(buf, (uint32_t)n->u.i) != ANX_OK) return &g_nil;
	return &g_t;
}

BUILTIN("buffer-goto", buf_goto, 2, 2)
{
	UNUSED(e);
	struct anx_ed_session *sess = (struct anx_ed_session *)ctx;
	struct anx_lv *bh = a->u.cons.car;
	struct anx_lv *n  = a->u.cons.cdr->u.cons.car;
	if (!bh || bh->tag != ANX_LV_BUF) return &g_nil;
	struct anx_ed_buffer *buf = session_get_buf(sess, bh->u.buf_handle);
	if (!buf) return &g_nil;
	uint32_t pos = (n && n->tag == ANX_LV_INT) ? (uint32_t)n->u.i : 0;
	anx_ed_buf_goto(buf, pos);
	return &g_t;
}

BUILTIN("buffer-point", buf_point, 1, 1)
{
	UNUSED(e);
	struct anx_ed_session *sess = (struct anx_ed_session *)ctx;
	struct anx_lv *bh = a->u.cons.car;
	if (!bh || bh->tag != ANX_LV_BUF) return anx_lv_int(0);
	struct anx_ed_buffer *buf = session_get_buf(sess, bh->u.buf_handle);
	return anx_lv_int(buf ? (int64_t)buf->point : 0);
}

BUILTIN("buffer-text", buf_text, 1, 1)
{
	UNUSED(e);
	struct anx_ed_session *sess = (struct anx_ed_session *)ctx;
	struct anx_lv *bh = a->u.cons.car;
	if (!bh || bh->tag != ANX_LV_BUF) return &g_nil;
	struct anx_ed_buffer *buf = session_get_buf(sess, bh->u.buf_handle);
	if (!buf) return &g_nil;
	uint32_t len = anx_ed_buf_length(buf);
	char    *tmp = (char *)anx_alloc(len + 1);
	if (!tmp) return &g_nil;
	uint32_t w = 0;
	anx_ed_buf_text(buf, tmp, len + 1, &w);
	struct anx_lv *r = anx_lv_str(tmp, w);
	anx_free(tmp);
	return r;
}

BUILTIN("buffer-replace", buf_repl, 3, 3)
{
	UNUSED(e);
	struct anx_ed_session *sess = (struct anx_ed_session *)ctx;
	struct anx_lv *bh = a->u.cons.car;
	struct anx_lv *n  = a->u.cons.cdr->u.cons.car;
	struct anx_lv *r  = a->u.cons.cdr->u.cons.cdr->u.cons.car;
	if (!bh || bh->tag != ANX_LV_BUF || !n || n->tag != ANX_LV_STR ||
	    !r || r->tag != ANX_LV_STR) return anx_lv_int(0);
	struct anx_ed_buffer *buf = session_get_buf(sess, bh->u.buf_handle);
	if (!buf) return anx_lv_int(0);
	uint32_t cnt = 0;
	anx_ed_buf_replace_all(buf, n->u.s.bytes, r->u.s.bytes, &cnt);
	return anx_lv_int((int64_t)cnt);
}

BUILTIN("buffer-search", buf_search, 2, 2)
{
	UNUSED(e);
	struct anx_ed_session *sess = (struct anx_ed_session *)ctx;
	struct anx_lv *bh = a->u.cons.car;
	struct anx_lv *n  = a->u.cons.cdr->u.cons.car;
	if (!bh || bh->tag != ANX_LV_BUF || !n || n->tag != ANX_LV_STR)
		return &g_nil;
	struct anx_ed_buffer *buf = session_get_buf(sess, bh->u.buf_handle);
	if (!buf) return &g_nil;
	uint32_t pos;
	if (anx_ed_buf_search(buf, n->u.s.bytes, &pos) == ANX_OK)
		return anx_lv_int((int64_t)pos);
	return &g_nil;
}

/* ------------------------------------------------------------------ */
/* Customization-surface builtins                                       */
/* ------------------------------------------------------------------ */

BUILTIN("not", not_bi, 1, 1)
{
	UNUSED(e); UNUSED(ctx);
	return truthy(a->u.cons.car) ? &g_nil : &g_t;
}

/* (funcall F ARGS...) — apply F to ARGS without re-evaluating ARGS. */
BUILTIN("funcall", funcall_bi, 1, 0xff)
{
	struct anx_lv *fn = a->u.cons.car;
	struct anx_lv *rest = a->u.cons.cdr;
	struct anx_lv_env *env = (struct anx_lv_env *)e;
	struct anx_ed_session *sess = (struct anx_ed_session *)ctx;
	struct anx_lv *result = &g_nil;

	if (fn && fn->tag == ANX_LV_SYM) {
		struct anx_lv *v = env_lookup(env, fn);
		if (v) fn = v;
	}
	if (!fn) return &g_nil;
	if (fn->tag == ANX_LV_BUILTIN) {
		uint32_t n = list_length(rest);
		const struct anx_lv_builtin *b = fn->u.builtin;
		if (n < b->min_args || (b->max_args != 0xff && n > b->max_args))
			return &g_nil;
		return b->fn(rest, env, sess);
	}
	if (fn->tag == ANX_LV_FN) {
		struct anx_lv_env *call_env = env_new(fn->u.fn.closure);
		struct anx_lv *p = fn->u.fn.params;
		struct anx_lv *v = rest;
		anx_lv_retain(result);
		while (p && p->tag == ANX_LV_CONS &&
		       v && v->tag == ANX_LV_CONS) {
			env_set(call_env, p->u.cons.car, v->u.cons.car);
			p = p->u.cons.cdr;
			v = v->u.cons.cdr;
		}
		struct anx_lv *body = fn->u.fn.body;
		while (body && body->tag == ANX_LV_CONS) {
			anx_lv_release(result);
			result = eval(body->u.cons.car, call_env, sess);
			body = body->u.cons.cdr;
		}
		env_release(call_env);
		return result;
	}
	return &g_nil;
}

/* ------------------------------------------------------------------ */
/* Editing primitives bound to the active editor buffer                */
/* ------------------------------------------------------------------ */

#include <anx/namespace.h>

BUILTIN("point", ed_point, 0, 0)
{
	UNUSED(a); UNUSED(e); UNUSED(ctx);
	struct anx_ed_buffer *b = anx_ed_active_buffer();
	return anx_lv_int(b ? (int64_t)b->point : 0);
}

BUILTIN("point-min", ed_point_min, 0, 0)
{
	UNUSED(a); UNUSED(e); UNUSED(ctx);
	return anx_lv_int(0);
}

BUILTIN("point-max", ed_point_max, 0, 0)
{
	UNUSED(a); UNUSED(e); UNUSED(ctx);
	struct anx_ed_buffer *b = anx_ed_active_buffer();
	return anx_lv_int(b ? (int64_t)anx_ed_buf_length(b) : 0);
}

BUILTIN("goto-char", ed_goto_char, 1, 1)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_ed_buffer *b = anx_ed_active_buffer();
	struct anx_lv *n = a->u.cons.car;
	if (!b || !n || n->tag != ANX_LV_INT) return &g_nil;
	anx_ed_buf_goto(b, (uint32_t)n->u.i);
	return anx_lv_int((int64_t)b->point);
}

BUILTIN("insert", ed_insert, 1, 0xff)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *cur = a;
	while (cur && cur->tag == ANX_LV_CONS) {
		struct anx_lv *v = cur->u.cons.car;
		if (v && v->tag == ANX_LV_STR)
			anx_ed_cmd_insert_string(v->u.s.bytes, v->u.s.len);
		else if (v && v->tag == ANX_LV_INT)
			anx_ed_cmd_insert_char((uint32_t)v->u.i);
		cur = cur->u.cons.cdr;
	}
	return &g_nil;
}

BUILTIN("delete-char", ed_delete_char, 1, 1)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *n = a->u.cons.car;
	int64_t k = (n && n->tag == ANX_LV_INT) ? n->u.i : 1;
	while (k > 0) { anx_ed_cmd_delete_forward_char();  k--; }
	while (k < 0) { anx_ed_cmd_delete_backward_char(); k++; }
	return &g_nil;
}

BUILTIN("forward-char", ed_forward_char, 0, 1)
{
	UNUSED(e); UNUSED(ctx);
	int64_t k = 1;
	if (a && a->tag == ANX_LV_CONS && a->u.cons.car &&
	    a->u.cons.car->tag == ANX_LV_INT)
		k = a->u.cons.car->u.i;
	while (k > 0) { anx_ed_cmd_forward_char();  k--; }
	while (k < 0) { anx_ed_cmd_backward_char(); k++; }
	return &g_nil;
}

BUILTIN("backward-char", ed_backward_char, 0, 1)
{
	UNUSED(e); UNUSED(ctx);
	int64_t k = 1;
	if (a && a->tag == ANX_LV_CONS && a->u.cons.car &&
	    a->u.cons.car->tag == ANX_LV_INT)
		k = a->u.cons.car->u.i;
	while (k > 0) { anx_ed_cmd_backward_char(); k--; }
	while (k < 0) { anx_ed_cmd_forward_char();  k++; }
	return &g_nil;
}

BUILTIN("beginning-of-line", ed_bol, 0, 0)
{
	UNUSED(a); UNUSED(e); UNUSED(ctx);
	anx_ed_cmd_beginning_of_line();
	return &g_nil;
}

BUILTIN("end-of-line", ed_eol, 0, 0)
{
	UNUSED(a); UNUSED(e); UNUSED(ctx);
	anx_ed_cmd_end_of_line();
	return &g_nil;
}

BUILTIN("beginning-of-buffer", ed_bob, 0, 0)
{
	UNUSED(a); UNUSED(e); UNUSED(ctx);
	anx_ed_cmd_beginning_of_buffer();
	return &g_nil;
}

BUILTIN("end-of-buffer", ed_eob, 0, 0)
{
	UNUSED(a); UNUSED(e); UNUSED(ctx);
	anx_ed_cmd_end_of_buffer();
	return &g_nil;
}

BUILTIN("save-buffer", ed_save, 0, 0)
{
	UNUSED(a); UNUSED(e); UNUSED(ctx);
	int rc = anx_ed_save();
	return rc == ANX_OK ? &g_t : &g_nil;
}

BUILTIN("find-file", ed_find_file, 1, 1)
{
	UNUSED(e); UNUSED(ctx);
	struct anx_lv *p = a->u.cons.car;
	if (!p || p->tag != ANX_LV_STR) return &g_nil;
	int rc = anx_ed_open_path("posix", p->u.s.bytes);
	return rc == ANX_OK ? &g_t : &g_nil;
}

BUILTIN("current-buffer-name", ed_curbuf_name, 0, 0)
{
	UNUSED(a); UNUSED(e); UNUSED(ctx);
	const char *name = anx_ed_active_buffer_name();
	return anx_lv_str(name, (uint32_t)anx_strlen(name));
}

/* ------------------------------------------------------------------ */
/* Hook primitives                                                      */
/* ------------------------------------------------------------------ */

/* (add-hook 'HOOK FN) — push FN onto the value of HOOK in the root env. */
BUILTIN("add-hook", ed_add_hook, 2, 2)
{
	struct anx_lv *hook = a->u.cons.car;
	struct anx_lv *fn   = a->u.cons.cdr->u.cons.car;
	struct anx_lv *cur;
	struct anx_lv_env *env = (struct anx_lv_env *)e;
	UNUSED(ctx);
	if (!hook || hook->tag != ANX_LV_SYM) return &g_nil;
	cur = env_lookup(env, hook);
	if (!cur) cur = &g_nil;
	anx_lv_retain(fn);
	anx_lv_retain(cur);
	struct anx_lv *new_list = anx_lv_cons(fn, cur);
	env_set(env, hook, new_list);
	anx_lv_release(new_list);
	return &g_t;
}

/* (run-hooks 'HOOK) — funcall each entry in the value of HOOK. */
BUILTIN("run-hooks", ed_run_hooks, 1, 1)
{
	struct anx_lv *hook = a->u.cons.car;
	struct anx_lv_env *env = (struct anx_lv_env *)e;
	struct anx_ed_session *sess = (struct anx_ed_session *)ctx;
	struct anx_lv *list, *cur;
	if (!hook || hook->tag != ANX_LV_SYM) return &g_nil;
	list = env_lookup(env, hook);
	if (!list) return &g_nil;
	cur = list;
	while (cur && cur->tag == ANX_LV_CONS) {
		struct anx_lv *fn = cur->u.cons.car;
		/* Resolve symbol-named functions */
		if (fn && fn->tag == ANX_LV_SYM) {
			struct anx_lv *v = env_lookup(env, fn);
			if (v) fn = v;
		}
		if (fn && fn->tag == ANX_LV_BUILTIN) {
			fn->u.builtin->fn(&g_nil, env, sess);
		} else if (fn && fn->tag == ANX_LV_FN) {
			struct anx_lv_env *ce = env_new(fn->u.fn.closure);
			struct anx_lv *body = fn->u.fn.body;
			struct anx_lv *r = &g_nil;
			anx_lv_retain(r);
			while (body && body->tag == ANX_LV_CONS) {
				anx_lv_release(r);
				r = eval(body->u.cons.car, ce, sess);
				body = body->u.cons.cdr;
			}
			anx_lv_release(r);
			env_release(ce);
		}
		cur = cur->u.cons.cdr;
	}
	return &g_t;
}

/* ------------------------------------------------------------------ */
/* Builtin registry                                                    */
/* ------------------------------------------------------------------ */

static const struct anx_lv_builtin *const g_builtins[] = {
	&bd_add, &bd_sub, &bd_mul, &bd_div,
	&bd_eq_int, &bd_lt, &bd_gt,
	&bd_cons_bi, &bd_car_bi, &bd_cdr_bi, &bd_list_bi, &bd_length_bi,
	&bd_nullp, &bd_eqp,
	&bd_str_app, &bd_princ,
	&bd_buf_create, &bd_buf_insert, &bd_buf_delete, &bd_buf_goto,
	&bd_buf_point, &bd_buf_text, &bd_buf_repl, &bd_buf_search,
	/* Customization-surface builtins */
	&bd_not_bi, &bd_funcall_bi,
	/* Editing primitives bound to active editor */
	&bd_ed_point, &bd_ed_point_min, &bd_ed_point_max,
	&bd_ed_goto_char, &bd_ed_insert, &bd_ed_delete_char,
	&bd_ed_forward_char, &bd_ed_backward_char,
	&bd_ed_bol, &bd_ed_eol, &bd_ed_bob, &bd_ed_eob,
	&bd_ed_save, &bd_ed_find_file, &bd_ed_curbuf_name,
	&bd_ed_add_hook, &bd_ed_run_hooks,
	NULL,
};

static void install_builtins(struct anx_lv_env *env)
{
	uint32_t i;
	for (i = 0; g_builtins[i]; i++) {
		const struct anx_lv_builtin *b = g_builtins[i];
		struct anx_lv *sym = anx_lv_sym(b->name, (uint32_t)anx_strlen(b->name));
		struct anx_lv *bv  = lv_builtin(b);
		env_set(env, sym, bv);
		anx_lv_release(sym);
		anx_lv_release(bv);
	}
}

/* ------------------------------------------------------------------ */
/* Special-form recognition                                            */
/* ------------------------------------------------------------------ */

static struct anx_lv *eval(struct anx_lv *form, struct anx_lv_env *env,
			   struct anx_ed_session *sess)
{
	if (!form || form == &g_nil) return &g_nil;
	if (form == &g_t)             return &g_t;
	switch (form->tag) {
	case ANX_LV_INT:
	case ANX_LV_STR:
	case ANX_LV_BUF:
	case ANX_LV_BUILTIN:
	case ANX_LV_FN:
		anx_lv_retain(form);
		return form;
	case ANX_LV_SYM: {
		struct anx_lv *v = env_lookup(env, form);
		if (v) { anx_lv_retain(v); return v; }
		return &g_nil;
	}
	case ANX_LV_CONS:
		break;
	default:
		return &g_nil;
	}

	struct anx_lv *head = form->u.cons.car;
	struct anx_lv *args = form->u.cons.cdr;

	/* Special forms */
	if (head && head->tag == ANX_LV_SYM) {
		const char *name = head->u.s.bytes;
		if (anx_strcmp(name, "quote") == 0) {
			struct anx_lv *q = args && args->tag == ANX_LV_CONS
				? args->u.cons.car : &g_nil;
			anx_lv_retain(q);
			return q;
		}
		if (anx_strcmp(name, "if") == 0) {
			struct anx_lv *cond = nth(args, 0);
			struct anx_lv *thn  = nth(args, 1);
			struct anx_lv *els  = nth(args, 2);
			struct anx_lv *cv = eval(cond, env, sess);
			int t = truthy(cv);
			anx_lv_release(cv);
			return eval(t ? thn : els, env, sess);
		}
		if (anx_strcmp(name, "and") == 0) {
			struct anx_lv *cur = args;
			struct anx_lv *last = &g_t;
			anx_lv_retain(last);
			while (cur && cur->tag == ANX_LV_CONS) {
				anx_lv_release(last);
				last = eval(cur->u.cons.car, env, sess);
				if (!truthy(last)) return last;
				cur = cur->u.cons.cdr;
			}
			return last;
		}
		if (anx_strcmp(name, "or") == 0) {
			struct anx_lv *cur = args;
			while (cur && cur->tag == ANX_LV_CONS) {
				struct anx_lv *v = eval(cur->u.cons.car, env, sess);
				if (truthy(v)) return v;
				anx_lv_release(v);
				cur = cur->u.cons.cdr;
			}
			return &g_nil;
		}
		if (anx_strcmp(name, "progn") == 0) {
			struct anx_lv *cur = args;
			struct anx_lv *last = &g_nil;
			anx_lv_retain(last);
			while (cur && cur->tag == ANX_LV_CONS) {
				anx_lv_release(last);
				last = eval(cur->u.cons.car, env, sess);
				cur = cur->u.cons.cdr;
			}
			return last;
		}
		if (anx_strcmp(name, "while") == 0) {
			struct anx_lv *cond = nth(args, 0);
			struct anx_lv *cur;
			while (1) {
				struct anx_lv *cv = eval(cond, env, sess);
				int t = truthy(cv);
				anx_lv_release(cv);
				if (!t) break;
				cur = args->u.cons.cdr;
				while (cur && cur->tag == ANX_LV_CONS) {
					struct anx_lv *r = eval(cur->u.cons.car, env, sess);
					anx_lv_release(r);
					cur = cur->u.cons.cdr;
				}
			}
			return &g_nil;
		}
		if (anx_strcmp(name, "setq") == 0) {
			struct anx_lv *sym = nth(args, 0);
			struct anx_lv *val_form = nth(args, 1);
			struct anx_lv *val = eval(val_form, env, sess);
			if (sym && sym->tag == ANX_LV_SYM) {
				env_set(env, sym, val);
			}
			return val;
		}
		if (anx_strcmp(name, "let") == 0 ||
		    anx_strcmp(name, "let*") == 0) {
			struct anx_lv_env *new_env = env_new(env);
			struct anx_lv *bindings = nth(args, 0);
			struct anx_lv *cur = bindings;
			while (cur && cur->tag == ANX_LV_CONS) {
				struct anx_lv *pair = cur->u.cons.car;
				if (pair && pair->tag == ANX_LV_CONS) {
					struct anx_lv *sym = pair->u.cons.car;
					struct anx_lv *vform = pair->u.cons.cdr->u.cons.car;
					struct anx_lv_env *eval_env =
						(name[3] == '*') ? new_env : env;
					struct anx_lv *val = eval(vform, eval_env, sess);
					env_set(new_env, sym, val);
					anx_lv_release(val);
				}
				cur = cur->u.cons.cdr;
			}
			struct anx_lv *body = args->u.cons.cdr;
			struct anx_lv *last = &g_nil;
			anx_lv_retain(last);
			while (body && body->tag == ANX_LV_CONS) {
				anx_lv_release(last);
				last = eval(body->u.cons.car, new_env, sess);
				body = body->u.cons.cdr;
			}
			env_release(new_env);
			return last;
		}
		if (anx_strcmp(name, "lambda") == 0) {
			struct anx_lv *params = nth(args, 0);
			struct anx_lv *body   = args->u.cons.cdr;
			struct anx_lv *fn = anx_lv_new(ANX_LV_FN);
			if (!fn) return &g_nil;
			anx_lv_retain(params); anx_lv_retain(body);
			fn->u.fn.params = params;
			fn->u.fn.body   = body;
			env_retain(env);
			fn->u.fn.closure = env;
			return fn;
		}
		/* (defun NAME (PARAMS) BODY...) */
		if (anx_strcmp(name, "defun") == 0) {
			struct anx_lv *fname  = nth(args, 0);
			struct anx_lv *params = nth(args, 1);
			struct anx_lv *body   = args && args->tag == ANX_LV_CONS
				? args->u.cons.cdr : &g_nil;
			if (body && body->tag == ANX_LV_CONS) body = body->u.cons.cdr;
			if (!fname || fname->tag != ANX_LV_SYM) return &g_nil;
			struct anx_lv *fn = anx_lv_new(ANX_LV_FN);
			if (!fn) return &g_nil;
			anx_lv_retain(params); anx_lv_retain(body);
			fn->u.fn.params = params;
			fn->u.fn.body   = body;
			env_retain(env);
			fn->u.fn.closure = env;
			env_set(env, fname, fn);
			anx_lv_release(fn);
			anx_lv_retain(fname);
			return fname;
		}
		/* (cond (TEST EXPR...)...) */
		if (anx_strcmp(name, "cond") == 0) {
			struct anx_lv *cur = args;
			while (cur && cur->tag == ANX_LV_CONS) {
				struct anx_lv *clause = cur->u.cons.car;
				if (clause && clause->tag == ANX_LV_CONS) {
					struct anx_lv *test = clause->u.cons.car;
					struct anx_lv *cv = eval(test, env, sess);
					if (truthy(cv)) {
						anx_lv_release(cv);
						struct anx_lv *body = clause->u.cons.cdr;
						struct anx_lv *last = &g_nil;
						anx_lv_retain(last);
						while (body && body->tag == ANX_LV_CONS) {
							anx_lv_release(last);
							last = eval(body->u.cons.car, env, sess);
							body = body->u.cons.cdr;
						}
						return last;
					}
					anx_lv_release(cv);
				}
				cur = cur->u.cons.cdr;
			}
			return &g_nil;
		}
		/* (when TEST BODY...) */
		if (anx_strcmp(name, "when") == 0) {
			struct anx_lv *test = nth(args, 0);
			struct anx_lv *cv = eval(test, env, sess);
			int t = truthy(cv);
			anx_lv_release(cv);
			if (!t) return &g_nil;
			struct anx_lv *body = args->u.cons.cdr;
			struct anx_lv *last = &g_nil;
			anx_lv_retain(last);
			while (body && body->tag == ANX_LV_CONS) {
				anx_lv_release(last);
				last = eval(body->u.cons.car, env, sess);
				body = body->u.cons.cdr;
			}
			return last;
		}
		/* (unless TEST BODY...) */
		if (anx_strcmp(name, "unless") == 0) {
			struct anx_lv *test = nth(args, 0);
			struct anx_lv *cv = eval(test, env, sess);
			int t = truthy(cv);
			anx_lv_release(cv);
			if (t) return &g_nil;
			struct anx_lv *body = args->u.cons.cdr;
			struct anx_lv *last = &g_nil;
			anx_lv_retain(last);
			while (body && body->tag == ANX_LV_CONS) {
				anx_lv_release(last);
				last = eval(body->u.cons.car, env, sess);
				body = body->u.cons.cdr;
			}
			return last;
		}
		/* (dolist (VAR LIST [RESULT]) BODY...) */
		if (anx_strcmp(name, "dolist") == 0) {
			struct anx_lv *spec = nth(args, 0);
			if (!spec || spec->tag != ANX_LV_CONS) return &g_nil;
			struct anx_lv *var       = spec->u.cons.car;
			struct anx_lv *list_form = spec->u.cons.cdr->u.cons.car;
			struct anx_lv *list = eval(list_form, env, sess);
			struct anx_lv_env *new_env = env_new(env);
			struct anx_lv *cur = list;
			while (cur && cur->tag == ANX_LV_CONS) {
				env_set(new_env, var, cur->u.cons.car);
				struct anx_lv *body = args->u.cons.cdr;
				while (body && body->tag == ANX_LV_CONS) {
					struct anx_lv *r = eval(body->u.cons.car,
								new_env, sess);
					anx_lv_release(r);
					body = body->u.cons.cdr;
				}
				cur = cur->u.cons.cdr;
			}
			anx_lv_release(list);
			env_release(new_env);
			return &g_nil;
		}
	}

	/* Function application */
	struct anx_lv *fn = eval(head, env, sess);
	if (!fn) return &g_nil;
	struct anx_lv *evargs = eval_args(args, env, sess);
	struct anx_lv *result = &g_nil;

	if (fn->tag == ANX_LV_BUILTIN) {
		uint32_t n = list_length(evargs);
		const struct anx_lv_builtin *b = fn->u.builtin;
		if (n < b->min_args || (b->max_args != 0xff && n > b->max_args)) {
			result = &g_nil;
		} else {
			result = b->fn(evargs, env, sess);
		}
	} else if (fn->tag == ANX_LV_FN) {
		struct anx_lv_env *call_env = env_new(fn->u.fn.closure);
		struct anx_lv *p = fn->u.fn.params;
		struct anx_lv *v = evargs;
		while (p && p->tag == ANX_LV_CONS &&
		       v && v->tag == ANX_LV_CONS) {
			env_set(call_env, p->u.cons.car, v->u.cons.car);
			p = p->u.cons.cdr;
			v = v->u.cons.cdr;
		}
		struct anx_lv *body = fn->u.fn.body;
		anx_lv_retain(result);
		while (body && body->tag == ANX_LV_CONS) {
			anx_lv_release(result);
			result = eval(body->u.cons.car, call_env, sess);
			body = body->u.cons.cdr;
		}
		env_release(call_env);
	}
	anx_lv_release(fn);
	anx_lv_release(evargs);
	return result;
}

/* ------------------------------------------------------------------ */
/* Session API                                                         */
/* ------------------------------------------------------------------ */

int anx_ed_session_create(struct anx_ed_session **out)
{
	if (!out) return ANX_EINVAL;
	struct anx_ed_session *s = (struct anx_ed_session *)anx_zalloc(sizeof(*s));
	if (!s) return ANX_ENOMEM;
	s->root_env = env_new(NULL);
	install_builtins(s->root_env);
	*out = s;
	return ANX_OK;
}

void anx_ed_session_free(struct anx_ed_session *sess)
{
	if (!sess) return;
	uint32_t i;
	for (i = 0; i < ANX_ED_MAX_BUFFERS; i++) {
		if (sess->buffers[i]) anx_ed_buf_free(sess->buffers[i]);
	}
	env_release(sess->root_env);
	anx_free(sess);
}

int anx_ed_eval(struct anx_ed_session *sess, const char *src,
		bool sequence, char *out, uint32_t out_size)
{
	struct reader  r = { src, 0, src ? (uint32_t)anx_strlen(src) : 0 };
	struct anx_lv *last = &g_nil;
	int rc = ANX_OK;

	if (!sess || !src || !out || out_size == 0) return ANX_EINVAL;

	anx_lv_retain(last);
	while (1) {
		int c = peek(&r);
		if (c == -1) break;
		struct anx_lv *form = read_form(&r);
		anx_lv_release(last);
		last = eval(form, sess->root_env, sess);
		anx_lv_release(form);
		if (!sequence) break;
	}

	{
		struct sb sb_ = { out, out_size, 0 };
		print_to(&sb_, last);
		if (sb_.len < out_size) sb_.buf[sb_.len] = '\0';
		else sb_.buf[out_size - 1] = '\0';
	}
	anx_lv_release(last);
	return rc;
}
