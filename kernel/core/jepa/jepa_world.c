/*
 * jepa_world.c — Built-in world profiles and data management APIs.
 *
 * Four profiles are registered at init time:
 *
 *   anx:world/os-default     — AI researcher / OS developer workloads.
 *                              Fully implemented collect_obs.
 *
 *   anx:world/cellular       — Phones and mobile devices.
 *                              Stub: collect_obs wired to NULL until a
 *                              cellular platform provides the subsystem
 *                              counters (radio, power, modem, battery).
 *
 *   anx:world/robotics       — Robot platforms with sensor fusion.
 *                              Stub: collect_obs NULL; obs schema defined
 *                              for documentation and tooling.
 *
 *   anx:world/enterprise-it  — Enterprise IT / service mesh / compliance.
 *                              Stub: collect_obs NULL.
 *
 * Platform code may call anx_jepa_world_register() before anx_jepa_init()
 * to add custom profiles or replace stub collectors.
 */

#include "jepa_internal.h"
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/state_object.h>
#include <anx/memplane.h>
#include <anx/string.h>
#include <anx/alloc.h>

/* ------------------------------------------------------------------ */
/* Shared architecture defaults                                        */
/* ------------------------------------------------------------------ */

/*
 * os-default: small transformer stand-in (MLP 2-layer, 256-dim).
 * Sized for real-time inference on AMD XDNA NPU (50 INT8 TOPS).
 * obs_dim is the flattened size of anx_jepa_obs; kept in sync with
 * anx_jepa_obs_linearize() in jepa_encoder.c.
 */
#define OS_DEFAULT_OBS_DIM	\
	(ANX_JEPA_OBS_SCHED_CLASSES + 1 +	/* scheduler + cell count */   \
	 ANX_JEPA_OBS_MEM_TIERS * 2 +		/* decay avg + entry count */  \
	 2 + 2 + 2 + 2)				/* routing, compute, cap, err */

static const struct anx_jepa_arch_config arch_os_default = {
	.obs_dim         = OS_DEFAULT_OBS_DIM,
	.latent_dim      = 256,
	.encoder_layers  = 4,
	.encoder_heads   = 4,
	.predictor_layers = 2,
	.predictor_heads = 4,
	.action_count    = ANX_JEPA_ACT_COUNT,
	.action_embed_dim = 32,
};

static const struct anx_jepa_train_config train_default = {
	.lr              = 1e-4f,
	.ema_decay       = 0.996f,
	.vicreg_lambda_inv = 25.0f,
	.vicreg_lambda_var = 25.0f,
	.vicreg_lambda_cov =  1.0f,
	.batch_size      = 32,
	.warmup_steps    = 100,
};

/* Cellular: smaller model for power-constrained NPU inference */
static const struct anx_jepa_arch_config arch_cellular = {
	.obs_dim         = 24,	/* radio + power + modem + battery + thermal */
	.latent_dim      = 128,
	.encoder_layers  = 2,
	.encoder_heads   = 4,
	.predictor_layers = 2,
	.predictor_heads = 2,
	.action_count    = 8,
	.action_embed_dim = 16,
};

/* Robotics: larger model, richer spatial state */
static const struct anx_jepa_arch_config arch_robotics = {
	.obs_dim         = 96,	/* IMU + lidar + actuators + pose + obstacles */
	.latent_dim      = 512,
	.encoder_layers  = 6,
	.encoder_heads   = 8,
	.predictor_layers = 4,
	.predictor_heads = 8,
	.action_count    = 16,
	.action_embed_dim = 64,
};

/* Enterprise-IT: standard size, longer temporal patterns */
static const struct anx_jepa_arch_config arch_enterprise = {
	.obs_dim         = 48,	/* network + services + sessions + alerts */
	.latent_dim      = 256,
	.encoder_layers  = 4,
	.encoder_heads   = 4,
	.predictor_layers = 2,
	.predictor_heads = 4,
	.action_count    = 12,
	.action_embed_dim = 32,
};

/* ------------------------------------------------------------------ */
/* os-default profile                                                  */
/* ------------------------------------------------------------------ */

static struct anx_jepa_world_profile g_world_os_default = {
	.uri          = "anx:world/os-default",
	.display_name = "OS Default",
	.description  = "AI researcher and OS developer workloads. "
			"Observes scheduler, memory, routing, tensor compute, "
			"capability validation, and security event counters. "
			"Suggested for bare-metal developer machines, "
			"AI research nodes, and general Anunix deployments.",
	.arch         = { 0 },	/* populated below */
	.train        = { 0 },	/* populated below */

	.obs_field_count = OS_DEFAULT_OBS_DIM,
	.obs_field_names = {
		"sched_q_interactive", "sched_q_background",
		"sched_q_latency",     "sched_q_batch",
		"sched_q_validation",  "sched_q_replication",
		"active_cell_count",
		"mem_decay_l0", "mem_entries_l0",
		"mem_decay_l1", "mem_entries_l1",
		"mem_decay_l2", "mem_entries_l2",
		"mem_decay_l3", "mem_entries_l3",
		"mem_decay_l4", "mem_entries_l4",
		"mem_decay_l5", "mem_entries_l5",
		"route_fallbacks", "route_avg_score",
		"tensor_cpu_util", "tensor_npu_util",
		"cap_validation_avg", "cap_failures",
		"error_count", "security_events",
	},

	.action_count = ANX_JEPA_ACT_COUNT,
	.action_names = {
		"idle",
		"route_local",    "route_remote",   "route_fallback",
		"mem_promote",    "mem_demote",      "mem_forget",
		"cell_spawn",     "cell_cancel",
		"cap_validate",   "cap_suspend",
		"security_alert",
	},

	.collect_obs = anx_jepa_obs_collect_os_default,
};

/* ------------------------------------------------------------------ */
/* cellular profile (stub)                                             */
/* ------------------------------------------------------------------ */

static struct anx_jepa_world_profile g_world_cellular = {
	.uri          = "anx:world/cellular",
	.display_name = "Cellular",
	.description  = "Mobile devices. Observes radio, power, battery, "
			"modem, thermals, wake locks, and app lifecycle.",
	.arch         = { 0 },
	.train        = { 0 },

	.obs_field_count = 8,
	.obs_field_names = {
		"radio_rssi",     "power_state",
		"battery_pct",    "modem_state",
		"thermal_level",  "wake_lock_count",
		"fg_app_cpu",     "bg_app_count",
	},

	.action_count = 8,
	.action_names = {
		"idle",
		"radio_policy_aggressive", "radio_policy_conservative",
		"power_doze",              "power_active",
		"app_schedule_fg",         "app_schedule_bg",
		"wake_lock_release",
	},

	.collect_obs = NULL,	/* stub */
};

/* ------------------------------------------------------------------ */
/* robotics profile (stub)                                             */
/* ------------------------------------------------------------------ */

static struct anx_jepa_world_profile g_world_robotics = {
	.uri          = "anx:world/robotics",
	.display_name = "Robotics",
	.description  = "Robot platforms with sensor fusion. Observes IMU "
			"(6-axis accel + gyro), lidar point density, camera "
			"embedding norm, actuator state vector, SLAM pose "
			"confidence, and obstacle map density.",
	.arch         = { 0 },
	.train        = { 0 },

	.obs_field_count = 16,
	.obs_field_names = {
		"imu_accel_x", "imu_accel_y", "imu_accel_z",
		"imu_gyro_x",  "imu_gyro_y",  "imu_gyro_z",
		"lidar_density",   "camera_embed_norm",
		"actuator_vel_norm", "actuator_torque_norm",
		"slam_confidence", "slam_position_err",
		"obstacle_density", "goal_distance",
		"battery_level",  "estop_active",
	},

	.action_count = 16,
	.action_names = {
		"idle",
		"motor_forward", "motor_reverse", "motor_turn_left",
		"motor_turn_right", "motor_stop",
		"nav_set_waypoint", "nav_cancel",
		"sensor_recalibrate",
		"arm_extend", "arm_retract", "arm_grip", "arm_release",
		"estop_assert", "estop_clear",
		"report_status",
	},

	.collect_obs = NULL,	/* stub */
};

/* ------------------------------------------------------------------ */
/* enterprise-it profile (stub)                                        */
/* ------------------------------------------------------------------ */

static struct anx_jepa_world_profile g_world_enterprise = {
	.uri          = "anx:world/enterprise-it",
	.display_name = "Enterprise IT",
	.description  = "Enterprise IT, service mesh, and compliance "
			"workloads. Observes network flow rates, service "
			"health scores, user session counts, alert rates, "
			"compliance event rates, and node resource utilization.",
	.arch         = { 0 },
	.train        = { 0 },

	.obs_field_count = 12,
	.obs_field_names = {
		"net_ingress_mbps",  "net_egress_mbps",
		"svc_health_avg",    "svc_error_rate",
		"active_sessions",   "auth_failure_rate",
		"alert_count",       "compliance_event_count",
		"cpu_util_avg",      "mem_util_avg",
		"disk_io_mbps",      "patch_lag_days",
	},

	.action_count = 12,
	.action_names = {
		"idle",
		"scale_up", "scale_down",
		"net_block_source", "net_allow_source",
		"incident_open", "incident_close",
		"patch_schedule_now", "patch_schedule_defer",
		"session_terminate",
		"compliance_report",
		"audit_log_flush",
	},

	.collect_obs = NULL,	/* stub */
};

/* ------------------------------------------------------------------ */
/* Built-in profile registration                                       */
/* ------------------------------------------------------------------ */

int anx_jepa_world_register_builtins(void)
{
	int rc;

	/* Populate arch/train from constants (avoids designated-init issues
	 * with nested struct literals in C99 kernels). */
	g_world_os_default.arch  = arch_os_default;
	g_world_os_default.train = train_default;

	g_world_cellular.arch    = arch_cellular;
	g_world_cellular.train   = train_default;

	g_world_robotics.arch    = arch_robotics;
	g_world_robotics.train   = train_default;

	g_world_enterprise.arch  = arch_enterprise;
	g_world_enterprise.train = train_default;

	rc = anx_jepa_world_register(&g_world_os_default);
	if (rc != ANX_OK) return rc;

	rc = anx_jepa_world_register(&g_world_cellular);
	if (rc != ANX_OK) return rc;

	rc = anx_jepa_world_register(&g_world_robotics);
	if (rc != ANX_OK) return rc;

	rc = anx_jepa_world_register(&g_world_enterprise);
	if (rc != ANX_OK) return rc;

	kprintf("[jepa] registered worlds: os-default, cellular, robotics, enterprise-it\n");
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Data management                                                     */
/* ------------------------------------------------------------------ */

int anx_jepa_world_ingest(const char *uri,
			  const void *data, uint64_t size,
			  uint32_t obs_count,
			  anx_oid_t *dataset_oid_out)
{
	struct anx_jepa_world_profile *profile;
	struct anx_so_create_params params;
	struct anx_state_object *so;
	int rc;

	if (!uri || !data || size == 0 || !dataset_oid_out)
		return ANX_EINVAL;

	profile = anx_jepa_world_lookup(uri);
	if (!profile)
		return ANX_ENOENT;

	anx_memset(&params, 0, sizeof(params));
	params.object_type    = ANX_OBJ_STRUCTURED_DATA;
	params.schema_uri     = "anx:schema/jepa-dataset/v1";
	params.payload        = data;
	params.payload_size   = size;

	rc = anx_so_create(&params, &so);
	if (rc != ANX_OK)
		return rc;

	/* Register dataset OID with profile */
	anx_spin_lock(&profile->lock);
	if (profile->dataset_count < ANX_JEPA_MAX_DATASETS) {
		profile->datasets[profile->dataset_count++] = so->oid;
		rc = ANX_OK;
	} else {
		rc = ANX_ENOMEM;
	}
	anx_spin_unlock(&profile->lock);

	/* Admit to L3 (long-term semantic tier) for persistence */
	anx_memplane_admit(&so->oid, ANX_ADMIT_LONG_TERM_CANDIDATE, NULL);

	*dataset_oid_out = so->oid;
	anx_objstore_release(so);

	if (rc == ANX_OK)
		kprintf("[jepa] ingested dataset (%llu bytes, %u obs) for %s\n",
			(unsigned long long)size, obs_count, uri);

	return rc;
}

int anx_jepa_world_rebuild(const char *uri, uint32_t max_steps,
			   anx_oid_t *checkpoint_oid_out)
{
	struct anx_jepa_world_profile *profile;
	uint32_t i, step;
	int rc;

	if (!uri || max_steps == 0 || !checkpoint_oid_out)
		return ANX_EINVAL;

	profile = anx_jepa_world_lookup(uri);
	if (!profile)
		return ANX_ENOENT;

	if (profile->dataset_count == 0) {
		kprintf("[jepa] rebuild: no datasets for %s\n", uri);
		return ANX_ENOENT;
	}

	kprintf("[jepa] rebuilding %s from %u dataset(s), max_steps=%u\n",
		uri, profile->dataset_count, max_steps);

	/* Iterate stored dataset OIDs and run training steps */
	for (step = 0; step < max_steps; step++) {
		i = step % profile->dataset_count;
		rc = anx_jepa_train_step(&profile->datasets[i], 1);
		if (rc != ANX_OK) {
			kprintf("[jepa] train step %u failed (%d)\n", step, rc);
			return rc;
		}

		if ((step & 0x1f) == 0)
			kprintf("[jepa] rebuild step %u/%u, loss=%.4f\n",
				step, max_steps,
				anx_jepa_ctx_get()->last_loss);
	}

	/* The active checkpoint is the encoder_weights_oid after training */
	*checkpoint_oid_out = anx_jepa_ctx_get()->encoder_weights_oid;
	kprintf("[jepa] rebuild complete for %s\n", uri);
	return ANX_OK;
}

int anx_jepa_world_activate(const char *uri, const anx_oid_t *checkpoint_oid)
{
	struct anx_jepa_world_profile *profile;

	if (!uri || !checkpoint_oid)
		return ANX_EINVAL;

	profile = anx_jepa_world_lookup(uri);
	if (!profile)
		return ANX_ENOENT;

	anx_spin_lock(&profile->lock);
	profile->active_checkpoint  = *checkpoint_oid;
	profile->checkpoint_loaded  = true;
	anx_spin_unlock(&profile->lock);

	/* If this is the active world, swap weights in the global context */
	if (anx_jepa_world_get_active() == profile)
		return anx_jepa_world_set_active(uri);

	return ANX_OK;
}
