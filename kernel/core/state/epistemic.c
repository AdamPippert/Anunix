#include <anx/epistemic.h>
#include <anx/cell.h>
#include <anx/identity.h>
#include <anx/effect_fence.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>
struct evidence_node {
	anx_oid_t oid, parents[ANX_EPISTEMIC_ROOTS];
	uint64_t version;
	uint32_t size, sensitivity, parent_count;
	uint8_t digest[32];
	uint32_t nodes, roots;
};
struct evidence_vote { bool cast, approve; uint32_t nodes, roots; };
struct epistemic_record {
	struct anx_epistemic_view view;
	struct anx_epistemic_spec spec;
	struct anx_cell *owner, *reviewers[ANX_EPISTEMIC_REVIEWERS];
	anx_oid_t owner_identity, reviewer_identity[ANX_EPISTEMIC_REVIEWERS], roots[ANX_EPISTEMIC_ROOTS];
	struct evidence_node nodes[ANX_EPISTEMIC_NODES];
	struct evidence_vote votes[ANX_EPISTEMIC_REVIEWERS];
	uint32_t node_count, root_count, proposal_size, sensitivity;
	uint64_t base_version;
};
static struct epistemic_record *records[ANX_EPISTEMIC_MAX];
static struct anx_spinlock epistemic_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct epistemic_record *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_EPISTEMIC_MAX; i++) if (records[i] && records[i]->view.id == id) return records[i];
	return NULL;
}
static uint32_t bits(uint32_t value)
{
	uint32_t count = 0;
	for (; value; value &= value - 1) count++;
	return count;
}
static int access(struct epistemic_record *r)
{
	if (!r) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, &r->view.owner) ? ANX_EPERM : ANX_OK;
}
/* The controller's evidence graph names dependencies; it does not attest external independence. */
static int snapshot(struct epistemic_record *r, const anx_oid_t *oid, uint32_t voter, struct evidence_node *out)
{
	if (!anx_uuid_compare(oid, &r->view.target)) return ANX_EINVAL;
	struct anx_state_object *object = anx_objstore_lookup(oid);
	if (!object) return ANX_ENOENT;
	struct evidence_node copy;
	anx_memset(&copy, 0, sizeof(copy)); copy.oid = *oid;
	anx_spin_lock(&object->lock);
	int ret = object->state != ANX_OBJ_SEALED || !object->version || !object->payload || !object->payload_size ||
		object->payload_size > 4096 || object->parent_count > ANX_EPISTEMIC_ROOTS ||
		(object->parent_count && !object->parent_oids) || object->access_policy.rule_count > ANX_MAX_ACCESS_RULES ||
		object->sensitivity > r->sensitivity ||
		(object->object_type != ANX_OBJ_BYTE_DATA && object->object_type != ANX_OBJ_STRUCTURED_DATA) ? ANX_EINVAL : ANX_OK;
	if (ret == ANX_OK) ret = anx_access_evaluate(&object->access_policy, &r->spec.reviewers[voter], &object->creator_cell, ANX_ACCESS_READ_PAYLOAD);
	if (ret == ANX_OK) {
		copy.version = object->version; copy.size = object->payload_size; copy.sensitivity = object->sensitivity;
		copy.parent_count = object->parent_count;
		if (copy.parent_count) anx_memcpy(copy.parents, object->parent_oids, copy.parent_count * sizeof(anx_oid_t));
		anx_sha256(object->payload, copy.size, copy.digest);
		const anx_cid_t *caller = anx_cell_current_id();
		if (caller && !anx_uuid_compare(caller, &r->spec.reviewers[voter])) ret = anx_effect_fence_observe_read(oid, object->sensitivity);
	}
	anx_spin_unlock(&object->lock); anx_objstore_release(object);
	if (ret == ANX_OK) *out = copy;
	return ret;
}
static int node_current(struct epistemic_record *r, uint32_t node, uint32_t voter)
{
	struct evidence_node now;
	const struct evidence_node *old = &r->nodes[node];
	int ret = snapshot(r, &old->oid, voter, &now);
	if (ret == ANX_OK && (now.version != old->version || now.size != old->size || now.sensitivity != old->sensitivity ||
	    now.parent_count != old->parent_count || anx_memcmp(now.parents, old->parents, sizeof(now.parents)) ||
	    anx_memcmp(now.digest, old->digest, 32))) ret = ANX_EBUSY;
	return ret;
}
static int capture(struct epistemic_record *r, const anx_oid_t *oid, uint32_t voter, uint32_t visiting, uint32_t depth, uint32_t *index)
{
	if (anx_uuid_is_nil(oid) || depth >= ANX_EPISTEMIC_ROOTS) return ANX_EINVAL;
	uint32_t i;
	for (i = 0; i < r->node_count; i++) if (!anx_uuid_compare(oid, &r->nodes[i].oid)) break;
	if (i < r->node_count) {
		if (visiting & (1U << i)) return ANX_EINVAL;
		int ret = node_current(r, i, voter);
		if (ret == ANX_OK) *index = i;
		return ret;
	}
	if (i == ANX_EPISTEMIC_NODES) return ANX_EFULL;
	struct evidence_node value;
	int ret = snapshot(r, oid, voter, &value);
	if (ret != ANX_OK) return ret;
	r->nodes[i] = value; r->node_count++;
	struct evidence_node *node = &r->nodes[i];
	node->nodes = 1U << i;
	if (!node->parent_count) {
		if (r->root_count == ANX_EPISTEMIC_ROOTS) return ANX_EFULL;
		r->roots[r->root_count] = *oid; node->roots = 1U << r->root_count++;
	}
	for (uint32_t j = 0; ret == ANX_OK && j < node->parent_count; j++) {
		uint32_t parent;
		ret = capture(r, &node->parents[j], voter, visiting | (1U << i), depth + 1, &parent);
		if (ret == ANX_OK) { node->nodes |= r->nodes[parent].nodes; node->roots |= r->nodes[parent].roots; }
	}
	if (ret == ANX_OK) *index = i;
	return ret;
}
static int voter_current(struct epistemic_record *r, uint32_t voter)
{
	anx_oid_t identity;
	int ret = anx_identity_admit(r->reviewers[voter], &identity);
	if (ret == ANX_OK && anx_uuid_compare(&identity, &r->reviewer_identity[voter])) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = anx_cell_check_scope(r->reviewers[voter]);
	return ret;
}
static bool vote_current(struct epistemic_record *r, uint32_t i)
{
	if (!r->votes[i].cast || !r->votes[i].approve || voter_current(r, i) != ANX_OK) return false;
	for (uint32_t j = 0; j < r->node_count; j++)
		if ((r->votes[i].nodes & (1U << j)) && node_current(r, j, i) != ANX_OK) return false;
	return true;
}
static void assess(struct epistemic_record *r)
{
	uint32_t valid = 0;
	for (uint32_t i = 0; i < r->spec.count; i++) if (vote_current(r, i)) valid |= 1U << i;
	r->view.valid_approvals = bits(valid); r->view.structural_cut = 0; r->view.roots = r->root_count;
	if (r->view.valid_approvals >= r->spec.threshold) {
		uint32_t minimum = ANX_EPISTEMIC_ROOTS + 1;
		for (uint32_t faults = 1; faults < (1U << r->root_count); faults++) {
			if (bits(faults) >= minimum) continue;
			uint32_t covered = 0;
			for (uint32_t i = 0; i < r->spec.count; i++)
				if ((valid & (1U << i)) && (r->votes[i].roots & faults)) covered |= 1U << i;
			if (bits(covered) >= r->spec.threshold) minimum = bits(faults);
		}
		if (minimum <= ANX_EPISTEMIC_ROOTS) r->view.structural_cut = minimum;
	}
	r->view.ready = r->view.valid_approvals >= r->spec.threshold && r->view.structural_cut >= r->spec.minimum_cut;
}
int anx_epistemic_begin(struct anx_object_handle *handle, const struct anx_epistemic_spec *spec, struct anx_epistemic_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!handle || !handle->obj || !spec || !out) return ANX_EINVAL;
	if (handle->mode != ANX_OPEN_WRITE && handle->mode != ANX_OPEN_READWRITE) return ANX_EPERM;
	struct epistemic_record *r = anx_zalloc(sizeof(*r));
	if (!r) return ANX_ENOMEM;
	r->spec = *spec;
	int ret = !r->spec.count || r->spec.count > ANX_EPISTEMIC_REVIEWERS || !r->spec.threshold ||
		r->spec.threshold > r->spec.count || !r->spec.minimum_cut || r->spec.minimum_cut > r->spec.threshold ? ANX_EINVAL : ANX_OK;
	struct anx_state_object *object = handle->obj;
	anx_spin_lock(&object->lock);
	struct anx_staged_mutation *stage = object->staged;
	if (ret == ANX_OK && (!stage || stage->epistemic_id || !stage->shadow_payload || !stage->shadow_size || stage->shadow_size > 4096 ||
	    stage->base_version != object->version || (uint32_t)object->sensitivity > ANX_SENSITIVITY_RESTRICTED ||
	    anx_uuid_is_nil(&stage->staging_cell))) ret = ANX_EBUSY;
	if (ret == ANX_OK) {
		r->view.owner = stage->staging_cell; r->view.target = object->oid; r->base_version = stage->base_version;
		r->proposal_size = stage->shadow_size; r->sensitivity = object->sensitivity;
		r->owner = anx_cell_store_lookup(&r->view.owner);
		ret = r->owner ? anx_identity_admit(r->owner, &r->owner_identity) : ANX_ENOENT;
	}
	for (uint32_t i = 0; ret == ANX_OK && i < ANX_EPISTEMIC_REVIEWERS; i++) {
		if (i >= r->spec.count) { if (!anx_uuid_is_nil(&r->spec.reviewers[i])) ret = ANX_EINVAL; continue; }
		if (anx_uuid_is_nil(&r->spec.reviewers[i]) || !anx_uuid_compare(&r->spec.reviewers[i], &r->view.owner)) { ret = ANX_EINVAL; break; }
		for (uint32_t j = 0; j < i; j++) if (!anx_uuid_compare(&r->spec.reviewers[i], &r->spec.reviewers[j])) ret = ANX_EINVAL;
		if (ret != ANX_OK) break;
		r->reviewers[i] = anx_cell_store_lookup(&r->spec.reviewers[i]);
		ret = r->reviewers[i] ? anx_identity_admit(r->reviewers[i], &r->reviewer_identity[i]) : ANX_ENOENT;
		if (ret == ANX_OK) ret = voter_current(r, i);
	}
	if (ret == ANX_OK) {
		anx_sha256(stage->shadow_payload, r->proposal_size, r->view.proposal_digest);
		r->view.threshold = r->spec.threshold; r->view.minimum_cut = r->spec.minimum_cut;
		bool flags; anx_spin_lock_irqsave(&epistemic_lock, &flags);
		uint32_t slot;
		for (slot = 0; slot < ANX_EPISTEMIC_MAX; slot++) if (!records[slot]) break;
		if (slot == ANX_EPISTEMIC_MAX || sequence == ~(uint64_t)0) ret = ANX_EFULL;
		else { r->view.id = ++sequence; records[slot] = r; stage->epistemic_id = r->view.id; *out = r->view; }
		anx_spin_unlock_irqrestore(&epistemic_lock, flags);
	}
	anx_spin_unlock(&object->lock);
	if (ret != ANX_OK) {
		if (r->owner) anx_cell_store_release(r->owner);
		for (uint32_t i = 0; i < ANX_EPISTEMIC_REVIEWERS; i++) if (r->reviewers[i]) anx_cell_store_release(r->reviewers[i]);
		anx_free(r);
	}
	return ret;
}
int anx_epistemic_vote(uint64_t id, const anx_oid_t *evidence, bool approve, struct anx_epistemic_view *out)
{
	const anx_cid_t *caller = anx_cell_current_id();
	if (!caller) return ANX_EPERM;
	if (!id || !evidence || !out) return ANX_EINVAL;
	anx_oid_t source = *evidence;
	bool flags; anx_spin_lock_irqsave(&epistemic_lock, &flags);
	struct epistemic_record *r = find(id);
	int ret = r ? ANX_OK : ANX_ENOENT; uint32_t voter = 0;
	if (ret == ANX_OK) {
		for (; voter < r->spec.count; voter++) if (!anx_uuid_compare(caller, &r->spec.reviewers[voter])) break;
		if (voter == r->spec.count) ret = ANX_EPERM;
		else if (r->view.state != ANX_EPISTEMIC_OPEN) ret = ANX_EBUSY;
		else if (r->votes[voter].cast) ret = ANX_EEXIST;
	}
	if (ret == ANX_OK) ret = voter_current(r, voter);
	uint32_t old_nodes = r ? r->node_count : 0, old_roots = r ? r->root_count : 0, root = 0;
	if (ret == ANX_OK) ret = capture(r, &source, voter, 0, 0, &root);
	if (ret == ANX_OK) for (uint32_t i = 0; i < r->node_count; i++)
		if ((r->nodes[root].nodes & (1U << i)) && node_current(r, i, voter) != ANX_OK) { ret = ANX_EBUSY; break; }
	if (ret == ANX_OK) {
		r->votes[voter] = (struct evidence_vote){true, approve, r->nodes[root].nodes, r->nodes[root].roots};
		r->view.votes++; assess(r); *out = r->view;
	} else if (r) {
		for (uint32_t i = old_nodes; i < r->node_count; i++) anx_memset(&r->nodes[i], 0, sizeof(r->nodes[i]));
		for (uint32_t i = old_roots; i < r->root_count; i++) r->roots[i] = ANX_UUID_NIL;
		r->node_count = old_nodes; r->root_count = old_roots;
	}
	anx_spin_unlock_irqrestore(&epistemic_lock, flags); return ret;
}
int anx_epistemic_get(uint64_t id, struct anx_epistemic_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&epistemic_lock, &flags);
	struct epistemic_record *r = find(id); int ret = access(r);
	if (ret == ANX_OK) { if (r->view.state == ANX_EPISTEMIC_OPEN) assess(r); *out = r->view; }
	anx_spin_unlock_irqrestore(&epistemic_lock, flags); return ret;
}
int anx_epistemic_stage_check(const struct anx_state_object *object)
{
	if (!object || !object->staged) return ANX_EINVAL;
	const struct anx_staged_mutation *stage = object->staged;
	if (!stage->epistemic_id) return ANX_OK;
	bool flags; anx_spin_lock_irqsave(&epistemic_lock, &flags);
	struct epistemic_record *r = find(stage->epistemic_id);
	int ret = access(r);
	if (ret == ANX_OK && (r->view.state != ANX_EPISTEMIC_OPEN || anx_uuid_compare(&r->view.target, &object->oid) ||
	    anx_uuid_compare(&r->view.owner, &stage->staging_cell) || r->base_version != stage->base_version ||
	    stage->shadow_size != r->proposal_size || !stage->shadow_payload || object->sensitivity != r->sensitivity)) ret = ANX_EBUSY;
	if (ret == ANX_OK) {
		uint8_t digest[32]; anx_sha256(stage->shadow_payload, r->proposal_size, digest);
		if (anx_memcmp(digest, r->view.proposal_digest, 32)) ret = ANX_EBUSY;
	}
	anx_oid_t identity;
	if (ret == ANX_OK) ret = anx_identity_admit(r->owner, &identity);
	if (ret == ANX_OK && anx_uuid_compare(&identity, &r->owner_identity)) ret = ANX_EBUSY;
	if (ret == ANX_OK) { assess(r); if (!r->view.ready) ret = ANX_EAUDIT; }
	anx_spin_unlock_irqrestore(&epistemic_lock, flags); return ret;
}
void anx_epistemic_stage_resolve(const struct anx_state_object *object, bool committed)
{
	if (!object || !object->staged || !object->staged->epistemic_id) return;
	bool flags; anx_spin_lock_irqsave(&epistemic_lock, &flags);
	struct epistemic_record *r = find(object->staged->epistemic_id);
	if (r && r->view.state == ANX_EPISTEMIC_OPEN && !anx_uuid_compare(&r->view.target, &object->oid) &&
	    !anx_uuid_compare(&r->view.owner, &object->staged->staging_cell)) {
		r->view.state = committed ? ANX_EPISTEMIC_COMMITTED : ANX_EPISTEMIC_ABORTED; r->view.ready = false;
	}
	anx_spin_unlock_irqrestore(&epistemic_lock, flags);
}
int anx_epistemic_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&epistemic_lock, &flags);
	struct epistemic_record *r = find(id);
	int ret = !r ? ANX_ENOENT : r->view.state == ANX_EPISTEMIC_OPEN ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_EPISTEMIC_MAX; i++) if (records[i] == r) records[i] = NULL;
		anx_cell_store_release(r->owner);
		for (uint32_t i = 0; i < r->spec.count; i++) anx_cell_store_release(r->reviewers[i]);
		anx_memset(r, 0, sizeof(*r)); anx_free(r);
	}
	anx_spin_unlock_irqrestore(&epistemic_lock, flags); return ret;
}
