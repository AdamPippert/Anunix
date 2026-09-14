#include <anx/route_binding.h>
int anx_route_binding_create(const anx_cid_t *cell, const struct anx_route_binding_spec *spec,
		struct anx_route_binding_view *out)
{
	(void)cell; (void)spec; (void)out;
	return ANX_ENOTSUP;
}
