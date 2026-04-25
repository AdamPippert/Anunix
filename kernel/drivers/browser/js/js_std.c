/*
 * js_std.c — JavaScript standard library.
 */

#include "js_std.h"
#include "js_engine.h"
#include "js_obj.h"
#include "js_str.h"
#include "js_vm.h"
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/alloc.h>

typedef js_val (*js_native_fn)(struct js_engine *eng, js_val this_val,
                               js_val *args, uint8_t argc);

/* Reuse make_native from js_dom.c via forward declaration */
js_val js_engine_make_native(struct js_engine *eng, js_native_fn fn);

/* ── Math ──────────────────────────────────────────────────────────── */

/* Integer sqrt via Newton's method */
static double _sqrt(double x)
{
	if (x <= 0.0) return 0.0;
	double r = x * 0.5;
	int i;
	for (i = 0; i < 30; i++) r = 0.5 * (r + x / r);
	return r;
}

static double _abs(double x)  { return x < 0 ? -x : x; }
static double _floor(double x) { return (double)(int64_t)x - (x < (double)(int64_t)x ? 1.0 : 0.0); }
static double _ceil(double x)  { return -_floor(-x); }
static double _round(double x) { return _floor(x + 0.5); }
static double _min(double a, double b) { return a < b ? a : b; }
static double _max(double a, double b) { return a > b ? a : b; }
static double _pow(double base, double exp)
{
	if (exp == 0.0) return 1.0;
	/* Approximation for integer exponents */
	int64_t e = (int64_t)exp;
	if ((double)e == exp) {
		double r = 1.0;
		bool neg = e < 0;
		if (neg) e = -e;
		while (e--) r *= base;
		return neg ? 1.0 / r : r;
	}
	return base; /* stub for fractional exponents */
}

static js_val math_abs(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{ (void)eng;(void)t; return n?jv_double(_abs(jv_to_double(a[0]))):JV_NAN; }

static js_val math_floor(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{ (void)eng;(void)t; return n?jv_double(_floor(jv_to_double(a[0]))):JV_NAN; }

static js_val math_ceil(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{ (void)eng;(void)t; return n?jv_double(_ceil(jv_to_double(a[0]))):JV_NAN; }

static js_val math_round(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{ (void)eng;(void)t; return n?jv_double(_round(jv_to_double(a[0]))):JV_NAN; }

static js_val math_sqrt(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{ (void)eng;(void)t; return n?jv_double(_sqrt(jv_to_double(a[0]))):JV_NAN; }

static js_val math_min(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{
	(void)eng;(void)t;
	if (!n) return jv_double(1.0/0.0); /* Infinity */
	double r = jv_to_double(a[0]);
	uint8_t i;
	for (i = 1; i < n; i++) r = _min(r, jv_to_double(a[i]));
	return jv_double(r);
}

static js_val math_max(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{
	(void)eng;(void)t;
	if (!n) return jv_double(-1.0/0.0); /* -Infinity */
	double r = jv_to_double(a[0]);
	uint8_t i;
	for (i = 1; i < n; i++) r = _max(r, jv_to_double(a[i]));
	return jv_double(r);
}

static js_val math_pow(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{ (void)eng;(void)t; return n>=2?jv_double(_pow(jv_to_double(a[0]),jv_to_double(a[1]))):JV_NAN; }

/* LCG random [0,1) */
static uint32_t s_rand_state = 42;
static js_val math_random(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{
	(void)eng;(void)t;(void)a;(void)n;
	s_rand_state = s_rand_state * 1664525u + 1013904223u;
	return jv_double((double)(s_rand_state >> 1) / (double)0x7FFFFFFFu);
}

static js_val math_trunc(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{ (void)eng;(void)t; return n?jv_double((double)(int64_t)jv_to_double(a[0])):JV_NAN; }

static js_val math_sign(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{
	(void)eng;(void)t;
	if (!n) return JV_NAN;
	double v = jv_to_double(a[0]);
	return jv_double(v > 0 ? 1.0 : v < 0 ? -1.0 : 0.0);
}

/* ── JSON ──────────────────────────────────────────────────────────── */

#define JSON_BUF_CAP  4096

/* Append n bytes from src to *buf at *pos, staying within cap. */
static void jb_append(char *buf, uint32_t *pos, uint32_t cap,
                       const char *src, uint32_t n)
{
	if (*pos + n >= cap) n = cap - 1 - *pos;
	if (n == 0) return;
	anx_memcpy(buf + *pos, src, n);
	*pos += n;
}

static void jb_char(char *buf, uint32_t *pos, uint32_t cap, char c)
{
	if (*pos + 1 < cap) buf[(*pos)++] = c;
}

/* Forward declaration for recursive stringify */
static void stringify_val(struct js_engine *eng, js_val v, char *buf,
                          uint32_t *pos, uint32_t cap, int depth);

static void stringify_str(struct js_engine *eng, const struct js_str *str,
                          char *buf, uint32_t *pos, uint32_t cap)
{
	(void)eng;
	uint32_t i;
	jb_char(buf, pos, cap, '"');
	for (i = 0; i < str->len; i++) {
		char c = js_str_data(str)[i];
		if (c == '"')       { jb_append(buf, pos, cap, "\\\"", 2); }
		else if (c == '\\') { jb_append(buf, pos, cap, "\\\\", 2); }
		else if (c == '\n') { jb_append(buf, pos, cap, "\\n",  2); }
		else if (c == '\r') { jb_append(buf, pos, cap, "\\r",  2); }
		else if (c == '\t') { jb_append(buf, pos, cap, "\\t",  2); }
		else jb_char(buf, pos, cap, c);
	}
	jb_char(buf, pos, cap, '"');
}

static void stringify_number(char *buf, uint32_t *pos, uint32_t cap, double d)
{
	char tmp[32];
	int32_t iv = (int32_t)d;
	uint32_t tpos;
	if ((double)iv == d) {
		int neg = iv < 0;
		int32_t av = neg ? -iv : iv;
		tpos = 31; tmp[31] = '\0';
		if (av == 0) { tmp[30] = '0'; tpos = 30; }
		else while (av) { tmp[--tpos] = '0' + (av % 10); av /= 10; }
		if (neg) tmp[--tpos] = '-';
		jb_append(buf, pos, cap, tmp + tpos, 31 - tpos);
	} else {
		/* Simple decimal notation for doubles */
		int neg = d < 0.0;
		if (neg) { d = -d; jb_char(buf, pos, cap, '-'); }
		int32_t ipart = (int32_t)d;
		double fpart = d - (double)ipart;
		int32_t av = ipart;
		tpos = 31; tmp[31] = '\0';
		if (av == 0) { tmp[30] = '0'; tpos = 30; }
		else while (av) { tmp[--tpos] = '0' + (av % 10); av /= 10; }
		jb_append(buf, pos, cap, tmp + tpos, 31 - tpos);
		if (fpart > 0.0) {
			jb_char(buf, pos, cap, '.');
			int prec;
			for (prec = 0; prec < 6; prec++) {
				fpart *= 10.0;
				int digit = (int)fpart;
				jb_char(buf, pos, cap, '0' + digit);
				fpart -= digit;
				if (fpart < 1e-9) break;
			}
		}
	}
}

static void stringify_val(struct js_engine *eng, js_val v, char *buf,
                          uint32_t *pos, uint32_t cap, int depth)
{
	if (depth > 8) { jb_append(buf, pos, cap, "null", 4); return; }
	if (jv_is_null(v))        { jb_append(buf, pos, cap, "null",  4); return; }
	if (jv_is_undef(v))       { jb_append(buf, pos, cap, "null",  4); return; }
	if (jv_is_bool(v)) {
		if (jv_to_bool_raw(v)) jb_append(buf, pos, cap, "true",  4);
		else                   jb_append(buf, pos, cap, "false", 5);
		return;
	}
	if (jv_is_str(v))  { stringify_str(eng, jv_to_str(v), buf, pos, cap); return; }
	if (jv_is_number(v)) { stringify_number(buf, pos, cap, jv_to_double(v)); return; }
	if (jv_is_obj(v)) {
		struct js_obj *o = (struct js_obj *)jv_to_ptr(v);
		if (o->is_array) {
			jb_char(buf, pos, cap, '[');
			uint32_t i;
			for (i = 0; i < o->array_len && i < (uint32_t)JS_OBJ_MAX_PROPS; i++) {
				char idx_str[12];
				uint32_t idx = i, ipos = 11;
				idx_str[11] = '\0';
				if (idx == 0) { idx_str[10] = '0'; ipos = 10; }
				else while (idx) { idx_str[--ipos] = '0' + (idx % 10); idx /= 10; }
				struct js_str *ks = js_str_new(eng->heap, idx_str + ipos, 11 - ipos);
				js_val elem = ks ? js_obj_get(eng->heap, o, ks) : JV_UNDEF;
				if (i) jb_char(buf, pos, cap, ',');
				stringify_val(eng, elem, buf, pos, cap, depth + 1);
			}
			jb_char(buf, pos, cap, ']');
		} else {
			jb_char(buf, pos, cap, '{');
			uint32_t i, first = 1;
			for (i = 0; i < o->n_props; i++) {
				if (!o->props[i].enumerable) continue;
				if (!jv_is_str(o->props[i].key)) continue;
				const struct js_str *ks = jv_to_str(o->props[i].key);
				/* Skip internal properties */
				if (ks->len > 2 && js_str_data(ks)[0] == '_' &&
				    js_str_data(ks)[1] == '_') continue;
				if (!first) jb_char(buf, pos, cap, ',');
				first = 0;
				stringify_str(eng, ks, buf, pos, cap);
				jb_char(buf, pos, cap, ':');
				stringify_val(eng, o->props[i].value, buf, pos, cap, depth + 1);
			}
			jb_char(buf, pos, cap, '}');
		}
		return;
	}
	jb_append(buf, pos, cap, "null", 4);
}

static js_val json_stringify(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{
	(void)t;
	if (!n) return JV_UNDEF;
	char *buf = (char *)anx_alloc(JSON_BUF_CAP);
	if (!buf) return JV_NULL;
	uint32_t pos = 0;
	stringify_val(eng, a[0], buf, &pos, JSON_BUF_CAP, 0);
	buf[pos] = '\0';
	struct js_str *rs = js_str_new(eng->heap, buf, pos);
	anx_free(buf);
	return rs ? jv_str(rs) : JV_NULL;
}

/* Skip whitespace */
static const char *jp_ws(const char *p) { while (*p == ' '||*p == '\t'||*p == '\n'||*p == '\r') p++; return p; }

static js_val jp_val(struct js_engine *eng, const char *p, const char **end);

static js_val jp_string(struct js_engine *eng, const char *p, const char **end)
{
	/* p points just past opening '"' */
	char buf[256];
	uint32_t pos = 0;
	while (*p && *p != '"' && pos < 255) {
		if (*p == '\\') {
			p++;
			char c = *p++;
			if (c == 'n') buf[pos++] = '\n';
			else if (c == 't') buf[pos++] = '\t';
			else if (c == 'r') buf[pos++] = '\r';
			else buf[pos++] = c;
		} else {
			buf[pos++] = *p++;
		}
	}
	if (*p == '"') p++;
	*end = p;
	struct js_str *s = js_str_new(eng->heap, buf, pos);
	return s ? jv_str(s) : JV_NULL;
}

static js_val jp_number(const char *p, const char **end)
{
	int neg = 0; double r = 0.0;
	if (*p == '-') { neg = 1; p++; }
	for (; *p >= '0' && *p <= '9'; p++) r = r * 10.0 + (*p - '0');
	if (*p == '.') {
		double frac = 0.1; p++;
		for (; *p >= '0' && *p <= '9'; p++) { r += (*p - '0') * frac; frac *= 0.1; }
	}
	*end = p;
	return jv_double(neg ? -r : r);
}

static js_val jp_val(struct js_engine *eng, const char *p, const char **end)
{
	p = jp_ws(p);
	if (!*p) { *end = p; return JV_NULL; }
	if (*p == '"') {
		return jp_string(eng, p + 1, end);
	}
	if (*p == '{') {
		p++;
		struct js_obj *o = js_obj_new(eng->heap, JV_NULL);
		if (!o) { *end = p; return JV_NULL; }
		p = jp_ws(p);
		while (*p && *p != '}') {
			p = jp_ws(p);
			if (*p != '"') break;
			const char *kend;
			js_val key = jp_string(eng, p + 1, &kend);
			p = jp_ws(kend);
			if (*p == ':') p++;
			const char *vend;
			js_val val = jp_val(eng, p, &vend);
			p = jp_ws(vend);
			if (jv_is_str(key))
				js_obj_set(o, jv_to_str(key), val);
			if (*p == ',') p++;
		}
		if (*p == '}') p++;
		*end = p;
		return jv_obj(o);
	}
	if (*p == '[') {
		p++;
		struct js_obj *arr = js_arr_new(eng->heap);
		if (!arr) { *end = p; return JV_NULL; }
		uint32_t idx = 0;
		p = jp_ws(p);
		while (*p && *p != ']') {
			const char *vend;
			js_val val = jp_val(eng, p, &vend);
			p = jp_ws(vend);
			/* Set arr[idx] */
			char idx_str[12]; uint32_t ipos = 11;
			idx_str[11] = '\0';
			uint32_t ii = idx;
			if (ii == 0) { idx_str[10] = '0'; ipos = 10; }
			else while (ii) { idx_str[--ipos] = '0' + (ii % 10); ii /= 10; }
			struct js_str *ks = js_str_new(eng->heap, idx_str + ipos, 11 - ipos);
			if (ks) js_obj_set(arr, ks, val);
			arr->array_len = ++idx;
			if (*p == ',') p++;
			p = jp_ws(p);
		}
		if (*p == ']') p++;
		*end = p;
		return jv_obj(arr);
	}
	if (*p == 't' && p[1]=='r'&&p[2]=='u'&&p[3]=='e') { *end=p+4; return JV_TRUE; }
	if (*p == 'f' && p[1]=='a'&&p[2]=='l'&&p[3]=='s'&&p[4]=='e') { *end=p+5; return JV_FALSE; }
	if (*p == 'n' && p[1]=='u'&&p[2]=='l'&&p[3]=='l') { *end=p+4; return JV_NULL; }
	if (*p == '-' || (*p >= '0' && *p <= '9')) return jp_number(p, end);
	*end = p + 1;
	return JV_NULL;
}

static js_val json_parse(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{
	(void)t;
	if (!n || !jv_is_str(a[0])) return JV_NULL;
	const char *src = js_str_data(jv_to_str(a[0]));
	const char *end;
	return jp_val(eng, src, &end);
}

/* ── Global functions ──────────────────────────────────────────────── */

static js_val fn_parse_int(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{
	(void)eng;(void)t;
	if (!n || !jv_is_str(a[0])) return JV_NAN;
	const char *s = js_str_data(jv_to_str(a[0]));
	int radix = (n >= 2) ? jv_to_int(a[1]) : 10;
	if (radix < 2 || radix > 36) radix = 10;
	int32_t r = 0;
	uint32_t i = 0;
	int sign = 1;
	while (s[i] == ' ' || s[i] == '\t') i++;
	if (s[i] == '-') { sign = -1; i++; }
	else if (s[i] == '+') i++;
	if (radix == 16 && s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) i += 2;
	for (; s[i]; i++) {
		int d;
		char c = s[i];
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
		else break;
		if (d >= radix) break;
		r = r * radix + d;
	}
	return jv_int(r * sign);
}

static js_val fn_parse_float(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{
	(void)eng;(void)t;
	if (!n || !jv_is_str(a[0])) return JV_NAN;
	const char *s = js_str_data(jv_to_str(a[0]));
	double r = 0.0, frac = 0.1;
	int sign = 1;
	uint32_t i = 0;
	while (s[i] == ' ') i++;
	if (s[i] == '-') { sign = -1; i++; }
	else if (s[i] == '+') i++;
	for (; s[i] >= '0' && s[i] <= '9'; i++) r = r * 10.0 + (s[i] - '0');
	if (s[i] == '.') {
		i++;
		for (; s[i] >= '0' && s[i] <= '9'; i++) { r += (s[i] - '0') * frac; frac *= 0.1; }
	}
	return jv_double(r * sign);
}

static js_val fn_is_nan(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{
	(void)eng;(void)t;
	if (!n) return JV_TRUE;
	double d = jv_to_double(a[0]);
	return jv_bool(d != d);
}

static js_val fn_is_finite(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{
	(void)eng;(void)t;
	if (!n) return JV_FALSE;
	double d = jv_to_double(a[0]);
	return jv_bool(d == d && d != 1.0/0.0 && d != -1.0/0.0);
}

static js_val fn_string(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{
	(void)t;
	if (!n) {
		struct js_str *s = js_str_new(eng->heap, "", 0);
		return s ? jv_str(s) : JV_NULL;
	}
	js_val v = a[0];
	if (jv_is_str(v)) return v;
	if (jv_is_null(v))  { struct js_str *s = js_str_from_cstr(eng->heap, "null"); return s?jv_str(s):JV_NULL; }
	if (jv_is_undef(v)) { struct js_str *s = js_str_from_cstr(eng->heap, "undefined"); return s?jv_str(s):JV_NULL; }
	if (jv_is_bool(v))  {
		const char *bs = jv_to_bool_raw(v) ? "true" : "false";
		struct js_str *s = js_str_from_cstr(eng->heap, bs);
		return s ? jv_str(s) : JV_NULL;
	}
	if (jv_is_int(v)) {
		char buf[16]; int32_t iv = jv_to_int(v);
		int neg = iv < 0; if (neg) iv = -iv;
		uint32_t pos = 15; buf[15] = '\0';
		if (iv == 0) { buf[14] = '0'; pos = 14; }
		else while (iv) { buf[--pos] = '0' + (iv % 10); iv /= 10; }
		if (neg) buf[--pos] = '-';
		struct js_str *s = js_str_new(eng->heap, buf + pos, 15 - pos);
		return s ? jv_str(s) : JV_NULL;
	}
	struct js_str *s = js_str_from_cstr(eng->heap, "[object]");
	return s ? jv_str(s) : JV_NULL;
}

static js_val fn_number(struct js_engine *eng, js_val t, js_val *a, uint8_t n)
{ (void)eng;(void)t; return n ? jv_double(jv_to_double(a[0])) : jv_int(0); }

/* ── init ──────────────────────────────────────────────────────────── */

/* Forward: js_engine.c provides make_native wrapper */
extern js_val js_engine_make_native(struct js_engine *eng, js_native_fn fn);

void js_std_init(struct js_engine *eng)
{
	/* Math */
	struct js_obj *math = js_obj_new(eng->heap, JV_NULL);
	if (math) {
		js_obj_set_cstr(eng->heap, math, "abs",    js_engine_make_native(eng, math_abs));
		js_obj_set_cstr(eng->heap, math, "floor",  js_engine_make_native(eng, math_floor));
		js_obj_set_cstr(eng->heap, math, "ceil",   js_engine_make_native(eng, math_ceil));
		js_obj_set_cstr(eng->heap, math, "round",  js_engine_make_native(eng, math_round));
		js_obj_set_cstr(eng->heap, math, "sqrt",   js_engine_make_native(eng, math_sqrt));
		js_obj_set_cstr(eng->heap, math, "min",    js_engine_make_native(eng, math_min));
		js_obj_set_cstr(eng->heap, math, "max",    js_engine_make_native(eng, math_max));
		js_obj_set_cstr(eng->heap, math, "pow",    js_engine_make_native(eng, math_pow));
		js_obj_set_cstr(eng->heap, math, "random", js_engine_make_native(eng, math_random));
		js_obj_set_cstr(eng->heap, math, "trunc",  js_engine_make_native(eng, math_trunc));
		js_obj_set_cstr(eng->heap, math, "sign",   js_engine_make_native(eng, math_sign));
		js_obj_set_cstr(eng->heap, math, "PI",     jv_double(3.14159265358979));
		js_obj_set_cstr(eng->heap, math, "E",      jv_double(2.71828182845904));
		js_obj_set_cstr(eng->heap, math, "LN2",    jv_double(0.693147180559945));
		js_obj_set_cstr(eng->heap, eng->vm.globals,"Math", jv_obj(math));
	}

	/* JSON */
	struct js_obj *json = js_obj_new(eng->heap, JV_NULL);
	if (json) {
		js_obj_set_cstr(eng->heap, json, "stringify", js_engine_make_native(eng, json_stringify));
		js_obj_set_cstr(eng->heap, json, "parse",     js_engine_make_native(eng, json_parse));
		js_obj_set_cstr(eng->heap, eng->vm.globals,"JSON", jv_obj(json));
	}

	/* Global functions */
	js_obj_set_cstr(eng->heap, eng->vm.globals,"parseInt",   js_engine_make_native(eng, fn_parse_int));
	js_obj_set_cstr(eng->heap, eng->vm.globals,"parseFloat", js_engine_make_native(eng, fn_parse_float));
	js_obj_set_cstr(eng->heap, eng->vm.globals,"isNaN",      js_engine_make_native(eng, fn_is_nan));
	js_obj_set_cstr(eng->heap, eng->vm.globals,"isFinite",   js_engine_make_native(eng, fn_is_finite));
	js_obj_set_cstr(eng->heap, eng->vm.globals,"String",     js_engine_make_native(eng, fn_string));
	js_obj_set_cstr(eng->heap, eng->vm.globals,"Number",     js_engine_make_native(eng, fn_number));
}
