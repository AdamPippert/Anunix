#include <anx/control_image.h>
int anx_control_create(const anx_cid_t *owner, const anx_oid_t *image, const struct anx_control_authority *authority, struct anx_control_view *out)
{
	(void)owner; (void)image; (void)authority; (void)out; return ANX_ENOTSUP;
}
int anx_control_destroy(uint64_t id) { (void)id; return ANX_ENOTSUP; }
