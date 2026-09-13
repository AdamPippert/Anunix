#include <anx/memplane.h>

int anx_memplane_hint(struct anx_mem_entry *entry, const struct anx_mem_retention_hint *hint)
{
	(void)entry; (void)hint;
	return ANX_ENOSYS;
}

int anx_memplane_protect(struct anx_mem_entry *entry, uint32_t tiers)
{
	(void)entry; (void)tiers;
	return ANX_ENOSYS;
}

int anx_memplane_evict(const anx_oid_t *candidates, uint32_t count,
		       enum anx_mem_tier tier, anx_oid_t *victim_out)
{
	(void)candidates; (void)count; (void)tier; (void)victim_out;
	return ANX_ENOSYS;
}
