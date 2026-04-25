/*
 * anx/tensor.h — Tensor Objects (RFC-0013) + wire codec.
 *
 * Two distinct but related facilities:
 *
 * 1. Tensor Object Model (RFC-0013): first-class multi-dimensional arrays
 *    as State Objects with shape, dtype, BRIN stats, and kernel semantics.
 *    Lives in kernel/core/tensor/.
 *
 * 2. Wire Codec: compact self-describing header for the byte payload of
 *    State Objects with type ANX_OBJ_EMBEDDING or ANX_OBJ_GRAPH_NODE.
 *    Lives in kernel/lib/tensor.c. Used by LARQL vindex blocks and
 *    embedding-plane residuals.
 */

#ifndef ANX_TENSOR_H
#define ANX_TENSOR_H

#include <anx/types.h>

/* Forward declaration */
struct anx_state_object;

/* --- Tensor data types (RFC-0013 full model) --- */

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
	ANX_TENSOR_DTYPE_COUNT,
};
#define ANX_DTYPE_COUNT ANX_TENSOR_DTYPE_COUNT	/* back-compat alias */

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

/* --- RFC-0013 Tensor Object API --- */

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
uint32_t anx_sf_sqrt(uint32_t a);

/* --- BRIN computation --- */

/* Compute BRIN stats from tensor payload (streaming) */
int anx_tensor_compute_brin(struct anx_state_object *obj,
			     struct anx_tensor_meta *meta);

/* ===== Wire Codec (kernel/lib/tensor.c) ===== */

/* "ANXT" in ASCII — identifies the wire format in hex dumps. */
#define ANX_TENSOR_MAGIC	0x414E5854u
#define ANX_TENSOR_VERSION	1u
#define ANX_TENSOR_MAX_WIRE_DIMS	4u

enum anx_dtype {
	ANX_DTYPE_F32  = 0,
	ANX_DTYPE_F16  = 1,
	ANX_DTYPE_BF16 = 2,
	ANX_DTYPE_I8   = 3,
	ANX_DTYPE_U8   = 4,
	ANX_DTYPE_I4   = 5,	/* packed, two elements per byte */
	ANX_DTYPE_WIRE_COUNT = 6
};

struct anx_tensor_header {
	uint32_t magic;
	uint16_t version;
	uint16_t dtype;			/* enum anx_dtype */
	uint16_t ndim;			/* 1..ANX_TENSOR_MAX_WIRE_DIMS */
	uint16_t flags;			/* reserved, must be 0 */
	uint32_t shape[ANX_TENSOR_MAX_WIRE_DIMS];
} __attribute__((packed));

#define ANX_TENSOR_HEADER_SIZE	28u

/* Bits per element. 0 on invalid dtype. */
uint32_t anx_tensor_dtype_bits(uint16_t dtype);

/* Total element count = product of shape[0..ndim). */
uint64_t anx_tensor_element_count(const struct anx_tensor_header *h);

/* Byte count of the packed element data. */
uint64_t anx_tensor_data_bytes(const struct anx_tensor_header *h);

/* Validate magic/version/dtype/ndim/shape ranges. */
int anx_tensor_validate(const struct anx_tensor_header *h);

/* Encode header + data into out_buf. */
int anx_tensor_encode(const struct anx_tensor_header *h,
		      const void *data, uint64_t data_size,
		      void *out_buf, uint64_t out_size,
		      uint64_t *bytes_written);

/* Decode a payload in place. out_data_ptr points into buf. */
int anx_tensor_decode(const void *buf, uint64_t buf_size,
		      struct anx_tensor_header *out_header,
		      const void **out_data_ptr,
		      uint64_t *out_data_size);

#endif /* ANX_TENSOR_H */
