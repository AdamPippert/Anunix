#include <anx/route_catalog.h>
int anx_route_catalog_create(const anx_cid_t *owner, const anx_oid_t *profiles, uint32_t count, struct anx_route_catalog_view *out)
{ (void)owner; (void)profiles; (void)count; (void)out; return ANX_ENOSYS; }
int anx_route_catalog_replace(uint64_t id, uint64_t epoch, const anx_oid_t *profiles, uint32_t count, struct anx_route_catalog_view *out)
{ (void)id; (void)epoch; (void)profiles; (void)count; (void)out; return ANX_ENOSYS; }
int anx_route_catalog_get(uint64_t id, struct anx_route_catalog_view *out)
{ (void)id; (void)out; return ANX_ENOSYS; }
int anx_route_catalog_destroy(uint64_t id)
{ (void)id; return ANX_ENOSYS; }
int anx_route_catalog_choose(uint64_t id, uint64_t epoch, uint32_t index, const struct anx_cell *cell,
	const struct anx_route_tuning_state *incumbent, struct anx_route_weight_policy *out)
{ (void)id; (void)epoch; (void)index; (void)cell; (void)incumbent; (void)out; return ANX_ENOSYS; }
