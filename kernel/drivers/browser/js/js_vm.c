/*
 * js_vm.c — Bytecode interpreter.
 */

#include "js_vm.h"
#include "js_obj.h"
#include "js_str.h"
#include "js_std.h"
#include <anx/string.h>
#include <anx/kprintf.h>

/* ── Arithmetic helpers ─────────────────────────────────────────────── */

static double val_to_num(js_val v)
{
	if (jv_is_double(v)) return jv_to_double(v);
	if (jv_is_int(v))    return (double)jv_to_int(v);
	if (jv_is_bool(v))   return jv_to_bool_raw(v) ? 1.0 : 0.0;
	if (jv_is_null(v))   return 0.0;
	if (jv_is_str(v)) {
		/* Quick decimal parse */
		const struct js_str *s = jv_to_str(v);
		if (s->len == 0) return 0.0;
		double r = 0.0;
		const char *d = js_str_data(s);
		uint32_t i = 0;
		int sign = 1;
		if (d[i] == '-') { sign = -1; i++; }
		else if (d[i] == '+') i++;
		for (; i < s->len && d[i] >= '0' && d[i] <= '9'; i++)
			r = r * 10.0 + (d[i] - '0');
		if (i < s->len && d[i] == '.') {
			double frac = 0.1; i++;
			for (; i < s->len && d[i] >= '0' && d[i] <= '9'; i++) {
				r += (d[i] - '0') * frac; frac *= 0.1;
			}
		}
		return r * sign;
	}
	/* Object → NaN */
	uint64_t nan = UINT64_C(0x7FF8000000000000);
	double d; __builtin_memcpy(&d, &nan, 8); return d;
}

static int32_t val_to_i32(js_val v)
{
	double d = val_to_num(v);
	if (d != d || d == 0.0) return 0;
	return (int32_t)(uint32_t)d;
}

static bool val_truthy(js_val v)
{
	if (jv_is_bool(v))   return jv_to_bool_raw(v);
	if (jv_is_null(v))   return false;
	if (jv_is_undef(v))  return false;
	if (jv_is_int(v))    return jv_to_int(v) != 0;
	if (jv_is_double(v)) { double d = jv_to_double(v); return d != 0.0 && d == d; }
	if (jv_is_str(v))    return jv_to_str(v)->len > 0;
	if (jv_is_obj(v))    return true;
	return false;
}

static bool val_strict_eq(js_val a, js_val b)
{
	if (a == b) return true;
	if (jv_is_double(a) && jv_is_double(b))
		return jv_to_double(a) == jv_to_double(b);
	if (jv_is_str(a) && jv_is_str(b))
		return js_str_eq(jv_to_str(a), jv_to_str(b));
	return false;
}

static bool val_abstract_eq(js_val a, js_val b)
{
	if (val_strict_eq(a, b)) return true;
	/* number coercions */
	if ((jv_is_number(a) || jv_is_null(a) || jv_is_undef(a)) &&
	    (jv_is_number(b) || jv_is_null(b) || jv_is_undef(b)))
		return val_to_num(a) == val_to_num(b);
	/* null == undefined */
	if ((jv_is_null(a) && jv_is_undef(b)) ||
	    (jv_is_undef(a) && jv_is_null(b))) return true;
	return false;
}

static js_val val_add(struct js_vm *vm, js_val a, js_val b)
{
	if (jv_is_str(a) || jv_is_str(b)) {
		/* String concatenation */
		const char *sa = "", *sb = "";
		uint32_t la = 0, lb = 0;
		char nbuf_a[32], nbuf_b[32];
		if (jv_is_str(a)) { sa = js_str_data(jv_to_str(a)); la = jv_to_str(a)->len; }
		else {
			/* Number → string: quick itoa */
			double da = val_to_num(a);
			int32_t ia = (int32_t)da;
			if ((double)ia == da) {
				int neg = (ia < 0); if (neg) ia = -ia;
				uint32_t pos = 31; nbuf_a[31] = '\0';
				do { nbuf_a[--pos] = '0' + (ia % 10); ia /= 10; } while (ia);
				if (neg) nbuf_a[--pos] = '-';
				sa = nbuf_a + pos; la = 31 - pos;
			} else { sa = "NaN"; la = 3; }
		}
		if (jv_is_str(b)) { sb = js_str_data(jv_to_str(b)); lb = jv_to_str(b)->len; }
		else {
			double db = val_to_num(b);
			int32_t ib = (int32_t)db;
			if ((double)ib == db) {
				int neg = (ib < 0); if (neg) ib = -ib;
				uint32_t pos = 31; nbuf_b[31] = '\0';
				do { nbuf_b[--pos] = '0' + (ib % 10); ib /= 10; } while (ib);
				if (neg) nbuf_b[--pos] = '-';
				sb = nbuf_b + pos; lb = 31 - pos;
			} else { sb = "NaN"; lb = 3; }
		}
		struct js_str *cat = js_str_new(vm->heap, NULL, la + lb);
		if (!cat) return JV_UNDEF;
		char *dst = (char *)((uint8_t *)cat + sizeof(struct js_str));
		if (la) anx_memcpy(dst, sa, la);
		if (lb) anx_memcpy(dst + la, sb, lb);
		dst[la + lb] = '\0';
		cat->hash = js_str_hash_buf(dst, la + lb);
		return jv_str(cat);
	}
	return jv_double(val_to_num(a) + val_to_num(b));
}

/* ── Read 2-byte operand (little-endian) ─────────────────────────── */

static uint16_t read_u16(const uint8_t *code, uint32_t ip)
{
	return (uint16_t)(code[ip] | ((uint16_t)code[ip+1] << 8));
}

static int16_t read_i16(const uint8_t *code, uint32_t ip)
{
	return (int16_t)read_u16(code, ip);
}

/* ── typeof string ────────────────────────────────────────────────── */

static js_val typeof_val(struct js_vm *vm, js_val v)
{
	const char *s;
	if (jv_is_undef(v))  s = "undefined";
	else if (jv_is_null(v))   s = "object"; /* spec says "object" for null */
	else if (jv_is_bool(v))   s = "boolean";
	else if (jv_is_number(v)) s = "number";
	else if (jv_is_str(v))    s = "string";
	else if (jv_is_obj(v)) {
		struct js_obj *o = (struct js_obj *)jv_to_ptr(v);
		s = o->is_function ? "function" : "object";
	} else s = "unknown";
	uint32_t len = 0; while (s[len]) len++;
	struct js_str *ts = js_str_new(vm->heap, s, len);
	return ts ? jv_str(ts) : JV_UNDEF;
}

/* ── Main interpreter loop ────────────────────────────────────────── */

void js_vm_init(struct js_vm *vm, struct js_heap *h, struct js_obj *globals)
{
	vm->heap       = h;
	vm->globals    = globals;
	vm->sp         = 0;
	vm->call_depth = 0;
	vm->exception  = JV_UNDEF;
	vm->threw      = false;
}

js_val js_vm_exec(struct js_vm *vm,
                  const struct js_func_proto *fn,
                  const struct js_func_proto *all_fns, uint32_t n_fns,
                  js_val this_val, js_val *args, uint8_t argc)
{
	if (vm->call_depth >= JS_CALL_DEPTH) {
		vm->threw = true; vm->exception = JV_UNDEF;
		return JV_UNDEF;
	}
	struct js_call_frame *fr = &vm->frames[vm->call_depth++];
	fr->fn        = fn;
	fr->ip        = 0;
	fr->this_val  = this_val;
	fr->try_depth = 0;
	fr->n_locals  = fn->n_locals;

	/* Copy arguments into locals */
	uint8_t i;
	for (i = 0; i < fn->n_params && i < argc; i++)
		fr->locals[i] = args[i];
	for (; i < fn->n_locals; i++)
		fr->locals[i] = JV_UNDEF;

	uint16_t base_sp = vm->sp;

#define PUSH(v)  do { if (vm->sp < JS_STACK_MAX) vm->stack[vm->sp++] = (v); } while(0)
#define POP()    (vm->sp > base_sp ? vm->stack[--vm->sp] : JV_UNDEF)
#define PEEK()   (vm->sp > base_sp ? vm->stack[vm->sp-1] : JV_UNDEF)
#define PEEK2()  (vm->sp > base_sp+1 ? vm->stack[vm->sp-2] : JV_UNDEF)

	const uint8_t *code = fn->code;
	js_val result = JV_UNDEF;

	for (;;) {
		uint32_t ip = fr->ip;
		if (ip >= fn->code_len) break;
		uint8_t op = code[ip];
		fr->ip = ip + 1;

		switch ((js_opcode)op) {

		case OP_NOP: break;
		case OP_HALT: goto done;

		case OP_PUSH_CONST: {
			uint16_t ci = read_u16(code, fr->ip); fr->ip += 2;
			PUSH(fn->consts[ci]);
			break;
		}
		case OP_PUSH_INT: {
			int16_t v = read_i16(code, fr->ip); fr->ip += 2;
			PUSH(jv_int((int32_t)v));
			break;
		}
		case OP_PUSH_TRUE:  PUSH(JV_TRUE); break;
		case OP_PUSH_FALSE: PUSH(JV_FALSE); break;
		case OP_PUSH_NULL:  PUSH(JV_NULL); break;
		case OP_PUSH_UNDEF: PUSH(JV_UNDEF); break;
		case OP_POP:  POP(); break;
		case OP_DUP:  { js_val v = PEEK(); PUSH(v); break; }
		case OP_SWAP: {
			if (vm->sp >= base_sp + 2) {
				js_val t = vm->stack[vm->sp-1];
				vm->stack[vm->sp-1] = vm->stack[vm->sp-2];
				vm->stack[vm->sp-2] = t;
			}
			break;
		}

		case OP_LOAD_LOCAL: {
			uint8_t slot = code[fr->ip++];
			PUSH(slot < fr->n_locals ? fr->locals[slot] : JV_UNDEF);
			break;
		}
		case OP_STORE_LOCAL: {
			uint8_t slot = code[fr->ip++];
			js_val v = POP();
			if (slot < fr->n_locals) fr->locals[slot] = v;
			break;
		}
		case OP_LOAD_GLOBAL: {
			uint16_t ci = read_u16(code, fr->ip); fr->ip += 2;
			js_val key = fn->consts[ci];
			PUSH(js_obj_get_cstr(vm->heap, vm->globals,
			                     js_str_data(jv_to_str(key))));
			break;
		}
		case OP_STORE_GLOBAL: {
			uint16_t ci = read_u16(code, fr->ip); fr->ip += 2;
			js_val key = fn->consts[ci];
			js_val val = POP();
			js_obj_set_cstr(vm->heap, vm->globals,
			                js_str_data(jv_to_str(key)), val);
			break;
		}

		case OP_GET_PROP: {
			uint16_t ci = read_u16(code, fr->ip); fr->ip += 2;
			js_val obj = POP();
			if (jv_is_obj(obj)) {
				struct js_obj *o = (struct js_obj *)jv_to_ptr(obj);
				js_val key = fn->consts[ci];
				PUSH(js_obj_get_cstr(vm->heap, o, js_str_data(jv_to_str(key))));
			} else if (jv_is_str(obj)) {
				/* string.length */
				js_val key = fn->consts[ci];
				const char *kn = js_str_data(jv_to_str(key));
				if (anx_strcmp(kn, "length") == 0)
					PUSH(jv_int((int32_t)jv_to_str(obj)->len));
				else
					PUSH(JV_UNDEF);
			} else {
				PUSH(JV_UNDEF);
			}
			break;
		}
		case OP_SET_PROP: {
			uint16_t ci = read_u16(code, fr->ip); fr->ip += 2;
			js_val val = POP();
			js_val obj = POP();
			if (jv_is_obj(obj)) {
				struct js_obj *o = (struct js_obj *)jv_to_ptr(obj);
				js_val key = fn->consts[ci];
				js_obj_set_cstr(vm->heap, o, js_str_data(jv_to_str(key)), val);
			}
			break;
		}
		case OP_INIT_PROP: {
			uint16_t ci = read_u16(code, fr->ip); fr->ip += 2;
			js_val val = POP();
			js_val obj = PEEK(); /* object stays on stack */
			if (jv_is_obj(obj)) {
				struct js_obj *o = (struct js_obj *)jv_to_ptr(obj);
				js_val key = fn->consts[ci];
				js_obj_set_cstr(vm->heap, o, js_str_data(jv_to_str(key)), val);
			}
			break;
		}
		case OP_GET_INDEX: {
			js_val key = POP();
			js_val obj = POP();
			if (jv_is_obj(obj)) {
				struct js_obj *o = (struct js_obj *)jv_to_ptr(obj);
				if (jv_is_str(key)) {
					PUSH(js_obj_get_cstr(vm->heap, o, js_str_data(jv_to_str(key))));
				} else if (jv_is_int(key) || jv_is_double(key)) {
					int32_t idx = val_to_i32(key);
					char ibuf[12];
					int32_t tmp = idx; uint32_t pos = 11; ibuf[11] = '\0';
					if (tmp == 0) { ibuf[10] = '0'; pos = 10; }
					else { while (tmp > 0) { ibuf[--pos] = '0' + (tmp % 10); tmp /= 10; } }
					PUSH(js_obj_get_cstr(vm->heap, o, ibuf + pos));
				} else PUSH(JV_UNDEF);
			} else if (jv_is_str(obj)) {
				if (jv_is_int(key)) {
					int32_t idx = jv_to_int(key);
					const struct js_str *s = jv_to_str(obj);
					if (idx >= 0 && (uint32_t)idx < s->len) {
						struct js_str *ch = js_str_new(vm->heap,
						                               js_str_data(s) + idx, 1);
						PUSH(ch ? jv_str(ch) : JV_UNDEF);
					} else PUSH(JV_UNDEF);
				} else PUSH(JV_UNDEF);
			} else PUSH(JV_UNDEF);
			break;
		}
		case OP_SET_INDEX: {
			js_val val = POP();
			js_val key = POP();
			js_val obj = POP();
			if (jv_is_obj(obj) && jv_is_str(key)) {
				struct js_obj *o = (struct js_obj *)jv_to_ptr(obj);
				js_obj_set_cstr(vm->heap, o, js_str_data(jv_to_str(key)), val);
			}
			break;
		}
		case OP_DEL_PROP: {
			uint16_t ci = read_u16(code, fr->ip); fr->ip += 2;
			js_val obj = POP();
			if (jv_is_obj(obj)) {
				struct js_obj *o = (struct js_obj *)jv_to_ptr(obj);
				js_val key = fn->consts[ci];
				struct js_str *ks = js_str_from_cstr(vm->heap,
				                                     js_str_data(jv_to_str(key)));
				if (ks) js_obj_delete(o, ks);
			}
			break;
		}
		case OP_NEW_OBJECT: {
			struct js_obj *o = js_obj_new(vm->heap, JV_NULL);
			PUSH(o ? jv_obj(o) : JV_NULL);
			break;
		}
		case OP_NEW_ARRAY: {
			uint16_t cnt = read_u16(code, fr->ip); fr->ip += 2;
			struct js_obj *arr = js_arr_new(vm->heap);
			if (!arr) { PUSH(JV_NULL); break; }
			/* Pop elements in reverse, store as "0","1"... */
			for (int ai = (int)cnt - 1; ai >= 0; ai--) {
				js_val el = POP();
				char ibuf[12]; int tmp = ai; uint32_t pos = 11; ibuf[11] = '\0';
				if (tmp == 0) { ibuf[10] = '0'; pos = 10; }
				else { while (tmp > 0) { ibuf[--pos] = '0' + (tmp % 10); tmp /= 10; } }
				js_obj_set_cstr(vm->heap, arr, ibuf + pos, el);
			}
			arr->array_len = cnt;
			PUSH(jv_obj(arr));
			break;
		}

		/* ── Arithmetic ── */
		case OP_ADD: { js_val b = POP(); js_val a = POP(); PUSH(val_add(vm, a, b)); break; }
		case OP_SUB: { js_val b = POP(); js_val a = POP(); PUSH(jv_double(val_to_num(a) - val_to_num(b))); break; }
		case OP_MUL: { js_val b = POP(); js_val a = POP(); PUSH(jv_double(val_to_num(a) * val_to_num(b))); break; }
		case OP_DIV: { js_val b = POP(); js_val a = POP(); double db = val_to_num(b); PUSH(jv_double(db == 0.0 ? 0.0/0.0 : val_to_num(a) / db)); break; }
		case OP_MOD: { js_val b = POP(); js_val a = POP(); double db = val_to_num(b); int32_t ia = val_to_i32(a), ib = val_to_i32(b); PUSH(ib ? jv_int(ia % ib) : JV_NAN); (void)db; break; }
		case OP_POW: { js_val b = POP(); js_val a = POP(); PUSH(jv_double(js_math_pow(val_to_num(a), val_to_num(b)))); break; }
		case OP_NEG: { js_val a = POP(); PUSH(jv_double(-val_to_num(a))); break; }
		case OP_POS: { js_val a = POP(); PUSH(jv_double(val_to_num(a))); break; }
		case OP_BITAND: { int32_t b = val_to_i32(POP()); int32_t a = val_to_i32(POP()); PUSH(jv_int(a & b)); break; }
		case OP_BITOR:  { int32_t b = val_to_i32(POP()); int32_t a = val_to_i32(POP()); PUSH(jv_int(a | b)); break; }
		case OP_BITXOR: { int32_t b = val_to_i32(POP()); int32_t a = val_to_i32(POP()); PUSH(jv_int(a ^ b)); break; }
		case OP_BITNOT: { int32_t a = val_to_i32(POP()); PUSH(jv_int(~a)); break; }
		case OP_SHL:    { int32_t b = val_to_i32(POP()) & 31; int32_t a = val_to_i32(POP()); PUSH(jv_int(a << b)); break; }
		case OP_SHR:    { int32_t b = val_to_i32(POP()) & 31; int32_t a = val_to_i32(POP()); PUSH(jv_int(a >> b)); break; }
		case OP_USHR:   { int32_t b = val_to_i32(POP()) & 31; uint32_t a = (uint32_t)val_to_i32(POP()); PUSH(jv_int((int32_t)(a >> b))); break; }
		case OP_INC:    { js_val a = POP(); PUSH(jv_double(val_to_num(a) + 1.0)); break; }
		case OP_DEC:    { js_val a = POP(); PUSH(jv_double(val_to_num(a) - 1.0)); break; }

		/* ── Comparison ── */
		case OP_EQ:         { js_val b = POP(); js_val a = POP(); PUSH(jv_bool(val_abstract_eq(a, b))); break; }
		case OP_NEQ:        { js_val b = POP(); js_val a = POP(); PUSH(jv_bool(!val_abstract_eq(a, b))); break; }
		case OP_STRICT_EQ:  { js_val b = POP(); js_val a = POP(); PUSH(jv_bool(val_strict_eq(a, b))); break; }
		case OP_STRICT_NEQ: { js_val b = POP(); js_val a = POP(); PUSH(jv_bool(!val_strict_eq(a, b))); break; }
		case OP_LT: { double b = val_to_num(POP()); double a = val_to_num(POP()); PUSH(jv_bool(a < b)); break; }
		case OP_GT: { double b = val_to_num(POP()); double a = val_to_num(POP()); PUSH(jv_bool(a > b)); break; }
		case OP_LE: { double b = val_to_num(POP()); double a = val_to_num(POP()); PUSH(jv_bool(a <= b)); break; }
		case OP_GE: { double b = val_to_num(POP()); double a = val_to_num(POP()); PUSH(jv_bool(a >= b)); break; }
		case OP_IN: {
			js_val obj = POP(); js_val key = POP();
			bool found = false;
			if (jv_is_obj(obj) && jv_is_str(key)) {
				struct js_obj *o = (struct js_obj *)jv_to_ptr(obj);
				found = !jv_is_undef(js_obj_get_cstr(vm->heap, o,
				                                     js_str_data(jv_to_str(key))));
			}
			PUSH(jv_bool(found));
			break;
		}
		case OP_INSTANCEOF: { js_val b = POP(); (void)b; js_val a = POP(); PUSH(jv_bool(jv_is_obj(a))); break; }

		/* ── Logic ── */
		case OP_NOT: { js_val a = POP(); PUSH(jv_bool(!val_truthy(a))); break; }
		case OP_AND: {
			int16_t off = read_i16(code, fr->ip); fr->ip += 2;
			if (!val_truthy(PEEK())) { fr->ip += (uint32_t)(int32_t)off; }
			else { POP(); }
			break;
		}
		case OP_OR: {
			int16_t off = read_i16(code, fr->ip); fr->ip += 2;
			if (val_truthy(PEEK())) { fr->ip += (uint32_t)(int32_t)off; }
			else { POP(); }
			break;
		}

		/* ── Jumps ── */
		case OP_JUMP: {
			int16_t off = read_i16(code, fr->ip); fr->ip += 2;
			fr->ip = (uint32_t)((int32_t)fr->ip + off);
			break;
		}
		case OP_JUMP_IF_FALSE: {
			int16_t off = read_i16(code, fr->ip); fr->ip += 2;
			js_val v = POP();
			if (!val_truthy(v))
				fr->ip = (uint32_t)((int32_t)fr->ip + off);
			break;
		}
		case OP_JUMP_IF_TRUE: {
			int16_t off = read_i16(code, fr->ip); fr->ip += 2;
			js_val v = POP();
			if (val_truthy(v))
				fr->ip = (uint32_t)((int32_t)fr->ip + off);
			break;
		}

		/* ── Calls ── */
		case OP_CALL: {
			uint8_t argc_c = code[fr->ip++];
			/* Args are on stack: stack[sp-argc..sp-1], callee at sp-argc-1 */
			if (vm->sp < (uint16_t)(base_sp + argc_c + 1)) {
				PUSH(JV_UNDEF); break;
			}
			js_val call_args[JS_LOCAL_MAX];
			uint8_t ai;
			for (ai = 0; ai < argc_c && ai < JS_LOCAL_MAX; ai++)
				call_args[argc_c - 1 - ai] = POP();
			js_val callee = POP();
			js_val ret = js_vm_call(vm, callee, JV_UNDEF,
			                        call_args, argc_c, all_fns, n_fns);
			PUSH(ret);
			break;
		}
		case OP_CALL_METHOD: {
			uint16_t ci = read_u16(code, fr->ip); fr->ip += 2;
			uint8_t argc_m = code[fr->ip++];
			if (vm->sp < (uint16_t)(base_sp + argc_m + 1)) {
				PUSH(JV_UNDEF); break;
			}
			js_val margs[JS_LOCAL_MAX];
			uint8_t ai;
			for (ai = 0; ai < argc_m && ai < JS_LOCAL_MAX; ai++)
				margs[argc_m - 1 - ai] = POP();
			js_val obj = POP();
			js_val method_key = fn->consts[ci];
			js_val method = JV_UNDEF;
			if (jv_is_obj(obj)) {
				struct js_obj *o = (struct js_obj *)jv_to_ptr(obj);
				method = js_obj_get_cstr(vm->heap, o,
				                         js_str_data(jv_to_str(method_key)));
			}
			js_val ret = js_vm_call(vm, method, obj, margs, argc_m, all_fns, n_fns);
			PUSH(ret);
			break;
		}
		case OP_RETURN: {
			result = POP();
			goto done;
		}
		case OP_MAKE_FUNC: {
			uint16_t ci = read_u16(code, fr->ip); fr->ip += 2;
			int32_t fn_idx = jv_to_int(fn->consts[ci]);
			/* Wrap function index in a js_obj with is_function = 1 */
			struct js_obj *fo = js_obj_new(vm->heap, JV_NULL);
			if (!fo) { PUSH(JV_NULL); break; }
			fo->is_function = 1;
			js_obj_set_cstr(vm->heap, fo, "__fn_idx__",
			                jv_int(fn_idx));
			PUSH(jv_obj(fo));
			break;
		}

		/* ── Misc ── */
		case OP_TYPEOF: { js_val a = POP(); PUSH(typeof_val(vm, a)); break; }
		case OP_VOID:   { POP(); PUSH(JV_UNDEF); break; }
		case OP_THIS:   { PUSH(fr->this_val); break; }
		case OP_THROW: {
			js_val exc = POP();
			vm->threw = true;
			vm->exception = exc;
			/* Unwind try stack */
			if (fr->try_depth > 0) {
				struct js_try_frame *tf = &fr->try_stack[--fr->try_depth];
				vm->sp = tf->stack_top;
				fr->ip = tf->catch_ip;
				PUSH(exc);
				vm->threw = false;
				vm->exception = JV_UNDEF;
			} else {
				goto done;
			}
			break;
		}
		case OP_ENTER_TRY: {
			int16_t off = read_i16(code, fr->ip); fr->ip += 2;
			if (fr->try_depth < JS_TRY_MAX) {
				struct js_try_frame *tf = &fr->try_stack[fr->try_depth++];
				tf->catch_ip  = (uint32_t)((int32_t)fr->ip + off);
				tf->stack_top = vm->sp;
			}
			break;
		}
		case OP_LEAVE_TRY: {
			if (fr->try_depth > 0) fr->try_depth--;
			break;
		}

		default:
			kprintf("js_vm: unknown opcode %u at ip=%u\n", op, ip);
			goto done;
		}
	}

done:
	vm->sp = base_sp;
	vm->call_depth--;
	return result;

#undef PUSH
#undef POP
#undef PEEK
#undef PEEK2
}

js_val js_vm_call(struct js_vm *vm, js_val callee, js_val this_val,
                  js_val *args, uint8_t argc,
                  const struct js_func_proto *all_fns, uint32_t n_fns)
{
	if (!jv_is_obj(callee)) return JV_UNDEF;
	struct js_obj *fo = (struct js_obj *)jv_to_ptr(callee);
	if (!fo->is_function) return JV_UNDEF;

	/* Native function: stored as "__native__" property pointing to func idx -1 */
	js_val native = js_obj_get_cstr(vm->heap, fo, "__native__");
	if (!jv_is_undef(native)) {
		/* Native functions are called via the js_engine dispatch table */
		return JV_UNDEF;
	}

	js_val fidx_val = js_obj_get_cstr(vm->heap, fo, "__fn_idx__");
	if (jv_is_undef(fidx_val)) return JV_UNDEF;
	int32_t fn_idx = jv_to_int(fidx_val);
	if (fn_idx < 0 || (uint32_t)fn_idx >= n_fns) return JV_UNDEF;

	return js_vm_exec(vm, &all_fns[fn_idx], all_fns, n_fns,
	                  this_val, args, argc);
}
