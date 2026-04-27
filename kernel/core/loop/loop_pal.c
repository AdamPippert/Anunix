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
#include <anx/objstore_disk.h>

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
static bool                   g_pal_disk_ok;  /* set after disk store confirmed */

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

	{
		uint32_t sc = e->session_count;

		anx_spin_unlock(&g_pal_lock);
		kprintf("[pal] updated world=%s sessions=%u\n", world_uri, sc);
		/* Auto-save every 5 organic sessions once disk is confirmed */
		if (g_pal_disk_ok && sc % 5 == 0)
			anx_pal_persist_save();
	}
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

/* ------------------------------------------------------------------ */
/* Persistence: save / load to disk object store                       */
/* ------------------------------------------------------------------ */

/*
 * Fixed OID for the PAL state blob.  Two 64-bit halves spell "PALSTATE"
 * and version 1 so the object is unambiguously identifiable on disk.
 */
#define PAL_DISK_OID_HI  0x50414C5354415445ULL  /* "PALSTATE" */
#define PAL_DISK_OID_LO  0x0000000000000001ULL  /* version 1  */
#define PAL_SAVE_MAGIC   0x50414C31u            /* "PAL1"     */
#define PAL_DISK_TYPE    0xDA0A5E00u            /* arbitrary subsystem tag */

/* On-disk action record (mirrors pal_action_ema without spinlock noise) */
struct pal_disk_action {
	float    avg_energy;
	float    win_rate;
	float    min_energy;
	uint32_t sample_count;
	uint32_t session_count;
};

/* On-disk world record */
struct pal_disk_world {
	char                world_uri[128];
	uint32_t            session_count;
	uint32_t            _pad;
	struct pal_disk_action actions[ANX_MEMORY_ACT_COUNT];
};

/* On-disk header */
struct pal_disk_hdr {
	uint32_t magic;
	uint32_t world_count;
	uint32_t act_count;   /* must equal ANX_MEMORY_ACT_COUNT */
	uint32_t _pad;
};

void anx_pal_persist_save(void)
{
	static uint8_t buf[sizeof(struct pal_disk_hdr) +
			   PAL_MAX_WORLDS * sizeof(struct pal_disk_world)];
	struct pal_disk_hdr   *hdr;
	struct pal_disk_world *worlds;
	anx_oid_t oid;
	uint32_t i, j, count;
	uint32_t total;

	pal_init_once();
	anx_spin_lock(&g_pal_lock);

	count = 0;
	hdr    = (struct pal_disk_hdr *)buf;
	worlds = (struct pal_disk_world *)(buf + sizeof(*hdr));

	for (i = 0; i < g_pal_count; i++) {
		if (!g_pal_worlds[i].valid)
			continue;
		anx_strlcpy(worlds[count].world_uri,
			    g_pal_worlds[i].world_uri,
			    sizeof(worlds[count].world_uri));
		worlds[count].session_count = g_pal_worlds[i].session_count;
		worlds[count]._pad          = 0;
		for (j = 0; j < ANX_MEMORY_ACT_COUNT; j++) {
			worlds[count].actions[j].avg_energy   =
				g_pal_worlds[i].actions[j].avg_energy;
			worlds[count].actions[j].win_rate     =
				g_pal_worlds[i].actions[j].win_rate;
			worlds[count].actions[j].min_energy   =
				g_pal_worlds[i].actions[j].min_energy;
			worlds[count].actions[j].sample_count =
				g_pal_worlds[i].actions[j].sample_count;
			worlds[count].actions[j].session_count =
				g_pal_worlds[i].actions[j].session_count;
		}
		count++;
	}

	anx_spin_unlock(&g_pal_lock);

	hdr->magic       = PAL_SAVE_MAGIC;
	hdr->world_count = count;
	hdr->act_count   = ANX_MEMORY_ACT_COUNT;
	hdr->_pad        = 0;

	total = (uint32_t)(sizeof(*hdr) + count * sizeof(struct pal_disk_world));

	oid.hi = PAL_DISK_OID_HI;
	oid.lo = PAL_DISK_OID_LO;

	/* Overwrite existing record (delete + rewrite) */
	anx_disk_delete_obj(&oid);
	if (anx_disk_write_obj(&oid, PAL_DISK_TYPE, buf, total) == ANX_OK)
		kprintf("[pal] persisted %u worlds to disk\n", count);
	else
		kprintf("[pal] persist save failed\n");
}

void anx_pal_persist_load(void)
{
	static uint8_t buf[sizeof(struct pal_disk_hdr) +
			   PAL_MAX_WORLDS * sizeof(struct pal_disk_world)];
	struct pal_disk_hdr   *hdr;
	struct pal_disk_world *worlds;
	anx_oid_t oid;
	uint32_t actual, obj_type;
	uint32_t i, j;
	int rc;

	g_pal_disk_ok = true;  /* disk is now available regardless of load result */

	oid.hi = PAL_DISK_OID_HI;
	oid.lo = PAL_DISK_OID_LO;

	rc = anx_disk_read_obj(&oid, buf, sizeof(buf), &actual, &obj_type);
	if (rc != ANX_OK) {
		kprintf("[pal] no persisted state (first boot or fresh format)\n");
		return;
	}

	hdr = (struct pal_disk_hdr *)buf;
	if (hdr->magic != PAL_SAVE_MAGIC ||
	    hdr->act_count != ANX_MEMORY_ACT_COUNT ||
	    hdr->world_count > PAL_MAX_WORLDS) {
		kprintf("[pal] persisted state has wrong magic/version — ignoring\n");
		return;
	}

	worlds = (struct pal_disk_world *)(buf + sizeof(*hdr));

	pal_init_once();
	anx_spin_lock(&g_pal_lock);

	for (i = 0; i < hdr->world_count; i++) {
		struct pal_world_entry *e;

		e = pal_find_or_create(worlds[i].world_uri);
		if (!e)
			break;

		e->session_count = worlds[i].session_count;
		for (j = 0; j < ANX_MEMORY_ACT_COUNT; j++) {
			e->actions[j].avg_energy    = worlds[i].actions[j].avg_energy;
			e->actions[j].win_rate      = worlds[i].actions[j].win_rate;
			e->actions[j].min_energy    = worlds[i].actions[j].min_energy;
			e->actions[j].sample_count  = worlds[i].actions[j].sample_count;
			e->actions[j].session_count = worlds[i].actions[j].session_count;
		}
	}

	anx_spin_unlock(&g_pal_lock);
	kprintf("[pal] loaded %u worlds from disk\n", hdr->world_count);
}
