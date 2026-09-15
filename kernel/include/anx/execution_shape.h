#ifndef ANX_EXECUTION_SHAPE_H
#define ANX_EXECUTION_SHAPE_H
#include <anx/model_use.h>
#define ANX_SHAPE_MAX 16U
#define ANX_SHAPE_NODES_MAX 8U
enum anx_shape_mode { ANX_SHAPE_LITERAL, ANX_SHAPE_REUSE };
enum anx_shape_opcode { ANX_SHAPE_INFER };
enum anx_shape_state { ANX_SHAPE_READY, ANX_SHAPE_RUNNING, ANX_SHAPE_COMPLETED, ANX_SHAPE_FAILED };
struct anx_shape_node { uint64_t use; uint32_t dependencies, opcode; };
struct anx_shape_spec { uint32_t count; enum anx_shape_mode mode; struct anx_shape_node nodes[ANX_SHAPE_NODES_MAX]; };
struct anx_shape_view {
	uint64_t id, epoch;
	anx_cid_t owner;
	enum anx_shape_state state;
	uint32_t logical_operations, planned_physical_operations, physical_operations, reused_operations, generated_tokens, completed;
	uint32_t source_node[ANX_SHAPE_NODES_MAX];
	int result;
};
int anx_shape_compile(const anx_cid_t *owner, const struct anx_shape_spec *spec, struct anx_shape_view *out);
int anx_shape_get(uint64_t id, struct anx_shape_view *out);
int anx_shape_run(uint64_t id, uint64_t epoch, struct anx_shape_view *out);
int anx_shape_read(uint64_t id, uint32_t node, struct anx_anxml_response *out);
int anx_shape_destroy(uint64_t id);
#endif
