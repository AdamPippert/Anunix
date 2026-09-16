#ifndef ANX_CONTROL_IMAGE_H
#define ANX_CONTROL_IMAGE_H
#include <anx/branch_group.h>
#define ANX_CONTROL_MAX 16U
#define ANX_CONTROL_STEPS_MAX 16U
#define ANX_CONTROL_GROUPS_MAX 4U
#define ANX_CONTROL_HALT 0xffffffffU
#define ANX_CONTROL_SCHEMA "anx:harness/control/v1"
enum anx_control_opcode { ANX_CONTROL_RUN, ANX_CONTROL_VERIFY, ANX_CONTROL_READ, ANX_CONTROL_RETURN, ANX_CONTROL_FAIL, ANX_CONTROL_OPCODE_COUNT };
enum anx_control_state { ANX_CONTROL_READY, ANX_CONTROL_BUSY, ANX_CONTROL_COMPLETED, ANX_CONTROL_FAILED };
/* Wire images encode these uint32 words in little-endian order, including zero unused slots. */
struct anx_control_instruction { uint32_t opcode, slot, branch, on_success, on_failure; };
struct anx_control_program {
	uint32_t abi, count, maximum_steps, reserved;
	struct anx_control_instruction instructions[ANX_CONTROL_STEPS_MAX];
};
/* Controller-owned authority is supplied separately from the generated image. */
struct anx_control_authority {
	uint32_t allowed_operations, maximum_steps, group_count;
	uint64_t groups[ANX_CONTROL_GROUPS_MAX];
};
struct anx_control_view {
	uint64_t id, epoch;
	anx_cid_t owner;
	anx_oid_t image;
	enum anx_control_state state;
	uint32_t program_counter, event_count, result_size;
	int result;
};
struct anx_control_event { uint32_t sequence, instruction, opcode, slot, next_instruction; int result; };
int anx_control_create(const anx_cid_t *owner, const anx_oid_t *image, const struct anx_control_authority *authority, struct anx_control_view *out);
int anx_control_get(uint64_t id, struct anx_control_view *out);
int anx_control_step(uint64_t id, uint64_t epoch, struct anx_control_view *out);
int anx_control_event_get(uint64_t id, uint32_t event, struct anx_control_event *out);
int anx_control_read(uint64_t id, void *bytes, uint32_t capacity, uint32_t *size);
int anx_control_destroy(uint64_t id);
#endif
