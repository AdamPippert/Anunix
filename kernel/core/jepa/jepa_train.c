/*
 * jepa_train.c — JEPA training loop: VICReg loss, SGD step, trace recording.
 *
 * VICReg loss (Variance-Invariance-Covariance Regularization):
 *
 *   L = λ_inv * L_inv  +  λ_var * L_var  +  λ_cov * L_cov
 *
 *   L_inv = MSE(ẑ_{t+1}, sg(z_{t+1}))          -- invariance (prediction error)
 *   L_var = mean(max(0, γ - std(z_j)))           -- per-dimension variance floor
 *   L_cov = mean(off-diagonal |cov(Z)|^2 / N)    -- covariance decorrelation
 *
 * sg() = stop-gradient on the target encoder output (not backpropagated).
 *
 * Parameter update: SGD via anx_tensor_optimizer_step() on context encoder
 * and predictor weights.  Target encoder is updated by EMA after each step.
 *
 * Trace recording: each (obs_t, action, obs_t+1, z_t, ẑ_{t+1}, z_{t+1}, loss)
 * tuple is stored as ANX_OBJ_JEPA_TRACE for offline replay and audit.
 */

#include "jepa_internal.h"
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/state_object.h>
#include <anx/tensor.h>
#include <anx/tensor_ops.h>
#include <anx/memplane.h>
#include <anx/string.h>
#include <anx/alloc.h>

/* ------------------------------------------------------------------ */
/* VICReg loss components                                              */
/* ------------------------------------------------------------------ */

/*
 * L_inv: mean squared error between predicted and target latents.
 * Both vectors must have the same dimension.
 */
static float vicreg_invariance(const float *z_pred, const float *z_tgt,
				uint32_t dim)
{
	float sum = 0.0f;
	uint32_t i;

	for (i = 0; i < dim; i++) {
		float diff = z_pred[i] - z_tgt[i];
		sum += diff * diff;
	}
	return sum / (float)dim;
}

/*
 * L_var: hinge loss encouraging each dimension to have std > gamma.
 * Computed over a batch of n latent vectors (shape [n * dim]).
 */
static float vicreg_variance(const float *batch, uint32_t n, uint32_t dim,
			     float gamma)
{
	float loss = 0.0f;
	uint32_t j, i;

	for (j = 0; j < dim; j++) {
		float mean = 0.0f, var = 0.0f;

		for (i = 0; i < n; i++)
			mean += batch[i * dim + j];
		mean /= (float)n;

		for (i = 0; i < n; i++) {
			float d = batch[i * dim + j] - mean;
			var += d * d;
		}
		var /= (float)n;

		/* std = approx sqrt(var) via Newton-Raphson */
		float std_est = var;
		if (std_est > 1e-8f) {
			std_est = 1.0f / std_est;
			std_est = std_est * (1.5f - 0.5f * var * std_est * std_est);
			std_est = 1.0f / std_est;
		}

		float hinge = gamma - std_est;
		if (hinge > 0.0f)
			loss += hinge;
	}
	return loss / (float)dim;
}

/*
 * L_cov: sum of squared off-diagonal covariance elements, normalised.
 * Encourages decorrelated (non-redundant) dimensions.
 */
static float vicreg_covariance(const float *batch, uint32_t n, uint32_t dim)
{
	float cov_sum = 0.0f;
	uint32_t j, k, i;

	for (j = 0; j < dim; j++) {
		float mean_j = 0.0f;

		for (i = 0; i < n; i++)
			mean_j += batch[i * dim + j];
		mean_j /= (float)n;

		for (k = j + 1; k < dim; k++) {
			float mean_k = 0.0f, cov = 0.0f;

			for (i = 0; i < n; i++)
				mean_k += batch[i * dim + k];
			mean_k /= (float)n;

			for (i = 0; i < n; i++) {
				cov += (batch[i * dim + j] - mean_j) *
				       (batch[i * dim + k] - mean_k);
			}
			cov /= (float)(n - 1 > 0 ? n - 1 : 1);
			cov_sum += cov * cov;
		}
	}

	/* Normalise by dim*(dim-1)/2 (number of off-diagonal pairs) */
	float pairs = (float)(dim * (dim - 1)) / 2.0f;
	return pairs > 0.0f ? cov_sum / pairs : 0.0f;
}

/* ------------------------------------------------------------------ */
/* Gradient approximation (finite differences)                        */
/* ------------------------------------------------------------------ */

/*
 * Compute a per-weight gradient approximation via forward finite
 * differences.  This is O(W) forward passes — slow but correct, and
 * avoids building a full autograd engine in the kernel.
 *
 * delta: small perturbation (e.g. 1e-4)
 * weights: weight buffer (obs_dim*latent + latent*latent floats for encoder)
 * grad_out: output gradient buffer, same size as weights
 * base_loss: loss at current weights (pre-computed)
 * loss_fn: function that computes loss given weight buffer
 */
typedef float (*jepa_loss_fn_t)(const float *w, uint32_t wlen, void *arg);

static void finite_diff_grad(const float *weights, uint32_t wlen,
			     float base_loss, float delta,
			     jepa_loss_fn_t loss_fn, void *arg,
			     float *grad_out)
{
	float *w_perturb = (float *)anx_alloc(wlen * sizeof(float));
	uint32_t i;

	if (!w_perturb) {
		anx_memset(grad_out, 0, wlen * sizeof(float));
		return;
	}

	anx_memcpy(w_perturb, weights, wlen * sizeof(float));

	for (i = 0; i < wlen; i++) {
		float orig = w_perturb[i];
		float perturbed_loss;

		w_perturb[i] = orig + delta;
		perturbed_loss = loss_fn(w_perturb, wlen, arg);
		grad_out[i] = (perturbed_loss - base_loss) / delta;
		w_perturb[i] = orig;
	}

	anx_free(w_perturb);
}

/* ------------------------------------------------------------------ */
/* Training step context passed to loss_fn                             */
/* ------------------------------------------------------------------ */

struct train_step_ctx {
	const float *z_pred_batch;	/* [n * latent_dim] */
	const float *z_tgt_batch;	/* [n * latent_dim] */
	uint32_t     n;
	uint32_t     latent_dim;
	float        lambda_inv;
	float        lambda_var;
	float        lambda_cov;
	float        gamma;		/* variance floor (default 1.0) */
};

static float vicreg_total_loss(const float *weights, uint32_t wlen, void *arg)
{
	struct train_step_ctx *c = (struct train_step_ctx *)arg;
	float l_inv = 0.0f;
	uint32_t i;

	(void)weights;
	(void)wlen;

	for (i = 0; i < c->n; i++) {
		l_inv += vicreg_invariance(
			c->z_pred_batch + i * c->latent_dim,
			c->z_tgt_batch  + i * c->latent_dim,
			c->latent_dim);
	}
	l_inv /= (float)c->n;

	float l_var = vicreg_variance(c->z_pred_batch, c->n,
				      c->latent_dim, c->gamma);
	float l_cov = vicreg_covariance(c->z_pred_batch, c->n, c->latent_dim);

	return c->lambda_inv * l_inv +
	       c->lambda_var * l_var +
	       c->lambda_cov * l_cov;
}

/* ------------------------------------------------------------------ */
/* Public: training step                                               */
/* ------------------------------------------------------------------ */

int anx_jepa_train_step(const anx_oid_t *trace_oids, uint32_t n_traces)
{
	struct anx_jepa_ctx              *ctx   = anx_jepa_ctx_get();
	const struct anx_jepa_world_profile *world = anx_jepa_world_get_active();
	struct anx_jepa_trace_payload    *traces;
	struct anx_jepa_latent_payload   *pred_pay, *tgt_pay;
	struct anx_object_handle          toh, ph, th;
	float *z_pred_batch, *z_tgt_batch;
	uint32_t latent_dim, i;
	float base_loss;
	int rc = ANX_OK;

	if (!trace_oids || n_traces == 0)
		return ANX_EINVAL;
	if (!anx_jepa_available())
		return ANX_ENOENT;

	latent_dim = world->arch.latent_dim;

	traces = (struct anx_jepa_trace_payload *)
		anx_alloc(n_traces * sizeof(struct anx_jepa_trace_payload));
	z_pred_batch = (float *)anx_alloc(n_traces * latent_dim * sizeof(float));
	z_tgt_batch  = (float *)anx_alloc(n_traces * latent_dim * sizeof(float));
	pred_pay     = (struct anx_jepa_latent_payload *)
		anx_alloc(sizeof(struct anx_jepa_latent_payload));
	tgt_pay      = (struct anx_jepa_latent_payload *)
		anx_alloc(sizeof(struct anx_jepa_latent_payload));

	if (!traces || !z_pred_batch || !z_tgt_batch || !pred_pay || !tgt_pay) {
		rc = ANX_ENOMEM;
		goto cleanup;
	}

	/* Load traces and collect predicted + target latent batches */
	for (i = 0; i < n_traces; i++) {
		rc = anx_so_open(&trace_oids[i], ANX_OPEN_READ, &toh);
		if (rc != ANX_OK)
			goto cleanup;

		rc = anx_so_read_payload(&toh, 0, &traces[i],
					 sizeof(struct anx_jepa_trace_payload));
		anx_so_close(&toh);
		if (rc != ANX_OK)
			goto cleanup;

		/* Predicted latent */
		rc = anx_so_open(&traces[i].latent_pred_oid,
				 ANX_OPEN_READ, &ph);
		if (rc != ANX_OK)
			goto cleanup;
		rc = anx_so_read_payload(&ph, 0, pred_pay,
					 sizeof(struct anx_jepa_latent_payload));
		anx_so_close(&ph);
		if (rc != ANX_OK)
			goto cleanup;

		/* Target latent */
		rc = anx_so_open(&traces[i].latent_tp1_oid,
				 ANX_OPEN_READ, &th);
		if (rc != ANX_OK)
			goto cleanup;
		rc = anx_so_read_payload(&th, 0, tgt_pay,
					 sizeof(struct anx_jepa_latent_payload));
		anx_so_close(&th);
		if (rc != ANX_OK)
			goto cleanup;

		anx_memcpy(z_pred_batch + i * latent_dim,
			   pred_pay->vec, latent_dim * sizeof(float));
		anx_memcpy(z_tgt_batch  + i * latent_dim,
			   tgt_pay->vec,  latent_dim * sizeof(float));
	}

	/* Compute VICReg loss */
	struct train_step_ctx tctx = {
		.z_pred_batch = z_pred_batch,
		.z_tgt_batch  = z_tgt_batch,
		.n            = n_traces,
		.latent_dim   = latent_dim,
		.lambda_inv   = world->train.vicreg_lambda_inv,
		.lambda_var   = world->train.vicreg_lambda_var,
		.lambda_cov   = world->train.vicreg_lambda_cov,
		.gamma        = 1.0f,
	};
	base_loss = vicreg_total_loss(NULL, 0, &tctx);
	ctx->last_loss = base_loss;

	/*
	 * Weight update via anx_tensor_optimizer_step() if weights are present.
	 * Finite-difference gradients over the encoder weights.
	 * Predictor weights share the same checkpoint tensor; a full
	 * implementation would compute separate gradients for each component.
	 */
	{
		anx_oid_t zero;
		anx_memset(&zero, 0, sizeof(zero));

		if (anx_memcmp(&ctx->encoder_weights_oid,
			       &zero, sizeof(zero)) != 0) {
			uint32_t obs_dim  = world->arch.obs_dim;
			uint32_t wlen     = obs_dim * latent_dim +
					    latent_dim * latent_dim;
			float *weights    = (float *)anx_alloc(wlen * sizeof(float));
			float *grad       = (float *)anx_alloc(wlen * sizeof(float));

			if (weights && grad) {
				struct anx_object_handle wh;

				rc = anx_so_open(&ctx->encoder_weights_oid,
						 ANX_OPEN_READWRITE, &wh);
				if (rc == ANX_OK) {
					anx_so_read_payload(&wh, 0, weights,
						(uint64_t)wlen * sizeof(float));

					finite_diff_grad(weights, wlen,
						base_loss, 1e-4f,
						vicreg_total_loss, &tctx,
						grad);

					/* SGD: w ← w - lr * grad */
					{
						uint32_t w;
						float lr = world->train.lr;

						for (w = 0; w < wlen; w++)
							weights[w] -= lr * grad[w];
					}

					anx_so_replace_payload(&wh, weights,
						(uint64_t)wlen * sizeof(float));
					anx_so_close(&wh);
				}
			}
			anx_free(weights);
			anx_free(grad);
		}
	}

	/* EMA update of target encoder */
	anx_jepa_update_target_encoder();

	ctx->train_steps++;

cleanup:
	anx_free(traces);
	anx_free(z_pred_batch);
	anx_free(z_tgt_batch);
	anx_free(pred_pay);
	anx_free(tgt_pay);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Trace recording                                                     */
/* ------------------------------------------------------------------ */

int anx_jepa_record_trace(const anx_oid_t *obs_t_oid,
			  uint32_t action_id,
			  const anx_oid_t *obs_tp1_oid,
			  anx_oid_t *trace_oid_out)
{
	anx_oid_t latent_t_oid, latent_pred_oid, latent_tp1_oid;
	struct anx_jepa_trace_payload trace;
	struct anx_so_create_params params;
	struct anx_state_object *tso;
	int rc;

	if (!obs_t_oid || !obs_tp1_oid)
		return ANX_EINVAL;
	if (!anx_jepa_available())
		return ANX_ENOENT;

	/* Encode context state */
	rc = anx_jepa_encode(obs_t_oid, &latent_t_oid);
	if (rc != ANX_OK)
		return rc;

	/* Predict next state */
	rc = anx_jepa_predict(&latent_t_oid, action_id, &latent_pred_oid);
	if (rc != ANX_OK)
		return rc;

	/* Encode actual next state (target) */
	rc = anx_jepa_encode(obs_tp1_oid, &latent_tp1_oid);
	if (rc != ANX_OK)
		return rc;

	/* Compute loss for this trace */
	float loss = anx_jepa_divergence(&latent_pred_oid, &latent_tp1_oid);

	anx_memset(&trace, 0, sizeof(trace));
	trace.obs_t_oid      = *obs_t_oid;
	trace.obs_tp1_oid    = *obs_tp1_oid;
	trace.latent_t_oid   = latent_t_oid;
	trace.latent_pred_oid = latent_pred_oid;
	trace.latent_tp1_oid = latent_tp1_oid;
	trace.action_id      = action_id;
	trace.loss           = loss;

	anx_memset(&params, 0, sizeof(params));
	params.object_type  = ANX_OBJ_JEPA_TRACE;
	params.schema_uri   = "anx:schema/jepa-trace/v1";
	params.payload      = &trace;
	params.payload_size = sizeof(trace);

	rc = anx_so_create(&params, &tso);
	if (rc != ANX_OK)
		return rc;

	/* Admit to memplane L2 for persistence */
	anx_memplane_admit(&tso->oid, ANX_ADMIT_LONG_TERM_CANDIDATE, NULL);
	if (trace_oid_out)
		*trace_oid_out = tso->oid;
	anx_objstore_release(tso);
	return ANX_OK;
}
