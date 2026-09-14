#ifndef ANX_BRANCH_GROUP_H
#define ANX_BRANCH_GROUP_H
#include <anx/adapter.h>
#include <anx/cell.h>
#define ANX_BRANCH_GROUP_MAX 16U
#define ANX_BRANCH_MAX 8U
#define ANX_BRANCH_TOKEN_MAX 1024U
enum anx_branch_semantics { ANX_BRANCH_REQUIRED, ANX_BRANCH_TRIAL };
enum anx_branch_state { ANX_BRANCH_READY, ANX_BRANCH_RUNNING, ANX_BRANCH_COMPLETED, ANX_BRANCH_FAILED, ANX_BRANCH_ABORTED };
struct anx_branch_candidate {
	struct anx_adapter_image image;
	uint32_t maximum_tokens, expected_size;
	char expected[129];
};
struct anx_branch_spec {
	uint32_t schema, count, token_budget;
	enum anx_branch_semantics semantics;
	anx_oid_t prompt;
	struct anx_branch_candidate candidates[ANX_BRANCH_MAX];
};
struct anx_branch_view {
	uint64_t id, epoch;
	anx_cid_t owner, branches[ANX_BRANCH_MAX];
	enum anx_branch_state state;
	uint32_t count, attempted, accepted, cancelled, winner, charged_tokens;
	int result;
};
/* Controller creates pure CPU-model branches beneath an existing parent Cell. */
int anx_branch_group_create(const anx_cid_t *owner, const struct anx_branch_spec *spec, struct anx_branch_view *out);
int anx_branch_group_get(uint64_t id, struct anx_branch_view *out);
/* A settled run returns ANX_OK; the view carries its completion state and result. */
int anx_branch_group_run(uint64_t id, uint64_t epoch, struct anx_branch_view *out);
int anx_branch_group_read(uint64_t id, uint32_t branch, void *bytes, uint32_t capacity, uint32_t *size);
int anx_branch_group_abort(uint64_t id, uint64_t epoch);
int anx_branch_group_destroy(uint64_t id);
/* Internal runtime hooks; registered branches cannot dispatch external effects. */
int anx_branch_group_check(struct anx_cell *cell);
int anx_branch_group_execute(struct anx_cell *cell, bool *handled);
int anx_branch_group_effect_check(const anx_cid_t *cell);
#endif
