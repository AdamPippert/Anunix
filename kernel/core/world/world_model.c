/*
 * world_model.c — Dispatch to the registered world model backend.
 *
 * Nothing here knows how a model works. Each call forwards to the ops
 * table, and a missing backend or a missing operation is an answer
 * ("unavailable", ANX_ENOSYS), never a crash.
 */

#include <anx/types.h>
#include <anx/world_model.h>
#include <anx/kprintf.h>

static const struct anx_world_model_ops *g_ops;

#define CALL(op, fallback, ...) \
	(g_ops && g_ops->op ? g_ops->op(__VA_ARGS__) : (fallback))

void anx_world_model_register(const struct anx_world_model_ops *ops)
{
	if (g_ops && ops && g_ops != ops)
		kprintf("world: model %s replaces %s\n", ops->name,
			g_ops->name);
	g_ops = ops;
}

const char *anx_world_model_name(void)
{
	return g_ops && g_ops->name ? g_ops->name : "none";
}

bool anx_world_available(void)
{
	return g_ops && g_ops->available ? g_ops->available() : false;
}

enum anx_world_status anx_world_status_get(void)
{
	return g_ops && g_ops->status ? g_ops->status()
				      : ANX_WORLD_UNAVAILABLE;
}

const char *anx_world_status_name(enum anx_world_status st)
{
	static const char *const names[] = {
		"uninitialized", "initializing", "ready",
		"training", "degraded", "unavailable",
	};

	return (unsigned)st < sizeof(names) / sizeof(names[0]) ?
	       names[st] : "?";
}

uint32_t anx_world_train_steps(void)
{
	return g_ops && g_ops->train_steps ? g_ops->train_steps() : 0;
}

int anx_world_observe(struct anx_world_obs *obs_out)
{
	return CALL(observe, ANX_ENOSYS, obs_out);
}

int anx_world_observe_store(const struct anx_world_obs *obs,
			    anx_oid_t *obs_oid_out)
{
	return CALL(observe_store, ANX_ENOSYS, obs, obs_oid_out);
}

int anx_world_encode(const anx_oid_t *obs_oid, anx_oid_t *latent_oid_out)
{
	return CALL(encode, ANX_ENOSYS, obs_oid, latent_oid_out);
}

int anx_world_predict(const anx_oid_t *latent_oid, uint32_t action_id,
		      anx_oid_t *predicted_oid_out)
{
	return CALL(predict, ANX_ENOSYS, latent_oid, action_id,
		    predicted_oid_out);
}

float anx_world_divergence(const anx_oid_t *a, const anx_oid_t *b)
{
	return CALL(divergence, -1.0f, a, b);
}

int anx_world_record_winner(uint32_t action_id)
{
	return CALL(record_winner, ANX_ENOSYS, action_id);
}

int anx_world_traj_ingest(const struct anx_world_obs *obs,
			  uint32_t action_id, const char *world_uri)
{
	return CALL(traj_ingest, ANX_ENOSYS, obs, action_id, world_uri);
}

int anx_world_traj_count(uint32_t *entries_out, uint32_t *bytes_out)
{
	return CALL(traj_count, ANX_ENOSYS, entries_out, bytes_out);
}

int anx_world_traj_dump(anx_oid_t *oid_out, uint32_t *bytes_out)
{
	return CALL(traj_dump, ANX_ENOSYS, oid_out, bytes_out);
}

void anx_world_traj_reset(void)
{
	if (g_ops && g_ops->traj_reset)
		g_ops->traj_reset();
}

int anx_world_list(const char **uris_out, uint32_t max, uint32_t *count_out)
{
	if (!g_ops || !g_ops->world_list) {
		if (count_out)
			*count_out = 0;
		return ANX_ENOSYS;
	}
	return g_ops->world_list(uris_out, max, count_out);
}

int anx_world_info_get(const char *uri, struct anx_world_info *out)
{
	return CALL(world_info, ANX_ENOSYS, uri, out);
}

int anx_world_set_active(const char *uri)
{
	return CALL(world_set_active, ANX_ENOSYS, uri);
}

uint32_t anx_world_action_count(void)
{
	struct anx_world_info info;

	if (anx_world_info_get(NULL, &info) == ANX_OK && info.action_count)
		return info.action_count;
	return ANX_WORLD_ACT_COUNT;
}

const char *anx_world_action_name(uint32_t action_id)
{
	return CALL(action_name, NULL, action_id);
}

int anx_world_rebuild(const char *uri, uint32_t max_steps,
		      anx_oid_t *checkpoint_oid_out)
{
	return CALL(world_rebuild, ANX_ENOSYS, uri, max_steps,
		    checkpoint_oid_out);
}

int anx_world_activate(const char *uri, const anx_oid_t *checkpoint_oid)
{
	return CALL(world_activate, ANX_ENOSYS, uri, checkpoint_oid);
}
