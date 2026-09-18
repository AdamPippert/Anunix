/*
 * jepa_world_model.c — JEPA as the backend of the world model interface.
 *
 * The loop, the EBM scorer and the `world` command reach JEPA only
 * through anx/world_model.h. This table maps that interface onto the
 * JEPA API; a different architecture registers its own table instead.
 */

#include <anx/types.h>
#include <anx/jepa.h>
#include <anx/world_model.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>

#define TRAJ_EXPORT_MAX		32768

/* Export the trajectory ring into a fresh buffer the caller frees. */
static int traj_export(uint8_t **buf_out, uint32_t *bytes_out)
{
	uint8_t *buf = anx_alloc(TRAJ_EXPORT_MAX);
	int ret;

	if (!buf)
		return ANX_ENOMEM;
	ret = anx_jepa_export_trajectory(buf, TRAJ_EXPORT_MAX, bytes_out);
	if (ret != ANX_OK) {
		anx_free(buf);
		return ret;
	}
	*buf_out = buf;
	return ANX_OK;
}

static int jepa_traj_count(uint32_t *entries_out, uint32_t *bytes_out)
{
	uint8_t *buf;
	uint32_t bytes = 0;
	int ret = traj_export(&buf, &bytes);

	if (ret != ANX_OK)
		return ret;
	if (entries_out)
		*entries_out =
			((const struct anx_jepa_traj_header *)buf)->entry_count;
	if (bytes_out)
		*bytes_out = bytes;
	anx_free(buf);
	return ANX_OK;
}

static int jepa_traj_dump(anx_oid_t *oid_out, uint32_t *bytes_out)
{
	struct anx_so_create_params params;
	struct anx_state_object *obj;
	uint8_t *buf;
	uint32_t bytes = 0;
	int ret = traj_export(&buf, &bytes);

	if (ret != ANX_OK)
		return ret;
	anx_memset(&params, 0, sizeof(params));
	params.object_type  = ANX_OBJ_BYTE_DATA;
	params.schema_uri   = "anx:schema/jepa-trajectory/v1";
	params.payload      = buf;
	params.payload_size = bytes;
	ret = anx_so_create(&params, &obj);
	anx_free(buf);
	if (ret != ANX_OK)
		return ret;
	*oid_out = obj->oid;
	if (bytes_out)
		*bytes_out = bytes;
	anx_objstore_release(obj);
	return ANX_OK;
}

static int jepa_world_info(const char *uri, struct anx_world_info *out)
{
	const struct anx_jepa_world_profile *w;

	w = uri ? anx_jepa_world_lookup(uri) : anx_jepa_world_get_active();
	if (!w)
		return ANX_ENOENT;
	anx_memset(out, 0, sizeof(*out));
	out->uri          = w->uri;
	out->display_name = w->display_name;
	out->obs_dim      = w->arch.obs_dim;
	out->latent_dim   = w->arch.latent_dim;
	out->action_count = w->action_count;
	out->collects_obs = w->collect_obs != NULL;
	return ANX_OK;
}

static const char *jepa_action_name(uint32_t action_id)
{
	const struct anx_jepa_world_profile *w = anx_jepa_world_get_active();

	if (!w || action_id >= w->action_count)
		return NULL;
	return w->action_names[action_id];
}

static const struct anx_world_model_ops jepa_ops = {
	.name             = "jepa",
	.available        = anx_jepa_available,
	.status           = anx_jepa_status_get,
	.train_steps      = anx_jepa_get_train_step_count,
	.observe          = anx_jepa_observe,
	.observe_store    = anx_jepa_observe_store,
	.encode           = anx_jepa_encode,
	.predict          = anx_jepa_predict,
	.divergence       = anx_jepa_divergence,
	.record_winner    = anx_jepa_record_winner,
	.traj_ingest      = anx_jepa_traj_ingest,
	.traj_count       = jepa_traj_count,
	.traj_dump        = jepa_traj_dump,
	.traj_reset       = anx_jepa_traj_reset,
	.world_list       = anx_jepa_world_list,
	.world_info       = jepa_world_info,
	.world_set_active = anx_jepa_world_set_active,
	.action_name      = jepa_action_name,
	.world_rebuild    = anx_jepa_world_rebuild,
	.world_activate   = anx_jepa_world_activate,
};

void anx_jepa_world_model_register(void)
{
	anx_world_model_register(&jepa_ops);
}
