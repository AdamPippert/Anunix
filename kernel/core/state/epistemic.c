#include <anx/epistemic.h>
int anx_epistemic_begin(struct anx_object_handle *handle, const struct anx_epistemic_spec *spec, struct anx_epistemic_view *out)
{ (void)handle; (void)spec; (void)out; return ANX_ENOSYS; }
int anx_epistemic_vote(uint64_t id, const anx_oid_t *evidence, bool approve, struct anx_epistemic_view *out)
{ (void)id; (void)evidence; (void)approve; (void)out; return ANX_ENOSYS; }
int anx_epistemic_get(uint64_t id, struct anx_epistemic_view *out)
{ (void)id; (void)out; return ANX_ENOSYS; }
int anx_epistemic_destroy(uint64_t id)
{ (void)id; return ANX_ENOSYS; }
int anx_epistemic_stage_check(const struct anx_state_object *object)
{ (void)object; return ANX_ENOSYS; }
void anx_epistemic_stage_resolve(const struct anx_state_object *object, bool committed)
{ (void)object; (void)committed; }
