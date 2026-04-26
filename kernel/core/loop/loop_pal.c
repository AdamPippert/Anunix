/*
 * loop_pal.c — PAL (Policy and Action Learning) cross-session accumulator.
 *
 * RFC-0020 Phase 12: real implementation replacing the Phase 5 stub.
 *
 * Maintains per-world EMA statistics over per-action energy and win-rate.
 * anx_pal_memory_update() is called by loop_memory.c after each session.
 * anx_pal_action_prior() is called at the start of each iteration to bias
 * proposal generation toward historically lower-cost actions.
 *
 * EMA parameters:
 *   α = 0.7  (weight of new session data)
 *   1-α = 0.3 (retention of historical data)
 * Cold-start gate = 3 sessions (returns neutral 0.5 before this).
 */

#include <anx/memory.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define PAL_MAX_WORLDS   8
#define PAL_COLD_GATE    3	/* min sessions before biasing */
#define PAL_ALPHA        0.7f	/* new-data weight */

/* ------------------------------------------------------------------ */
/* Per-world accumulator                                               */
/* ------------------------------------------------------------------ */

struct pal_action_ema {
	float    avg_energy;
	float    win_rate;
	float    min_energy;
	uint32_t sample_count;
	uint32_t session_count;
};

struct pal_world_entry {
	char                world_uri[128];
	bool                valid;
	uint32_t            session_count;
	struct pal_action_ema actions[ANX_MEMORY_ACT_COUNT];
};

static struct pal_world_entry g_pal_worlds[PAL_MAX_WORLDS];
static uint32_t               g_pal_count;
static struct anx_spinlock    g_pal_lock;
static bool                   g_pal_ready;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void pal_init_once(void)
{
	if (g_pal_ready)
		return;
	anx_spin_init(&g_pal_lock);
	anx_memset(g_pal_worlds, 0, sizeof(g_pal_worlds));
	g_pal_count = 0;
	g_pal_ready = true;
}

static struct pal_world_entry *pal_find_or_create(const char *uri)
{
	uint32_t i;

	if (!uri || !uri[0])
		return NULL;

	for (i = 0; i < g_pal_count; i++) {
		if (g_pal_worlds[i].valid &&
		    anx_strcmp(g_pal_worlds[i].world_uri, uri) == 0)
			return &g_pal_worlds[i];
	}

	if (g_pal_count >= PAL_MAX_WORLDS)
		return NULL;

	{
		struct pal_world_entry *e = &g_pal_worlds[g_pal_count++];

		anx_memset(e, 0, sizeof(*e));
		anx_strlcpy(e->world_uri, uri, sizeof(e->world_uri));
		/* Initialise min_energy to 1.0 (will decay toward real min) */
		for (i = 0; i < ANX_MEMORY_ACT_COUNT; i++)
			e->actions[i].min_energy = 1.0f;
		e->valid = true;
		return e;
	}
}

/* ------------------------------------------------------------------ */
/* Public: update                                                      */
/* ------------------------------------------------------------------ */

void anx_pal_memory_update(const char *world_uri,
			    const struct anx_loop_memory_payload *payload)
{
	struct pal_world_entry *e;
	uint32_t i;

	if (!world_uri || !payload)
		return;

	pal_init_once();
	anx_spin_lock(&g_pal_lock);

	e = pal_find_or_create(world_uri);
	if (!e) {
		anx_spin_unlock(&g_pal_lock);
		kprintf("[pal] world table full, dropping update for %s\n",
			world_uri);
		return;
	}

	e->session_count++;

	for (i = 0; i < ANX_MEMORY_ACT_COUNT; i++) {
		const struct anx_loop_action_stats *src = &payload->action_stats[i];
		struct pal_action_ema              *dst = &e->actions[i];

		if (src->total_updates == 0)
			continue;

		dst->sample_count  += src->total_updates;
		dst->session_count++;

		if (e->session_count == 1) {
			/* Cold-start: take first session as-is */
			dst->avg_energy = src->avg_energy;
			dst->win_rate   = src->win_rate;
			dst->min_energy = src->min_energy;
		} else {
			/* EMA update */
			dst->avg_energy = PAL_ALPHA * src->avg_energy +
					  (1.0f - PAL_ALPHA) * dst->avg_energy;
			dst->win_rate   = PAL_ALPHA * src->win_rate +
					  (1.0f - PAL_ALPHA) * dst->win_rate;
			if (src->min_energy < dst->min_energy)
				dst->min_energy = src->min_energy;
		}
	}

	anx_spin_unlock(&g_pal_lock);

	kprintf("[pal] updated world=%s sessions=%u\n",
		world_uri, e->session_count);
}

/* ------------------------------------------------------------------ */
/* Public: query                                                       */
/* ------------------------------------------------------------------ */

float anx_pal_action_prior(const char *world_uri, uint32_t action_id)
{
	const struct pal_world_entry *e;
	float prior;
	uint32_t i;

	if (!world_uri || action_id >= ANX_MEMORY_ACT_COUNT)
		return 0.5f;

	pal_init_once();
	anx_spin_lock(&g_pal_lock);

	e = NULL;
	for (i = 0; i < g_pal_count; i++) {
		if (g_pal_worlds[i].valid &&
		    anx_strcmp(g_pal_worlds[i].world_uri, world_uri) == 0) {
			e = &g_pal_worlds[i];
			break;
		}
	}

	if (!e || e->session_count < PAL_COLD_GATE) {
		anx_spin_unlock(&g_pal_lock);
		return 0.5f;
	}

	/* Action never observed → neutral prior (not spuriously zero) */
	if (e->actions[action_id].session_count == 0) {
		anx_spin_unlock(&g_pal_lock);
		return 0.5f;
	}

	prior = e->actions[action_id].avg_energy;
	anx_spin_unlock(&g_pal_lock);

	if (prior < 0.0f) prior = 0.0f;
	if (prior > 1.0f) prior = 1.0f;
	return prior;
}

int anx_pal_stats_get(const char *world_uri,
		      struct anx_pal_action_info *stats_out,
		      uint32_t max_actions, uint32_t *count_out)
{
	const struct pal_world_entry *e;
	uint32_t n, i;

	if (!world_uri || !stats_out || !count_out)
		return ANX_EINVAL;

	pal_init_once();
	anx_spin_lock(&g_pal_lock);

	e = NULL;
	for (i = 0; i < g_pal_count; i++) {
		if (g_pal_worlds[i].valid &&
		    anx_strcmp(g_pal_worlds[i].world_uri, world_uri) == 0) {
			e = &g_pal_worlds[i];
			break;
		}
	}

	if (!e) {
		anx_spin_unlock(&g_pal_lock);
		*count_out = 0;
		return ANX_ENOENT;
	}

	n = (max_actions < ANX_MEMORY_ACT_COUNT)
		? max_actions : ANX_MEMORY_ACT_COUNT;

	for (i = 0; i < n; i++) {
		stats_out[i].avg_energy   = e->actions[i].avg_energy;
		stats_out[i].win_rate     = e->actions[i].win_rate;
		stats_out[i].min_energy   = e->actions[i].min_energy;
		stats_out[i].sample_count = e->actions[i].sample_count;
		stats_out[i].session_count = e->actions[i].session_count;
	}
	*count_out = n;

	anx_spin_unlock(&g_pal_lock);
	return ANX_OK;
}

uint32_t anx_pal_session_count(const char *world_uri)
{
	uint32_t i, cnt = 0;

	if (!world_uri)
		return 0;

	pal_init_once();
	anx_spin_lock(&g_pal_lock);

	for (i = 0; i < g_pal_count; i++) {
		if (g_pal_worlds[i].valid &&
		    anx_strcmp(g_pal_worlds[i].world_uri, world_uri) == 0) {
			cnt = g_pal_worlds[i].session_count;
			break;
		}
	}

	anx_spin_unlock(&g_pal_lock);
	return cnt;
}
