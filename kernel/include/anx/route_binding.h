#ifndef ANX_ROUTE_BINDING_H
#define ANX_ROUTE_BINDING_H
#include <anx/route.h>
#define ANX_ROUTE_BINDING_MAX 32U
#define ANX_ROUTE_BINDING_OBJECT_MAX (64U * 1024U)
struct anx_route_binding_spec {
	uint32_t schema;
	anx_oid_t model;
	uint32_t required_context_tokens, required_caps, engine_count;
	anx_eid_t engines[ANX_MAX_ROUTE_CANDIDATES];
};
struct anx_route_binding_view {
	uint64_t id, epoch;
	anx_cid_t cell;
	anx_oid_t model;
	uint64_t model_version;
	anx_eid_t engine;
};
/* Controller-created compatible pool; the owner or controller can rebind it. */
int anx_route_binding_create(const anx_cid_t *cell, const struct anx_route_binding_spec *spec,
		struct anx_route_binding_view *out);
int anx_route_binding_select(uint64_t id, uint64_t epoch, struct anx_route_binding_view *out);
int anx_route_binding_get(uint64_t id, struct anx_route_binding_view *out);
int anx_route_binding_check(uint64_t id, uint64_t epoch);
int anx_route_binding_destroy(uint64_t id);
#endif
