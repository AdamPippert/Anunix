#include <anx/branch_group.h>
int anx_branch_group_create(const anx_cid_t *owner, const struct anx_branch_spec *spec, struct anx_branch_view *out)
{
	(void)owner; (void)spec; (void)out; return ANX_ENOTSUP;
}
int anx_branch_group_destroy(uint64_t id) { (void)id; return ANX_ENOTSUP; }
