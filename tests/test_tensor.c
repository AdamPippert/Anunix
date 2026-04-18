/*
 * test_tensor.c — Tensor/embedding payload codec.
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/string.h>

static void init_header(struct anx_tensor_header *h, uint16_t dtype,
			uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
	anx_memset(h, 0, sizeof(*h));
	h->magic = ANX_TENSOR_MAGIC;
	h->version = ANX_TENSOR_VERSION;
	h->dtype = dtype;
	h->flags = 0;
	h->shape[0] = d0;
	h->shape[1] = d1;
	h->shape[2] = d2;
	h->shape[3] = d3;
	if (d0 && d1 && d2 && d3)
		h->ndim = 4;
	else if (d0 && d1 && d2)
		h->ndim = 3;
	else if (d0 && d1)
		h->ndim = 2;
	else if (d0)
		h->ndim = 1;
}

int test_tensor(void)
{
	/* --- dtype bit sizes --- */
	if (anx_tensor_dtype_bits(ANX_DTYPE_F32) != 32)
		return -1;
	if (anx_tensor_dtype_bits(ANX_DTYPE_F16) != 16)
		return -2;
	if (anx_tensor_dtype_bits(ANX_DTYPE_BF16) != 16)
		return -3;
	if (anx_tensor_dtype_bits(ANX_DTYPE_I8) != 8)
		return -4;
	if (anx_tensor_dtype_bits(ANX_DTYPE_U8) != 8)
		return -5;
	if (anx_tensor_dtype_bits(ANX_DTYPE_I4) != 4)
		return -6;
	if (anx_tensor_dtype_bits(0xFFFF) != 0)
		return -7;

	/* --- byte counts --- */
	{
		struct anx_tensor_header h;

		init_header(&h, ANX_DTYPE_F32, 4, 8, 0, 0);
		if (anx_tensor_element_count(&h) != 32)
			return -10;
		if (anx_tensor_data_bytes(&h) != 128)
			return -11;

		/* I4 packed: 15 elements -> 8 bytes (rounded up). */
		init_header(&h, ANX_DTYPE_I4, 15, 0, 0, 0);
		if (anx_tensor_element_count(&h) != 15)
			return -12;
		if (anx_tensor_data_bytes(&h) != 8)
			return -13;

		/* I4 packed: 16 elements -> 8 bytes (exact). */
		init_header(&h, ANX_DTYPE_I4, 16, 0, 0, 0);
		if (anx_tensor_data_bytes(&h) != 8)
			return -14;

		/* BF16 2x3 -> 12 bytes. */
		init_header(&h, ANX_DTYPE_BF16, 2, 3, 0, 0);
		if (anx_tensor_data_bytes(&h) != 12)
			return -15;
	}

	/* --- validation rejects malformed headers --- */
	{
		struct anx_tensor_header h;

		init_header(&h, ANX_DTYPE_F32, 2, 2, 0, 0);
		if (anx_tensor_validate(&h) != ANX_OK)
			return -20;

		h.magic = 0xDEADBEEF;
		if (anx_tensor_validate(&h) != ANX_EINVAL)
			return -21;
		h.magic = ANX_TENSOR_MAGIC;

		h.version = 99;
		if (anx_tensor_validate(&h) != ANX_EINVAL)
			return -22;
		h.version = ANX_TENSOR_VERSION;

		h.dtype = ANX_DTYPE_COUNT;
		if (anx_tensor_validate(&h) != ANX_EINVAL)
			return -23;
		h.dtype = ANX_DTYPE_F32;

		h.ndim = 0;
		if (anx_tensor_validate(&h) != ANX_EINVAL)
			return -24;
		h.ndim = 5;
		if (anx_tensor_validate(&h) != ANX_EINVAL)
			return -25;
		h.ndim = 2;

		h.shape[0] = 0;
		if (anx_tensor_validate(&h) != ANX_EINVAL)
			return -26;
		h.shape[0] = 2;

		h.flags = 1;
		if (anx_tensor_validate(&h) != ANX_EINVAL)
			return -27;
		h.flags = 0;

		/* Unused trailing slot must be zero. */
		h.shape[2] = 7;
		if (anx_tensor_validate(&h) != ANX_EINVAL)
			return -28;
		h.shape[2] = 0;

		if (anx_tensor_validate(&h) != ANX_OK)
			return -29;
	}

	/* --- round-trip --- */
	{
		struct anx_tensor_header h;
		struct anx_tensor_header got;
		const void *data_ptr;
		uint64_t data_size;
		uint64_t written;
		uint8_t payload[128];
		uint8_t buf[256];

		init_header(&h, ANX_DTYPE_I8, 4, 4, 0, 0);
		{
			uint32_t i;

			for (i = 0; i < 16; i++)
				payload[i] = (uint8_t)(i * 3 + 1);
		}

		if (anx_tensor_encode(&h, payload, 16, buf, sizeof(buf),
				      &written) != ANX_OK)
			return -40;
		if (written != ANX_TENSOR_HEADER_SIZE + 16)
			return -41;

		if (anx_tensor_decode(buf, written, &got, &data_ptr,
				      &data_size) != ANX_OK)
			return -42;
		if (got.magic != ANX_TENSOR_MAGIC)
			return -43;
		if (got.dtype != ANX_DTYPE_I8)
			return -44;
		if (got.ndim != 2)
			return -45;
		if (got.shape[0] != 4 || got.shape[1] != 4)
			return -46;
		if (data_size != 16)
			return -47;
		if (anx_memcmp(data_ptr, payload, 16) != 0)
			return -48;
	}

	/* --- data size mismatch on encode --- */
	{
		struct anx_tensor_header h;
		uint8_t buf[64];

		init_header(&h, ANX_DTYPE_F32, 2, 2, 0, 0);	/* 16 bytes */
		if (anx_tensor_encode(&h, "short", 5, buf, sizeof(buf),
				      NULL) != ANX_EINVAL)
			return -50;
	}

	/* --- out_buf too small --- */
	{
		struct anx_tensor_header h;
		uint8_t payload[16] = {0};
		uint8_t small[8];

		init_header(&h, ANX_DTYPE_F32, 2, 2, 0, 0);
		if (anx_tensor_encode(&h, payload, 16, small, sizeof(small),
				      NULL) != ANX_ENOMEM)
			return -51;
	}

	/* --- decode of truncated buffer --- */
	{
		struct anx_tensor_header h;
		uint8_t payload[16] = {0};
		uint8_t buf[64];
		uint64_t written;

		init_header(&h, ANX_DTYPE_F32, 2, 2, 0, 0);
		if (anx_tensor_encode(&h, payload, 16, buf, sizeof(buf),
				      &written) != ANX_OK)
			return -60;
		/* Chop off one byte of data. */
		if (anx_tensor_decode(buf, written - 1, NULL, NULL,
				      NULL) != ANX_EINVAL)
			return -61;
		/* Truncated header. */
		if (anx_tensor_decode(buf, ANX_TENSOR_HEADER_SIZE - 1,
				      NULL, NULL, NULL) != ANX_EINVAL)
			return -62;
	}

	/* --- 4D shape (typical LARQL vindex: layer, heads, rows, cols) --- */
	{
		struct anx_tensor_header h;
		uint8_t buf[256];
		uint64_t written;

		init_header(&h, ANX_DTYPE_F16, 2, 4, 3, 2);	/* 48 elems */
		if (anx_tensor_data_bytes(&h) != 96)
			return -70;
		{
			uint8_t payload[96];
			uint32_t i;

			for (i = 0; i < 96; i++)
				payload[i] = (uint8_t)i;
			if (anx_tensor_encode(&h, payload, 96, buf,
					      sizeof(buf),
					      &written) != ANX_OK)
				return -71;
			if (written != ANX_TENSOR_HEADER_SIZE + 96)
				return -72;
		}
	}

	return 0;
}
