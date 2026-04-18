/*
 * tensor_ops.c — Tensor operation dispatch (RFC-0013 Phase 4).
 *
 * Resolves input tensors, validates shapes, selects an engine via
 * the routing plane, and delegates execution to the engine's
 * compute functions. Currently dispatches directly to the CPU
 * reference engine.
 */

#include <anx/types.h>
#include <anx/tensor_ops.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/arch.h>

/* CPU engine functions (defined in engine_cpu.c) */
int anx_tensor_cpu_matmul(struct anx_state_object *a,
			    struct anx_state_object *b,
			    struct anx_tensor_meta *ma,
			    struct anx_tensor_meta *mb,
			    struct anx_state_object **out);
int anx_tensor_cpu_binary(enum anx_tensor_op op,
			    struct anx_state_object *a,
			    struct anx_state_object *b,
			    struct anx_tensor_meta *ma,
			    struct anx_state_object **out);
int anx_tensor_cpu_unary(enum anx_tensor_op op,
			   struct anx_state_object *a,
			   struct anx_tensor_meta *ma,
			   int64_t scalar,
			   struct anx_state_object **out);

const char *anx_tensor_op_name(enum anx_tensor_op op)
{
	switch (op) {
	case ANX_OP_MATMUL:	return "matmul";
	case ANX_OP_ADD:	return "add";
	case ANX_OP_SUB:	return "sub";
	case ANX_OP_MUL_ELEM:	return "mul";
	case ANX_OP_RELU:	return "relu";
	case ANX_OP_SOFTMAX:	return "softmax";
	case ANX_OP_TRANSPOSE:	return "transpose";
	case ANX_OP_SCALE:	return "scale";
	default:		return "unknown";
	}
}

int anx_tensor_op_execute(const struct anx_tensor_op_request *req,
			    struct anx_tensor_op_result *result)
{
	struct anx_state_object *inputs[4] = {NULL};
	struct anx_tensor_meta metas[4];
	struct anx_state_object *output = NULL;
	uint64_t t0, t1;
	uint32_t i;
	int ret;

	if (!req || !result)
		return ANX_EINVAL;
	if (req->input_count == 0 || req->input_count > 4)
		return ANX_EINVAL;

	/* Resolve inputs */
	for (i = 0; i < req->input_count; i++) {
		if (!req->inputs[i])
			return ANX_EINVAL;

		inputs[i] = anx_objstore_lookup(req->inputs[i]);
		if (!inputs[i]) {
			ret = ANX_ENOENT;
			goto cleanup;
		}
		if (inputs[i]->object_type != ANX_OBJ_TENSOR ||
		    !inputs[i]->payload) {
			ret = ANX_EINVAL;
			goto cleanup;
		}

		ret = anx_tensor_meta_get(req->inputs[i], &metas[i]);
		if (ret != ANX_OK)
			goto cleanup;
	}

	t0 = arch_timer_ticks();

	/* Dispatch to CPU engine based on operation type */
	switch (req->op) {
	case ANX_OP_MATMUL:
		if (req->input_count < 2) {
			ret = ANX_EINVAL;
			goto cleanup;
		}
		ret = anx_tensor_cpu_matmul(inputs[0], inputs[1],
					     &metas[0], &metas[1], &output);
		break;

	case ANX_OP_ADD:
	case ANX_OP_SUB:
	case ANX_OP_MUL_ELEM:
		if (req->input_count < 2) {
			ret = ANX_EINVAL;
			goto cleanup;
		}
		ret = anx_tensor_cpu_binary(req->op, inputs[0], inputs[1],
					     &metas[0], &output);
		break;

	case ANX_OP_RELU:
	case ANX_OP_SOFTMAX:
	case ANX_OP_TRANSPOSE:
	case ANX_OP_SCALE:
		ret = anx_tensor_cpu_unary(req->op, inputs[0], &metas[0],
					    req->scalar, &output);
		break;

	default:
		ret = ANX_EINVAL;
		goto cleanup;
	}

	t1 = arch_timer_ticks();

	if (ret != ANX_OK)
		goto cleanup;

	result->output_oid = output->oid;
	result->cycles = t1 - t0;

	anx_objstore_release(output);

cleanup:
	for (i = 0; i < req->input_count; i++) {
		if (inputs[i])
			anx_objstore_release(inputs[i]);
	}
	return ret;
}
