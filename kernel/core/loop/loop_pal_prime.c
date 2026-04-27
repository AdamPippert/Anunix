/*
 * loop_pal_prime.c — PAL warm-start priming (RFC-0020 Phase 14).
 *
 * Provides three priming paths that pre-seed the PAL cross-session
 * accumulator so the cold-start gate is already crossed on first use:
 *
 *   anx_pal_prime_hardware()   — called at boot after driver probe;
 *                                 detects NPU, GPU, WiFi, storage and
 *                                 boosts the matching workflow categories.
 *
 *   anx_pal_prime_kickstart()  — called per workflow URI when a kickstart
 *                                 [workflows] section is processed; records
 *                                 a synthetic win for that URI's category.
 *
 *   anx_pal_prime_install()    — called at end of installer; writes three
 *                                 synthetic sessions so fresh installs boot
 *                                 with an immediately active prior.
 *
 * All three ultimately call anx_pal_memory_update() with total_updates=3
 * so the PAL_COLD_GATE (3 sessions) is pre-satisfied.
 */

#include <anx/types.h>
#include <anx/memory.h>
#include <anx/loop.h>
#include <anx/jepa.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/xdna.h>
#include <anx/fb.h>
#include <anx/blk.h>
#include <anx/mt7925.h>
#include <anx/driver_table.h>

#define SEARCH_WORLD  "anx:world/workflow-search/v1"
#define AGENT_WORLD   "anx:world/os-default"

/* ------------------------------------------------------------------ */
/* Internal: emit one priming record                                   */
/* ------------------------------------------------------------------ */

/*
 * Emit a synthetic memory record that crosses the cold-start gate.
 * avg_energy: 0.0 = strongly preferred, 1.0 = strongly avoided.
 * win_rate:   0.0–1.0 fraction of iterations that selected this action.
 */
static void prime_action(const char *world, uint32_t action_id,
			  float avg_energy, float win_rate)
{
	struct anx_loop_memory_payload mp;
	uint32_t i;

	anx_memset(&mp, 0, sizeof(mp));
	anx_strlcpy(mp.world_uri, world, sizeof(mp.world_uri));

	mp.action_stats[action_id].total_updates = 3; /* cross cold-start gate */
	mp.action_stats[action_id].avg_energy    = avg_energy;
	mp.action_stats[action_id].win_rate      = win_rate;
	mp.action_stats[action_id].min_energy    = avg_energy * 0.5f;

	/*
	 * Call three times so the EWA accumulator folds in the signal
	 * at the same weight as three organic sessions.
	 */
	for (i = 0; i < 3; i++)
		anx_pal_memory_update(world, &mp);
}

/* ------------------------------------------------------------------ */
/* URI → JEPA action mapping (mirrors wm_search.c's uri_to_action)    */
/* ------------------------------------------------------------------ */

static uint32_t uri_to_action(const char *uri)
{
	const char *p;

	if (!uri) return ANX_JEPA_ACT_IDLE;

	/* simple substring walk without anx_strstr */
#define CONTAINS(needle) \
	({ const char *h = uri; bool found = false; \
	   for (; *h && !found; h++) { \
	       const char *hh = h; const char *nn = (needle); \
	       while (*nn && *hh == *nn) { hh++; nn++; } \
	       if (!*nn) found = true; \
	   } found; })

	if (CONTAINS("ibal") || CONTAINS("loop") || CONTAINS("memory") ||
	    CONTAINS("pal"))
		return ANX_JEPA_ACT_MEM_PROMOTE;
	if (CONTAINS("browser") || CONTAINS("fetch") || CONTAINS("remote"))
		return ANX_JEPA_ACT_ROUTE_REMOTE;
	if (CONTAINS("model") || CONTAINS("infer") || CONTAINS("agent") ||
	    CONTAINS("rag"))
		return ANX_JEPA_ACT_CAP_VALIDATE;
	if (CONTAINS("cell") || CONTAINS("system") || CONTAINS("vm") ||
	    CONTAINS("spawn"))
		return ANX_JEPA_ACT_CELL_SPAWN;
	if (CONTAINS("route") || CONTAINS("net") || CONTAINS("local"))
		return ANX_JEPA_ACT_ROUTE_LOCAL;

#undef CONTAINS
	(void)p;
	return ANX_JEPA_ACT_IDLE;
}

/* ------------------------------------------------------------------ */
/* Public: hardware-profile priming                                    */
/* ------------------------------------------------------------------ */

void anx_pal_prime_hardware(void)
{
	bool has_npu     = anx_xdna_present();
	bool has_fb      = anx_fb_available();
	bool has_wifi    = (anx_net_probe_ok() &&
			    anx_mt7925_state() >= MT7925_STATE_FW_UP);
	bool has_storage = anx_blk_ready();

	kprintf("[pal-prime] hw: npu=%d fb=%d wifi=%d storage=%d\n",
		(int)has_npu, (int)has_fb, (int)has_wifi, (int)has_storage);

	/*
	 * NPU / GPU → AI Loop and model inference workflows are
	 * immediately relevant.  Give them a strong preference (low energy).
	 */
	if (has_npu) {
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_MEM_PROMOTE, 0.1f, 0.9f);
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_CAP_VALIDATE, 0.15f, 0.8f);
		prime_action(AGENT_WORLD,  ANX_JEPA_ACT_MEM_PROMOTE,  0.1f, 0.9f);
	}

	/*
	 * Framebuffer → visual / desktop workflows relevant.
	 * No framebuffer → system/headless workflows preferred.
	 */
	if (has_fb) {
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_CELL_SPAWN, 0.2f, 0.7f);
	} else {
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_ROUTE_LOCAL, 0.2f, 0.7f);
		prime_action(AGENT_WORLD,  ANX_JEPA_ACT_ROUTE_LOCAL,  0.2f, 0.7f);
	}

	/*
	 * WiFi present → network/browser workflows immediately useful.
	 */
	if (has_wifi) {
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_ROUTE_REMOTE, 0.2f, 0.7f);
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_ROUTE_LOCAL,  0.3f, 0.6f);
	}

	/*
	 * Storage → object/file workflows available.
	 */
	if (has_storage) {
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_MEM_DEMOTE, 0.3f, 0.5f);
	}
}

/* ------------------------------------------------------------------ */
/* Public: kickstart intent priming                                    */
/* ------------------------------------------------------------------ */

void anx_pal_prime_kickstart(const char *workflow_uri)
{
	uint32_t act = uri_to_action(workflow_uri);

	kprintf("[pal-prime] kickstart uri=%s → action=%u\n",
		workflow_uri, act);

	/*
	 * A workflow explicitly loaded or autorun in the kickstart is a
	 * strong intent signal — low energy, high win rate.
	 */
	prime_action(SEARCH_WORLD, act, 0.05f, 1.0f);
	prime_action(AGENT_WORLD,  act, 0.05f, 1.0f);
}

/* ------------------------------------------------------------------ */
/* Public: install-time priming                                        */
/* ------------------------------------------------------------------ */

/*
 * Called at end of install (both interactive and automated).
 * Writes to the in-memory PAL accumulator; once PAL persistence is
 * added, this state will survive into the first real boot session.
 */
void anx_pal_prime_install(uint32_t hardware_flags)
{
	kprintf("[pal-prime] install: hw_flags=0x%x\n", hardware_flags);

	if (hardware_flags & (ANX_PAL_PRIME_HW_NPU | ANX_PAL_PRIME_HW_GPU)) {
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_MEM_PROMOTE,  0.1f, 0.9f);
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_CAP_VALIDATE, 0.15f, 0.8f);
	}
	if (hardware_flags & ANX_PAL_PRIME_HW_WIFI) {
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_ROUTE_REMOTE, 0.2f, 0.7f);
	}
	if (hardware_flags & ANX_PAL_PRIME_HW_ETHERNET) {
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_ROUTE_LOCAL,  0.2f, 0.7f);
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_ROUTE_REMOTE, 0.25f, 0.65f);
	}
	if (hardware_flags & ANX_PAL_PRIME_HW_STORAGE) {
		prime_action(SEARCH_WORLD, ANX_JEPA_ACT_MEM_DEMOTE, 0.3f, 0.5f);
	}
}
