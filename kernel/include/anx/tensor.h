/*
 * anx/tensor.h — Tensor/embedding payload codec.
 *
 * A compact self-describing header for the byte payload of State
 * Objects with type ANX_OBJ_EMBEDDING or ANX_OBJ_GRAPH_NODE. Before
 * this codec the payload was opaque bytes: callers had to track
 * dtype, shape, and layout out of band. LARQL-style vindex blocks
 * and embedding-plane residuals need structural typing so the
 * planner can reason about memory footprint and consumers can
 * validate shape before reading.
 *
 * Layout on the wire:
 *
 *   [ anx_tensor_header (28 bytes, packed) ][ element data ]
 *
 * Strides are implicit row-major contiguous. Sub-byte dtypes (I4)
 * are packed two elements per byte, low nibble first. The codec
 * does not own memory — encode writes into a caller buffer and
 * decode returns a pointer into the input buffer.
 */

#ifndef ANX_TENSOR_H
#define ANX_TENSOR_H

#include <anx/types.h>

/* "ANXT" in ASCII, little-endian-agnostic byte order for readability
 * in hex dumps. */
#define ANX_TENSOR_MAGIC	0x414E5854u
#define ANX_TENSOR_VERSION	1u
#define ANX_TENSOR_MAX_DIMS	4u

enum anx_dtype {
	ANX_DTYPE_F32  = 0,
	ANX_DTYPE_F16  = 1,
	ANX_DTYPE_BF16 = 2,
	ANX_DTYPE_I8   = 3,
	ANX_DTYPE_U8   = 4,
	ANX_DTYPE_I4   = 5,	/* packed, two elements per byte */
	ANX_DTYPE_COUNT
};

struct anx_tensor_header {
	uint32_t magic;
	uint16_t version;
	uint16_t dtype;			/* enum anx_dtype */
	uint16_t ndim;			/* 1..ANX_TENSOR_MAX_DIMS */
	uint16_t flags;			/* reserved, must be 0 */
	uint32_t shape[ANX_TENSOR_MAX_DIMS];
} __attribute__((packed));

#define ANX_TENSOR_HEADER_SIZE	28u

/* Bits per element. 0 on invalid dtype. */
uint32_t anx_tensor_dtype_bits(uint16_t dtype);

/* Total element count = product of shape[0..ndim). */
uint64_t anx_tensor_element_count(const struct anx_tensor_header *h);

/* Byte count of the packed element data. For I4 this rounds up so
 * odd element counts consume the trailing nibble's byte. */
uint64_t anx_tensor_data_bytes(const struct anx_tensor_header *h);

/* Validate magic/version/dtype/ndim/shape ranges. ANX_OK if usable,
 * ANX_EINVAL otherwise. Does not touch payload bytes. */
int anx_tensor_validate(const struct anx_tensor_header *h);

/* Encode header + data into out_buf. out_size must be at least
 * ANX_TENSOR_HEADER_SIZE + anx_tensor_data_bytes(h). On success
 * writes the byte count to *bytes_written if non-NULL.
 *
 * Returns ANX_OK, ANX_EINVAL (bad header or size mismatch), or
 * ANX_ENOMEM (buffer too small). */
int anx_tensor_encode(const struct anx_tensor_header *h,
		      const void *data, uint64_t data_size,
		      void *out_buf, uint64_t out_size,
		      uint64_t *bytes_written);

/* Decode a payload in place. out_header is a copy (unpacked).
 * out_data_ptr points into buf. out_data_size is the element byte
 * count. Any pointer parameter may be NULL if the caller doesn't
 * need that output.
 *
 * Returns ANX_OK on success, ANX_EINVAL on any validation failure. */
int anx_tensor_decode(const void *buf, uint64_t buf_size,
		      struct anx_tensor_header *out_header,
		      const void **out_data_ptr,
		      uint64_t *out_data_size);

#endif /* ANX_TENSOR_H */
