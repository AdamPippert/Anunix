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

struct state060 {
	struct anx_adapter_image image;
	struct anx_adapter_cache_key key;
	uint8_t padding[3300];
};
struct fixture060 {
	struct state060 original, restored;
	struct anx_anxml_request request;
	struct anx_anxml_response response;
};
int anx_research_day060(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_adapter_view adapter = {0};
	anx_oid_t pools[2] = {0}, handle = ANX_UUID_NIL, alias = ANX_UUID_NIL, prefix[2] = {0};
	struct anx_resource_move_result moved;
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
	ret = -6001;
	if (anx_resource_view_move(&alias, &pools[1], 1, &moved) != ANX_OK) goto out;
	ret = ANX_OK;
out:
	if (!anx_uuid_is_nil(&handle)) anx_resource_view_release(&handle);
	if (!anx_uuid_is_nil(&alias)) anx_resource_view_release(&alias);
	for (uint32_t i = 0; i < 2; i++) {
		if (!anx_uuid_is_nil(&prefix[i])) anx_resource_view_release(&prefix[i]);
		if (!anx_uuid_is_nil(&pools[i])) anx_resource_pool_destroy(&pools[i]);
	}
	if (!anx_uuid_is_nil(&adapter.id)) anx_adapter_destroy(&adapter.id);
	if (owner) anx_cell_destroy(owner);
	anx_free(f);
	return ret;
}
#endif
