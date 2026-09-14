/* An executable adapter materialization keeps identity across a physical move. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/resource_view.h>
#include <anx/adapter.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>
#include <anx/page.h>
#include <anx/crypto.h>
#include <anx/external_call.h>

struct state060 {
	struct anx_adapter_image image;
	struct anx_adapter_cache_key key;
	uint8_t padding[3300];
};
struct fixture060 {
	struct state060 original, restored;
	struct anx_anxml_request request;
	struct anx_anxml_response response;
	struct anx_adapter_trial trial;
	anx_oid_t old_handle, fresh_handle, destination, extras[ANX_RESOURCE_ALIASES_MAX];
	bool foreign;
};
static int materialize060(struct fixture060 *f, const anx_oid_t *handle)
{
	uint8_t digest[32];
	int ret = anx_resource_view_read(handle, 0, &f->restored, sizeof(f->restored));
	if (ret != sizeof(f->restored)) return ret < 0 ? ret : ANX_EIO;
	ret = anx_adapter_cache_check(&f->restored.key.version.id, &f->request, &f->restored.key);
	if (ret != ANX_OK) return ret;
	anx_sha256(&f->restored.image, sizeof(f->restored.image), digest);
	if (anx_memcmp(digest, f->restored.key.version.image_hash.bytes, 32)) return ANX_EIO;
	ret = anx_anxml_generate_image(&f->request, &f->restored.image, &f->response);
	return ret == ANX_OK && f->response.output_len == 4 && !anx_memcmp(f->response.output, "AAAA", 4) ? ANX_OK : -6010;
}
static int active060(struct anx_external_call *call, void *arg)
{
	struct fixture060 *f = arg;
	struct anx_resource_move_result result;
	struct anx_resource_view_info info;
	anx_oid_t clone = ANX_UUID_NIL;
	uint8_t byte = 0x55;
	(void)call;
	if (anx_resource_view_move(&f->old_handle, &f->destination, 1, &result) != ANX_EPERM ||
	    anx_resource_view_test_move_fault(true) != ANX_EPERM) return -6011;
	if (f->foreign) {
		return anx_resource_view_read(&f->old_handle, 0, &byte, 1) == ANX_EPERM && byte == 0x55 &&
			anx_resource_view_clone(&f->old_handle, &clone) == ANX_EPERM && anx_uuid_is_nil(&clone) &&
			anx_resource_view_info(&f->old_handle, &info) == ANX_EPERM &&
			anx_resource_view_release(&f->old_handle) == ANX_EPERM ? ANX_OK : -6012;
	}
	if (materialize060(f, &f->old_handle) != ANX_EBUSY) return -6013;
	return materialize060(f, &f->fresh_handle);
}
static int denied060(const anx_oid_t *handle, const anx_oid_t *source, const anx_oid_t *destination,
		uint32_t headroom, int expected)
{
	struct anx_resource_pool_stats before[2], after[2];
	struct anx_resource_move_result result, sentinel;
	uint64_t total, free_before, free_after;
	anx_resource_pool_stats(source, &before[0]); anx_resource_pool_stats(destination, &before[1]);
	anx_memset(&result, 0x55, sizeof(result)); sentinel = result;
	anx_page_stats(&total, &free_before);
	int ret = anx_resource_view_move(handle, destination, headroom, &result);
	anx_page_stats(&total, &free_after);
	anx_resource_pool_stats(source, &after[0]); anx_resource_pool_stats(destination, &after[1]);
	return ret == expected && free_before == free_after && !anx_memcmp(before, after, sizeof(before)) &&
		!anx_memcmp(&result, &sentinel, sizeof(result)) ? ANX_OK : -6003;
}
int anx_research_day060(void)
{
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_adapter_view adapter = {0};
	anx_oid_t pools[4] = {0}, handle = ANX_UUID_NIL, alias = ANX_UUID_NIL, prefix[3] = {0};
	struct anx_resource_move_result moved;
	struct anx_resource_view_info identity, after;
	struct anx_resource_pool_stats stats;
	uint64_t total, free_before, free_after, pending = 0;
	struct fixture060 *f = anx_zalloc(sizeof(*f));
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	f->original.image = (struct anx_adapter_image){ .format = 1, .count = 2,
		.deltas = {{'~','A',4096},{'A','A',4096}} };
	anx_memset(f->original.padding, 0x63, sizeof(f->original.padding));
	f->request.prompt[0] = '~'; f->request.prompt_len = 1; f->request.max_tokens = 4;
	anx_strlcpy(intent.name, "research-day-060", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_adapter_create(&owner->cid, &f->original.image, &adapter);
	if (ret == ANX_OK) ret = anx_adapter_generate(&adapter.id, adapter.generation, &f->request, &f->response, &f->original.key);
	if (ret == ANX_OK) ret = anx_resource_pool_create(&owner->cid, 3, &pools[0]);
	if (ret == ANX_OK) ret = anx_resource_pool_create(&owner->cid, 3, &pools[1]);
	if (ret == ANX_OK) ret = anx_resource_view_insert(&pools[0], f->original.padding, 1000, &prefix[0]);
	if (ret == ANX_OK) ret = anx_resource_view_insert(&pools[1], f->original.padding, 3000, &prefix[1]);
	if (ret == ANX_OK) ret = anx_resource_view_insert(&pools[0], &f->original, sizeof(f->original), &handle);
	if (ret == ANX_OK) ret = anx_resource_view_clone(&handle, &alias);
	if (ret != ANX_OK) { kprintf("day060 setup rc=%d\n", ret); goto out; }
	ret = -6002;
	if (f->response.output_len != 4 || anx_memcmp(f->response.output, "AAAA", 4)) goto out;
	ret = anx_resource_view_info(&handle, &identity);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret == ANX_OK) ret = anx_resource_pool_create(&foreign->cid, 3, &pools[2]);
	if (ret == ANX_OK) ret = anx_resource_pool_create(&owner->cid, 1, &pools[3]);
	if (ret == ANX_OK) ret = anx_resource_view_insert(&pools[3], f->original.padding, 3000, &prefix[2]);
	if (ret != ANX_OK) goto out;
	if ((ret = denied060(&handle, &pools[0], &pools[2], 1, ANX_EPERM)) != ANX_OK ||
	    (ret = denied060(&handle, &pools[0], &pools[3], 1, ANX_ENOMEM)) != ANX_OK ||
	    (ret = denied060(&handle, &pools[0], &pools[0], 1, ANX_EBUSY)) != ANX_OK ||
	    (ret = denied060(&handle, &pools[0], &pools[1], 0, ANX_ENOMEM)) != ANX_OK ||
	    (ret = denied060(&handle, &pools[0], &pools[1], 33, ANX_EINVAL)) != ANX_OK) goto out;
	/* Both aliases must fit at the destination before any copy or mapping change. */
	for (uint32_t i = 0; i < ANX_RESOURCE_ALIASES_MAX - 2; i++) {
		ret = anx_resource_view_clone(&prefix[1], &f->extras[i]);
		if (ret != ANX_OK) goto out;
	}
	ret = denied060(&handle, &pools[0], &pools[1], 1, ANX_EFULL);
	for (uint32_t i = 0; i < ANX_RESOURCE_ALIASES_MAX - 2; i++) {
		anx_resource_view_release(&f->extras[i]); f->extras[i] = ANX_UUID_NIL;
	}
	if (ret != ANX_OK) goto out;
	ret = anx_resource_view_test_corrupt(&alias);
	if (ret != ANX_OK) goto out;
	ret = denied060(&handle, &pools[0], &pools[1], 1, ANX_EIO);
	anx_resource_view_test_corrupt(&alias);
	if (ret != ANX_OK) goto out;
	anx_resource_view_test_move_fault(true);
	ret = denied060(&handle, &pools[0], &pools[1], 1, ANX_EIO);
	if (ret != ANX_OK) goto out;
	ret = materialize060(f, &handle);
	if (ret != ANX_OK) goto out;
	anx_page_stats(&total, &free_before);
	ret = -6001;
	if (anx_resource_view_move(&alias, &pools[1], 1, &moved) != ANX_OK) goto out;
	anx_page_stats(&total, &free_after);
	ret = -6004;
	if (free_before != free_after + 1 || moved.bytes_moved != sizeof(f->original) || moved.physical_pages_added != 1 ||
	    anx_uuid_compare(&moved.source_pool, &pools[0]) || anx_uuid_compare(&moved.destination_pool, &pools[1]) ||
	    anx_resource_view_info(&alias, &after) != ANX_OK || after.bytes != identity.bytes ||
	    anx_uuid_compare(&identity.logical_id, &after.logical_id) || anx_uuid_compare(&identity.owner, &after.owner) ||
	    anx_resource_view_info(&handle, &after) != ANX_OK || anx_uuid_compare(&identity.logical_id, &after.logical_id) ||
	    anx_resource_pool_stats(&pools[0], &stats) != ANX_OK || stats.live_aliases != 1 ||
	    stats.dead_bytes != sizeof(f->original) || stats.physical_pages != 2 ||
	    anx_resource_pool_stats(&pools[1], &stats) != ANX_OK || stats.live_aliases != 3 ||
	    stats.live_bytes != 3000 + sizeof(f->original) || stats.physical_pages != 2) goto out;
	ret = materialize060(f, &alias);
	if (ret != ANX_OK || anx_memcmp(&f->original, &f->restored, sizeof(f->original))) { ret = -6005; goto out; }
	/* Remove the old physical pool; both stable aliases must still execute. */
	ret = anx_resource_view_release(&prefix[0]); prefix[0] = ANX_UUID_NIL;
	if (ret != ANX_OK) goto out;
	anx_page_stats(&total, &free_before);
	ret = anx_resource_pool_compact(&pools[0], 1, 0, &stats);
	anx_page_stats(&total, &free_after);
	if (ret != ANX_OK || free_after != free_before + 2 || stats.physical_pages || stats.live_bytes) { ret = -6006; goto out; }
	ret = anx_resource_pool_destroy(&pools[0]); pools[0] = ANX_UUID_NIL;
	if (ret == ANX_OK) ret = materialize060(f, &handle);
	if (ret != ANX_OK) goto out;
	/* Physical copying does not renew the derivation's adapter generation. */
	struct anx_adapter_image image_b = { .format = 1, .count = 2, .deltas = {{'~','B',4096},{'B','B',4096}} };
	f->trial.request = f->request; f->trial.expected_len = 4; anx_memcpy(f->trial.expected, "BBBB", 4);
	ret = anx_adapter_stage(&adapter.id, adapter.generation, &image_b, &pending);
	if (ret == ANX_OK) ret = anx_adapter_verify(&adapter.id, pending, &f->trial, 1);
	if (ret == ANX_OK) ret = anx_adapter_publish(&adapter.id, pending, adapter.generation, &adapter);
	if (ret != ANX_OK) goto out;
	pending = 0;
	if (materialize060(f, &handle) != ANX_EBUSY) { ret = -6007; goto out; }
	ret = anx_adapter_rollback(&adapter.id, adapter.generation, &adapter);
	if (ret != ANX_OK) goto out;
	if (materialize060(f, &alias) != ANX_EBUSY) { ret = -6007; goto out; }
	/* Regeneration issues a new logical record; the original stays immutable. */
	f->restored = f->original;
	ret = anx_adapter_generate(&adapter.id, adapter.generation, &f->request, &f->response, &f->restored.key);
	if (ret == ANX_OK) ret = anx_resource_view_insert(&pools[1], &f->restored, sizeof(f->restored), &f->fresh_handle);
	if (ret != ANX_OK) goto out;
	ret = -6008;
	if (anx_resource_view_info(&f->fresh_handle, &after) != ANX_OK || !anx_uuid_compare(&after.logical_id, &identity.logical_id) ||
	    anx_resource_view_read(&handle, 0, &f->restored, sizeof(f->restored)) != sizeof(f->restored) ||
	    anx_memcmp(&f->restored, &f->original, sizeof(f->restored))) goto out;
	f->old_handle = alias; f->destination = pools[1]; f->foreign = true;
	call = anx_zalloc(sizeof(*call));
	if (!call) { ret = ANX_ENOMEM; goto out; }
	anx_strlcpy(call->endpoint, "anxresearch060://materialization", sizeof(call->endpoint));
	ret = anx_external_register_handler("anxresearch060", active060, f);
	if (ret != ANX_OK) goto out;
	owner->ext_call = foreign->ext_call = call;
	owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	ret = anx_cell_run(foreign);
	f->foreign = false;
	if (ret == ANX_OK) ret = anx_cell_run(owner);
	if (ret != ANX_OK) goto out;
	ret = ANX_OK;
out:
	if (ret != ANX_OK) kprintf("day060 failure rc=%d\n", ret);
	anx_resource_view_test_move_fault(false);
	anx_external_unregister_handler("anxresearch060");
	for (uint32_t i = 0; i < ANX_RESOURCE_ALIASES_MAX; i++)
		if (!anx_uuid_is_nil(&f->extras[i])) anx_resource_view_release(&f->extras[i]);
	if (!anx_uuid_is_nil(&f->fresh_handle)) anx_resource_view_release(&f->fresh_handle);
	if (!anx_uuid_is_nil(&handle)) anx_resource_view_release(&handle);
	if (!anx_uuid_is_nil(&alias)) anx_resource_view_release(&alias);
	for (uint32_t i = 0; i < 3; i++) {
		if (!anx_uuid_is_nil(&prefix[i])) anx_resource_view_release(&prefix[i]);
	}
	for (uint32_t i = 0; i < 4; i++) {
		if (!anx_uuid_is_nil(&pools[i])) anx_resource_pool_destroy(&pools[i]);
	}
	if (pending) anx_adapter_abort(&adapter.id, pending);
	if (!anx_uuid_is_nil(&adapter.id)) anx_adapter_destroy(&adapter.id);
	if (foreign) anx_cell_destroy(foreign);
	if (owner) anx_cell_destroy(owner);
	anx_free(call); anx_free(f);
	return ret;
}
#endif
