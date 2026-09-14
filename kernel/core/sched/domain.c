#include <anx/sched_domain.h>
int anx_sched_domain_create(const anx_cid_t *owner, uint64_t parent,
		const struct anx_sched_domain_spec *spec, struct anx_sched_domain_view *out)
{
	(void)owner; (void)parent; (void)spec; (void)out; return ANX_ENOTSUP;
}
int anx_sched_domain_destroy(uint64_t id) { (void)id; return ANX_ENOTSUP; }
