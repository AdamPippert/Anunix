#ifndef ANX_ROUTE_CATALOG_H
#define ANX_ROUTE_CATALOG_H
#include <anx/route_profile.h>
#define ANX_ROUTE_CATALOG_MAX 16U
#define ANX_ROUTE_CATALOG_ENTRIES 4U
struct anx_route_catalog_view {
	uint64_t id, epoch;
	anx_cid_t owner;
	uint32_t count;
	anx_oid_t profiles[ANX_ROUTE_CATALOG_ENTRIES];
};
/* Controller-issued catalogs contain only currently valid, previously compiled profiles. */
int anx_route_catalog_create(const anx_cid_t *owner, const anx_oid_t *profiles, uint32_t count, struct anx_route_catalog_view *out);
int anx_route_catalog_replace(uint64_t id, uint64_t epoch, const anx_oid_t *profiles, uint32_t count, struct anx_route_catalog_view *out);
int anx_route_catalog_get(uint64_t id, struct anx_route_catalog_view *out);
int anx_route_catalog_destroy(uint64_t id);
/* Planner hook: a failed indexed selection leaves the supplied fallback weights unchanged. */
int anx_route_catalog_choose(uint64_t id, uint64_t epoch, uint32_t index, const struct anx_cell *cell,
	const struct anx_route_tuning_state *incumbent, struct anx_route_weight_policy *out);
#endif
