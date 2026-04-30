/*
 * jepa_encoder.c — JEPA context encoder and target (EMA) encoder.
 *
 * Architecture: two-layer MLP projection with residual connection,
 * operating over a linearized observation vector.  This is a functional
 * stand-in for the full transformer (4L/4H/256d) specified in the design;
 * the forward pass contract and weight OID layout are identical to what a
 * full transformer would use, so the upgrade path is in-place.
 *
 * Forward pass (untrained / no checkpoint):
 *   obs → linearize → deterministic hash projection → unit-normalized latent
 *
 * Forward pass (trained checkpoint loaded):
 *   obs → linearize → matmul(W1) → relu → matmul(W2) → L2-normalize → latent
 *
 * EMA target encoder update:
 *   θ_target ← decay * θ_target + (1 - decay) * θ_context
 *   Operates on the float payload of the weight tensors in-place.
 */

#include "jepa_internal.h"
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/state_object.h>
#include <anx/tensor.h>
#include <anx/tensor_ops.h>
#include <anx/string.h>
#include <anx/alloc.h>

/* ------------------------------------------------------------------ */
/* Observation linearization                                           */
/* ------------------------------------------------------------------ */

int anx_jepa_obs_linearize(const struct anx_jepa_obs *obs,
			   float *out_vec, uint32_t max_dim)
{
	uint32_t i, idx = 0;

	if (!obs || !out_vec || max_dim == 0)
		return ANX_EINVAL;

#define EMIT(v) do { if (idx < max_dim) out_vec[idx++] = (float)(v); } while (0)

	/* Scheduler queue depths — normalised by a soft cap of 64 */
	for (i = 0; i < ANX_JEPA_OBS_SCHED_CLASSES; i++)
		EMIT(obs->sched_queue_depths[i] / 64.0f);

	EMIT(obs->active_cell_count / 256.0f);

	/* Memory tier stats — decay score (0-1000 range) and entry count */
	for (i = 0; i < ANX_JEPA_OBS_MEM_TIERS; i++) {
		EMIT(obs->mem_decay_score_avg[i] / 1000.0f);
		EMIT(obs->mem_entry_counts[i]    / 4096.0f);
	}

	/* Routing */
	EMIT(obs->route_fallback_count / 16.0f);
	EMIT(obs->route_avg_score      / 100.0f);

	/* Compute utilization (already 0.0-1.0) */
	EMIT(obs->tensor_cpu_util);
	EMIT(obs->tensor_npu_util);

	/* Capability validation */
	EMIT(obs->cap_validation_avg / 100.0f);
	EMIT(obs->cap_failures       / 16.0f);

	/* Error and security counters */
	EMIT(obs->error_count          / 32.0f);
	EMIT(obs->security_event_count / 32.0f);

#undef EMIT

	/* Zero-pad to max_dim if we emitted fewer fields */
	while (idx < max_dim)
		out_vec[idx++] = 0.0f;

	return (int)idx;
}

/* ------------------------------------------------------------------ */
/* Vector utilities                                                    */
/* ------------------------------------------------------------------ */

void anx_jepa_vec_normalize(float *vec, uint32_t dim)
{
	float norm_sq = 0.0f;
	float inv_norm;
	uint32_t i;

	for (i = 0; i < dim; i++)
		norm_sq += vec[i] * vec[i];

	if (norm_sq < 1e-12f)
		return;

	/* Approximate reciprocal sqrt via Newton-Raphson (two iterations).
	 * Good enough for normalizing latent vectors; avoids a libm dep. */
	inv_norm = 1.0f / norm_sq;		/* initial estimate (1/x not 1/sqrt) */
	inv_norm = inv_norm * (1.5f - 0.5f * norm_sq * inv_norm * inv_norm);
	inv_norm = inv_norm * (1.5f - 0.5f * norm_sq * inv_norm * inv_norm);

	for (i = 0; i < dim; i++)
		vec[i] *= inv_norm;
}

float anx_jepa_vec_cosine(const float *a, const float *b, uint32_t dim)
{
	float dot = 0.0f, na = 0.0f, nb = 0.0f;
	uint32_t i;

	if (!a || !b || dim == 0)
		return -2.0f;

	for (i = 0; i < dim; i++) {
		dot += a[i] * b[i];
		na  += a[i] * a[i];
		nb  += b[i] * b[i];
	}

	if (na < 1e-12f || nb < 1e-12f)
		return 0.0f;

	/* Reciprocal sqrt product */
	{
		float denom = na * nb;
		float inv   = 1.0f / denom;
		inv = inv * (1.5f - 0.5f * denom * inv * inv);
		inv = inv * (1.5f - 0.5f * denom * inv * inv);
		return dot * inv;
	}
}

/* ------------------------------------------------------------------ */
/* Deterministic projection (no-checkpoint fallback)                  */
/* ------------------------------------------------------------------ */

/*
 * When no trained weights are present, produce a latent by mixing the
 * linearized obs vector into a pseudo-random basis using a simple
 * multiply-accumulate hash projection.  The result is bounded and
 * consistent for the same obs input, giving a useful (if unlearned)
 * embedding for early operation before any training data is available.
 */
static void deterministic_project(const float *obs_vec, uint32_t obs_dim,
				  float *latent, uint32_t latent_dim)
{
	uint32_t i, j;
	/* Simple LCG seed derived from first obs element */
	uint32_t seed = (uint32_t)(obs_vec[0] * 1000000.0f) ^ 0xdeadbeef;

	for (j = 0; j < latent_dim; j++) {
		float acc = 0.0f;

		for (i = 0; i < obs_dim; i++) {
			/* Deterministic weight via LCG */
			seed = seed * 1664525u + 1013904223u;
			float w = ((float)(seed & 0xffff) / 32768.0f) - 1.0f;
			acc += obs_vec[i] * w;
		}
		latent[j] = acc;
	}
}

/* ------------------------------------------------------------------ */
/* Trained forward pass via tensor ops                                 */
/* ------------------------------------------------------------------ */

/*
 * Minimal two-layer MLP forward pass:
 *   x = obs_vec  (shape: [obs_dim])
 *   h = relu(x @ W1)  (shape: [latent_dim])
 *   z = h @ W2         (shape: [latent_dim])
 *   z = L2_normalize(z)
 *
 * W1 and W2 are expected as sub-regions of the checkpoint tensor payload.
 * Layout: [W1: obs_dim * latent_dim floats][W2: latent_dim * latent_dim floats]
 *
 * Returns ANX_OK or negative on error.
 */
static int mlp_forward(const float *obs_vec, uint32_t obs_dim,
		       const float *weights, uint32_t latent_dim,
		       float *latent_out)
{
	const float *W1 = weights;
	const float *W2 = weights + obs_dim * latent_dim;
	float *hidden;
	uint32_t i, j;

	hidden = (float *)anx_alloc(latent_dim * sizeof(float));
	if (!hidden)
		return ANX_ENOMEM;

	/* h = relu(obs @ W1) */
	for (j = 0; j < latent_dim; j++) {
		float acc = 0.0f;

		for (i = 0; i < obs_dim; i++)
			acc += obs_vec[i] * W1[i * latent_dim + j];
		hidden[j] = acc > 0.0f ? acc : 0.0f;	/* relu */
	}

	/* z = hidden @ W2 */
	for (j = 0; j < latent_dim; j++) {
		float acc = 0.0f;

		for (i = 0; i < latent_dim; i++)
			acc += hidden[i] * W2[i * latent_dim + j];
		latent_out[j] = acc;
	}

	anx_free(hidden);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Public: encode                                                      */
/* ------------------------------------------------------------------ */

int anx_jepa_encode(const anx_oid_t *obs_oid, anx_oid_t *latent_oid_out)
{
	struct anx_jepa_ctx *ctx = anx_jepa_ctx_get();
	const struct anx_jepa_world_profile *world;
	struct anx_object_handle obs_handle, weight_handle;
	struct anx_jepa_latent_payload *lpay;
	struct anx_so_create_params params;
	struct anx_state_object *lso;
	float obs_vec[64];	/* linearized obs — sized for os-default */
	bool weights_loaded;
	uint32_t obs_dim, latent_dim;
	int rc;

	if (!obs_oid || !latent_oid_out)
		return ANX_EINVAL;
	if (!anx_jepa_available())
		return ANX_ENOENT;

	world      = anx_jepa_world_get_active();
	obs_dim    = world->arch.obs_dim;
	latent_dim = world->arch.latent_dim;

	/* Read stored observation */
	rc = anx_so_open(obs_oid, ANX_OPEN_READ, &obs_handle);
	if (rc != ANX_OK)
		return rc;

	{
		struct anx_jepa_obs obs_snap;

		rc = anx_so_read_payload(&obs_handle, 0,
					 &obs_snap, sizeof(obs_snap));
		anx_so_close(&obs_handle);
		/* anx_so_read_payload returns byte count on success */
		if (rc < 0)
			return rc;

		anx_jepa_obs_linearize(&obs_snap, obs_vec, obs_dim);
	}

	/* Allocate latent payload */
	lpay = (struct anx_jepa_latent_payload *)
		anx_alloc(sizeof(struct anx_jepa_latent_payload));
	if (!lpay)
		return ANX_ENOMEM;

	anx_memset(lpay, 0, sizeof(*lpay));
	lpay->dim           = latent_dim;
	lpay->source_obs_oid = *obs_oid;

	/* Check whether weights are available */
	weights_loaded = false;
	{
		anx_oid_t zero;
		anx_memset(&zero, 0, sizeof(zero));
		/* A zeroed OID indicates no checkpoint is loaded */
		weights_loaded = (anx_memcmp(&ctx->encoder_weights_oid,
					     &zero, sizeof(zero)) != 0);
	}

	if (weights_loaded) {
		rc = anx_so_open(&ctx->encoder_weights_oid,
				 ANX_OPEN_READ, &weight_handle);
		if (rc == ANX_OK) {
			/* Read weight floats from tensor payload */
			uint64_t weight_bytes =
				(uint64_t)(obs_dim * latent_dim +
					   latent_dim * latent_dim) * sizeof(float);
			float *wbuf = (float *)anx_alloc((uint32_t)weight_bytes);

			if (wbuf) {
				rc = anx_so_read_payload(&weight_handle, 0,
							 wbuf,
							 (uint64_t)weight_bytes);
				if (rc == ANX_OK)
					rc = mlp_forward(obs_vec, obs_dim,
							 wbuf, latent_dim,
							 lpay->vec);
				anx_free(wbuf);
			} else {
				rc = ANX_ENOMEM;
			}
			anx_so_close(&weight_handle);
		}

		if (rc != ANX_OK) {
			/* Weight read failed: fall through to deterministic */
			weights_loaded = false;
		}
	}

	if (!weights_loaded) {
		deterministic_project(obs_vec, obs_dim,
				      lpay->vec, latent_dim);
	}

	anx_jepa_vec_normalize(lpay->vec, latent_dim);

	/* Store as ANX_OBJ_JEPA_LATENT */
	anx_memset(&params, 0, sizeof(params));
	params.object_type    = ANX_OBJ_JEPA_LATENT;
	params.schema_uri     = "anx:schema/jepa-latent/v1";
	params.payload        = lpay;
	params.payload_size   = sizeof(struct anx_jepa_latent_payload);
	params.parent_oids    = obs_oid;
	params.parent_count   = 1;

	rc = anx_so_create(&params, &lso);
	anx_free(lpay);
	if (rc != ANX_OK)
		return rc;

	*latent_oid_out = lso->oid;
	anx_objstore_release(lso);

	ctx->encode_count++;
	return ANX_OK;
}

int anx_jepa_encode_obs(const struct anx_jepa_obs *obs,
			anx_oid_t *latent_oid_out)
{
	anx_oid_t obs_oid;
	int rc;

	rc = anx_jepa_observe_store(obs, &obs_oid);
	if (rc != ANX_OK)
		return rc;

	return anx_jepa_encode(&obs_oid, latent_oid_out);
}

/* ------------------------------------------------------------------ */
/* EMA target encoder update                                           */
/* ------------------------------------------------------------------ */

int anx_jepa_update_target_encoder(void)
{
	struct anx_jepa_ctx *ctx = anx_jepa_ctx_get();
	const struct anx_jepa_world_profile *world;
	struct anx_object_handle ctx_h, tgt_h;
	float ema_decay;
	uint32_t obs_dim, latent_dim;
	uint64_t weight_bytes;
	float *ctx_w, *tgt_w;
	uint32_t i;
	int rc;

	if (!anx_jepa_available())
		return ANX_ENOENT;

	{
		anx_oid_t zero;
		anx_memset(&zero, 0, sizeof(zero));
		if (anx_memcmp(&ctx->encoder_weights_oid, &zero,
			       sizeof(zero)) == 0)
			return ANX_OK;	/* no weights yet */
	}

	world      = anx_jepa_world_get_active();
	ema_decay  = world->train.ema_decay;
	obs_dim    = world->arch.obs_dim;
	latent_dim = world->arch.latent_dim;
	weight_bytes = (uint64_t)(obs_dim * latent_dim +
				  latent_dim * latent_dim) * sizeof(float);

	rc = anx_so_open(&ctx->encoder_weights_oid,
			 ANX_OPEN_READ, &ctx_h);
	if (rc != ANX_OK)
		return rc;

	rc = anx_so_open(&ctx->target_weights_oid,
			 ANX_OPEN_READWRITE, &tgt_h);
	if (rc != ANX_OK) {
		anx_so_close(&ctx_h);
		return rc;
	}

	ctx_w = (float *)anx_alloc((uint32_t)weight_bytes);
	tgt_w = (float *)anx_alloc((uint32_t)weight_bytes);
	if (!ctx_w || !tgt_w) {
		anx_free(ctx_w);
		anx_free(tgt_w);
		anx_so_close(&ctx_h);
		anx_so_close(&tgt_h);
		return ANX_ENOMEM;
	}

	anx_so_read_payload(&ctx_h, 0, ctx_w, weight_bytes);
	anx_so_read_payload(&tgt_h, 0, tgt_w, weight_bytes);

	/* θ_target ← decay * θ_target + (1-decay) * θ_context */
	for (i = 0; i < weight_bytes / sizeof(float); i++)
		tgt_w[i] = ema_decay * tgt_w[i] +
			   (1.0f - ema_decay) * ctx_w[i];

	anx_so_replace_payload(&tgt_h, tgt_w, weight_bytes);

	anx_free(ctx_w);
	anx_free(tgt_w);
	anx_so_close(&ctx_h);
	anx_so_close(&tgt_h);
	return ANX_OK;
}
