/*
 * engine.c — Engine Registry implementation.
 *
 * Hash table of registered engines, keyed by EID.
 * Supports registration, lookup, capability-based search,
 * status updates, and unregistration.
 */

#include <anx/types.h>
#include <anx/engine.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/hashtable.h>
#include <anx/string.h>

#define ENGINE_REGISTRY_BITS	6	/* 64 buckets */

static struct anx_htable engine_table;

void anx_engine_registry_init(void)
{
	anx_htable_init(&engine_table, ENGINE_REGISTRY_BITS);
}

int anx_engine_register(const char *name,
			enum anx_engine_class engine_class,
			uint32_t capabilities,
			struct anx_engine **out)
{
	struct anx_engine *eng;

	if (!name)
		return ANX_EINVAL;
	if ((int)engine_class < 0 || engine_class >= ANX_ENGINE_CLASS_COUNT)
		return ANX_EINVAL;

	eng = anx_zalloc(sizeof(*eng));
	if (!eng)
		return ANX_ENOMEM;

	anx_uuid_generate(&eng->eid);
	anx_strlcpy(eng->name, name, sizeof(eng->name));
	eng->engine_class = engine_class;
	eng->status = ANX_ENGINE_AVAILABLE;
	eng->capabilities = capabilities;
	eng->is_local = true;
	eng->quality_score = 50;	/* default: average */

	anx_spin_init(&eng->lock);
	anx_list_init(&eng->registry_link);

	uint64_t hash = anx_uuid_hash(&eng->eid);
	anx_htable_add(&engine_table, &eng->registry_link, hash);

	if (out)
		*out = eng;
	return ANX_OK;
}

struct anx_engine *anx_engine_lookup(const anx_eid_t *eid)
{
	uint64_t hash = anx_uuid_hash(eid);
	struct anx_list_head *pos;

	ANX_HTABLE_FOR_BUCKET(pos, &engine_table, hash) {
		struct anx_engine *eng;

		eng = ANX_LIST_ENTRY(pos, struct anx_engine, registry_link);
		if (anx_uuid_compare(&eng->eid, eid) == 0)
			return eng;
	}
	return NULL;
}

int anx_engine_find(enum anx_engine_class engine_class,
		    uint32_t required_caps,
		    struct anx_engine **results,
		    uint32_t max_results,
		    uint32_t *found_count)
{
	uint32_t bucket_count;
	uint32_t found = 0;
	uint32_t i;

	if (!results || !found_count)
		return ANX_EINVAL;

	/* Handle uninitialized registry gracefully */
	if (!engine_table.buckets) {
		*found_count = 0;
		return ANX_OK;
	}

	bucket_count = 1U << engine_table.bits;

	for (i = 0; i < bucket_count && found < max_results; i++) {
		struct anx_list_head *head = &engine_table.buckets[i];
		struct anx_list_head *pos;

		ANX_LIST_FOR_EACH(pos, head) {
			struct anx_engine *eng;

			eng = ANX_LIST_ENTRY(pos, struct anx_engine,
					     registry_link);

			/* Filter by class */
			if (eng->engine_class != engine_class)
				continue;

			/* Filter by required capabilities */
			if ((eng->capabilities & required_caps) != required_caps)
				continue;

			/* Only include engines that can serve */
			if (eng->status != ANX_ENGINE_AVAILABLE &&
			    eng->status != ANX_ENGINE_DEGRADED)
				continue;

			results[found++] = eng;
			if (found >= max_results)
				break;
		}
	}

	*found_count = found;
	return ANX_OK;
}

int anx_engine_set_status(struct anx_engine *engine,
			  enum anx_engine_status status)
{
	if (!engine)
		return ANX_EINVAL;

	anx_spin_lock(&engine->lock);
	engine->status = status;
	anx_spin_unlock(&engine->lock);

	return ANX_OK;
}

/*
 * Engine status transition table.
 * MAINTENANCE is reachable from any state (admin override).
 */
static const bool engine_transitions[ANX_ENGINE_STATUS_COUNT][ANX_ENGINE_STATUS_COUNT] = {
	[ANX_ENGINE_REGISTERED] = {
		[ANX_ENGINE_LOADING] = true,
		[ANX_ENGINE_MAINTENANCE] = true,
	},
	[ANX_ENGINE_LOADING] = {
		[ANX_ENGINE_READY] = true,
		[ANX_ENGINE_OFFLINE] = true,		/* load failed */
		[ANX_ENGINE_MAINTENANCE] = true,
	},
	[ANX_ENGINE_READY] = {
		[ANX_ENGINE_AVAILABLE] = true,
		[ANX_ENGINE_UNLOADING] = true,
		[ANX_ENGINE_MAINTENANCE] = true,
	},
	[ANX_ENGINE_AVAILABLE] = {
		[ANX_ENGINE_DEGRADED] = true,
		[ANX_ENGINE_DRAINING] = true,
		[ANX_ENGINE_OFFLINE] = true,		/* crash */
		[ANX_ENGINE_MAINTENANCE] = true,
	},
	[ANX_ENGINE_DEGRADED] = {
		[ANX_ENGINE_AVAILABLE] = true,		/* recovered */
		[ANX_ENGINE_DRAINING] = true,
		[ANX_ENGINE_OFFLINE] = true,
		[ANX_ENGINE_MAINTENANCE] = true,
	},
	[ANX_ENGINE_DRAINING] = {
		[ANX_ENGINE_UNLOADING] = true,
		[ANX_ENGINE_MAINTENANCE] = true,
	},
	[ANX_ENGINE_UNLOADING] = {
		[ANX_ENGINE_OFFLINE] = true,
		[ANX_ENGINE_REGISTERED] = true,		/* can reload */
		[ANX_ENGINE_MAINTENANCE] = true,
	},
	[ANX_ENGINE_OFFLINE] = {
		[ANX_ENGINE_LOADING] = true,
		[ANX_ENGINE_REGISTERED] = true,
		[ANX_ENGINE_MAINTENANCE] = true,
	},
	[ANX_ENGINE_MAINTENANCE] = {
		[ANX_ENGINE_LOADING] = true,		/* admin re-enable */
		[ANX_ENGINE_REGISTERED] = true,
		[ANX_ENGINE_OFFLINE] = true,
	},
};

int anx_engine_transition(struct anx_engine *engine,
			  enum anx_engine_status new_status)
{
	int ret;

	if (!engine)
		return ANX_EINVAL;
	if ((int)new_status < 0 || new_status >= ANX_ENGINE_STATUS_COUNT)
		return ANX_EINVAL;

	anx_spin_lock(&engine->lock);

	if (!engine_transitions[engine->status][new_status]) {
		anx_spin_unlock(&engine->lock);
		return ANX_EPERM;
	}

	engine->status = new_status;
	ret = ANX_OK;

	anx_spin_unlock(&engine->lock);
	return ret;
}

int anx_engine_register_model(const char *name,
			      enum anx_engine_class engine_class,
			      uint32_t capabilities,
			      const struct anx_model_desc *model,
			      struct anx_engine **out)
{
	struct anx_engine *eng;
	int ret;

	if (!model)
		return ANX_EINVAL;
	if (engine_class != ANX_ENGINE_LOCAL_MODEL &&
	    engine_class != ANX_ENGINE_REMOTE_MODEL)
		return ANX_EINVAL;

	ret = anx_engine_register(name, engine_class, capabilities, &eng);
	if (ret != ANX_OK)
		return ret;

	/* Copy model descriptor */
	eng->model = *model;

	/* Model engines start in REGISTERED, not AVAILABLE */
	eng->status = ANX_ENGINE_REGISTERED;

	/* Inherit locality from model descriptor */
	eng->is_local = (engine_class == ANX_ENGINE_LOCAL_MODEL);
	eng->requires_network = (engine_class == ANX_ENGINE_REMOTE_MODEL);

	/* Use context window from descriptor */
	if (model->context_window > 0)
		eng->max_context_tokens = model->context_window;

	if (out)
		*out = eng;
	return ANX_OK;
}

int anx_engine_unregister(struct anx_engine *engine)
{
	if (!engine)
		return ANX_EINVAL;

	anx_htable_del(&engine_table, &engine->registry_link);
	anx_free(engine);
	return ANX_OK;
}
