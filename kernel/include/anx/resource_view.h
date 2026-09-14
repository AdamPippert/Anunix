#ifndef ANX_RESOURCE_VIEW_H
#define ANX_RESOURCE_VIEW_H
#include <anx/types.h>

#define ANX_RESOURCE_POOLS_MAX 16U
#define ANX_RESOURCE_PAGES_MAX 32U
#define ANX_RESOURCE_RECORDS_MAX 64U
#define ANX_RESOURCE_ALIASES_MAX 128U
#define ANX_RESOURCE_ITEM_MAX 4096U

struct anx_resource_pool_stats {
	uint32_t physical_pages, live_records, live_aliases, live_bytes, dead_bytes;
};
struct anx_resource_view_info {
	anx_oid_t logical_id;
	anx_cid_t owner;
	uint32_t bytes;
};

/* The controller owns pool capacity and physical reclamation. */
int anx_resource_pool_create(const anx_cid_t *owner, uint32_t capacity_pages, anx_oid_t *pool);
int anx_resource_pool_destroy(const anx_oid_t *pool);
int anx_resource_pool_stats(const anx_oid_t *pool, struct anx_resource_pool_stats *out);
int anx_resource_pool_compact(const anx_oid_t *pool, uint32_t minimum_dead_bytes,
		uint32_t headroom_pages, struct anx_resource_pool_stats *out);
/* Owner or controller; immutable bytes are read through handles, never raw mappings. */
int anx_resource_view_insert(const anx_oid_t *pool, const void *bytes, uint32_t size, anx_oid_t *handle);
int anx_resource_view_clone(const anx_oid_t *handle, anx_oid_t *alias);
int anx_resource_view_release(const anx_oid_t *handle);
int anx_resource_view_info(const anx_oid_t *handle, struct anx_resource_view_info *out);
int anx_resource_view_read(const anx_oid_t *handle, uint32_t offset, void *bytes, uint32_t size);
#endif
