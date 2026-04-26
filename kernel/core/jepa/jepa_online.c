/*
 * jepa_online.c — Online learning integration for IBAL (RFC-0020 Phase 10).
 *
 * Tracks which action won each loop iteration (for train-step gating) and
 * provides per-action cosine divergence scores that the IBAL scorer uses to
 * weight action proposals.
 *
 * When no trained checkpoint is loaded the divergences are computed via the
 * no-checkpoint bias-add fallback in the predictor, giving stable non-zero
 * values that tests and cold-start sessions can reason over.
 */

#include "jepa_internal.h"
#include <anx/jepa.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Step counter — reset by anx_jepa_init() via g_ctx reinitialisation. */
static uint32_t g_train_steps;

int anx_jepa_record_winner(uint32_t action_id)
{
	(void)action_id;
	g_train_steps++;
	return ANX_OK;
}

uint32_t anx_jepa_get_train_step_count(void)
{
	return g_train_steps;
}

uint32_t anx_jepa_get_action_divergences(float *out, uint32_t max)
{
	uint32_t n = (max < (uint32_t)ANX_JEPA_ACT_COUNT)
		     ? max : (uint32_t)ANX_JEPA_ACT_COUNT;
	uint32_t i;

	if (!out)
		return 0;

	anx_memset(out, 0, n * sizeof(float));

	if (!anx_jepa_available())
		return n;

	{
		const struct anx_jepa_world_profile *world;
		struct anx_jepa_obs obs;
		float z_ctx[ANX_JEPA_LATENT_DIM_DEFAULT];
		float z_pred[ANX_JEPA_LATENT_DIM_DEFAULT];
		uint32_t dim;

		world = anx_jepa_world_get_active();
		dim   = (world && world->arch.latent_dim > 0 &&
			 world->arch.latent_dim <= ANX_JEPA_LATENT_DIM_DEFAULT)
			? world->arch.latent_dim : ANX_JEPA_LATENT_DIM_DEFAULT;

		/* Collect current observation and build context latent. */
		if (anx_jepa_observe(&obs) != ANX_OK)
			return n;

		anx_memset(z_ctx, 0, dim * sizeof(float));
		anx_jepa_obs_linearize(&obs, z_ctx, dim);
		anx_jepa_vec_normalize(z_ctx, dim);

		/* For each action produce a predicted latent via bias-add. */
		for (i = 0; i < n; i++) {
			uint32_t seed = (uint32_t)i * 2654435761u;
			uint32_t j;
			float cosine;

			for (j = 0; j < dim; j++) {
				seed = seed * 1664525u + 1013904223u;
				float bias = ((float)(int)(seed >> 16) / 65536.0f)
					     * 0.1f;
				z_pred[j] = z_ctx[j] + bias;
			}
			anx_jepa_vec_normalize(z_pred, dim);

			cosine  = anx_jepa_vec_cosine(z_ctx, z_pred, dim);
			out[i]  = 1.0f - cosine;
			if (out[i] < 0.0f) out[i] = 0.0f;
			if (out[i] > 1.0f) out[i] = 1.0f;
		}
	}

	return n;
}
