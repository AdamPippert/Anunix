#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/epistemic.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
struct fixture072 {
	struct anx_epistemic_spec spec;
	struct anx_epistemic_view view, output, sentinel;
	struct anx_object_handle handle;
	struct anx_external_call call;
	anx_oid_t evidence, cycle;
	uint64_t version;
	uint32_t role, voter;
	bool shared, mixed;
};
static int active072(struct anx_external_call *call, void *arg)
{
	struct fixture072 *f = arg; (void)call;
	if (anx_epistemic_begin(&f->handle, &f->spec, &f->output) != ANX_EPERM ||
	    anx_epistemic_destroy(f->view.id) != ANX_EPERM) return -7202;
	if (f->role == 1) return anx_epistemic_vote(f->view.id, &f->evidence, true, &f->output) == ANX_EPERM &&
		anx_epistemic_get(f->view.id, &f->output) == ANX_EPERM && anx_object_commit(&f->handle) == ANX_EPERM ? ANX_OK : -7203;
	if (f->role == 2) {
		int ret = anx_epistemic_get(f->view.id, &f->view);
		if (ret != ANX_OK || f->view.valid_approvals != (f->shared ? (f->mixed ? 1U : 0U) : 2U) ||
		    f->view.structural_cut != (f->shared ? 0U : 2U)) return -7204;
		ret = anx_object_commit(&f->handle);
		if (ret != (f->shared ? ANX_EAUDIT : ANX_OK)) return -7205;
		if (f->shared) return f->handle.obj->staged && f->handle.obj->version == f->version &&
			!anx_memcmp(f->handle.obj->payload, "old!", 4) ? ANX_OK : -7206;
		return !f->handle.obj->staged && f->handle.obj->version == f->version + 1 &&
			!anx_memcmp(f->handle.obj->payload, "new!", 4) &&
			anx_epistemic_get(f->view.id, &f->view) == ANX_OK && f->view.state == ANX_EPISTEMIC_COMMITTED ? ANX_OK : -7207;
	}
	if (anx_epistemic_get(f->view.id, &f->output) != ANX_EPERM) return -7208;
	if (!f->voter && (anx_epistemic_vote(f->view.id, &f->view.target, true, &f->output) != ANX_EINVAL ||
	    anx_epistemic_vote(f->view.id, &f->cycle, true, &f->output) != ANX_EINVAL)) return -7209;
	int ret = anx_epistemic_vote(f->view.id, &f->evidence, true, &f->view);
	if (ret != ANX_OK || f->view.votes != f->voter + 1 ||
	    f->view.roots != (f->shared ? (f->mixed && f->voter == 2 ? 2U : 1U) : f->voter + 1)) return -7210;
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->sentinel = f->output;
	return anx_epistemic_vote(f->view.id, &f->evidence, false, &f->output) == ANX_EEXIST &&
		!anx_memcmp(&f->output, &f->sentinel, sizeof(f->output)) ? ANX_OK : -7211;
}
static int case072(bool shared, bool mixed)
{
	struct fixture072 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *reviewers[3] = {0}, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *target = NULL, *roots[3] = {0}, *evidence[3] = {0}, *cycle = NULL;
	struct anx_so_create_params params = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "old!", .payload_size = 4 };
	bool corrupted = false;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	f->shared = shared; f->mixed = mixed;
	f->spec = (struct anx_epistemic_spec){ .count = 3, .threshold = 2, .minimum_cut = 2 };
	anx_strlcpy(intent.name, "research-day-072", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	for (uint32_t i = 0; i < 3; i++) {
		ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &reviewers[i]);
		if (ret != ANX_OK) goto out;
		reviewers[i]->execution.allow_side_effects = true; f->spec.reviewers[i] = reviewers[i]->cid;
	}
	ret = anx_so_create(&params, &target);
	if (ret == ANX_OK) ret = anx_so_open(&target->oid, ANX_OPEN_READWRITE, &f->handle);
	if (ret == ANX_OK) ret = anx_object_stage(&f->handle, owner->cid);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&f->handle, "new!", 4);
	if (ret != ANX_OK) goto out;
	f->version = target->version;
	ret = -7201;
	if (anx_epistemic_begin(&f->handle, &f->spec, &f->view) != ANX_OK) goto out;
	if (anx_epistemic_begin(&f->handle, &f->spec, &f->output) != ANX_EBUSY ||
	    anx_epistemic_destroy(f->view.id) != ANX_EBUSY ||
	    anx_epistemic_vote(f->view.id, &target->oid, true, &f->output) != ANX_EPERM ||
	    anx_object_commit(&f->handle) != ANX_EAUDIT) { ret = -7212; goto out; }
	params.payload = "fact"; params.payload_size = 4;
	for (uint32_t i = 0; i < 3; i++) {
		ret = anx_so_create(&params, &roots[i]);
		if (ret == ANX_OK) ret = anx_so_seal(&roots[i]->oid);
		if (ret != ANX_OK) goto out;
	}
	params.payload = "derived";
	params.payload_size = 7; params.parent_count = 1;
	for (uint32_t i = 0; i < 3; i++) {
		params.parent_oids = &roots[shared ? (mixed && i == 2 ? 1 : 0) : i]->oid;
		ret = anx_so_create(&params, &evidence[i]);
		if (ret == ANX_OK) ret = anx_so_seal(&evidence[i]->oid);
		if (ret != ANX_OK) goto out;
	}
	anx_oid_t parents[2] = { roots[0]->oid, roots[1]->oid };
	params.parent_oids = parents; params.parent_count = 2;
	ret = anx_so_create(&params, &cycle);
	if (ret == ANX_OK) ret = anx_so_seal(&cycle->oid);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch072", active072, f);
	if (ret != ANX_OK) goto out;
	cycle->parent_oids[1] = cycle->oid; f->cycle = cycle->oid;
	anx_strlcpy(f->call.endpoint, "anxresearch072://inspect", sizeof(f->call.endpoint));
	for (uint32_t i = 0; i < 3; i++) {
		f->voter = i; f->evidence = evidence[i]->oid; reviewers[i]->ext_call = &f->call;
		ret = anx_cell_run(reviewers[i]);
		if (ret != ANX_OK) goto out;
	}
	ret = anx_epistemic_get(f->view.id, &f->view);
	if (ret != ANX_OK || f->view.valid_approvals != 3 || f->view.structural_cut != (shared ? 1U : 2U) ||
	    f->view.ready == shared) { ret = -7213; goto out; }
	f->output = f->view; f->output.structural_cut = 8; f->output.ready = true;
	if (shared && anx_object_commit(&f->handle) != ANX_EAUDIT) { ret = -7214; goto out; }
	if (!shared) {
		ret = anx_so_replace_payload(&f->handle, "evil", 4);
		if (ret != ANX_OK || anx_object_commit(&f->handle) != ANX_EBUSY || target->version != f->version ||
		    anx_memcmp(target->payload, "old!", 4)) { ret = -7215; goto out; }
		ret = anx_so_replace_payload(&f->handle, "new!", 4);
		if (ret != ANX_OK) goto out;
	}
	((uint8_t *)roots[0]->payload)[0] ^= 1; corrupted = true;
	if (!shared) {
		evidence[1]->parent_oids[0] = roots[0]->oid;
		ret = anx_object_commit(&f->handle);
		evidence[1]->parent_oids[0] = roots[1]->oid;
		if (ret != ANX_EAUDIT) { ret = -7216; goto out; }
		roots[1]->version++; ret = anx_object_commit(&f->handle); roots[1]->version--;
		if (ret != ANX_EAUDIT) { ret = -7216; goto out; }
		roots[1]->access_policy.rule_count = 1;
		roots[1]->access_policy.rules[0] = (struct anx_access_rule){ .operations = ANX_ACCESS_READ_PAYLOAD, .effect = ANX_EFFECT_DENY };
		ret = anx_object_commit(&f->handle); roots[1]->access_policy.rule_count = 0;
		if (ret != ANX_EAUDIT) { ret = -7216; goto out; }
	}
	f->role = 1; foreign->ext_call = &f->call; ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	f->role = 2; owner->ext_call = &f->call; ret = anx_cell_run(owner);
	if (ret != ANX_OK) goto out;
	if (shared) {
		ret = anx_object_abort(&f->handle);
		if (ret == ANX_OK) ret = anx_epistemic_get(f->view.id, &f->view);
		if (ret != ANX_OK || f->view.state != ANX_EPISTEMIC_ABORTED || target->version != f->version) { ret = -7217; goto out; }
	}
	ret = ANX_OK;
out:
	if (corrupted) ((uint8_t *)roots[0]->payload)[0] ^= 1;
	if (f->handle.obj) { if (f->handle.obj->staged) anx_object_abort(&f->handle); anx_so_close(&f->handle); }
	if (f->view.id && anx_epistemic_destroy(f->view.id) != ANX_OK && ret == ANX_OK) ret = -7218;
	anx_external_unregister_handler("anxresearch072");
	for (uint32_t i = 0; i < 3; i++) if (evidence[i]) { anx_so_delete(&evidence[i]->oid, false); anx_objstore_release(evidence[i]); }
	if (cycle) { anx_so_delete(&cycle->oid, false); anx_objstore_release(cycle); }
	for (uint32_t i = 0; i < 3; i++) if (roots[i]) { anx_so_delete(&roots[i]->oid, false); anx_objstore_release(roots[i]); }
	if (target) { anx_so_delete(&target->oid, false); anx_objstore_release(target); }
	for (uint32_t i = 0; i < 3; i++) if (reviewers[i]) anx_cell_destroy(reviewers[i]);
	if (foreign) anx_cell_destroy(foreign);
	if (owner) anx_cell_destroy(owner);
	if (ret != ANX_OK) kprintf("day072 native failure rc=%d shared=%u role=%u voter=%u\n", ret, shared, f->role, f->voter);
	anx_free(f); return ret;
}
int anx_research_day072(void)
{
	int ret = case072(true, false);
	if (ret == ANX_OK) ret = case072(true, true);
	if (ret == ANX_OK) ret = case072(false, false);
	if (ret == ANX_OK) kprintf("day072 shared_cut=1 mixed_cut=1 separate_cut=2 surviving_approvals=2 commit=checked\n");
	return ret;
}
#endif
