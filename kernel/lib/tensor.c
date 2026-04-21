/*
 * tensor.c — Tensor/embedding payload codec.
 *
 * Pure data plumbing with no allocation: encode copies into a caller
 * buffer, decode returns a pointer into the caller's buffer.
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/string.h>

uint32_t anx_tensor_dtype_bits(uint16_t dtype)
{
	switch (dtype) {
	case ANX_DTYPE_F32:	return 32;
	case ANX_DTYPE_F16:	return 16;
	case ANX_DTYPE_BF16:	return 16;
	case ANX_DTYPE_I8:	return 8;
	case ANX_DTYPE_U8:	return 8;
	case ANX_DTYPE_I4:	return 4;
	default:		return 0;
	}
}

uint64_t anx_tensor_element_count(const struct anx_tensor_header *h)
{
	uint64_t n = 1;
	uint32_t i;

	if (!h || h->ndim == 0 || h->ndim > ANX_TENSOR_MAX_DIMS)
		return 0;
	for (i = 0; i < h->ndim; i++) {
		if (h->shape[i] == 0)
			return 0;
		n *= h->shape[i];
	}
	return n;
}

uint64_t anx_tensor_data_bytes(const struct anx_tensor_header *h)
{
	uint64_t n;
	uint32_t bits;

	n = anx_tensor_element_count(h);
	if (n == 0)
		return 0;
	bits = anx_tensor_dtype_bits(h->dtype);
	if (bits == 0)
		return 0;
	/* Packed sub-byte dtypes: round up. */
	return (n * bits + 7) / 8;
}

int anx_tensor_validate(const struct anx_tensor_header *h)
{
	uint32_t i;

	if (!h)
		return ANX_EINVAL;
	if (h->magic != ANX_TENSOR_MAGIC)
		return ANX_EINVAL;
	if (h->version != ANX_TENSOR_VERSION)
		return ANX_EINVAL;
	if (h->flags != 0)
		return ANX_EINVAL;
	if (h->dtype >= ANX_DTYPE_WIRE_COUNT)
		return ANX_EINVAL;
	if (h->ndim == 0 || h->ndim > ANX_TENSOR_MAX_WIRE_DIMS)
		return ANX_EINVAL;
	for (i = 0; i < h->ndim; i++) {
		if (h->shape[i] == 0)
			return ANX_EINVAL;
	}
	/* Unused trailing shape slots must be zero so two equal tensors
	 * serialize identically. */
	for (i = h->ndim; i < ANX_TENSOR_MAX_WIRE_DIMS; i++) {
		if (h->shape[i] != 0)
			return ANX_EINVAL;
	}
	return ANX_OK;
}

int anx_tensor_encode(const struct anx_tensor_header *h,
		      const void *data, uint64_t data_size,
		      void *out_buf, uint64_t out_size,
		      uint64_t *bytes_written)
{
	uint64_t expected;
	uint64_t total;
	int ret;

	if (!h || !out_buf)
		return ANX_EINVAL;

	ret = anx_tensor_validate(h);
	if (ret != ANX_OK)
		return ret;

	expected = anx_tensor_data_bytes(h);
	if (data_size != expected)
		return ANX_EINVAL;
	if (expected > 0 && !data)
		return ANX_EINVAL;

	total = ANX_TENSOR_HEADER_SIZE + expected;
	if (out_size < total)
		return ANX_ENOMEM;

	anx_memcpy(out_buf, h, ANX_TENSOR_HEADER_SIZE);
	if (expected > 0) {
		uint8_t *dst = (uint8_t *)out_buf + ANX_TENSOR_HEADER_SIZE;

		anx_memcpy(dst, data, expected);
	}
	if (bytes_written)
		*bytes_written = total;
	return ANX_OK;
}

int anx_tensor_decode(const void *buf, uint64_t buf_size,
		      struct anx_tensor_header *out_header,
		      const void **out_data_ptr,
		      uint64_t *out_data_size)
{
	struct anx_tensor_header h;
	uint64_t data_bytes;
	int ret;

	if (!buf)
		return ANX_EINVAL;
	if (buf_size < ANX_TENSOR_HEADER_SIZE)
		return ANX_EINVAL;

	anx_memcpy(&h, buf, ANX_TENSOR_HEADER_SIZE);
	ret = anx_tensor_validate(&h);
	if (ret != ANX_OK)
		return ret;

	data_bytes = anx_tensor_data_bytes(&h);
	if (buf_size < ANX_TENSOR_HEADER_SIZE + data_bytes)
		return ANX_EINVAL;

	if (out_header)
		*out_header = h;
	if (out_data_ptr)
		*out_data_ptr = (const uint8_t *)buf + ANX_TENSOR_HEADER_SIZE;
	if (out_data_size)
		*out_data_size = data_bytes;
	return ANX_OK;
}
