#ifndef ANX_EXPOSURE_H
#define ANX_EXPOSURE_H
#include <anx/types.h>
#define ANX_EXPOSURE_MAX 32U
#define ANX_EXPOSURE_DEPTH 8U
struct anx_exposure_view {
	uint64_t id, parent, limit, reserved, committed, uncertain;
	anx_cid_t owner;
	uint32_t depth, children, in_flight;
	bool revoked, closed;
};
/* Controller-assigned, immutable units represent one domain-specific exposure measure. */
int anx_exposure_create(const anx_cid_t *owner, uint64_t parent, uint64_t limit, struct anx_exposure_view *out);
int anx_exposure_get(uint64_t id, struct anx_exposure_view *out);
int anx_exposure_revoke(uint64_t id, struct anx_exposure_view *out);
int anx_exposure_destroy(uint64_t id);
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
/* Revoke after entering the next budgeted provider; excluded from production builds. */
int anx_exposure_test_revoke_on_dispatch(uint64_t id);
#endif
#endif
