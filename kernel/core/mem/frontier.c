#include <anx/frontier.h>
int anx_frontier_create(const anx_cid_t *owner, const struct anx_frontier_spec *spec, struct anx_frontier_view *out)
{ (void)owner; (void)spec; (void)out; return ANX_ENOSYS; }
int anx_frontier_restore(uint64_t id, uint64_t epoch, struct anx_frontier_view *out)
{ (void)id; (void)epoch; (void)out; return ANX_ENOSYS; }
int anx_frontier_reclaim(uint64_t id, uint64_t epoch, struct anx_frontier_view *out)
{ (void)id; (void)epoch; (void)out; return ANX_ENOSYS; }
int anx_frontier_step(uint64_t id, uint64_t epoch, struct anx_anxml_response *response, struct anx_frontier_view *out)
{ (void)id; (void)epoch; (void)response; (void)out; return ANX_ENOSYS; }
int anx_frontier_get(uint64_t id, struct anx_frontier_view *out)
{ (void)id; (void)out; return ANX_ENOSYS; }
int anx_frontier_read(uint64_t id, uint32_t node, struct anx_anxml_response *out)
{ (void)id; (void)node; (void)out; return ANX_ENOSYS; }
int anx_frontier_destroy(uint64_t id)
{ (void)id; return ANX_ENOSYS; }
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
int anx_frontier_test_corrupt(uint64_t id, uint32_t node)
{ (void)id; (void)node; return ANX_ENOSYS; }
#endif
