/*
 * js_val.h — NaN-boxed 64-bit JavaScript value type.
 *
 * Every JS value is a uint64_t.  IEEE 754 doubles occupy the full range
 * EXCEPT the quiet-NaN space (bits 63-51 all ones).  We steal that space
 * for tagged non-double types.
 *
 * Tag encoding (bits 62-48, when bits 63-51 are all 1):
 *   0x7FF8_xxxx_xxxx_xxxx  — canonical quiet NaN  (JS NaN)
 *   0x7FF9_0000_PPPP_PPPP  — int32   (P = int32_t payload in low 32)
 *   0x7FFA_0000_0000_000B  — bool    (B = 0 or 1)
 *   0x7FFB_0000_0000_0000  — null
 *   0x7FFC_0000_0000_0000  — undefined
 *   0x7FFD_PPPP_PPPP_PPPP  — string  (P = heap pointer, ≤ 48 bits)
 *   0x7FFE_PPPP_PPPP_PPPP  — object  (P = heap pointer, ≤ 48 bits)
 *
 * All arithmetic NaN results are normalized to the canonical NaN.
 * Negative NaN (from C FPU) must NOT enter our value space; the
 * jv_double() constructor guards against this.
 */

#ifndef ANX_JS_VAL_H
#define ANX_JS_VAL_H

#include <anx/types.h>

typedef uint64_t js_val;

/* Tag constants */
#define JV_NAN       UINT64_C(0x7FF8000000000000)  /* canonical NaN */
#define JV_TAG_INT   UINT64_C(0x7FF9000000000000)
#define JV_TAG_BOOL  UINT64_C(0x7FFA000000000000)
#define JV_TAG_NULL  UINT64_C(0x7FFB000000000000)
#define JV_TAG_UNDEF UINT64_C(0x7FFC000000000000)
#define JV_TAG_STR   UINT64_C(0x7FFD000000000000)
#define JV_TAG_OBJ   UINT64_C(0x7FFE000000000000)

#define JV_PTR_MASK  UINT64_C(0x0000FFFFFFFFFFFF)
#define JV_TAG_MASK  UINT64_C(0xFFFF000000000000)

/* Immediate constants */
#define JV_NULL      JV_TAG_NULL
#define JV_UNDEF     JV_TAG_UNDEF
#define JV_TRUE      (JV_TAG_BOOL | UINT64_C(1))
#define JV_FALSE     (JV_TAG_BOOL | UINT64_C(0))

/* ── Type tests ───────────────────────────────────────────────────── */

/* A double is any value that is NOT in our tagged NaN range.
 * Invariant: no negative NaN ever enters our value space. */
static inline bool jv_is_double(js_val v)
{
	/* Doubles (incl. ±Inf) have (v & 0x7FF8...) != 0x7FF8...
	 * OR are exactly 0x7FF0... or 0x7FF8... (±Inf, canonical NaN) */
	return (v & UINT64_C(0xFFF8000000000000)) !=
	       UINT64_C(0x7FF8000000000000);
}

static inline bool jv_is_int   (js_val v) { return (v & JV_TAG_MASK) == JV_TAG_INT;  }
static inline bool jv_is_bool  (js_val v) { return (v & JV_TAG_MASK) == JV_TAG_BOOL; }
static inline bool jv_is_null  (js_val v) { return v == JV_TAG_NULL;  }
static inline bool jv_is_undef (js_val v) { return v == JV_TAG_UNDEF; }
static inline bool jv_is_str   (js_val v) { return (v & JV_TAG_MASK) == JV_TAG_STR;  }
static inline bool jv_is_obj   (js_val v) { return (v & JV_TAG_MASK) == JV_TAG_OBJ;  }
static inline bool jv_is_nan   (js_val v) { return v == JV_NAN; }

static inline bool jv_is_number(js_val v) { return jv_is_double(v) || jv_is_int(v); }
static inline bool jv_is_prim  (js_val v) { return !jv_is_obj(v); }

/* ── Constructors ─────────────────────────────────────────────────── */

static inline js_val jv_double(double d)
{
	js_val v;
	/* Normalize any NaN to canonical */
	if (d != d) return JV_NAN;
	__builtin_memcpy(&v, &d, 8);
	return v;
}

static inline js_val jv_int(int32_t n)
{
	return JV_TAG_INT | (uint32_t)n;
}

static inline js_val jv_bool(bool b)
{
	return JV_TAG_BOOL | (b ? 1u : 0u);
}

static inline js_val jv_str(const void *ptr)
{
	return JV_TAG_STR | ((uint64_t)(uintptr_t)ptr & JV_PTR_MASK);
}

static inline js_val jv_obj(const void *ptr)
{
	return JV_TAG_OBJ | ((uint64_t)(uintptr_t)ptr & JV_PTR_MASK);
}

/* ── Extractors ───────────────────────────────────────────────────── */

static inline double jv_to_double(js_val v)
{
	double d;
	__builtin_memcpy(&d, &v, 8);
	return d;
}

static inline int32_t jv_to_int(js_val v)
{
	return (int32_t)(v & 0xFFFFFFFFu);
}

static inline bool jv_to_bool_raw(js_val v)
{
	return (v & 1u) != 0;
}

static inline void *jv_to_ptr(js_val v)
{
	return (void *)(uintptr_t)(v & JV_PTR_MASK);
}

/* ── Numeric coercions ────────────────────────────────────────────── */

/* ToNumber(v): returns double.  NaN for non-numeric types. */
double jv_to_number(js_val v);

/* ToInt32(v): spec §7.1.6 */
int32_t jv_to_int32(js_val v);

/* ToUint32(v) */
uint32_t jv_to_uint32(js_val v);

/* ToBoolean(v): spec §7.1.2 */
bool jv_to_boolean(js_val v);

#endif /* ANX_JS_VAL_H */
