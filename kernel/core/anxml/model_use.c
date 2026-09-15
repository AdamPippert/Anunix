#include <anx/model_use.h>
int anx_model_use_prepare(const anx_cid_t *owner, const struct anx_model_use_spec *spec, struct anx_model_use_view *out)
{ (void)owner; (void)spec; (void)out; return ANX_ENOSYS; }
int anx_model_use_get(uint64_t id, struct anx_model_use_view *out)
{ (void)id; (void)out; return ANX_ENOSYS; }
int anx_model_use_execute(uint64_t id, uint64_t epoch, struct anx_anxml_response *response, struct anx_model_use_view *out)
{ (void)id; (void)epoch; (void)response; (void)out; return ANX_ENOSYS; }
int anx_model_use_destroy(uint64_t id)
{ (void)id; return ANX_ENOSYS; }
