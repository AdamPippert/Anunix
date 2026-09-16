/* A surviving alias keeps its logical identity and bytes after page compaction. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/resource_view.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/page.h>
#include <anx/external_call.h>

struct view_context { anx_oid_t pool, handle; anx_cid_t owner; };
static int intruder(struct anx_external_call *call, void *arg)
{
	struct view_context *c = arg;
	struct anx_resource_pool_stats s;
	anx_oid_t denied = ANX_UUID_NIL;
	uint8_t byte = 0x55;
	(void)call;
	return anx_resource_view_read(&c->handle, 0, &byte, 1) == ANX_EPERM && byte == 0x55 &&
		anx_resource_view_clone(&c->handle, &denied) == ANX_EPERM &&
		anx_resource_view_release(&c->handle) == ANX_EPERM &&
		anx_resource_view_insert(&c->pool, &byte, 1, &denied) == ANX_EPERM &&
		anx_resource_pool_stats(&c->pool, &s) == ANX_EPERM &&
		anx_resource_pool_compact(&c->pool, 0, 1, &s) == ANX_EPERM &&
		anx_resource_pool_destroy(&c->pool) == ANX_EPERM &&
		anx_resource_pool_create(&c->owner, 1, &denied) == ANX_EPERM && anx_uuid_is_nil(&denied) ? ANX_OK : -5010;
}
static int own_view(struct anx_external_call *call, void *arg)
{
	struct view_context *c = arg;
	struct anx_resource_pool_stats s;
	anx_oid_t alias = ANX_UUID_NIL;
	uint8_t byte = 0;
	(void)call;
	int ret = anx_resource_view_clone(&c->handle, &alias);
	if (ret != ANX_OK) return ret;
	bool valid = anx_resource_view_read(&alias, 1000, &byte, 1) == 1 && byte == 3 &&
		anx_resource_pool_compact(&c->pool, 0, 1, &s) == ANX_EPERM;
	ret = anx_resource_view_release(&alias);
	return valid ? ret : -5011;
}

int anx_research_day050(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell *foreign = NULL;
	struct anx_external_call *owner_call = NULL, *foreign_call = NULL;
	struct anx_cell_intent intent = {0};
	anx_oid_t pool = ANX_UUID_NIL, handles[8] = {0}, alias = ANX_UUID_NIL, denied = ANX_UUID_NIL;
	anx_oid_t stale = ANX_UUID_NIL;
	struct anx_resource_pool_stats before, after;
	struct anx_resource_view_info identity, moved;
	struct view_context context;
	uint8_t *data = anx_alloc(1536);
	anx_oid_t *aliases = anx_zalloc(ANX_RESOURCE_ALIASES_MAX * sizeof(*aliases));
	uint64_t pages_total, pages_before, pages_after;
	int ret = ANX_ENOMEM;
	if (!data || !aliases) goto out;
	anx_strlcpy(intent.name, "research-day-050", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_resource_pool_create(&owner->cid, 3, &pool);
	if (ret != ANX_OK) goto out;
	for (uint32_t i = 0; i < 8; i++) {
		anx_memset(data, (int)(i + 1), 1536);
		ret = anx_resource_view_insert(&pool, data, 1536, &handles[i]);
		if (ret != ANX_OK) goto out;
		anx_memset(data, 0, 1536);
		ret = -5002;
		if (anx_resource_view_read(&handles[i], 0, data, 1536) != 1536) goto out;
		for (uint32_t j = 0; j < 1536; j++) if (data[j] != (uint8_t)(i + 1)) goto out;
	}
	ret = anx_resource_view_info(&handles[2], &identity);
	if (ret == ANX_OK) ret = anx_resource_view_clone(&handles[2], &alias);
	if (ret != ANX_OK) goto out;
	stale = handles[0];
	for (uint32_t i = 0; i < 7; i++) {
		ret = anx_resource_view_release(&handles[i]);
		if (ret != ANX_OK) goto out;
		handles[i] = ANX_UUID_NIL;
	}
	ret = anx_resource_pool_stats(&pool, &before);
	if (ret != ANX_OK) goto out;
	ret = -5003;
	if (before.live_records != 2 || before.live_aliases != 2 || before.dead_bytes != 9216 ||
	    anx_resource_view_insert(&pool, data, 1, &denied) != ANX_ENOMEM || !anx_uuid_is_nil(&denied) ||
	    anx_resource_pool_destroy(&pool) != ANX_EBUSY || anx_cell_destroy(owner) != ANX_EBUSY) goto out;
	anx_memset(&after, 0x55, sizeof(after));
	struct anx_resource_pool_stats sentinel = after;
	if (anx_resource_pool_compact(&pool, 9217, 1, &after) != ANX_EBUSY ||
	    anx_resource_pool_compact(&pool, 4096, 0, &after) != ANX_ENOMEM || anx_memcmp(&after, &sentinel, sizeof(after))) goto out;
	/* Corruption rejects both reads and relocation before publishing new mappings. */
	ret = anx_resource_view_test_corrupt(&alias);
	if (ret != ANX_OK) goto out;
	anx_memset(data, 0x55, 1536);
	anx_page_stats(&pages_total, &pages_before);
	int read_bad = anx_resource_view_read(&alias, 0, data, 1536);
	int copy_bad = anx_resource_pool_compact(&pool, 4096, 1, &after);
	anx_page_stats(&pages_total, &pages_after);
	anx_resource_view_test_corrupt(&alias);
	ret = -5004;
	if (read_bad != ANX_EIO || copy_bad != ANX_EIO || pages_before != pages_after ||
	    anx_memcmp(&after, &sentinel, sizeof(after))) goto out;
	for (uint32_t i = 0; i < 1536; i++) if (data[i] != 0x55) goto out;
	anx_page_stats(&pages_total, &pages_before);
	ret = -5001;
	if (anx_resource_pool_compact(&pool, 4096, 1, &after) != ANX_OK ||
	    before.physical_pages != 3 || after.physical_pages != 1 || after.live_bytes != 3072) goto out;
	anx_page_stats(&pages_total, &pages_after);
	ret = -5005;
	if (pages_after != pages_before + 2 || after.dead_bytes || after.live_records != 2 || after.live_aliases != 2 ||
	    anx_resource_view_info(&alias, &moved) != ANX_OK ||
	    anx_uuid_compare(&identity.logical_id, &moved.logical_id) || anx_uuid_compare(&identity.owner, &moved.owner) || moved.bytes != 1536 ||
	    anx_resource_view_read(&alias, 0, data, 1536) != 1536) goto out;
	for (uint32_t i = 0; i < 1536; i++) if (data[i] != 3) goto out;
	if (anx_resource_view_read(&handles[7], 0, data, 1536) != 1536) goto out;
	for (uint32_t i = 0; i < 1536; i++) if (data[i] != 8) goto out;
	ret = -5006;
	if (anx_resource_pool_compact(&pool, 0, 1, &after) != ANX_EBUSY ||
	    anx_resource_view_read(&alias, ~(uint32_t)0, data, 1) != ANX_EINVAL ||
	    anx_resource_view_read(&alias, 1535, data, 2) != ANX_EINVAL ||
	    anx_resource_view_read(&alias, 1536, NULL, 0) != 0 ||
	    anx_resource_pool_create(&owner->cid, 33, &denied) != ANX_EINVAL || !anx_uuid_is_nil(&denied)) goto out;
	for (uint32_t i = 0; i < ANX_RESOURCE_ALIASES_MAX - 2; i++) {
		ret = anx_resource_view_clone(&alias, &aliases[i]);
		if (ret != ANX_OK) goto out;
	}
	ret = -5007;
	if (anx_resource_view_clone(&alias, &denied) != ANX_EFULL || !anx_uuid_is_nil(&denied)) goto out;
	for (uint32_t i = 0; i < ANX_RESOURCE_ALIASES_MAX - 2; i++) {
		ret = anx_resource_view_release(&aliases[i]);
		if (ret != ANX_OK) goto out;
		aliases[i] = ANX_UUID_NIL;
	}
	/* Released identifiers stay invalid after slots and physical capacity are reused. */
	ret = anx_resource_view_insert(&pool, data, 1536, &handles[0]);
	if (ret != ANX_OK) goto out;
	ret = -5008;
	if (anx_resource_view_read(&stale, 0, data, 1) != ANX_ENOENT || anx_resource_view_release(&stale) != ANX_ENOENT) goto out;
	context = (struct view_context){pool, alias, owner->cid};
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch050foreign", intruder, &context);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch050owner", own_view, &context);
	if (ret != ANX_OK) goto out;
	foreign_call = anx_zalloc(sizeof(*foreign_call)); owner_call = anx_zalloc(sizeof(*owner_call));
	if (!foreign_call || !owner_call) { ret = ANX_ENOMEM; goto out; }
	anx_strlcpy(foreign_call->endpoint, "anxresearch050foreign://view", sizeof(foreign_call->endpoint));
	anx_strlcpy(owner_call->endpoint, "anxresearch050owner://view", sizeof(owner_call->endpoint));
	foreign->ext_call = foreign_call; owner->ext_call = owner_call;
	foreign->execution.allow_side_effects = owner->execution.allow_side_effects = true;
	ret = anx_cell_run(foreign);
	if (ret == ANX_OK) ret = anx_cell_run(owner);
	if (ret != ANX_OK) goto out;
	ret = -5009;
	if (anx_resource_view_clone(&alias, &denied) != ANX_EPERM ||
	    anx_resource_view_insert(&pool, data, 1, &denied) != ANX_EPERM || !anx_uuid_is_nil(&denied)) goto out;
	ret = anx_resource_view_release(&alias); alias = ANX_UUID_NIL;
	if (ret != ANX_OK) goto out;
	for (uint32_t i = 0; i < 8; i++) if (!anx_uuid_is_nil(&handles[i])) {
		ret = anx_resource_view_release(&handles[i]);
		if (ret != ANX_OK) goto out;
		handles[i] = ANX_UUID_NIL;
	}
	ret = anx_resource_pool_compact(&pool, 1, 0, &after);
	if (ret != ANX_OK) goto out;
	ret = -5012;
	if (after.physical_pages || after.live_records || after.live_aliases || after.live_bytes || after.dead_bytes) goto out;
	ret = ANX_OK;
out:
	anx_external_unregister_handler("anxresearch050foreign");
	anx_external_unregister_handler("anxresearch050owner");
	if (aliases) for (uint32_t i = 0; i < ANX_RESOURCE_ALIASES_MAX; i++)
		if (!anx_uuid_is_nil(&aliases[i])) anx_resource_view_release(&aliases[i]);
	if (!anx_uuid_is_nil(&alias)) anx_resource_view_release(&alias);
	for (uint32_t i = 0; i < 8; i++) if (!anx_uuid_is_nil(&handles[i])) anx_resource_view_release(&handles[i]);
	if (!anx_uuid_is_nil(&pool)) anx_resource_pool_destroy(&pool);
	if (foreign) anx_cell_destroy(foreign);
	if (owner) anx_cell_destroy(owner);
	anx_free(owner_call); anx_free(foreign_call); anx_free(aliases); anx_free(data);
	return ret;
}
#endif
