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

			/* Filter out offline engines */
			if (eng->status == ANX_ENGINE_OFFLINE)
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

int anx_engine_unregister(struct anx_engine *engine)
{
	if (!engine)
		return ANX_EINVAL;

	anx_htable_del(&engine_table, &engine->registry_link);
	anx_free(engine);
	return ANX_OK;
}
