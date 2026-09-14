/* A surviving alias keeps its logical identity and bytes after page compaction. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/resource_view.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

int anx_research_day050(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	anx_oid_t pool = ANX_UUID_NIL, handles[8] = {0};
	struct anx_resource_pool_stats before, after;
	uint8_t *data = anx_alloc(1536);
	int ret = ANX_ENOMEM;
	if (!data) goto out;
	anx_strlcpy(intent.name, "research-day-050", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_resource_pool_create(&owner->cid, 3, &pool);
	if (ret != ANX_OK) goto out;
	for (uint32_t i = 0; i < 8; i++) {
		anx_memset(data, (int)(i + 1), 1536);
		ret = anx_resource_view_insert(&pool, data, 1536, &handles[i]);
		if (ret != ANX_OK) goto out;
	}
	for (uint32_t i = 0; i < 6; i++) {
		ret = anx_resource_view_release(&handles[i]);
		if (ret != ANX_OK) goto out;
		handles[i] = ANX_UUID_NIL;
	}
	ret = anx_resource_pool_stats(&pool, &before);
	if (ret != ANX_OK) goto out;
	ret = -5001;
	if (anx_resource_pool_compact(&pool, 4096, 1, &after) != ANX_OK ||
	    before.physical_pages != 3 || after.physical_pages != 1 || after.live_bytes != 3072) goto out;
	ret = ANX_OK;
out:
	for (uint32_t i = 0; i < 8; i++) if (!anx_uuid_is_nil(&handles[i])) anx_resource_view_release(&handles[i]);
	if (!anx_uuid_is_nil(&pool)) anx_resource_pool_destroy(&pool);
	if (owner) anx_cell_destroy(owner);
	anx_free(data);
	return ret;
}
#endif
