/*
 * anx/tensor.h — Tensor Objects (RFC-0013).
 *
 * First-class multi-dimensional arrays with shape, dtype, and
 * BRIN statistical summaries. Tensors are State Objects that the
 * kernel understands semantically — enabling metadata queries,
 * sub-tensor access, and dtype-aware routing.
 */

#ifndef ANX_TENSOR_H
#define ANX_TENSOR_H

#include <anx/types.h>

/* Forward declaration */
struct anx_state_object;

/* --- Tensor data types --- */

enum anx_tensor_dtype {
	ANX_DTYPE_FLOAT16,	/* IEEE 754 half-precision */
	ANX_DTYPE_BFLOAT16,	/* Brain floating point */
	ANX_DTYPE_FLOAT32,	/* IEEE 754 single-precision */
	ANX_DTYPE_FLOAT64,	/* IEEE 754 double-precision */
	ANX_DTYPE_INT8,		/* signed 8-bit integer */
	ANX_DTYPE_UINT8,	/* unsigned 8-bit integer */
	ANX_DTYPE_INT4,		/* packed 4-bit integer (2 per byte) */
	ANX_DTYPE_INT32,	/* signed 32-bit integer */
	ANX_DTYPE_BOOL,		/* 1-bit packed boolean */
	ANX_DTYPE_COUNT,
};

#define ANX_TENSOR_MAX_DIMS	8

/* --- Tensor metadata --- */

/*
 * BRIN statistics are stored as uint32_t IEEE 754 bit patterns
 * because the kernel compiles with -mgeneral-regs-only (no FP
 * registers). Use anx_sf_* softfloat functions to operate on them.
 */
struct anx_tensor_meta {
	uint32_t ndim;				/* 1-8 */
	uint64_t shape[ANX_TENSOR_MAX_DIMS];
	enum anx_tensor_dtype dtype;
	uint64_t elem_count;			/* product of shape */
	uint64_t byte_size;			/* total payload bytes */

	/* BRIN summary (computed on seal) */
	uint32_t stat_mean_bits;	/* IEEE 754 float32 */
	uint32_t stat_variance_bits;
	uint32_t stat_l2_norm_bits;
	uint32_t stat_sparsity_bits;	/* fraction near zero */
	uint32_t stat_min_bits;
	uint32_t stat_max_bits;

	/* Lineage */
	anx_oid_t parent_tensor;	/* base for delta versioning */
	bool is_delta;			/* payload is diff from parent */
};

/* --- BRIN block index --- */

struct anx_tensor_brin_block {
	uint64_t byte_offset;
	uint64_t byte_size;
	uint64_t dim_start[ANX_TENSOR_MAX_DIMS];
	uint64_t dim_end[ANX_TENSOR_MAX_DIMS];
	uint32_t block_mean_bits;
	uint32_t block_max_bits;
	uint32_t block_min_bits;
	uint32_t block_sparsity_bits;
};

/* --- API --- */

/* Get the byte size of a single element for a dtype */
uint32_t anx_tensor_dtype_size(enum anx_tensor_dtype dtype);

/* Get human-readable name for a dtype */
const char *anx_tensor_dtype_name(enum anx_tensor_dtype dtype);

/* Create a tensor object with shape/dtype metadata */
int anx_tensor_create(const struct anx_tensor_meta *meta,
		       const void *data, uint64_t data_size,
		       struct anx_state_object **out);

/* Seal a tensor (computes BRIN stats, makes immutable) */
int anx_tensor_seal(const anx_oid_t *oid);

/* Read tensor metadata without loading payload */
int anx_tensor_meta_get(const anx_oid_t *oid,
			 struct anx_tensor_meta *meta_out);

/* Fill a tensor with a pattern (zeros, ones, range) */
int anx_tensor_fill(const anx_oid_t *oid, const char *pattern);

/* --- Tensor operations (Phase 2) --- */

/* Slice along first dimension: new tensor = src[start:end] */
int anx_tensor_slice(const anx_oid_t *src_oid, uint64_t start, uint64_t end,
		      struct anx_state_object **out);

/* Element-wise diff: result = a - b (same shape/dtype required) */
int anx_tensor_diff(const anx_oid_t *oid_a, const anx_oid_t *oid_b,
		     struct anx_state_object **out);

/* Convert tensor to target dtype (quantize/widen) */
int anx_tensor_quantize(const anx_oid_t *src_oid,
			 enum anx_tensor_dtype target_dtype,
			 struct anx_state_object **out);

/* Search tensors by BRIN predicate (e.g. "sparsity>0.5") */
int anx_tensor_search(const char *predicate_str,
		       anx_oid_t *results, uint32_t max_results,
		       uint32_t *count_out);

/* --- Softfloat helpers (uint32_t IEEE 754 bit patterns) --- */

uint32_t anx_sf_from_int(int64_t val);
int64_t  anx_sf_to_int(uint32_t bits);
uint32_t anx_sf_add(uint32_t a, uint32_t b);
uint32_t anx_sf_mul(uint32_t a, uint32_t b);
uint32_t anx_sf_div(uint32_t a, uint32_t b);
bool     anx_sf_lt(uint32_t a, uint32_t b);
bool     anx_sf_gt(uint32_t a, uint32_t b);
uint32_t anx_sf_abs(uint32_t a);
uint32_t anx_sf_zero(void);

/* --- BRIN computation --- */

/* Compute BRIN stats from tensor payload (streaming) */
int anx_tensor_compute_brin(struct anx_state_object *obj,
			     struct anx_tensor_meta *meta);

#endif /* ANX_TENSOR_H */
