#include <anx/resource_decision.h>
int anx_resource_decision_propose(uint64_t graph, uint64_t shape, uint32_t replicas, struct anx_resource_decision_view *out)
{ (void)graph; (void)shape; (void)replicas; (void)out; return ANX_ENOSYS; }
int anx_resource_decision_commit(uint64_t id, struct anx_resource_shape_view *out)
{ (void)id; (void)out; return ANX_ENOSYS; }
int anx_resource_decision_get(uint64_t id, struct anx_resource_decision_view *out)
{ (void)id; (void)out; return ANX_ENOSYS; }
int anx_resource_decision_destroy(uint64_t id)
{ (void)id; return ANX_ENOSYS; }
