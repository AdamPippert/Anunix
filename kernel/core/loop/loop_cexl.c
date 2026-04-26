/*
 * loop_cexl.c — CounterEXample Learning critic-loop (RFC-0020 Phase 12).
 *
 * Iterates all ANX_OBJ_COUNTEREXAMPLE objects belonging to a given session,
 * looks up the rejected proposal's action_id, accumulates per-action
 * rejection statistics, then calls anx_pal_memory_update() so that future
 * sessions bias away from consistently-rejected actions.
 *
 * The synthesized memory payload encodes:
 *   avg_energy  = mean rejection_score for the action (high = bad)
 *   win_rate    = 0.0  (rejected actions never won)
 *   min_energy  = lowest rejection_score seen (worst-case signal)
 *   total_updates = rejection count (for cold-start gate)
 */

#include <anx/cexl.h>
#include <anx/loop.h>
#include <anx/memory.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Iterator context                                                    */
/* ------------------------------------------------------------------ */

#define CEXL_MAX_SAMPLES 64

struct cexl_ctx {
	anx_oid_t target_session;
	anx_oid_t rejected_oids[CEXL_MAX_SAMPLES];
	float     rejection_scores[CEXL_MAX_SAMPLES];
	uint32_t  n_rejected;
};

static int cexl_iter_cb(struct anx_state_object *obj, void *arg)
{
	struct cexl_ctx *ctx = arg;
	const struct anx_loop_counterexample_payload *pay;

	if (obj->object_type != ANX_OBJ_COUNTEREXAMPLE)
		return 0;
	if (!obj->payload || obj->payload_size < sizeof(*pay))
		return 0;

	pay = (const struct anx_loop_counterexample_payload *)obj->payload;

	if (pay->session_oid.hi != ctx->target_session.hi ||
	    pay->session_oid.lo != ctx->target_session.lo)
		return 0;

	if (ctx->n_rejected < CEXL_MAX_SAMPLES) {
		ctx->rejected_oids[ctx->n_rejected]    = pay->rejected_oid;
		ctx->rejection_scores[ctx->n_rejected] = pay->rejection_score;
		ctx->n_rejected++;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int anx_loop_cexl_process(anx_oid_t session_oid, const char *world_uri)
{
	struct cexl_ctx ctx;
	struct anx_loop_memory_payload payload;
	uint32_t act_count[ANX_MEMORY_ACT_COUNT];
	float    act_score_sum[ANX_MEMORY_ACT_COUNT];
	float    act_min[ANX_MEMORY_ACT_COUNT];
	uint32_t i;

	if (!world_uri)
		return ANX_EINVAL;

	anx_memset(&ctx, 0, sizeof(ctx));
	ctx.target_session = session_oid;

	anx_objstore_iterate(cexl_iter_cb, &ctx);

	if (ctx.n_rejected == 0)
		return ANX_ENOENT;

	/* Accumulate per-action rejection statistics */
	anx_memset(act_count,     0, sizeof(act_count));
	anx_memset(act_score_sum, 0, sizeof(act_score_sum));
	for (i = 0; i < ANX_MEMORY_ACT_COUNT; i++)
		act_min[i] = 1.0f;

	for (i = 0; i < ctx.n_rejected; i++) {
		uint32_t action_id = ANX_MEMORY_ACT_COUNT; /* sentinel = invalid */
		float    score     = ctx.rejection_scores[i];

		if (anx_loop_proposal_get_action_id(ctx.rejected_oids[i],
						     &action_id) != ANX_OK)
			continue;
		if (action_id >= ANX_MEMORY_ACT_COUNT)
			continue;

		act_count[action_id]++;
		act_score_sum[action_id] += score;
		if (score < act_min[action_id])
			act_min[action_id] = score;
	}

	/* Build memory payload from rejection stats */
	anx_memset(&payload, 0, sizeof(payload));
	anx_strlcpy(payload.world_uri, world_uri, sizeof(payload.world_uri));
	/* session_version = 0: CEXL signal, not a full committed session */

	for (i = 0; i < ANX_MEMORY_ACT_COUNT; i++) {
		if (act_count[i] == 0)
			continue;

		payload.action_stats[i].total_updates = act_count[i];
		payload.action_stats[i].avg_energy    =
			act_score_sum[i] / (float)act_count[i];
		payload.action_stats[i].win_rate      = 0.0f;
		payload.action_stats[i].min_energy    = act_min[i];
	}

	anx_pal_memory_update(world_uri, &payload);

	kprintf("[cexl] session %016llx: %u counterexamples → PAL update\n",
		(unsigned long long)session_oid.lo, ctx.n_rejected);

	return ANX_OK;
}
