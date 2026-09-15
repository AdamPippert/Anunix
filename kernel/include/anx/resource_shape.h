#ifndef ANX_RESOURCE_SHAPE_H
#define ANX_RESOURCE_SHAPE_H
#include <anx/model_use.h>
#define ANX_RESOURCE_SHAPE_MAX 8U
#define ANX_RESOURCE_SHAPE_REPLICAS 4U
enum anx_memory_geometry { ANX_MEMORY_COPIES, ANX_MEMORY_SHARED_READONLY };
enum anx_memory_topology { ANX_MEMORY_COHERENT_CPU, ANX_MEMORY_SEPARATE_DEVICE };
struct anx_memory_lower_contract {
	enum anx_memory_topology topology;
	bool read_only_consumers, copy_orders_writes;
	uint32_t minimum_page_saving_pct;
};
struct anx_resource_shape_view {
	uint64_t id, epoch;
	anx_cid_t owner;
	struct anx_model_use_source source;
	uint32_t replicas, resident_replicas, physical_pages, resident_bytes;
	enum anx_memory_geometry geometry;
};
int anx_resource_shape_create(const anx_cid_t *owner, const anx_oid_t *source, uint32_t replicas, struct anx_resource_shape_view *out);
int anx_resource_shape_get(uint64_t id, struct anx_resource_shape_view *out);
int anx_resource_shape_resize(uint64_t id, uint64_t epoch, uint32_t replicas, struct anx_resource_shape_view *out);
int anx_resource_shape_destroy(uint64_t id);
int anx_resource_shape_share(uint64_t id, uint64_t epoch, const struct anx_memory_lower_contract *contract, struct anx_resource_shape_view *out);
/* Physical-plan integration retains records, validates epochs, and consumes private replicas. */
int anx_resource_shape_bind(uint64_t id, uint64_t epoch, uint32_t replica, const anx_cid_t *owner, const struct anx_model_use_source *source);
int anx_resource_shape_unbind(uint64_t id);
int anx_resource_shape_check(uint64_t id, uint64_t epoch, uint32_t replica, const anx_cid_t *owner, const struct anx_model_use_source *source);
int anx_resource_shape_execute(uint64_t id, uint64_t epoch, uint32_t replica, uint64_t use, uint64_t use_epoch,
		struct anx_anxml_response *response, struct anx_model_use_view *out);
#endif
