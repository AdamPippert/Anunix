#include <anx/effect_fence.h>

int anx_effect_fence_create(struct anx_effect_fence_view *out)
{
	(void)out; return ANX_ENOSYS;
}
int anx_effect_fence_get(const anx_oid_t *id, struct anx_effect_fence_view *out)
{
	(void)id; (void)out; return ANX_ENOSYS;
}
int anx_effect_fence_bind(struct anx_cell *cell, const anx_oid_t *id)
{
	(void)cell; (void)id; return ANX_ENOSYS;
}
int anx_effect_fence_transition(const anx_oid_t *id, uint64_t generation, enum anx_effect_fence_state state)
{
	(void)id; (void)generation; (void)state; return ANX_ENOSYS;
}
int anx_effect_fence_hold(struct anx_cell *cell)
{
	(void)cell; return ANX_ENOSYS;
}
int anx_effect_fence_cancel(struct anx_cell *cell)
{
	(void)cell; return ANX_ENOSYS;
}
int anx_effect_fence_check(const struct anx_cell *cell, anx_oid_t *id_out, uint64_t *epoch_out)
{
	(void)cell; (void)id_out; (void)epoch_out; return ANX_ENOSYS;
}
int anx_effect_fence_dispatch(struct anx_pending_effect *effect)
{
	(void)effect; return ANX_ENOSYS;
}
