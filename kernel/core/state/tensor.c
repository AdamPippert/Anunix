/*
 * tensor.c — Tensor Object implementation (RFC-0013 Phase 1).
 *
 * Tensor Objects are State Objects whose payload carries a
 * metadata header followed by raw element bytes. The kernel
 * knows the shape and dtype but does not perform linear algebra:
 * create, seal (with BRIN summary computation), and metadata
 * queries are the only operations at this layer.
 *
 * Summary statistics (mean, variance, L2 norm, sparsity, min,
 * max) are computed once at seal time. Since sealed objects are
 * immutable, these values never go stale — the same principle
 * PostgreSQL uses for BRIN indexes on append-only tables.
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

/* Per-dtype bytes-per-element, 0 for sub-byte packed dtypes. */
static const uint64_t DTYPE_BYTES[ANX_DTYPE_COUNT] = {
	[ANX_DTYPE_FLOAT32]  = 4,
	[ANX_DTYPE_FLOAT64]  = 8,
	[ANX_DTYPE_FLOAT16]  = 2,
	[ANX_DTYPE_BFLOAT16] = 2,
	[ANX_DTYPE_INT8]     = 1,
	[ANX_DTYPE_UINT8]    = 1,
	[ANX_DTYPE_INT32]    = 4,
	[ANX_DTYPE_INT4]     = 0,
	[ANX_DTYPE_BOOL]     = 0,
};

static const char *const DTYPE_NAMES[ANX_DTYPE_COUNT] = {
	[ANX_DTYPE_FLOAT32]  = "float32",
	[ANX_DTYPE_FLOAT64]  = "float64",
	[ANX_DTYPE_FLOAT16]  = "float16",
	[ANX_DTYPE_BFLOAT16] = "bfloat16",
	[ANX_DTYPE_INT8]     = "int8",
	[ANX_DTYPE_UINT8]    = "uint8",
	[ANX_DTYPE_INT32]    = "int32",
	[ANX_DTYPE_INT4]     = "int4",
	[ANX_DTYPE_BOOL]     = "bool",
};

uint64_t anx_tensor_dtype_bytes(enum anx_tensor_dtype dt)
{
	if ((int)dt < 0 || dt >= ANX_DTYPE_COUNT)
		return 0;
	return DTYPE_BYTES[dt];
}

const char *anx_tensor_dtype_name(enum anx_tensor_dtype dt)
{
	if ((int)dt < 0 || dt >= ANX_DTYPE_COUNT)
		return "unknown";
	return DTYPE_NAMES[dt];
}

uint64_t anx_tensor_shape_elems(const uint64_t *shape, uint32_t ndim)
{
	uint64_t n = 1;
	uint32_t i;

	if (!shape || ndim == 0 || ndim > ANX_TENSOR_MAX_DIMS)
		return 0;

	for (i = 0; i < ndim; i++) {
		if (shape[i] == 0)
			return 0;
		/* Clamp on overflow. */
		if (n > ((uint64_t)-1) / shape[i])
			return (uint64_t)-1;
		n *= shape[i];
	}
	return n;
}

uint64_t anx_tensor_compute_byte_size(enum anx_tensor_dtype dt,
				      uint64_t elem_count)
{
	if (elem_count == 0)
		return 0;

	switch (dt) {
	case ANX_DTYPE_INT4:
		return (elem_count + 1) / 2;
	case ANX_DTYPE_BOOL:
		return (elem_count + 7) / 8;
	default: {
		uint64_t bpe = anx_tensor_dtype_bytes(dt);

		if (bpe == 0)
			return 0;
		return elem_count * bpe;
	}
	}
}

/*
 * BRIN summary computation uses floating-point arithmetic. The
 * freestanding kernel is built with -mgeneral-regs-only and has
 * no FPU state save/restore, so the code below compiles only in
 * builds that define ANX_HAVE_FLOAT (test harness, future
 * userland compute cells). In-kernel seal still works; the
 * tensor is simply sealed without a BRIN summary and
 * brin_valid stays false until a compute engine fills it in.
 */
#ifdef ANX_HAVE_FLOAT

/* --- Dtype conversion helpers --- */

static float bf16_to_f32(uint16_t bits)
{
	union {
		uint32_t u;
		float f;
	} u;

	u.u = ((uint32_t)bits) << 16;
	return u.f;
}

/* IEEE 754 binary16 → binary32 conversion. Handles normals,
 * subnormals, zero, and inf/NaN. */
static float f16_to_f32(uint16_t h)
{
	uint32_t sign = (uint32_t)(h & 0x8000) << 16;
	uint32_t exp = (h >> 10) & 0x1F;
	uint32_t mant = h & 0x3FF;
	uint32_t out;
	union {
		uint32_t u;
		float f;
	} u;

	if (exp == 0) {
		if (mant == 0) {
			out = sign;		/* ±0 */
		} else {
			/* Subnormal — normalize. */
			while ((mant & 0x400) == 0) {
				mant <<= 1;
				exp--;
			}
			exp++;
			mant &= 0x3FF;
			out = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
		}
	} else if (exp == 0x1F) {
		/* inf / NaN */
		out = sign | 0x7F800000 | (mant << 13);
	} else {
		out = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
	}

	u.u = out;
	return u.f;
}

/* Newton-Raphson sqrt. Good to several ulps for the magnitudes
 * that appear in tensor statistics. */
static float approx_sqrtf(float x)
{
	union {
		uint32_t u;
		float f;
	} u;
	float y;
	int i;

	if (x <= 0.0f)
		return 0.0f;

	/* Magic-number seed: approximate log halving via exponent bias. */
	u.f = x;
	u.u = 0x1FBD1DF5 + (u.u >> 1);
	y = u.f;

	for (i = 0; i < 4; i++)
		y = 0.5f * (y + x / y);

	return y;
}

/* --- BRIN computation --- */

/* Read elem i from a raw buffer of the given dtype as float. Caller
 * guarantees i < meta->elem_count. Returns 0 for unsupported dtypes. */
static float elem_as_f32(const uint8_t *data, uint64_t i,
			 enum anx_tensor_dtype dt)
{
	switch (dt) {
	case ANX_DTYPE_FLOAT32: {
		float v;
		anx_memcpy(&v, data + i * 4, 4);
		return v;
	}
	case ANX_DTYPE_FLOAT64: {
		double v;
		anx_memcpy(&v, data + i * 8, 8);
		return (float)v;
	}
	case ANX_DTYPE_FLOAT16: {
		uint16_t v;
		anx_memcpy(&v, data + i * 2, 2);
		return f16_to_f32(v);
	}
	case ANX_DTYPE_BFLOAT16: {
		uint16_t v;
		anx_memcpy(&v, data + i * 2, 2);
		return bf16_to_f32(v);
	}
	case ANX_DTYPE_INT8:
		return (float)(int8_t)data[i];
	case ANX_DTYPE_UINT8:
		return (float)data[i];
	case ANX_DTYPE_INT32: {
		int32_t v;
		anx_memcpy(&v, data + i * 4, 4);
		return (float)v;
	}
	case ANX_DTYPE_INT4: {
		uint8_t b = data[i >> 1];
		uint8_t nib = (i & 1) ? (b >> 4) : (b & 0x0F);

		/* Sign-extend the 4-bit value. */
		if (nib & 0x08)
			return (float)(int8_t)(nib | 0xF0);
		return (float)nib;
	}
	case ANX_DTYPE_BOOL: {
		uint8_t b = data[i >> 3];
		return (b >> (i & 7)) & 1 ? 1.0f : 0.0f;
	}
	default:
		return 0.0f;
	}
}

/* Epsilon for sparsity: elements with |x| < SPARSITY_EPS count as
 * zero. 1e-6 matches common ML tooling conventions. */
#define SPARSITY_EPS	1.0e-6f

static float fabs_f32(float x)
{
	return x < 0.0f ? -x : x;
}

static void compute_brin(struct anx_tensor_meta *meta, const uint8_t *data)
{
	uint64_t n = meta->elem_count;
	uint64_t zero_count = 0;
	double sum = 0.0;
	double sum_sq = 0.0;
	float vmin, vmax;
	uint64_t i;

	if (n == 0) {
		meta->stat_mean = 0.0f;
		meta->stat_variance = 0.0f;
		meta->stat_l2_norm = 0.0f;
		meta->stat_sparsity = 0.0f;
		meta->stat_min = 0.0f;
		meta->stat_max = 0.0f;
		meta->brin_valid = true;
		return;
	}

	vmin = elem_as_f32(data, 0, meta->dtype);
	vmax = vmin;

	for (i = 0; i < n; i++) {
		float v = elem_as_f32(data, i, meta->dtype);

		sum += v;
		sum_sq += (double)v * (double)v;
		if (fabs_f32(v) < SPARSITY_EPS)
			zero_count++;
		if (v < vmin)
			vmin = v;
		if (v > vmax)
			vmax = v;
	}

	meta->stat_mean = (float)(sum / (double)n);
	{
		double mean = sum / (double)n;

		meta->stat_variance =
			(float)((sum_sq / (double)n) - (mean * mean));
	}
	meta->stat_l2_norm = approx_sqrtf((float)sum_sq);
	meta->stat_sparsity = (float)((double)zero_count / (double)n);
	meta->stat_min = vmin;
	meta->stat_max = vmax;
	meta->brin_valid = true;
}

#else  /* !ANX_HAVE_FLOAT — kernel build, no FPU */

static void compute_brin(struct anx_tensor_meta *meta, const uint8_t *data)
{
	(void)data;
	/*
	 * Kernel build has no floating-point registers available
	 * (-mgeneral-regs-only). BRIN summary computation is
	 * deferred to a userland compute engine (RFC-0013 Phase 4).
	 * The tensor is still sealed and immutable — just without
	 * statistical metadata.
	 */
	meta->brin_valid = false;
}

#endif  /* ANX_HAVE_FLOAT */

/* --- Public API --- */

int anx_tensor_create(const struct anx_tensor_meta *meta_in,
		      const void *data, uint64_t data_size,
		      struct anx_state_object **out)
{
	struct anx_tensor_meta meta;
	struct anx_so_create_params params;
	struct anx_state_object *obj;
	void *buf;
	uint64_t elem_count;
	uint64_t byte_size;
	uint64_t total;
	int ret;

	if (!meta_in || !out)
		return ANX_EINVAL;
	if (meta_in->ndim == 0 || meta_in->ndim > ANX_TENSOR_MAX_DIMS)
		return ANX_EINVAL;
	if ((int)meta_in->dtype < 0 || meta_in->dtype >= ANX_DTYPE_COUNT)
		return ANX_EINVAL;

	elem_count = anx_tensor_shape_elems(meta_in->shape, meta_in->ndim);
	if (elem_count == 0 || elem_count == (uint64_t)-1)
		return ANX_EINVAL;

	byte_size = anx_tensor_compute_byte_size(meta_in->dtype, elem_count);
	if (byte_size == 0)
		return ANX_EINVAL;

	if (data && data_size != byte_size)
		return ANX_EINVAL;

	meta = *meta_in;
	meta.elem_count = elem_count;
	meta.byte_size = byte_size;
	meta.brin_valid = false;
	meta.stat_mean = 0.0f;
	meta.stat_variance = 0.0f;
	meta.stat_l2_norm = 0.0f;
	meta.stat_sparsity = 0.0f;
	meta.stat_min = 0.0f;
	meta.stat_max = 0.0f;

	total = sizeof(meta) + byte_size;
	buf = anx_zalloc(total);
	if (!buf)
		return ANX_ENOMEM;

	anx_memcpy(buf, &meta, sizeof(meta));
	if (data && data_size > 0)
		anx_memcpy((uint8_t *)buf + sizeof(meta), data, data_size);

	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_TENSOR;
	params.payload = buf;
	params.payload_size = total;

	ret = anx_so_create(&params, &obj);
	anx_free(buf);
	if (ret != ANX_OK)
		return ret;

	*out = obj;
	return ANX_OK;
}

int anx_tensor_get_meta(const anx_oid_t *oid,
			struct anx_tensor_meta *meta_out)
{
	struct anx_state_object *obj;

	if (!oid || !meta_out)
		return ANX_EINVAL;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_ENOENT;

	if (obj->object_type != ANX_OBJ_TENSOR ||
	    obj->payload_size < sizeof(*meta_out)) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}

	anx_memcpy(meta_out, obj->payload, sizeof(*meta_out));
	anx_objstore_release(obj);
	return ANX_OK;
}

int anx_tensor_read_data(const anx_oid_t *oid, uint64_t offset,
			 void *dst, uint64_t len)
{
	struct anx_state_object *obj;
	struct anx_tensor_meta meta;
	uint64_t byte_size;

	if (!oid || !dst)
		return ANX_EINVAL;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_ENOENT;
	if (obj->object_type != ANX_OBJ_TENSOR ||
	    obj->payload_size < sizeof(meta)) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}

	anx_memcpy(&meta, obj->payload, sizeof(meta));
	byte_size = meta.byte_size;

	if (offset >= byte_size) {
		anx_objstore_release(obj);
		return 0;
	}
	if (offset + len > byte_size)
		len = byte_size - offset;

	anx_memcpy(dst,
		   (uint8_t *)obj->payload + sizeof(meta) + offset, len);
	anx_objstore_release(obj);
	return (int)len;
}

int anx_tensor_seal(const anx_oid_t *oid)
{
	struct anx_state_object *obj;
	struct anx_tensor_meta meta;
	int ret;

	if (!oid)
		return ANX_EINVAL;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_ENOENT;
	if (obj->object_type != ANX_OBJ_TENSOR ||
	    obj->payload_size < sizeof(meta)) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}

	if (obj->state == ANX_OBJ_SEALED) {
		anx_objstore_release(obj);
		return ANX_OK;
	}

	anx_memcpy(&meta, obj->payload, sizeof(meta));

	if (!meta.brin_valid) {
		compute_brin(&meta,
			     (const uint8_t *)obj->payload + sizeof(meta));
		/* Write updated meta back into the payload. This is a
		 * last mutation before seal — it stays consistent with
		 * the content hash that anx_so_seal recomputes. */
		anx_memcpy(obj->payload, &meta, sizeof(meta));
	}

	anx_objstore_release(obj);

	ret = anx_so_seal(oid);
	return ret;
}

/* --- Search --- */

struct tensor_search_ctx {
	float min_sparsity;
	float max_sparsity;
	enum anx_tensor_dtype dtype_filter;
	anx_oid_t *results;
	uint32_t max_results;
	uint32_t count;
};

static int tensor_search_cb(struct anx_state_object *obj, void *arg)
{
	struct tensor_search_ctx *ctx = arg;
	struct anx_tensor_meta meta;

	if (obj->object_type != ANX_OBJ_TENSOR)
		return 0;
	if (obj->payload_size < sizeof(meta))
		return 0;

	anx_memcpy(&meta, obj->payload, sizeof(meta));
	if (!meta.brin_valid)
		return 0;

	if (ctx->dtype_filter != ANX_DTYPE_COUNT &&
	    meta.dtype != ctx->dtype_filter)
		return 0;
#ifdef ANX_HAVE_FLOAT
	if (meta.stat_sparsity < ctx->min_sparsity ||
	    meta.stat_sparsity > ctx->max_sparsity)
		return 0;
#else
	(void)ctx;
#endif

	if (ctx->count < ctx->max_results)
		ctx->results[ctx->count] = obj->oid;
	ctx->count++;
	return 0;
}

int anx_tensor_search(float min_sparsity, float max_sparsity,
		      enum anx_tensor_dtype dtype_filter,
		      anx_oid_t *results, uint32_t max_results,
		      uint32_t *count)
{
	struct tensor_search_ctx ctx;

	if (!results || !count || max_results == 0)
		return ANX_EINVAL;

	ctx.min_sparsity = min_sparsity;
	ctx.max_sparsity = max_sparsity;
	ctx.dtype_filter = dtype_filter;
	ctx.results = results;
	ctx.max_results = max_results;
	ctx.count = 0;

	anx_objstore_iterate(tensor_search_cb, &ctx);

	*count = ctx.count;
	return ANX_OK;
}
