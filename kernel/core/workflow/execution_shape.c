#include <anx/execution_shape.h>
int anx_shape_compile(const anx_cid_t *owner, const struct anx_shape_spec *spec, struct anx_shape_view *out)
{ (void)owner; (void)spec; (void)out; return ANX_ENOSYS; }
int anx_shape_get(uint64_t id, struct anx_shape_view *out)
{ (void)id; (void)out; return ANX_ENOSYS; }
int anx_shape_run(uint64_t id, uint64_t epoch, struct anx_shape_view *out)
{ (void)id; (void)epoch; (void)out; return ANX_ENOSYS; }
int anx_shape_read(uint64_t id, uint32_t node, struct anx_anxml_response *out)
{ (void)id; (void)node; (void)out; return ANX_ENOSYS; }
int anx_shape_destroy(uint64_t id)
{ (void)id; return ANX_ENOSYS; }
