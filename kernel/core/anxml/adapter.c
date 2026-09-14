#include <anx/adapter.h>
int anx_adapter_create(const anx_cid_t *owner, const struct anx_adapter_image *image, struct anx_adapter_view *out)
{
	(void)owner; (void)image; (void)out; return ANX_ENOTSUP;
}
int anx_adapter_destroy(const anx_oid_t *id) { (void)id; return ANX_ENOTSUP; }
