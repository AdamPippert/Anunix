#ifndef ANX_RESOURCE_DECISION_H
#define ANX_RESOURCE_DECISION_H
#include <anx/physical_plan.h>
#include <anx/resource_shape.h>
#include <anx/phase.h>
#define ANX_RESOURCE_DECISION_MAX 16U
enum anx_resource_decision_state { ANX_DECISION_PROPOSED, ANX_DECISION_COMMITTED, ANX_DECISION_STALE };
struct anx_resource_decision_view {
	uint64_t id;
	anx_cid_t owner;
	enum anx_resource_decision_state state;
	uint32_t desired_replicas;
	struct anx_logical_graph_view logical;
	struct anx_phase_view phase;
	struct anx_resource_shape_view shape;
	anx_oid_t identity_record, fence;
	uint64_t fence_generation, applied_shape_epoch;
	int result;
};
/* Controller proposals capture private observations; commit accepts only their issued identity. */
int anx_resource_decision_propose(uint64_t graph, uint64_t shape, uint32_t replicas, struct anx_resource_decision_view *out);
int anx_resource_decision_commit(uint64_t id, struct anx_resource_shape_view *out);
int anx_resource_decision_get(uint64_t id, struct anx_resource_decision_view *out);
int anx_resource_decision_destroy(uint64_t id);
#endif
