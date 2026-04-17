/*
 * cell_store.c — Global cell registry.
 *
 * Hash table of all live cells, keyed by CID.
 * Provides create, lookup, release, and destroy operations.
 */

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/hashtable.h>
#include <anx/string.h>
#include <anx/arch.h>

#define CELL_STORE_BITS	8	/* 256 buckets */

static struct anx_htable cell_table;

void anx_cell_store_init(void)
{
	anx_htable_init(&cell_table, CELL_STORE_BITS);
}

int anx_cell_create(enum anx_cell_type type,
		    const struct anx_cell_intent *intent,
		    struct anx_cell **out)
{
	struct anx_cell *cell;

	if (!intent || !out)
		return ANX_EINVAL;
	if ((int)type < 0 || type >= ANX_CELL_TYPE_COUNT)
		return ANX_EINVAL;

	cell = anx_zalloc(sizeof(*cell));
	if (!cell)
		return ANX_ENOMEM;

	anx_uuid_generate(&cell->cid);
	cell->cell_type = type;
	cell->status = ANX_CELL_CREATED;

	/* Copy intent */
	anx_strlcpy(cell->intent.name, intent->name,
		     sizeof(cell->intent.name));
	anx_strlcpy(cell->intent.objective, intent->objective,
		     sizeof(cell->intent.objective));
	cell->intent.priority = intent->priority;

	/* Defaults */
	cell->constraints.max_child_cells = ANX_MAX_CHILD_CELLS;
	cell->constraints.max_recursion_depth = 4;
	cell->routing.strategy = ANX_ROUTE_DIRECT;
	cell->routing.decomposition = ANX_DECOMP_NONE;
	cell->validation.mode = ANX_VALIDATE_SCHEMA_ONLY;
	cell->validation.block_commit_on_failure = true;
	cell->commit.persist_outputs = true;
	cell->commit.write_trace = true;
	cell->execution.allow_recursive_cells = false;
	cell->execution.max_recursion_depth = 4;
	cell->retry.max_attempts = 1;
	cell->retry.backoff_base_ms = 100;

	cell->created_at = arch_time_now();
	cell->refcount = 1;
	anx_spin_init(&cell->lock);
	anx_list_init(&cell->store_link);

	/* Insert into global store */
	uint64_t hash = anx_uuid_hash(&cell->cid);
	anx_htable_add(&cell_table, &cell->store_link, hash);

	*out = cell;
	return ANX_OK;
}

struct anx_cell *anx_cell_store_lookup(const anx_cid_t *cid)
{
	uint64_t hash = anx_uuid_hash(cid);
	struct anx_list_head *pos;

	ANX_HTABLE_FOR_BUCKET(pos, &cell_table, hash) {
		struct anx_cell *cell = ANX_LIST_ENTRY(pos, struct anx_cell,
						       store_link);
		if (anx_uuid_compare(&cell->cid, cid) == 0) {
			anx_spin_lock(&cell->lock);
			cell->refcount++;
			anx_spin_unlock(&cell->lock);
			return cell;
		}
	}
	return NULL;
}

void anx_cell_store_release(struct anx_cell *cell)
{
	if (!cell)
		return;
	anx_spin_lock(&cell->lock);
	if (cell->refcount > 0)
		cell->refcount--;
	anx_spin_unlock(&cell->lock);
}

int anx_cell_destroy(struct anx_cell *cell)
{
	if (!cell)
		return ANX_EINVAL;

	anx_spin_lock(&cell->lock);
	if (cell->refcount > 1) {
		anx_spin_unlock(&cell->lock);
		return ANX_EBUSY;
	}
	anx_spin_unlock(&cell->lock);

	anx_htable_del(&cell_table, &cell->store_link);
	anx_free(cell);
	return ANX_OK;
}

int anx_cell_store_iterate(anx_cell_iter_fn cb, void *arg)
{
	uint32_t bucket;
	int ret;

	for (bucket = 0; bucket < (1u << CELL_STORE_BITS); bucket++) {
		struct anx_list_head *pos;
		struct anx_list_head *head;

		head = anx_htable_bucket(&cell_table, bucket);
		ANX_LIST_FOR_EACH(pos, head) {
			struct anx_cell *cell;

			cell = ANX_LIST_ENTRY(pos, struct anx_cell,
					     store_link);
			ret = cb(cell, arg);
			if (ret != 0)
				return ret;
		}
	}
	return ANX_OK;
}
