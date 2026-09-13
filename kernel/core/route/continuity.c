#include <anx/route.h>

int anx_route_plan_continuity(struct anx_cell *cell, struct anx_route_session *session,
			      const struct anx_continuity_hint *hint, struct anx_route_result *result)
{
	(void)cell; (void)session; (void)hint; (void)result;
	return ANX_ENOSYS;
}
