/*
 * loop_memory.c — Phase 5 memory consolidation (RFC-0020).
 *
 * anx_loop_consolidate() converts a session's per-action stats into the
 * anx_loop_memory_payload format and forwards it to anx_pal_memory_update().
 * A trajectory summary is extracted from the session's score history.
 */

#include <anx/loop.h>
#include <anx/memory.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include "loop_internal.h"

/* ------------------------------------------------------------------ */
/* anx_loop_consolidate                                                */
/* ------------------------------------------------------------------ */

int anx_loop_consolidate(anx_oid_t session_oid,
			 const struct anx_loop_session_action_stats *stats,
			 uint32_t stats_count)
{
	struct anx_loop_session      *s;
	struct anx_loop_memory_payload payload;
	uint32_t i, hist_count;
	float    final_energy = 0.0f;
	float    avg_iters    = 0.0f;
	char     world_uri[128];

	if (!stats || stats_count == 0)
		return ANX_EINVAL;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	anx_strlcpy(world_uri, s->world_uri, sizeof(world_uri));
	hist_count  = s->score_hist_count;
	avg_iters   = (float)s->iteration;

	if (hist_count > 0)
		final_energy = s->score_history[hist_count - 1].best_energy;

	/* Build trajectory waypoints from score history */
	anx_memset(&payload, 0, sizeof(payload));
	payload.avg_final_energy = final_energy;
	payload.avg_iters        = avg_iters;

	if (hist_count > 0) {
		uint32_t w;
		for (w = 0; w < ANX_MEMORY_WAYPOINTS; w++) {
			uint32_t idx = (hist_count * w) / ANX_MEMORY_WAYPOINTS;
			if (idx >= hist_count)
				idx = hist_count - 1;
			payload.energy_waypoints[w] =
				s->score_history[idx].best_energy;
		}
	}

	anx_spin_unlock(&g_loop_lock);

	/* Convert per-session action stats → payload per-action format */
	for (i = 0; i < stats_count && i < ANX_MEMORY_ACT_COUNT; i++) {
		const struct anx_loop_session_action_stats *src = &stats[i];
		struct anx_loop_action_stats               *dst = &payload.action_stats[i];

		if (src->total_proposals == 0) {
			dst->total_updates = 0;
			dst->win_rate      = 0.0f;
			dst->avg_energy    = 0.0f;
			dst->min_energy    = 1.0f;
			continue;
		}

		dst->total_updates = 1;  /* one session contributed */
		dst->win_rate      = (float)src->win_count / (float)src->total_proposals;
		dst->avg_energy    = src->energy_sum / (float)src->total_proposals;
		dst->min_energy    = src->min_energy;
	}

	/* Forward to the PAL memory accumulator */
	anx_pal_memory_update(world_uri, &payload);

	kprintf("[memory] consolidated %016llx world=%s iters=%.0f energy=%.4f\n",
		(unsigned long long)session_oid.lo,
		world_uri, avg_iters, final_energy);

	return ANX_OK;
}
