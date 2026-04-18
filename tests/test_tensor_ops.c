/*
 * test_tensor_ops.c — Tests for Tensor Operations (RFC-0013 Phase 4).
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/tensor_ops.h>
#include <anx/state_object.h>
#include <anx/engine.h>
#include <anx/string.h>

int test_tensor_ops(void)
{
	int ret;

	anx_objstore_init();
	anx_engine_registry_init();
	anx_tensor_cpu_engine_init();

	/* --- Test 1: INT8 matmul [2,3] @ [3,2] -> [2,2] --- */
	{
		struct anx_state_object *a, *b;
		struct anx_tensor_meta ma, mb;
		struct anx_tensor_op_request req;
		struct anx_tensor_op_result result;
		struct anx_tensor_meta mout;

		anx_memset(&ma, 0, sizeof(ma));
		ma.ndim = 2; ma.shape[0] = 2; ma.shape[1] = 3;
		ma.dtype = ANX_DTYPE_INT8;

		anx_memset(&mb, 0, sizeof(mb));
		mb.ndim = 2; mb.shape[0] = 3; mb.shape[1] = 2;
		mb.dtype = ANX_DTYPE_INT8;

		ret = anx_tensor_create(&ma, NULL, 0, &a);
		if (ret != ANX_OK) return -1;
		ret = anx_tensor_create(&mb, NULL, 0, &b);
		if (ret != ANX_OK) return -2;

		/* A = [[1,2,3],[4,5,6]] */
		{
			int8_t *pa = (int8_t *)a->payload;
			pa[0]=1; pa[1]=2; pa[2]=3;
			pa[3]=4; pa[4]=5; pa[5]=6;
		}
		/* B = [[7,8],[9,10],[11,12]] */
		{
			int8_t *pb = (int8_t *)b->payload;
			pb[0]=7;  pb[1]=8;
			pb[2]=9;  pb[3]=10;
			pb[4]=11; pb[5]=12;
		}

		anx_memset(&req, 0, sizeof(req));
		req.op = ANX_OP_MATMUL;
		req.inputs[0] = &a->oid;
		req.inputs[1] = &b->oid;
		req.input_count = 2;

		ret = anx_tensor_op_execute(&req, &result);
		if (ret != ANX_OK) return -3;

		/* C = [[58,64],[139,154]] but saturated to int8 max 127 */
		{
			struct anx_state_object *c;

			c = anx_objstore_lookup(&result.output_oid);
			if (!c) return -4;

			{
				const int8_t *pc = (const int8_t *)c->payload;

				/* C[0,0] = 1*7+2*9+3*11 = 7+18+33 = 58 */
				if (pc[0] != 58) return -5;
				/* C[0,1] = 1*8+2*10+3*12 = 8+20+36 = 64 */
				if (pc[1] != 64) return -6;
				/* C[1,0] = 4*7+5*9+6*11 = 28+45+66 = 139
				 * -> saturated to 127 */
				if (pc[2] != 127) return -7;
				/* C[1,1] = 4*8+5*10+6*12 = 32+50+72 = 154
				 * -> saturated to 127 */
				if (pc[3] != 127) return -8;
			}

			anx_tensor_meta_get(&c->oid, &mout);
			if (mout.shape[0] != 2 || mout.shape[1] != 2)
				return -9;

			anx_objstore_release(c);
		}

		anx_objstore_release(a);
		anx_objstore_release(b);
	}

	/* --- Test 2: Element-wise add --- */
	{
		struct anx_state_object *a, *b;
		struct anx_tensor_meta m;
		struct anx_tensor_op_request req;
		struct anx_tensor_op_result result;

		anx_memset(&m, 0, sizeof(m));
		m.ndim = 1; m.shape[0] = 4;
		m.dtype = ANX_DTYPE_INT8;

		ret = anx_tensor_create(&m, NULL, 0, &a);
		if (ret != ANX_OK) return -10;
		ret = anx_tensor_create(&m, NULL, 0, &b);
		if (ret != ANX_OK) return -11;

		{
			int8_t *pa = (int8_t *)a->payload;
			int8_t *pb = (int8_t *)b->payload;
			pa[0]=10; pa[1]=20; pa[2]=30; pa[3]=40;
			pb[0]=5;  pb[1]=10; pb[2]=15; pb[3]=20;
		}

		anx_memset(&req, 0, sizeof(req));
		req.op = ANX_OP_ADD;
		req.inputs[0] = &a->oid;
		req.inputs[1] = &b->oid;
		req.input_count = 2;

		ret = anx_tensor_op_execute(&req, &result);
		if (ret != ANX_OK) return -12;

		{
			struct anx_state_object *c;

			c = anx_objstore_lookup(&result.output_oid);
			if (!c) return -13;
			{
				const int8_t *pc = (const int8_t *)c->payload;

				if (pc[0] != 15 || pc[1] != 30 ||
				    pc[2] != 45 || pc[3] != 60)
					return -14;
			}
			anx_objstore_release(c);
		}

		anx_objstore_release(a);
		anx_objstore_release(b);
	}

	/* --- Test 3: ReLU --- */
	{
		struct anx_state_object *a;
		struct anx_tensor_meta m;
		struct anx_tensor_op_request req;
		struct anx_tensor_op_result result;

		anx_memset(&m, 0, sizeof(m));
		m.ndim = 1; m.shape[0] = 4;
		m.dtype = ANX_DTYPE_INT8;

		ret = anx_tensor_create(&m, NULL, 0, &a);
		if (ret != ANX_OK) return -20;

		{
			int8_t *pa = (int8_t *)a->payload;
			pa[0] = -5; pa[1] = 0; pa[2] = 3; pa[3] = -1;
		}

		anx_memset(&req, 0, sizeof(req));
		req.op = ANX_OP_RELU;
		req.inputs[0] = &a->oid;
		req.input_count = 1;

		ret = anx_tensor_op_execute(&req, &result);
		if (ret != ANX_OK) return -21;

		{
			struct anx_state_object *c;

			c = anx_objstore_lookup(&result.output_oid);
			if (!c) return -22;
			{
				const int8_t *pc = (const int8_t *)c->payload;

				if (pc[0] != 0 || pc[1] != 0 ||
				    pc[2] != 3 || pc[3] != 0)
					return -23;
			}
			anx_objstore_release(c);
		}
		anx_objstore_release(a);
	}

	/* --- Test 4: Transpose [2,3] -> [3,2] --- */
	{
		struct anx_state_object *a;
		struct anx_tensor_meta m, mout;
		struct anx_tensor_op_request req;
		struct anx_tensor_op_result result;

		anx_memset(&m, 0, sizeof(m));
		m.ndim = 2; m.shape[0] = 2; m.shape[1] = 3;
		m.dtype = ANX_DTYPE_INT8;

		ret = anx_tensor_create(&m, NULL, 0, &a);
		if (ret != ANX_OK) return -30;

		{
			int8_t *pa = (int8_t *)a->payload;
			pa[0]=1; pa[1]=2; pa[2]=3;
			pa[3]=4; pa[4]=5; pa[5]=6;
		}

		anx_memset(&req, 0, sizeof(req));
		req.op = ANX_OP_TRANSPOSE;
		req.inputs[0] = &a->oid;
		req.input_count = 1;

		ret = anx_tensor_op_execute(&req, &result);
		if (ret != ANX_OK) return -31;

		{
			struct anx_state_object *c;

			c = anx_objstore_lookup(&result.output_oid);
			if (!c) return -32;

			anx_tensor_meta_get(&c->oid, &mout);
			if (mout.shape[0] != 3 || mout.shape[1] != 2)
				return -33;

			{
				const int8_t *pc = (const int8_t *)c->payload;
				/* Transposed: [[1,4],[2,5],[3,6]] */
				if (pc[0] != 1 || pc[1] != 4 ||
				    pc[2] != 2 || pc[3] != 5 ||
				    pc[4] != 3 || pc[5] != 6)
					return -34;
			}
			anx_objstore_release(c);
		}
		anx_objstore_release(a);
	}

	/* --- Test 5: Optimizer step (SGD) --- */
	{
		struct anx_state_object *param, *grad, *updated;
		struct anx_tensor_meta m;

		anx_memset(&m, 0, sizeof(m));
		m.ndim = 1; m.shape[0] = 4;
		m.dtype = ANX_DTYPE_INT32;

		ret = anx_tensor_create(&m, NULL, 0, &param);
		if (ret != ANX_OK) return -40;
		ret = anx_tensor_create(&m, NULL, 0, &grad);
		if (ret != ANX_OK) return -41;

		{
			int32_t *pp = (int32_t *)param->payload;
			int32_t *gp = (int32_t *)grad->payload;
			pp[0]=1000; pp[1]=2000; pp[2]=3000; pp[3]=4000;
			gp[0]=100;  gp[1]=200;  gp[2]=300;  gp[3]=400;
		}

		/* lr_scaled=100 means lr=0.1 */
		ret = anx_tensor_optimizer_step(&param->oid, &grad->oid,
						 100, &updated);
		if (ret != ANX_OK) return -42;

		/* new = old - (grad * 100 / 1000) = old - grad/10 */
		{
			const int32_t *up = (const int32_t *)updated->payload;

			/* 1000 - 100*100/1000 = 1000 - 10 = 990 */
			if (up[0] != 990) return -43;
			/* 2000 - 200*100/1000 = 2000 - 20 = 1980 */
			if (up[1] != 1980) return -44;
			if (up[2] != 2970) return -45;
			if (up[3] != 3960) return -46;
		}

		anx_objstore_release(updated);
		anx_objstore_release(grad);
		anx_objstore_release(param);
	}

	/* --- Test 6: Operation name strings --- */
	if (anx_strcmp(anx_tensor_op_name(ANX_OP_MATMUL), "matmul") != 0)
		return -50;
	if (anx_strcmp(anx_tensor_op_name(ANX_OP_RELU), "relu") != 0)
		return -51;

	return 0;
}
