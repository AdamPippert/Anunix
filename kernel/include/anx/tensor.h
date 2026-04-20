/*
 * anx/tensor.h — Tensor Objects (RFC-0013).
 *
 * A Tensor Object is a State Object with a typed, shape-aware
 * payload. The kernel does not perform linear algebra; it provides
 * the object model, metadata, and indexing that make tensor
 * operations governable, auditable, and composable.
 *
 * Payload layout for a tensor-typed State Object is:
 *   [struct anx_tensor_meta][raw element bytes]
 *
 * The meta header is always present and carries shape, dtype,
 * element/byte counts, lineage, and (after seal) BRIN-style
 * summary statistics. Raw bytes follow in row-major order.
 */

#ifndef ANX_TENSOR_H
#define ANX_TENSOR_H

#include <anx/types.h>
#include <anx/state_object.h>

#define ANX_TENSOR_MAX_DIMS	8

/* Element data types (RFC-0013 Section 4.2). */
enum anx_tensor_dtype {
	ANX_DTYPE_FLOAT32 = 0,
	ANX_DTYPE_FLOAT64,
	ANX_DTYPE_FLOAT16,
	ANX_DTYPE_BFLOAT16,
	ANX_DTYPE_INT8,
	ANX_DTYPE_UINT8,
	ANX_DTYPE_INT32,
	ANX_DTYPE_INT4,		/* 2 elements per byte, low nibble first */
	ANX_DTYPE_BOOL,		/* 8 elements per byte, LSB first */
	ANX_DTYPE_COUNT,
};

/* Metadata stored at the start of every tensor's payload.
 * Fields below byte_size are only meaningful once brin_valid is
 * true (i.e. after anx_tensor_seal has run). */
struct anx_tensor_meta {
	uint32_t ndim;
	uint64_t shape[ANX_TENSOR_MAX_DIMS];
	enum anx_tensor_dtype dtype;

	uint64_t elem_count;		/* product of shape */
	uint64_t byte_size;		/* size of the raw element bytes */

	bool     brin_valid;		/* true after seal */
	float    stat_mean;
	float    stat_variance;
	float    stat_l2_norm;
	float    stat_sparsity;		/* fraction of |x| < epsilon */
	float    stat_min;
	float    stat_max;

	/* Lineage — set by derivation operations (Phase 2+). */
	anx_oid_t parent_tensor;
	bool      is_delta;
};

/* --- Dtype helpers --- */

/* Bytes per element, or 0 for sub-byte packed dtypes. */
uint64_t anx_tensor_dtype_bytes(enum anx_tensor_dtype dt);

/* Human-readable dtype name, never NULL. */
const char *anx_tensor_dtype_name(enum anx_tensor_dtype dt);

/* Product of shape[0..ndim-1], clamped at UINT64_MAX on overflow. */
uint64_t anx_tensor_shape_elems(const uint64_t *shape, uint32_t ndim);

/* Bytes required for a tensor with the given shape and dtype,
 * accounting for sub-byte packing. */
uint64_t anx_tensor_compute_byte_size(enum anx_tensor_dtype dt,
				      uint64_t elem_count);

/* --- Tensor lifecycle --- */

/* Create a tensor State Object. meta->{shape, ndim, dtype} must be
 * set; elem_count and byte_size are derived. If data is non-NULL
 * it must contain exactly meta's byte_size bytes and is copied in
 * as the initial payload; pass NULL/0 to create a zero-initialized
 * tensor. The returned object is unsealed. */
int anx_tensor_create(const struct anx_tensor_meta *meta,
		      const void *data, uint64_t data_size,
		      struct anx_state_object **out);

/* Compute BRIN summary statistics, mark brin_valid, and seal the
 * underlying State Object. Idempotent: sealing an already-sealed
 * tensor returns ANX_OK without recomputing. */
int anx_tensor_seal(const anx_oid_t *oid);

/* Read metadata without loading the element payload. */
int anx_tensor_get_meta(const anx_oid_t *oid,
			struct anx_tensor_meta *meta_out);

/* Copy a range of raw element bytes into dst. offset and len are
 * element-byte coordinates (relative to the start of the element
 * region, not the payload prefix). */
int anx_tensor_read_data(const anx_oid_t *oid, uint64_t offset,
			 void *dst, uint64_t len);

/* --- BRIN metadata queries --- */

/* Find tensors whose sparsity falls in [min_sparsity, max_sparsity]
 * and whose dtype matches dtype_filter (use ANX_DTYPE_COUNT to
 * match any dtype). Only sealed tensors participate — unsealed
 * tensors have no BRIN data yet. Writes up to max_results OIDs
 * into results and sets *count to the number written. */
int anx_tensor_search(float min_sparsity, float max_sparsity,
		      enum anx_tensor_dtype dtype_filter,
		      anx_oid_t *results, uint32_t max_results,
		      uint32_t *count);

#endif /* ANX_TENSOR_H */
