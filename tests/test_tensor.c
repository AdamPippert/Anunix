/*
 * test_tensor.c — Tests for Tensor Objects (RFC-0013).
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/string.h>

int test_tensor(void)
{
	struct anx_state_object *obj;
	struct anx_tensor_meta meta, meta_out;
	int ret;

	anx_objstore_init();

	/* --- Test 1: Create an INT8 tensor [4, 8] --- */
	anx_memset(&meta, 0, sizeof(meta));
	meta.ndim = 2;
	meta.shape[0] = 4;
	meta.shape[1] = 8;
	meta.dtype = ANX_DTYPE_INT8;

	ret = anx_tensor_create(&meta, NULL, 0, &obj);
	if (ret != ANX_OK)
		return -1;

	if (obj->object_type != ANX_OBJ_TENSOR)
		return -2;

	/* Payload should be allocated (4 * 8 * 1 = 32 bytes) */
	if (obj->payload_size != 32)
		return -3;

	/* --- Test 2: Read metadata back --- */
	ret = anx_tensor_meta_get(&obj->oid, &meta_out);
	if (ret != ANX_OK)
		return -4;

	if (meta_out.ndim != 2)
		return -5;
	if (meta_out.shape[0] != 4 || meta_out.shape[1] != 8)
		return -6;
	if (meta_out.dtype != ANX_DTYPE_INT8)
		return -7;
	if (meta_out.elem_count != 32)
		return -8;
	if (meta_out.byte_size != 32)
		return -9;

	/* --- Test 3: Fill with range pattern --- */
	ret = anx_tensor_fill(&obj->oid, "range");
	if (ret != ANX_OK)
		return -10;

	/* Verify first few elements */
	{
		const int8_t *p = (const int8_t *)obj->payload;

		if (p[0] != 0 || p[1] != 1 || p[2] != 2 || p[31] != 31)
			return -11;
	}

	/* --- Test 4: Seal and check BRIN stats --- */
	ret = anx_tensor_seal(&obj->oid);
	if (ret != ANX_OK)
		return -12;

	ret = anx_tensor_meta_get(&obj->oid, &meta_out);
	if (ret != ANX_OK)
		return -13;

	/* Mean of 0..31 = 15.5 */
	{
		int64_t mean_int = anx_sf_to_int(meta_out.stat_mean_bits);

		/* Softfloat truncation: expect ~15 */
		if (mean_int < 14 || mean_int > 16)
			return -14;
	}

	/* Min should be 0 */
	if (anx_sf_to_int(meta_out.stat_min_bits) != 0)
		return -15;

	/* Max should be 31 */
	if (anx_sf_to_int(meta_out.stat_max_bits) != 31)
		return -16;

	/* Sparsity: 1 zero out of 32 elements */
	/* Not exactly zero — it's 1/32 = 0.03125 */
	{
		int64_t sparsity_int = anx_sf_to_int(
			meta_out.stat_sparsity_bits);
		/* Should be ~0, truncated to 0 */
		if (sparsity_int != 0)
			return -17;
	}

	/* --- Test 5: INT32 tensor [2, 3] --- */
	{
		struct anx_state_object *obj32;
		struct anx_tensor_meta meta32;

		anx_memset(&meta32, 0, sizeof(meta32));
		meta32.ndim = 2;
		meta32.shape[0] = 2;
		meta32.shape[1] = 3;
		meta32.dtype = ANX_DTYPE_INT32;

		ret = anx_tensor_create(&meta32, NULL, 0, &obj32);
		if (ret != ANX_OK)
			return -20;

		/* Payload should be 2 * 3 * 4 = 24 bytes */
		if (obj32->payload_size != 24)
			return -21;

		/* Fill with ones */
		ret = anx_tensor_fill(&obj32->oid, "ones");
		if (ret != ANX_OK)
			return -22;

		{
			const int32_t *p = (const int32_t *)obj32->payload;

			if (p[0] != 1 || p[5] != 1)
				return -23;
		}

		anx_objstore_release(obj32);
	}

	/* --- Test 6: Softfloat round-trip --- */
	{
		uint32_t sf;
		int64_t back;

		sf = anx_sf_from_int(42);
		back = anx_sf_to_int(sf);
		if (back != 42)
			return -30;

		sf = anx_sf_from_int(-100);
		back = anx_sf_to_int(sf);
		if (back != -100)
			return -31;

		sf = anx_sf_from_int(0);
		back = anx_sf_to_int(sf);
		if (back != 0)
			return -32;
	}

	/* --- Test 7: Softfloat add --- */
	{
		uint32_t a = anx_sf_from_int(10);
		uint32_t b = anx_sf_from_int(20);
		uint32_t c = anx_sf_add(a, b);
		int64_t result = anx_sf_to_int(c);

		if (result != 30)
			return -33;
	}

	/* --- Test 8: Softfloat mul --- */
	{
		uint32_t a = anx_sf_from_int(6);
		uint32_t b = anx_sf_from_int(7);
		uint32_t c = anx_sf_mul(a, b);
		int64_t result = anx_sf_to_int(c);

		if (result != 42)
			return -34;
	}

	/* --- Test 9: Softfloat div --- */
	{
		uint32_t a = anx_sf_from_int(100);
		uint32_t b = anx_sf_from_int(4);
		uint32_t c = anx_sf_div(a, b);
		int64_t result = anx_sf_to_int(c);

		if (result != 25)
			return -35;
	}

	/* --- Test 10: Invalid inputs --- */
	{
		struct anx_state_object *bad_obj;
		struct anx_tensor_meta bad_meta;

		anx_memset(&bad_meta, 0, sizeof(bad_meta));
		bad_meta.ndim = 0;  /* invalid */
		bad_meta.dtype = ANX_DTYPE_INT8;

		ret = anx_tensor_create(&bad_meta, NULL, 0, &bad_obj);
		if (ret == ANX_OK)
			return -40;  /* should have failed */

		bad_meta.ndim = 9;  /* over max dims */
		ret = anx_tensor_create(&bad_meta, NULL, 0, &bad_obj);
		if (ret == ANX_OK)
			return -41;
	}

	/* === Phase 2 tests === */

	/* --- Test 11: Slice along first dimension --- */
	{
		struct anx_state_object *src, *sliced;
		struct anx_tensor_meta smeta, slice_meta;

		anx_memset(&smeta, 0, sizeof(smeta));
		smeta.ndim = 2;
		smeta.shape[0] = 8;
		smeta.shape[1] = 4;
		smeta.dtype = ANX_DTYPE_INT8;

		ret = anx_tensor_create(&smeta, NULL, 0, &src);
		if (ret != ANX_OK)
			return -50;

		ret = anx_tensor_fill(&src->oid, "range");
		if (ret != ANX_OK)
			return -51;

		/* Slice rows [2, 5) — 3 rows of 4 elements each */
		ret = anx_tensor_slice(&src->oid, 2, 5, &sliced);
		if (ret != ANX_OK)
			return -52;

		ret = anx_tensor_meta_get(&sliced->oid, &slice_meta);
		if (ret != ANX_OK)
			return -53;

		if (slice_meta.shape[0] != 3)
			return -54;
		if (slice_meta.shape[1] != 4)
			return -55;
		if (slice_meta.elem_count != 12)
			return -56;

		/* First element of slice should be element [2*4]=8 of source */
		{
			const int8_t *sp = (const int8_t *)sliced->payload;

			if (sp[0] != 8)
				return -57;
			/* Last element: row 4, col 3 = src[4*4+3] = 19 */
			if (sp[11] != 19)
				return -58;
		}

		anx_objstore_release(sliced);
		anx_objstore_release(src);
	}

	/* --- Test 12: Diff two tensors --- */
	{
		struct anx_state_object *ta, *tb, *td;
		struct anx_tensor_meta dmeta;

		anx_memset(&dmeta, 0, sizeof(dmeta));
		dmeta.ndim = 1;
		dmeta.shape[0] = 4;
		dmeta.dtype = ANX_DTYPE_INT8;

		ret = anx_tensor_create(&dmeta, NULL, 0, &ta);
		if (ret != ANX_OK)
			return -60;
		ret = anx_tensor_create(&dmeta, NULL, 0, &tb);
		if (ret != ANX_OK)
			return -61;

		/* Fill a = [10, 20, 30, 40], b = [1, 2, 3, 4] */
		{
			int8_t *pa = (int8_t *)ta->payload;
			int8_t *pb = (int8_t *)tb->payload;

			pa[0] = 10; pa[1] = 20; pa[2] = 30; pa[3] = 40;
			pb[0] = 1;  pb[1] = 2;  pb[2] = 3;  pb[3] = 4;
		}

		ret = anx_tensor_diff(&ta->oid, &tb->oid, &td);
		if (ret != ANX_OK)
			return -62;

		/* Result should be [9, 18, 27, 36] */
		{
			const int8_t *pr = (const int8_t *)td->payload;

			if (pr[0] != 9 || pr[1] != 18 ||
			    pr[2] != 27 || pr[3] != 36)
				return -63;
		}

		anx_objstore_release(td);
		anx_objstore_release(tb);
		anx_objstore_release(ta);
	}

	/* --- Test 13: Quantize INT32 → INT8 --- */
	{
		struct anx_state_object *src32, *dst8;
		struct anx_tensor_meta qmeta, qmeta_out;

		anx_memset(&qmeta, 0, sizeof(qmeta));
		qmeta.ndim = 1;
		qmeta.shape[0] = 4;
		qmeta.dtype = ANX_DTYPE_INT32;

		ret = anx_tensor_create(&qmeta, NULL, 0, &src32);
		if (ret != ANX_OK)
			return -70;

		{
			int32_t *p = (int32_t *)src32->payload;

			p[0] = -200;  /* should clamp to -128 */
			p[1] = 0;
			p[2] = 50;
			p[3] = 300;   /* should clamp to 127 */
		}

		ret = anx_tensor_quantize(&src32->oid, ANX_DTYPE_INT8, &dst8);
		if (ret != ANX_OK)
			return -71;

		{
			const int8_t *q = (const int8_t *)dst8->payload;

			if (q[0] != -128)
				return -72;
			if (q[1] != 0)
				return -73;
			if (q[2] != 50)
				return -74;
			if (q[3] != 127)
				return -75;
		}

		ret = anx_tensor_meta_get(&dst8->oid, &qmeta_out);
		if (ret != ANX_OK)
			return -76;
		if (qmeta_out.dtype != ANX_DTYPE_INT8)
			return -77;

		anx_objstore_release(dst8);
		anx_objstore_release(src32);
	}

	/* --- Test 14: Quantize INT8 → INT32 (widen) --- */
	{
		struct anx_state_object *src8, *dst32;
		struct anx_tensor_meta wmeta;

		anx_memset(&wmeta, 0, sizeof(wmeta));
		wmeta.ndim = 1;
		wmeta.shape[0] = 3;
		wmeta.dtype = ANX_DTYPE_INT8;

		ret = anx_tensor_create(&wmeta, NULL, 0, &src8);
		if (ret != ANX_OK)
			return -80;

		{
			int8_t *p = (int8_t *)src8->payload;

			p[0] = -5;
			p[1] = 0;
			p[2] = 100;
		}

		ret = anx_tensor_quantize(&src8->oid, ANX_DTYPE_INT32, &dst32);
		if (ret != ANX_OK)
			return -81;

		{
			const int32_t *w = (const int32_t *)dst32->payload;

			if (w[0] != -5 || w[1] != 0 || w[2] != 100)
				return -82;
		}

		anx_objstore_release(dst32);
		anx_objstore_release(src8);
	}

	/* --- Test 15: Search by dtype --- */
	{
		anx_oid_t results[32];
		uint32_t count = 0;

		/* There are INT8 tensors from earlier tests */
		ret = anx_tensor_search("dtype==int8", results, 32, &count);
		if (ret != ANX_OK)
			return -90;
		/* At least one INT8 tensor should exist */
		if (count == 0)
			return -91;
	}

	/* --- Test 16: Slice boundary checks --- */
	{
		struct anx_state_object *s, *bad_slice;
		struct anx_tensor_meta bmeta;

		anx_memset(&bmeta, 0, sizeof(bmeta));
		bmeta.ndim = 1;
		bmeta.shape[0] = 4;
		bmeta.dtype = ANX_DTYPE_INT8;

		ret = anx_tensor_create(&bmeta, NULL, 0, &s);
		if (ret != ANX_OK)
			return -95;

		/* start >= end should fail */
		ret = anx_tensor_slice(&s->oid, 3, 2, &bad_slice);
		if (ret == ANX_OK)
			return -96;

		/* end > shape[0] should fail */
		ret = anx_tensor_slice(&s->oid, 0, 10, &bad_slice);
		if (ret == ANX_OK)
			return -97;

		anx_objstore_release(s);
	}

	return 0;
}
