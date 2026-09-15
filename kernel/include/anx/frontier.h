#ifndef ANX_FRONTIER_H
#define ANX_FRONTIER_H
#include <anx/model_use.h>
#define ANX_FRONTIER_MAX 16U
#define ANX_FRONTIER_NODES 8U
enum anx_frontier_mode { ANX_FRONTIER_INCREMENTAL, ANX_FRONTIER_FULL };
struct anx_frontier_node { uint64_t use; uint32_t dependencies; };
struct anx_frontier_spec {
	uint64_t phase_epoch;
	uint32_t count;
	enum anx_frontier_mode mode;
	struct anx_frontier_node nodes[ANX_FRONTIER_NODES];
};
struct anx_frontier_view {
	uint64_t id, epoch, phase_epoch, restored_bytes, bytes_before_first_work;
	anx_cid_t owner;
	uint32_t count, next, resident, completed, physical_pages, pages_before_first_work;
	enum anx_frontier_mode mode;
};
/* Controller restores the earliest missing image and reclaims the farthest future image. */
int anx_frontier_create(const anx_cid_t *owner, const struct anx_frontier_spec *spec, struct anx_frontier_view *out);
int anx_frontier_restore(uint64_t id, uint64_t epoch, struct anx_frontier_view *out);
int anx_frontier_reclaim(uint64_t id, uint64_t epoch, struct anx_frontier_view *out);
/* Owner/controller execution advances one checked boundary at a time. */
int anx_frontier_step(uint64_t id, uint64_t epoch, struct anx_anxml_response *response, struct anx_frontier_view *out);
int anx_frontier_get(uint64_t id, struct anx_frontier_view *out);
int anx_frontier_read(uint64_t id, uint32_t node, struct anx_anxml_response *out);
int anx_frontier_destroy(uint64_t id);
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
int anx_frontier_test_corrupt(uint64_t id, uint32_t node);
#endif
#endif
