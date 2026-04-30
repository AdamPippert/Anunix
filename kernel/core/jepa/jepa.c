/*
 * jepa.c — JEPA Latent-State Subsystem: init, lifecycle, world registry.
 */

#include "jepa_internal.h"
#include <anx/jepa.h>
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/spinlock.h>
#include <anx/engine.h>
#include <anx/engine_lease.h>
#include <anx/state_object.h>
#include <anx/memplane.h>
#include <anx/sched.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/hwprobe.h>

/* Build-time sanity: obs constants must match subsystem enums. */
_Static_assert(ANX_JEPA_OBS_SCHED_CLASSES == ANX_QUEUE_CLASS_COUNT,
	"ANX_JEPA_OBS_SCHED_CLASSES must equal ANX_QUEUE_CLASS_COUNT");
_Static_assert(ANX_JEPA_OBS_MEM_TIERS == ANX_MEM_TIER_COUNT,
	"ANX_JEPA_OBS_MEM_TIERS must equal ANX_MEM_TIER_COUNT");

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static struct anx_jepa_ctx g_ctx;

/* World profile registry: flat array protected by g_ctx.lock. */
static struct anx_jepa_world_profile *g_worlds[ANX_JEPA_MAX_WORLDS];
static uint32_t g_world_count;

/* Forward declaration — built-in profiles live in jepa_world.c. */
int anx_jepa_world_register_builtins(void);

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static uint32_t jepa_best_caps(void)
{
	struct anx_engine *results[4];
	uint32_t found = 0;

	/* Prefer discrete GPU (training + fast inference) */
	if (anx_engine_find(ANX_ENGINE_DEVICE_SERVICE,
			    ANX_CAP_TENSOR_GPU, results, 4, &found) == ANX_OK
	    && found > 0)
		return ANX_CAP_TENSOR_GPU | ANX_CAP_TENSOR_FP32;

	/* NPU next (efficient inference, Strix Point = 50 INT8 TOPS) */
	found = 0;
	if (anx_engine_find(ANX_ENGINE_DEVICE_SERVICE,
			    ANX_CAP_TENSOR_NPU, results, 4, &found) == ANX_OK
	    && found > 0)
		return ANX_CAP_TENSOR_NPU | ANX_CAP_TENSOR_INT8;

	/* CPU tensor engine always present */
	found = 0;
	if (anx_engine_find(ANX_ENGINE_DETERMINISTIC_TOOL,
			    ANX_CAP_TENSOR_INT8, results, 4, &found) == ANX_OK
	    && found > 0)
		return ANX_CAP_TENSOR_INT8 | ANX_CAP_TENSOR_INT32;

	return 0;
}

static enum anx_jepa_status jepa_status_from_caps(uint32_t caps)
{
	if (caps & ANX_CAP_TENSOR_GPU)
		return ANX_JEPA_READY;
	if (caps & ANX_CAP_TENSOR_NPU)
		return ANX_JEPA_READY;
	if (caps & ANX_CAP_TENSOR_INT8)
		return ANX_JEPA_DEGRADED;	/* CPU only */
	return ANX_JEPA_UNAVAILABLE;
}

/* ------------------------------------------------------------------ */
/* Subsystem lifecycle                                                 */
/* ------------------------------------------------------------------ */

int anx_jepa_init(void)
{
	uint32_t compute_caps;
	uint32_t engine_caps;
	int rc;

	anx_spin_init(&g_ctx.lock);
	g_ctx.status = ANX_JEPA_INITIALIZING;
	g_ctx.mode   = ANX_JEPA_MODE_ONLINE;

	/* Probe compute availability */
	compute_caps = jepa_best_caps();
	g_ctx.status = jepa_status_from_caps(compute_caps);

	if (g_ctx.status == ANX_JEPA_UNAVAILABLE) {
		kprintf("[jepa] no tensor engine available — world model disabled\n");
		return ANX_OK;	/* non-fatal */
	}

	/* Register as a LOCAL_MODEL engine with JEPA capabilities */
	engine_caps = ANX_CAP_JEPA | ANX_CAP_JEPA_TRAIN | compute_caps;
	rc = anx_engine_register("anx-jepa-world-model",
				 ANX_ENGINE_LOCAL_MODEL,
				 engine_caps,
				 &g_ctx.engine);
	if (rc != ANX_OK) {
		kprintf("[jepa] engine registration failed (%d)\n", rc);
		g_ctx.status = ANX_JEPA_UNAVAILABLE;
		return ANX_OK;	/* non-fatal */
	}

	anx_engine_transition(g_ctx.engine, ANX_ENGINE_AVAILABLE);

	/* Register built-in world profiles */
	rc = anx_jepa_world_register_builtins();
	if (rc != ANX_OK) {
		kprintf("[jepa] failed to register built-in worlds (%d)\n", rc);
		anx_engine_unregister(g_ctx.engine);
		g_ctx.status = ANX_JEPA_UNAVAILABLE;
		return ANX_OK;
	}

	/* Select world profile based on hardware inventory. */
	{
		const struct anx_hw_inventory *hw = anx_hwprobe_get();
		const char *world_uri = "anx:world/os-default";

		if (hw && hw->cpu_count > 0) {
			/*
			 * Heuristic: cellular = genuinely constrained device
			 * (≤1 GiB RAM, ≤2 CPUs, no accel) — phones, IoT, SBCs.
			 * enterprise-it = many cores, no dedicated ML accel.
			 * os-default covers everything else, including dev machines
			 * and QEMU test environments with limited memory.
			 * Guard: cpu_count==0 means hwprobe not yet initialised;
			 * default to os-default rather than misclassifying as cellular.
			 */
			bool has_accel = hw->accel_count > 0;

			if (!has_accel && hw->ram_bytes <= (1ULL << 30) &&
			    hw->cpu_count <= 2)
				world_uri = "anx:world/cellular";
			else if (!has_accel && hw->cpu_count >= 16)
				world_uri = "anx:world/enterprise-it";
		}

		rc = anx_jepa_world_set_active(world_uri);
		if (rc != ANX_OK) {
			kprintf("[jepa] failed to activate world %s (%d)\n",
				world_uri, rc);
			anx_engine_unregister(g_ctx.engine);
			g_ctx.status = ANX_JEPA_UNAVAILABLE;
			return ANX_OK;
		}
	}

	/* Register JEPA tool engine (non-fatal) */
	anx_jepa_tool_register();

	/* Register standard agent workflows (non-fatal) */
	anx_jepa_workflow_register();

	const char *compute_desc =
		(compute_caps & ANX_CAP_TENSOR_GPU) ? "GPU" :
		(compute_caps & ANX_CAP_TENSOR_NPU) ? "NPU" : "CPU";

	kprintf("[jepa] latent-state subsystem ready (compute: %s, world: %s)\n",
		compute_desc, g_ctx.active_world->uri);

	return ANX_OK;
}

void anx_jepa_shutdown(void)
{
	anx_spin_lock(&g_ctx.lock);

	if (g_ctx.engine) {
		anx_engine_transition(g_ctx.engine, ANX_ENGINE_DRAINING);
		anx_engine_unregister(g_ctx.engine);
		g_ctx.engine = NULL;
	}

	g_ctx.status       = ANX_JEPA_UNINITIALIZED;
	g_ctx.active_world = NULL;

	anx_spin_unlock(&g_ctx.lock);
}

bool anx_jepa_available(void)
{
	return g_ctx.status == ANX_JEPA_READY ||
	       g_ctx.status == ANX_JEPA_DEGRADED ||
	       g_ctx.status == ANX_JEPA_TRAINING;
}

enum anx_jepa_status anx_jepa_status_get(void)
{
	return g_ctx.status;
}

/* ------------------------------------------------------------------ */
/* World profile registry                                              */
/* ------------------------------------------------------------------ */

int anx_jepa_world_register(struct anx_jepa_world_profile *profile)
{
	uint32_t i;

	if (!profile || profile->uri[0] == '\0')
		return ANX_EINVAL;

	anx_spin_lock(&g_ctx.lock);

	if (g_world_count >= ANX_JEPA_MAX_WORLDS) {
		anx_spin_unlock(&g_ctx.lock);
		return ANX_ENOMEM;
	}

	/* Reject duplicate URIs */
	for (i = 0; i < g_world_count; i++) {
		if (anx_strcmp(g_worlds[i]->uri, profile->uri) == 0) {
			anx_spin_unlock(&g_ctx.lock);
			return ANX_EEXIST;
		}
	}

	anx_spin_init(&profile->lock);
	g_worlds[g_world_count++] = profile;

	anx_spin_unlock(&g_ctx.lock);
	return ANX_OK;
}

struct anx_jepa_world_profile *anx_jepa_world_lookup(const char *uri)
{
	uint32_t i;
	struct anx_jepa_world_profile *found = NULL;

	if (!uri)
		return NULL;

	anx_spin_lock(&g_ctx.lock);
	for (i = 0; i < g_world_count; i++) {
		if (anx_strcmp(g_worlds[i]->uri, uri) == 0) {
			found = g_worlds[i];
			break;
		}
	}
	anx_spin_unlock(&g_ctx.lock);
	return found;
}

int anx_jepa_world_list(const char **uris_out, uint32_t max_count,
			uint32_t *found_out)
{
	uint32_t i, n;

	if (!uris_out || !found_out)
		return ANX_EINVAL;

	anx_spin_lock(&g_ctx.lock);
	n = (g_world_count < max_count) ? g_world_count : max_count;
	for (i = 0; i < n; i++)
		uris_out[i] = g_worlds[i]->uri;
	*found_out = g_world_count;
	anx_spin_unlock(&g_ctx.lock);
	return ANX_OK;
}

int anx_jepa_world_set_active(const char *uri)
{
	struct anx_jepa_world_profile *profile;

	profile = anx_jepa_world_lookup(uri);
	if (!profile)
		return ANX_ENOENT;

	anx_spin_lock(&g_ctx.lock);
	g_ctx.active_world = profile;

	/*
	 * If the new world has a loaded checkpoint, swap the weight OIDs
	 * so encode/predict calls use the correct model.
	 */
	if (profile->checkpoint_loaded) {
		/* Weight OIDs are embedded in the checkpoint tensor payload.
		 * jepa_encoder.c / jepa_predictor.c read g_ctx.*_weights_oid
		 * on each call, so updating here is sufficient. */
		anx_spin_lock(&profile->lock);
		g_ctx.encoder_weights_oid   = profile->active_checkpoint;
		g_ctx.target_weights_oid    = profile->active_checkpoint;
		g_ctx.predictor_weights_oid = profile->active_checkpoint;
		anx_spin_unlock(&profile->lock);
	} else {
		/* No checkpoint: zero the weight OIDs; hooks return neutral. */
		anx_memset(&g_ctx.encoder_weights_oid,   0, sizeof(anx_oid_t));
		anx_memset(&g_ctx.target_weights_oid,    0, sizeof(anx_oid_t));
		anx_memset(&g_ctx.predictor_weights_oid, 0, sizeof(anx_oid_t));
	}

	anx_spin_unlock(&g_ctx.lock);
	return ANX_OK;
}

const struct anx_jepa_world_profile *anx_jepa_world_get_active(void)
{
	return g_ctx.active_world;
}

/* ------------------------------------------------------------------ */
/* Context accessor used by other jepa_*.c files                       */
/* ------------------------------------------------------------------ */

struct anx_jepa_ctx *anx_jepa_ctx_get(void)
{
	return &g_ctx;
}
