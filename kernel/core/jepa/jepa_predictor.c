/*
 * jepa_predictor.c — JEPA predictor network.
 *
 * Given a context latent z_t and a discrete action_id, produces a
 * predicted future latent ẑ_{t+1}.  Operates in the same latent space
 * as the encoder so the VICReg loss is directly computable.
 *
 * Forward pass (trained):
 *   concat([z_t, action_embed]) → W1 → relu → W2 → L2-normalize → ẑ_{t+1}
 *
 * Forward pass (no checkpoint):
 *   ẑ_{t+1} = L2-normalize(z_t + action_bias[action_id])
 *   A lightweight identity-plus-bias that produces a distinct prediction
 *   per action without any learned weights.
 *
 * Weight layout in checkpoint tensor payload (after encoder weights):
 *   [enc_W1: obs_dim*latent_dim][enc_W2: latent_dim*latent_dim]
 *   [pred_W1: (latent_dim+action_embed_dim)*latent_dim]
 *   [pred_W2: latent_dim*latent_dim]
 *   [action_embed: action_count*action_embed_dim]
 */

#include "jepa_internal.h"
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/alloc.h>

/* ------------------------------------------------------------------ */
/* Action embedding offsets into checkpoint tensor                     */
/* ------------------------------------------------------------------ */

static uint64_t pred_weight_offset(const struct anx_jepa_world_profile *w)
{
	/* Encoder occupies the first two weight matrices */
	return (uint64_t)(w->arch.obs_dim    * w->arch.latent_dim +
			  w->arch.latent_dim * w->arch.latent_dim) * sizeof(float);
}

static uint64_t action_embed_offset(const struct anx_jepa_world_profile *w)
{
	uint64_t in_dim = w->arch.latent_dim + w->arch.action_embed_dim;
	return pred_weight_offset(w) +
	       (uint64_t)(in_dim * w->arch.latent_dim +
			  w->arch.latent_dim * w->arch.latent_dim) * sizeof(float);
}

/* ------------------------------------------------------------------ */
/* No-checkpoint fallback                                              */
/* ------------------------------------------------------------------ */

static void bias_predict(const float *z_t, uint32_t latent_dim,
			 uint32_t action_id, float *z_pred)
{
	uint32_t i;
	uint32_t seed = action_id * 2654435761u;	/* Knuth multiplicative */

	for (i = 0; i < latent_dim; i++) {
		seed = seed * 1664525u + 1013904223u;
		float bias = ((float)(seed & 0xff) / 128.0f) - 1.0f;
		bias *= 0.05f;				/* small perturbation */
		z_pred[i] = z_t[i] + bias;
	}
}

/* ------------------------------------------------------------------ */
/* Trained forward pass                                                */
/* ------------------------------------------------------------------ */

static int predictor_forward(const float *z_t,
			     uint32_t latent_dim,
			     uint32_t action_embed_dim,
			     const float *action_embed_table,
			     uint32_t action_id,
			     const float *pred_W1,
			     const float *pred_W2,
			     float *z_pred)
{
	uint32_t in_dim = latent_dim + action_embed_dim;
	float *inp, *hidden;
	const float *ae;
	uint32_t i, j;

	inp    = (float *)anx_alloc(in_dim    * sizeof(float));
	hidden = (float *)anx_alloc(latent_dim * sizeof(float));
	if (!inp || !hidden) {
		anx_free(inp);
		anx_free(hidden);
		return ANX_ENOMEM;
	}

	/* Build input: [z_t || action_embedding] */
	anx_memcpy(inp, z_t, latent_dim * sizeof(float));
	ae = action_embed_table + action_id * action_embed_dim;
	anx_memcpy(inp + latent_dim, ae, action_embed_dim * sizeof(float));

	/* hidden = relu(inp @ pred_W1) */
	for (j = 0; j < latent_dim; j++) {
		float acc = 0.0f;

		for (i = 0; i < in_dim; i++)
			acc += inp[i] * pred_W1[i * latent_dim + j];
		hidden[j] = acc > 0.0f ? acc : 0.0f;
	}

	/* z_pred = hidden @ pred_W2 */
	for (j = 0; j < latent_dim; j++) {
		float acc = 0.0f;

		for (i = 0; i < latent_dim; i++)
			acc += hidden[i] * pred_W2[i * latent_dim + j];
		z_pred[j] = acc;
	}

	anx_free(inp);
	anx_free(hidden);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Public: predict                                                     */
/* ------------------------------------------------------------------ */

int anx_jepa_predict(const anx_oid_t *latent_oid, uint32_t action_id,
		     anx_oid_t *pred_latent_oid_out)
{
	struct anx_jepa_ctx *ctx = anx_jepa_ctx_get();
	const struct anx_jepa_world_profile *world;
	struct anx_object_handle lat_h, wgt_h;
	struct anx_jepa_latent_payload src_pay, *pred_pay;
	struct anx_so_create_params params;
	struct anx_state_object *pso;
	bool weights_loaded;
	uint32_t latent_dim, action_count, action_embed_dim;
	int rc;

	if (!latent_oid || !pred_latent_oid_out)
		return ANX_EINVAL;
	if (!anx_jepa_available())
		return ANX_ENOENT;

	world            = anx_jepa_world_get_active();
	latent_dim       = world->arch.latent_dim;
	action_count     = world->arch.action_count;
	action_embed_dim = world->arch.action_embed_dim;

	if (action_id >= action_count)
		return ANX_EINVAL;

	/* Read source latent */
	rc = anx_so_open(latent_oid, ANX_OPEN_READ, &lat_h);
	if (rc != ANX_OK)
		return rc;
	rc = anx_so_read_payload(&lat_h, 0, &src_pay, sizeof(src_pay));
	anx_so_close(&lat_h);
	if (rc != ANX_OK)
		return rc;

	pred_pay = (struct anx_jepa_latent_payload *)
		anx_alloc(sizeof(struct anx_jepa_latent_payload));
	if (!pred_pay)
		return ANX_ENOMEM;

	anx_memset(pred_pay, 0, sizeof(*pred_pay));
	pred_pay->dim            = latent_dim;
	pred_pay->source_obs_oid = src_pay.source_obs_oid;

	/* Probe for weights */
	weights_loaded = false;
	{
		anx_oid_t zero;
		anx_memset(&zero, 0, sizeof(zero));
		weights_loaded = (anx_memcmp(&ctx->predictor_weights_oid,
					     &zero, sizeof(zero)) != 0);
	}

	if (weights_loaded) {
		uint32_t in_dim = latent_dim + action_embed_dim;
		uint64_t pred_w_bytes =
			(uint64_t)(in_dim * latent_dim +
				   latent_dim * latent_dim) * sizeof(float);
		uint64_t ae_bytes =
			(uint64_t)(action_count * action_embed_dim) * sizeof(float);
		float *pred_W1, *pred_W2, *ae_table;
		uint64_t base;

		rc = anx_so_open(&ctx->predictor_weights_oid,
				 ANX_OPEN_READ, &wgt_h);
		if (rc == ANX_OK) {
			pred_W1  = (float *)anx_alloc((uint32_t)pred_w_bytes);
			ae_table = (float *)anx_alloc((uint32_t)ae_bytes);

			if (pred_W1 && ae_table) {
				base = pred_weight_offset(world);
				rc   = anx_so_read_payload(&wgt_h, base,
							   pred_W1,
							   pred_w_bytes);
				if (rc == ANX_OK) {
					base = action_embed_offset(world);
					rc   = anx_so_read_payload(
						&wgt_h, base,
						ae_table, ae_bytes);
				}
				if (rc == ANX_OK) {
					pred_W2 = pred_W1 + in_dim * latent_dim;
					rc = predictor_forward(
						src_pay.vec, latent_dim,
						action_embed_dim, ae_table,
						action_id,
						pred_W1, pred_W2,
						pred_pay->vec);
				}
			} else {
				rc = ANX_ENOMEM;
			}

			anx_free(pred_W1);
			anx_free(ae_table);
			anx_so_close(&wgt_h);
		}

		if (rc != ANX_OK)
			weights_loaded = false;
	}

	if (!weights_loaded)
		bias_predict(src_pay.vec, latent_dim, action_id, pred_pay->vec);

	anx_jepa_vec_normalize(pred_pay->vec, latent_dim);

	anx_memset(&params, 0, sizeof(params));
	params.object_type  = ANX_OBJ_JEPA_LATENT;
	params.schema_uri   = "anx:schema/jepa-latent/v1";
	params.payload      = pred_pay;
	params.payload_size = sizeof(struct anx_jepa_latent_payload);
	params.parent_oids  = latent_oid;
	params.parent_count = 1;

	rc = anx_so_create(&params, &pso);
	anx_free(pred_pay);
	if (rc != ANX_OK)
		return rc;

	*pred_latent_oid_out = pso->oid;
	anx_objstore_release(pso);

	ctx->predict_count++;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Anomaly detection                                                   */
/* ------------------------------------------------------------------ */

float anx_jepa_divergence(const anx_oid_t *predicted_oid,
			  const anx_oid_t *actual_oid)
{
	struct anx_object_handle ph, ah;
	struct anx_jepa_latent_payload ppay, apay;
	float cosine, dist;
	int rc;

	if (!predicted_oid || !actual_oid)
		return -1.0f;
	if (!anx_jepa_available())
		return -1.0f;

	rc = anx_so_open(predicted_oid, ANX_OPEN_READ, &ph);
	if (rc != ANX_OK)
		return -1.0f;
	rc = anx_so_read_payload(&ph, 0, &ppay, sizeof(ppay));
	anx_so_close(&ph);
	if (rc != ANX_OK)
		return -1.0f;

	rc = anx_so_open(actual_oid, ANX_OPEN_READ, &ah);
	if (rc != ANX_OK)
		return -1.0f;
	rc = anx_so_read_payload(&ah, 0, &apay, sizeof(apay));
	anx_so_close(&ah);
	if (rc != ANX_OK)
		return -1.0f;

	if (ppay.dim != apay.dim)
		return -1.0f;

	cosine = anx_jepa_vec_cosine(ppay.vec, apay.vec, ppay.dim);
	dist   = 1.0f - cosine;	/* cosine distance: 0=identical, 2=opposite */

	anx_jepa_ctx_get()->last_divergence = dist;
	return dist;
}
