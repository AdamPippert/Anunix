/*
 * training.c — Optimizer step with provenance tracking (RFC-0013).
 *
 * Implements a simple SGD step: param_new = param - lr * gradient.
 * The new parameter tensor carries provenance linking it to the
 * old parameter and the gradient that produced it.
 *
 * Learning rate is passed as a scaled integer (lr * 1000) to avoid
 * floating point: lr_scaled=10 means lr=0.01.
 */

#include <anx/types.h>
#include <anx/tensor_ops.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/provenance.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/arch.h>

int anx_tensor_optimizer_step(const anx_oid_t *param_oid,
			       const anx_oid_t *grad_oid,
			       int64_t lr_scaled,
			       struct anx_state_object **updated_out)
{
	struct anx_state_object *param, *grad;
	struct anx_tensor_meta pmeta, gmeta, out_meta;
	int ret;

	if (!param_oid || !grad_oid || !updated_out)
		return ANX_EINVAL;

	param = anx_objstore_lookup(param_oid);
	if (!param)
		return ANX_ENOENT;
	grad = anx_objstore_lookup(grad_oid);
	if (!grad) {
		anx_objstore_release(param);
		return ANX_ENOENT;
	}

	ret = anx_tensor_meta_get(param_oid, &pmeta);
	if (ret != ANX_OK)
		goto out_release;
	ret = anx_tensor_meta_get(grad_oid, &gmeta);
	if (ret != ANX_OK)
		goto out_release;

	/* Shape and dtype must match */
	if (pmeta.ndim != gmeta.ndim || pmeta.dtype != gmeta.dtype) {
		ret = ANX_EINVAL;
		goto out_release;
	}
	{
		uint32_t d;

		for (d = 0; d < pmeta.ndim; d++) {
			if (pmeta.shape[d] != gmeta.shape[d]) {
				ret = ANX_EINVAL;
				goto out_release;
			}
		}
	}

	/* Create output tensor (same shape/dtype) */
	out_meta = pmeta;
	out_meta.parent_tensor = *param_oid;
	out_meta.is_delta = false;

	ret = anx_tensor_create(&out_meta, NULL, 0, updated_out);
	if (ret != ANX_OK)
		goto out_release;

	/* SGD step: param_new[i] = param[i] - (lr/1000) * grad[i] */
	if (pmeta.dtype == ANX_DTYPE_INT8) {
		const int8_t *pp = (const int8_t *)param->payload;
		const int8_t *gp = (const int8_t *)grad->payload;
		int8_t *op = (int8_t *)(*updated_out)->payload;
		uint64_t i;

		for (i = 0; i < pmeta.elem_count; i++) {
			int32_t update = (int32_t)gp[i] * (int32_t)lr_scaled
					  / 1000;
			int32_t v = (int32_t)pp[i] - update;

			if (v > 127) v = 127;
			if (v < -128) v = -128;
			op[i] = (int8_t)v;
		}
	} else if (pmeta.dtype == ANX_DTYPE_INT32) {
		const int32_t *pp = (const int32_t *)param->payload;
		const int32_t *gp = (const int32_t *)grad->payload;
		int32_t *op = (int32_t *)(*updated_out)->payload;
		uint64_t i;

		for (i = 0; i < pmeta.elem_count; i++) {
			int64_t update = (int64_t)gp[i] * lr_scaled / 1000;

			op[i] = pp[i] - (int32_t)update;
		}
	} else {
		ret = ANX_EINVAL;
		goto out_release;
	}

	/* Record provenance: optimizer step */
	if ((*updated_out)->provenance) {
		struct anx_prov_event ev;

		anx_memset(&ev, 0, sizeof(ev));
		ev.event_type = ANX_PROV_DERIVED_FROM;
		ev.timestamp = arch_time_now();
		ev.input_oids = (anx_oid_t *)param_oid;
		ev.input_count = 1;
		anx_strlcpy(ev.description, "sgd_step", sizeof(ev.description));
		ev.reproducible = true;
		anx_prov_log_append((*updated_out)->provenance, &ev);
	}

	anx_objstore_release(grad);
	anx_objstore_release(param);
	return ANX_OK;

out_release:
	anx_objstore_release(grad);
	anx_objstore_release(param);
	return ret;
}
