/* A parameter grant cannot replace implementations or widen revision authority. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/revision.h>
#include <anx/tuning.h>
#include <anx/capability.h>
#include <anx/engine.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct revision_context {
	struct anx_capability *candidate, *incumbent;
	struct anx_revision_lease_view lease;
	struct anx_cell *other;
	uint64_t trial;
	uint32_t mode;
};

static int revision_handler(struct anx_external_call *call, void *arg)
{
	struct revision_context *c = arg;
	struct anx_revision_lease_view view;
	struct anx_route_tuning_state state;
	struct anx_route_tuning_action action = {0};
	uint64_t trial = 0;
	int ret;
	(void)call;
	if (c->mode == 1)
		return anx_route_tuning_finish(c->trial, ANX_ROUTE_TRIAL_ACCEPT) == ANX_EPERM ? ANX_OK : -3305;
	if (c->mode == 2)
		return anx_cap_install(c->candidate);
	if (c->mode == 3) {
		if (anx_route_tuning_begin(&action, &trial) != ANX_EPERM ||
		    anx_cap_uninstall(c->candidate) != ANX_EPERM) return -3306;
		return ANX_OK;
	}
	if (anx_cap_install(c->candidate) != ANX_EPERM || c->candidate->status != ANX_CAP_VALIDATED ||
	    !anx_uuid_is_nil(&c->candidate->installed_engine_id)) return -3301;
	if (anx_cap_uninstall(c->incumbent) != ANX_EPERM ||
	    anx_cap_transition(c->incumbent, ANX_CAP_SUSPENDED) != ANX_EPERM ||
	    !anx_engine_lookup(&c->incumbent->installed_engine_id)) return -3302;
	if (anx_cap_set_authority_ceiling(c->candidate, ANX_CAP_AUTH_ALL) != ANX_EPERM ||
	    anx_revision_lease_create(ANX_REVISION_IMPLEMENTATION, &view) != ANX_EPERM ||
	    anx_revision_lease_revoke(&c->lease.id) != ANX_EPERM ||
	    anx_revision_lease_bind(c->other, &c->lease.id) != ANX_EPERM) return -3303;
	view = c->lease;
	view.ceiling = ANX_REVISION_IMPLEMENTATION;
	if (anx_revision_lease_get(&view.id, &view) != ANX_OK || view.ceiling != ANX_REVISION_PARAMETERS)
		return -3303;
	ret = anx_route_tuning_snapshot(&state);
	if (ret != ANX_OK) return ret;
	action.schema = 1;
	action.expected_generation = state.generation;
	action.weights = state.weights;
	action.weights.gpu_cost_divisor = 0;
	if (anx_route_tuning_begin(&action, &trial) != ANX_EINVAL || trial) return -3304;
	action.weights = state.weights;
	action.weights.locality_bonus = state.weights.locality_bonus == 20 ? 21 : 20;
	ret = anx_route_tuning_begin(&action, &trial);
	if (ret != ANX_OK) return ret;
	ret = anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT);
	if (ret != ANX_OK) return ret;
	ret = anx_route_tuning_snapshot(&state);
	if (ret != ANX_OK) return ret;
	action.expected_generation = state.generation;
	return anx_route_tuning_begin(&action, &c->trial);
}

static void retire(struct anx_capability *cap)
{
	if (!cap) return;
	if (cap->status == ANX_CAP_INSTALLED || cap->status == ANX_CAP_SUSPENDED) anx_cap_uninstall(cap);
	anx_cap_transition(cap, ANX_CAP_RETIRED);
}

int anx_research_day033(void)
{
	struct anx_cell *root = NULL, *child = NULL, *other = NULL, *installer = NULL, *revoked = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_revision_lease_view parameters, implementation;
	struct revision_context c = {0};
	struct anx_external_call *call = NULL;
	struct anx_route_tuning_state state;
	int ret = anx_revision_lease_create(ANX_REVISION_PARAMETERS, &parameters);
	if (ret == ANX_OK) ret = anx_revision_lease_create(ANX_REVISION_IMPLEMENTATION, &implementation);
	if (ret == ANX_OK) ret = anx_cap_create("research-day-033-candidate", "1", &c.candidate);
	if (ret == ANX_OK) ret = anx_cap_validate(c.candidate);
	if (ret == ANX_OK) ret = anx_cap_create("research-day-033-incumbent", "1", &c.incumbent);
	if (ret == ANX_OK) ret = anx_cap_validate(c.incumbent);
	if (ret == ANX_OK) ret = anx_cap_install(c.incumbent);
	if (ret != ANX_OK) goto out;
	ret = -3307;
	if (anx_revision_lease_create((enum anx_revision_class)3, &c.lease) != ANX_EINVAL) goto out;
	ret = anx_external_register_handler("anxresearch033", revision_handler, &c);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch033://revise", sizeof(call->endpoint));
	anx_strlcpy(intent.name, "research-day-033", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &root);
	if (ret != ANX_OK) goto out;
	root->execution.allow_side_effects = root->execution.allow_recursive_cells = true;
	ret = anx_revision_lease_bind(root, &parameters.id);
	if (ret == ANX_OK) ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &child);
	if (ret != ANX_OK) goto out;
	ret = -3308;
	if (anx_uuid_compare(&child->revision_lease_id, &parameters.id)) goto out;
	struct anx_cell **cells[] = {&other, &installer, &revoked};
	for (uint32_t i = 0; i < 3; i++) {
		ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, cells[i]);
		if (ret != ANX_OK) goto out;
		(*cells[i])->execution.allow_side_effects = true;
		(*cells[i])->ext_call = call;
		ret = anx_revision_lease_bind(*cells[i], i ? &implementation.id : &parameters.id);
		if (ret != ANX_OK) goto out;
	}
	child->ext_call = call;
	c.other = other;
	c.lease = parameters;
	ret = anx_cell_run(child);
	if (ret != ANX_OK) goto out;
	c.mode = 1;
	ret = anx_cell_run(other);
	if (ret == ANX_OK) ret = anx_route_tuning_finish(c.trial, ANX_ROUTE_TRIAL_REJECT);
	if (ret != ANX_OK) goto out;
	c.trial = 0;
	c.mode = 2;
	ret = anx_cell_run(installer);
	if (ret == ANX_OK) ret = anx_revision_lease_revoke(&implementation.id);
	if (ret != ANX_OK) goto out;
	c.mode = 3;
	ret = anx_cell_run(revoked);
out:
	if (anx_route_tuning_snapshot(&state) == ANX_OK && state.trial_active && c.trial)
		anx_route_tuning_finish(c.trial, ANX_ROUTE_TRIAL_REJECT);
	if (child) anx_cell_destroy(child);
	if (root) anx_cell_destroy(root);
	if (other) anx_cell_destroy(other);
	if (installer) anx_cell_destroy(installer);
	if (revoked) anx_cell_destroy(revoked);
	if (call) anx_free(call);
	anx_external_unregister_handler("anxresearch033");
	retire(c.candidate);
	retire(c.incumbent);
	return ret;
}
#endif
