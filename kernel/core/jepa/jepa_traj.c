/*
 * jepa_traj.c — Trajectory ring buffer and dataset export.
 *
 * Accumulates (obs_at_t, action_id) entries as the IBAL runs.  Consecutive
 * entries with the same world_uri form (s_t, a_t, s_{t+1}) triples that can
 * be used to train JEPA predictors on any domain — OS, robotics, cellular.
 *
 * anx_jepa_export_trajectory() serialises the buffer as a self-describing
 * binary blob (anx_jepa_traj_header + entry array) suitable for transfer to
 * RobotRock or any other domain-specific training pipeline.
 */

#include "jepa_internal.h"
#include <anx/jepa.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/arch.h>		/* arch_time_now() */

#define TRAJ_VERSION	1u

/* ------------------------------------------------------------------ */
/* Ring buffer                                                          */
/* ------------------------------------------------------------------ */

static struct anx_jepa_traj_entry g_ring[ANX_JEPA_TRAJ_RING_SIZE];
static uint32_t g_head;		/* next write slot (wraps) */
static uint32_t g_count;	/* entries written, capped at RING_SIZE */

/* ------------------------------------------------------------------ */
/* Internal: record one entry                                          */
/* ------------------------------------------------------------------ */

void anx_jepa_traj_record(const float *obs_vec, uint32_t obs_dim,
			  uint32_t action_id, const char *world_uri)
{
	struct anx_jepa_traj_entry *e;
	uint32_t copy_dim;

	if (!obs_vec || obs_dim == 0)
		return;

	e = &g_ring[g_head];
	anx_memset(e, 0, sizeof(*e));

	copy_dim = obs_dim < ANX_JEPA_MAX_OBS_FIELDS
		   ? obs_dim : ANX_JEPA_MAX_OBS_FIELDS;
	anx_memcpy(e->obs_vec, obs_vec, copy_dim * sizeof(float));
	e->obs_dim    = copy_dim;
	e->action_id  = action_id;

	if (world_uri)
		anx_strlcpy(e->world_uri, world_uri, ANX_JEPA_WORLD_URI_MAX);

	e->timestamp_ns = (uint64_t)arch_time_now();

	g_head = (g_head + 1) % ANX_JEPA_TRAJ_RING_SIZE;
	if (g_count < ANX_JEPA_TRAJ_RING_SIZE)
		g_count++;
}

/* ------------------------------------------------------------------ */
/* Public: ingest (called from modules outside core/jepa/)            */
/* ------------------------------------------------------------------ */

int anx_jepa_traj_ingest(const struct anx_jepa_obs *obs, uint32_t action_id,
			  const char *world_uri)
{
	const struct anx_jepa_world_profile *world;
	float obs_vec[ANX_JEPA_MAX_OBS_FIELDS];
	uint32_t dim;

	if (!obs)
		return ANX_EINVAL;
	if (!anx_jepa_available())
		return ANX_OK;	/* non-fatal no-op */

	world = anx_jepa_world_get_active();
	dim   = world ? world->arch.obs_dim : 0;
	if (dim > ANX_JEPA_MAX_OBS_FIELDS)
		dim = ANX_JEPA_MAX_OBS_FIELDS;

	anx_memset(obs_vec, 0, sizeof(obs_vec));
	if (dim > 0)
		anx_jepa_obs_linearize(obs, obs_vec, dim);

	anx_jepa_traj_record(obs_vec, dim, action_id, world_uri);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Public: export                                                      */
/* ------------------------------------------------------------------ */

int anx_jepa_export_trajectory(void *buf_out, uint32_t buf_size,
				uint32_t *bytes_written_out)
{
	const struct anx_jepa_world_profile *world;
	struct anx_jepa_traj_header         hdr;
	uint32_t need, i, src;
	uint8_t *p;

	if (!buf_out || !bytes_written_out)
		return ANX_EINVAL;

	*bytes_written_out = 0;

	if (g_count == 0)
		return ANX_ENOENT;

	need = (uint32_t)sizeof(struct anx_jepa_traj_header) +
	       g_count * (uint32_t)sizeof(struct anx_jepa_traj_entry);

	if (buf_size < need)
		return ANX_ENOMEM;

	/* Build header */
	anx_memset(&hdr, 0, sizeof(hdr));
	hdr.magic       = ANX_JEPA_TRAJ_MAGIC;
	hdr.version     = TRAJ_VERSION;
	hdr.entry_count = g_count;

	world = anx_jepa_world_get_active();
	if (world) {
		hdr.obs_dim      = world->arch.obs_dim;
		hdr.action_count = world->action_count;
		anx_strlcpy(hdr.world_uri, world->uri,
			    ANX_JEPA_WORLD_URI_MAX);
		for (i = 0; i < world->obs_field_count &&
		            i < ANX_JEPA_MAX_OBS_FIELDS; i++)
			anx_strlcpy(hdr.obs_field_names[i],
				    world->obs_field_names[i],
				    ANX_JEPA_FIELD_NAME_MAX);
		for (i = 0; i < world->action_count &&
		            i < ANX_JEPA_MAX_ACTIONS; i++)
			anx_strlcpy(hdr.action_names[i],
				    world->action_names[i],
				    ANX_JEPA_FIELD_NAME_MAX);
	}

	p = (uint8_t *)buf_out;
	anx_memcpy(p, &hdr, sizeof(hdr));
	p += sizeof(hdr);

	/* Copy entries in chronological order (oldest first) */
	src = (g_count < ANX_JEPA_TRAJ_RING_SIZE)
	      ? 0
	      : g_head;	/* oldest slot when buffer is full */

	for (i = 0; i < g_count; i++) {
		uint32_t slot = (src + i) % ANX_JEPA_TRAJ_RING_SIZE;

		anx_memcpy(p, &g_ring[slot], sizeof(struct anx_jepa_traj_entry));
		p += sizeof(struct anx_jepa_traj_entry);
	}

	*bytes_written_out = need;
	kprintf("[jepa-traj] exported %u entries (%u bytes)\n",
		g_count, need);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Public: reset                                                       */
/* ------------------------------------------------------------------ */

void anx_jepa_traj_reset(void)
{
	anx_memset(g_ring, 0, sizeof(g_ring));
	g_head  = 0;
	g_count = 0;
}
