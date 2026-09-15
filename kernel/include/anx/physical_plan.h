#ifndef ANX_PHYSICAL_PLAN_H
#define ANX_PHYSICAL_PLAN_H
#include <anx/execution_shape.h>
#define ANX_PHYSICAL_PLAN_MAX 32U
struct anx_logical_graph_spec { uint32_t count; struct anx_shape_node nodes[ANX_SHAPE_NODES_MAX]; };
struct anx_logical_graph_view {
	uint64_t id, epoch;
	anx_cid_t owner;
	uint8_t program_digest[32];
	uint32_t count, completed, physical_operations, reused_operations;
	enum anx_shape_state state;
	int result;
};
enum anx_physical_mode { ANX_PHYSICAL_DIRECT, ANX_PHYSICAL_REUSE };
enum anx_physical_state { ANX_PHYSICAL_ISSUED, ANX_PHYSICAL_RUNNING, ANX_PHYSICAL_COMMITTED, ANX_PHYSICAL_FAILED };
struct anx_physical_plan_view {
	uint64_t id, logical_graph, logical_epoch, phase_epoch;
	uint64_t resource_shape, resource_epoch;
	uint32_t replica;
	uint32_t node, source_node;
	enum anx_physical_mode mode;
	enum anx_physical_state state;
	int result;
};
/* Logical creation and physical compilation are separate controller operations. */
int anx_logical_graph_create(const anx_cid_t *owner, const struct anx_logical_graph_spec *spec, struct anx_logical_graph_view *out);
int anx_logical_graph_get(uint64_t id, struct anx_logical_graph_view *out);
int anx_logical_graph_read(uint64_t id, uint32_t node, struct anx_anxml_response *out);
int anx_logical_graph_destroy(uint64_t id);
int anx_physical_plan_compile(uint64_t graph, uint64_t logical_epoch, uint64_t phase_epoch,
		enum anx_physical_mode preference, struct anx_physical_plan_view *out);
int anx_physical_plan_compile_shaped(uint64_t graph, uint64_t logical_epoch, uint64_t phase_epoch,
		uint64_t resource_shape, uint64_t resource_epoch, uint32_t replica, struct anx_physical_plan_view *out);
/* Commit one issued prefix after checking the current logical and physical boundaries. */
int anx_physical_plan_commit(uint64_t id, struct anx_anxml_response *response, struct anx_logical_graph_view *out);
int anx_physical_plan_get(uint64_t id, struct anx_physical_plan_view *out);
int anx_physical_plan_destroy(uint64_t id);
#endif
