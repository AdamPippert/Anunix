/*
 * loop_memory.c — Post-commit memory consolidation (RFC-0020 Phase 5).
 *
 * After a session commits, anx_loop_consolidate() extracts 8 energy
 * waypoints from the session's score_history ring, bundles them with
 * the per-action win-rate and energy statistics accumulated during the
 * run, and creates an ANX_OBJ_MEMORY_CONSOLIDATION object.  It then
 * calls anx_pal_memory_update() to feed the result into the PAL
 * cross-session accumulator so that future sessions of the same world
 * benefit from learned action penalties.
 */

#include <anx/loop.h>
#include <anx/state_object.h>
#include <anx/memory.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include "loop_internal.h"

/* ------------------------------------------------------------------ */
/* Waypoint extraction                                                 */
/* ------------------------------------------------------------------ */

static void extract_waypoints(const struct anx_loop_iter_score *history,
			       uint32_t count,
			       float waypoints[ANX_MEMORY_WAYPOINTS])
{
	uint32_t j, idx;

	anx_memset(waypoints, 0, ANX_MEMORY_WAYPOINTS * sizeof(float));
	if (count == 0)
		return;

	for (j = 0; j < ANX_MEMORY_WAYPOINTS; j++) {
		if (ANX_MEMORY_WAYPOINTS > 1)
			idx = (j * (count - 1)) / (ANX_MEMORY_WAYPOINTS - 1);
		else
			idx = 0;
		if (idx >= count)
			idx = count - 1;
		waypoints[j] = history[idx].best_energy;
	}
}

/* ------------------------------------------------------------------ */
/* anx_loop_consolidate                                                */
/* ------------------------------------------------------------------ */

int anx_loop_consolidate(anx_oid_t session_oid,
			 const struct anx_loop_session_action_stats *action_stats,
			 uint32_t action_count)
{
	struct anx_loop_session    *s;
	struct anx_loop_memory_payload payload;
	struct anx_so_create_params cp;
	struct anx_state_object    *obj;
	uint32_t                    i;
	int                         rc;

	if (!action_stats)
		return ANX_EINVAL;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	anx_memset(&payload, 0, sizeof(payload));
	anx_strlcpy(payload.world_uri, s->world_uri, sizeof(payload.world_uri));
	payload.session_version = 1;

	extract_waypoints(s->score_history, s->score_hist_count,
			  payload.energy_waypoints);

	payload.avg_final_energy = (s->score_hist_count > 0)
		? s->score_history[s->score_hist_count - 1].best_energy
		: 0.0f;
	payload.avg_iters = (float)s->iteration;

	if (s->status == ANX_LOOP_ABORTED)
		payload.sessions_aborted  = 1;
	else
		payload.sessions_committed = 1;

	anx_spin_unlock(&g_loop_lock);

	/* Fill per-action stats */
	i = (action_count < ANX_MEMORY_ACT_COUNT)
		? action_count : ANX_MEMORY_ACT_COUNT;
	while (i-- > 0) {
		const struct anx_loop_session_action_stats *src = &action_stats[i];
		struct anx_loop_action_stats               *dst = &payload.action_stats[i];

		dst->total_updates = src->total_proposals;
		dst->min_energy    = src->min_energy;
		if (src->total_proposals > 0) {
			dst->avg_energy = src->energy_sum / (float)src->total_proposals;
			dst->win_rate   = (float)src->win_count /
					  (float)src->total_proposals;
		} else {
			dst->avg_energy = 0.5f;
			dst->win_rate   = 0.0f;
		}
	}

	/* Store as ANX_OBJ_MEMORY_CONSOLIDATION */
	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_MEMORY_CONSOLIDATION;
	cp.schema_uri     = "anx:schema/loop/memory/v1";
	cp.schema_version = "1";
	cp.payload        = &payload;
	cp.payload_size   = sizeof(payload);

	rc = anx_so_create(&cp, &obj);
	if (rc != ANX_OK) {
		kprintf("[memory] consolidation object create failed (%d)\n", rc);
		return rc;
	}

	/* Update the PAL cross-session accumulator */
	anx_pal_memory_update(payload.world_uri, &payload);

	kprintf("[memory] consolidated %016llx world=%s iters=%.0f energy=%.4f\n",
		(unsigned long long)session_oid.lo,
		payload.world_uri,
		payload.avg_iters,
		payload.avg_final_energy);

	return ANX_OK;
}
