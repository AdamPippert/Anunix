/*
 * rlm_pal.c — RLM → PAL score feedback bridge (RFC-0020 Phase 16).
 *
 * Maps a completed rollout's trajectory score into the PAL cross-session
 * accumulator so future IBAL sessions bias away from action paths that
 * produce poor rollout outcomes.
 *
 * Score → energy: energy = 1.0 - clamp(score / 100.0, 0.0, 1.0)
 * (score=100 → energy=0.0 optimal;  score=0 → energy=1.0 worst)
 *
 * Rollouts with energy ≥ 0.7 (score < 30) also emit a counterexample
 * State Object so the CEXL critic can refine per-action priors.
 */

#include <anx/types.h>
#include <anx/rlm.h>
#include <anx/loop.h>
#include <anx/memory.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define RLM_PAL_BAD_ENERGY_THRESH  0.7f

static float score_to_energy(int32_t score)
{
	if (score >= 100)
		return 0.0f;
	if (score <= 0)
		return 1.0f;
	return 1.0f - (float)score / 100.0f;
}

int anx_rlm_pal_feedback(struct anx_rlm_rollout *r,
			  const char *world_uri,
			  uint32_t action_id)
{
	struct anx_loop_memory_payload payload;
	float energy;

	if (!r || !world_uri)
		return ANX_EINVAL;
	if (r->status != ANX_RLM_COMPLETED)
		return ANX_EPERM;
	if (action_id >= ANX_MEMORY_ACT_COUNT)
		return ANX_EINVAL;

	energy = score_to_energy(r->score);

	anx_memset(&payload, 0, sizeof(payload));
	anx_strlcpy(payload.world_uri, world_uri, sizeof(payload.world_uri));
	payload.sessions_committed = 1;
	payload.avg_final_energy   = energy;

	payload.action_stats[action_id].total_updates = 1;
	payload.action_stats[action_id].avg_energy    = energy;
	payload.action_stats[action_id].win_rate      = (energy < 0.5f) ? 1.0f : 0.0f;
	payload.action_stats[action_id].min_energy    = energy;

	anx_pal_memory_update(world_uri, &payload);

	kprintf("[rlm-pal] world=%s action=%u score=%d energy=%u.%02u\n",
		world_uri, action_id, (int)r->score,
		(unsigned int)energy,
		(unsigned int)((energy - (float)(unsigned int)energy) * 100.0f
			       + 0.5f));

	if (energy >= RLM_PAL_BAD_ENERGY_THRESH) {
		struct anx_state_object *cx_obj;
		struct anx_so_create_params params;
		struct anx_loop_counterexample_payload cx;

		anx_memset(&cx, 0, sizeof(cx));
		cx.session_oid     = *(const anx_oid_t *)&r->root_cid;
		cx.rejected_oid    = r->response_oid;
		cx.rejection_score = energy;
		anx_strlcpy(cx.context_summary, world_uri,
			     sizeof(cx.context_summary));

		anx_memset(&params, 0, sizeof(params));
		params.object_type  = ANX_OBJ_COUNTEREXAMPLE;
		params.payload      = &cx;
		params.payload_size = sizeof(cx);

		if (anx_so_create(&params, &cx_obj) == ANX_OK) {
			anx_objstore_release(cx_obj);
			kprintf("[rlm-pal] counterexample stored (score<30)\n");
		}
	}

	return ANX_OK;
}
