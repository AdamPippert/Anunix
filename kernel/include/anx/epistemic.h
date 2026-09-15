#ifndef ANX_EPISTEMIC_H
#define ANX_EPISTEMIC_H
#include <anx/state_object.h>
#define ANX_EPISTEMIC_MAX 16U
#define ANX_EPISTEMIC_REVIEWERS 8U
#define ANX_EPISTEMIC_NODES 32U
#define ANX_EPISTEMIC_ROOTS 8U
enum anx_epistemic_state { ANX_EPISTEMIC_OPEN, ANX_EPISTEMIC_COMMITTED, ANX_EPISTEMIC_ABORTED };
struct anx_epistemic_spec {
	uint32_t count, threshold, minimum_cut;
	anx_cid_t reviewers[ANX_EPISTEMIC_REVIEWERS];
};
struct anx_epistemic_view {
	uint64_t id;
	anx_cid_t owner;
	anx_oid_t target;
	uint8_t proposal_digest[32];
	uint32_t votes, valid_approvals, structural_cut, threshold, minimum_cut, roots;
	enum anx_epistemic_state state;
	bool ready;
};
/* Controller binds the proposed shadow bytes and approved reviewers to an open stage. */
int anx_epistemic_begin(struct anx_object_handle *handle, const struct anx_epistemic_spec *spec, struct anx_epistemic_view *out);
/* Only a listed, active reviewer can cast its single vote from sealed evidence. */
int anx_epistemic_vote(uint64_t id, const anx_oid_t *evidence, bool approve, struct anx_epistemic_view *out);
int anx_epistemic_get(uint64_t id, struct anx_epistemic_view *out);
int anx_epistemic_destroy(uint64_t id);
/* Internal staged-mutation hooks; the caller holds the target object's lock. */
int anx_epistemic_stage_check(const struct anx_state_object *object);
void anx_epistemic_stage_resolve(const struct anx_state_object *object, bool committed);
#endif
