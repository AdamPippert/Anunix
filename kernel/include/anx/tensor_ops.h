/*
 * anx/tensor_ops.h — Tensor Compute Protocol (RFC-0013 Phase 4).
 *
 * Defines tensor operations and dispatch. Operations are executed by
 * engines registered with tensor capability bits. The CPU reference
 * engine handles INT8/INT32 natively; float dtypes use softfloat.
 */

#ifndef ANX_TENSOR_OPS_H
#define ANX_TENSOR_OPS_H

#include <anx/types.h>

/* Forward declarations */
struct anx_state_object;

/* --- Tensor capability bits (extend engine.h bit 16+) --- */

#define ANX_CAP_TENSOR_INT8	(1U << 16)
#define ANX_CAP_TENSOR_INT32	(1U << 17)
#define ANX_CAP_TENSOR_FP32	(1U << 18)
#define ANX_CAP_TENSOR_BF16	(1U << 19)
#define ANX_CAP_TENSOR_GPU	(1U << 20)

/* --- Operation types --- */

enum anx_tensor_op {
	ANX_OP_MATMUL,		/* C = A @ B */
	ANX_OP_ADD,		/* C = A + B */
	ANX_OP_SUB,		/* C = A - B */
	ANX_OP_MUL_ELEM,	/* C = A * B (element-wise) */
	ANX_OP_RELU,		/* B = relu(A) */
	ANX_OP_SOFTMAX,		/* B = softmax(A, dim=-1) */
	ANX_OP_TRANSPOSE,	/* B = A^T (2D only) */
	ANX_OP_SCALE,		/* B = A * scalar */
	ANX_OP_COUNT,
};

/* --- Operation request --- */

struct anx_tensor_op_request {
	enum anx_tensor_op op;
	const anx_oid_t *inputs[4];	/* up to 4 input tensors */
	uint32_t input_count;
	int64_t scalar;			/* for ANX_OP_SCALE */
};

/* --- Operation result --- */

struct anx_tensor_op_result {
	anx_oid_t output_oid;		/* OID of result tensor */
	uint64_t cycles;		/* TSC cycles for the operation */
};

/* --- Dispatch API --- */

/* Execute a tensor operation, creating a new result tensor */
int anx_tensor_op_execute(const struct anx_tensor_op_request *req,
			    struct anx_tensor_op_result *result);

/* Get human-readable name for an operation */
const char *anx_tensor_op_name(enum anx_tensor_op op);

/* --- CPU reference engine --- */

/* Register the CPU reference engine with the routing plane */
int anx_tensor_cpu_engine_init(void);

/* --- Training provenance --- */

/* Record an optimizer step: new tensor version from old + gradient */
int anx_tensor_optimizer_step(const anx_oid_t *param_oid,
			       const anx_oid_t *grad_oid,
			       int64_t learning_rate_scaled,
			       struct anx_state_object **updated_out);

/* --- Checkpoint --- */

/* Seal all unsealed tensors in a model namespace */
int anx_model_checkpoint(const char *model_name, uint32_t *sealed_count);

/* Verify all tensors in a model are sealed */
int anx_model_verify(const char *model_name, bool *all_sealed);

#endif /* ANX_TENSOR_OPS_H */
