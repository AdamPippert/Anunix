/* Capability improvements cannot grant their own execution authority. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/capability.h>
#include <anx/cell.h>
#include <anx/engine.h>
#include <anx/external_call.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/alloc.h>

struct authority_context {
	struct anx_capability *draft, *ready;
	int expected_install;
};

static int authority_handler(struct anx_external_call *call, void *context)
{
	struct authority_context *state = context;
	(void)call;
	if (anx_cap_set_authority_ceiling(state->draft, ANX_CAP_AUTH_ALL) != ANX_EPERM)
		return -1910;
	if (anx_cap_install(state->ready) != state->expected_install)
		return -1911;
	return ANX_OK;
}

static void retire_cap(struct anx_capability *cap)
{
	if (!cap)
		return;
	if (cap->status == ANX_CAP_INSTALLED || cap->status == ANX_CAP_SUSPENDED)
		anx_cap_uninstall(cap);
	anx_cap_transition(cap, ANX_CAP_RETIRED);
}

int anx_research_day019(void)
{
	struct anx_capability *denied = NULL, *incumbent = NULL, *candidate = NULL;
	struct anx_capability *draft = NULL, *ready = NULL, *next = NULL;
	struct anx_cell *caller = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_promotion_trial trial = {0};
	struct authority_context state = {0};
	anx_eid_t original;
	int rc;

	rc = anx_cap_create("research-day-019-denied", "1", &denied);
	if (rc != ANX_OK)
		goto out;
	denied->required_authority = ANX_CAP_AUTH_NETWORK;
	rc = anx_cap_validate(denied);
	if (rc != ANX_OK)
		goto out;
	rc = -1901;
	if (anx_cap_install(denied) != ANX_EPERM || denied->status != ANX_CAP_VALIDATED ||
	    !anx_uuid_is_nil(&denied->installed_engine_id))
		goto out;
	rc = anx_cap_create("research-day-019-incumbent", "1", &incumbent);
	if (rc != ANX_OK)
		goto out;
	incumbent->required_authority = ANX_CAP_AUTH_NETWORK;
	rc = -1902;
	if (anx_cap_set_authority_ceiling(incumbent, ANX_CAP_AUTH_NETWORK | ANX_CAP_AUTH_REMOTE_MODEL) != ANX_OK ||
	    anx_cap_validate(incumbent) != ANX_OK ||
	    anx_cap_set_authority_ceiling(incumbent, ANX_CAP_AUTH_ALL) != ANX_EPERM ||
	    anx_cap_install(incumbent) != ANX_OK)
		goto out;
	original = incumbent->installed_engine_id;
	rc = anx_cap_create("research-day-019-candidate", "2", &candidate);
	if (rc != ANX_OK)
		goto out;
	candidate->supersedes_oid = incumbent->cap_oid;
	candidate->required_authority = ANX_CAP_AUTH_NETWORK | ANX_CAP_AUTH_SIDE_EFFECT;
	rc = anx_cap_validate(candidate);
	if (rc != ANX_OK)
		goto out;
	trial.n = 2;
	trial.incumbent_scores[0] = trial.incumbent_scores[1] = 50;
	trial.candidate_scores[0] = trial.candidate_scores[1] = 80;
	rc = -1903;
	if (anx_cap_install_gated(candidate, &trial, 1) != ANX_EPERM ||
	    anx_cap_install(candidate) != ANX_EPERM)
		goto out;
	/* Mutable incumbent declarations cannot enlarge its installed scope. */
	incumbent->required_authority = ANX_CAP_AUTH_ALL;
	rc = -1904;
	if (anx_cap_install_gated(candidate, &trial, 1) != ANX_EPERM)
		goto out;
	candidate->required_authority = ANX_CAP_AUTH_NETWORK | ANX_CAP_AUTH_REMOTE_MODEL;
	if (anx_cap_install_gated(candidate, &trial, 1) != ANX_EPERM ||
	    candidate->status != ANX_CAP_VALIDATED ||
	    !anx_uuid_is_nil(&candidate->installed_engine_id) ||
	    incumbent->status != ANX_CAP_INSTALLED ||
	    anx_uuid_compare(&original, &incumbent->installed_engine_id) ||
	    !anx_engine_lookup(&original))
		goto out;
	candidate->required_authority = ANX_CAP_AUTH_NETWORK;
	trial.candidate_scores[0] = 50;
	rc = -1905;
	if (anx_cap_install_gated(candidate, &trial, 1) != ANX_EPERM)
		goto out;
	trial.candidate_scores[0] = 80;
	if (anx_cap_install_gated(candidate, &trial, 1) != ANX_OK ||
	    candidate->status != ANX_CAP_INSTALLED || !anx_engine_lookup(&candidate->installed_engine_id))
		goto out;
	candidate->required_authority = ANX_CAP_AUTH_ALL;
	rc = anx_cap_create("research-day-019-next", "3", &next);
	if (rc != ANX_OK)
		goto out;
	next->supersedes_oid = candidate->cap_oid;
	next->required_authority = ANX_CAP_AUTH_NETWORK | ANX_CAP_AUTH_SIDE_EFFECT;
	rc = -1906;
	if (anx_cap_validate(next) != ANX_OK || anx_cap_install_gated(next, &trial, 1) != ANX_EPERM)
		goto out;
	rc = anx_cap_create("research-day-019-draft", "1", &draft);
	if (rc != ANX_OK)
		goto out;
	draft->required_authority = ANX_CAP_AUTH_ALL + 1;
	rc = -1907;
	if (anx_cap_set_authority_ceiling(draft, ANX_CAP_AUTH_ALL + 1) != ANX_EINVAL ||
	    anx_cap_validate(draft) != ANX_EINVAL || draft->status != ANX_CAP_DRAFT)
		goto out;
	draft->required_authority = ANX_CAP_AUTH_NETWORK;
	rc = anx_cap_create("research-day-019-ready", "1", &ready);
	if (rc != ANX_OK)
		goto out;
	ready->required_authority = ANX_CAP_AUTH_NETWORK;
	rc = -1908;
	if (anx_cap_set_authority_ceiling(ready, ANX_CAP_AUTH_NETWORK) != ANX_OK ||
	    anx_cap_validate(ready) != ANX_OK)
		goto out;
	state.draft = draft;
	state.ready = ready;
	state.expected_install = ANX_EPERM;
	rc = anx_external_register_handler("anxresearch019", authority_handler, &state);
	if (rc != ANX_OK)
		goto out;
	call = anx_zalloc(sizeof(*call));
	rc = ANX_ENOMEM;
	if (!call)
		goto out;
	anx_strlcpy(call->endpoint, "anxresearch019://promotion", sizeof(call->endpoint));
	anx_strlcpy(intent.name, "research-day-019-caller", sizeof(intent.name));
	for (int network = 0; network < 2; network++) {
		rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
		if (rc != ANX_OK)
			goto out;
		caller->execution.allow_side_effects = true;
		caller->execution.allow_network = network != 0;
		caller->ext_call = call;
		state.expected_install = network ? ANX_OK : ANX_EPERM;
		rc = anx_cell_run(caller);
		if (rc != ANX_OK)
			goto out;
		anx_cell_destroy(caller);
		caller = NULL;
	}
	rc = -1912;
	if (ready->status != ANX_CAP_INSTALLED || anx_cap_validate(draft) != ANX_OK ||
	    anx_cap_install(draft) != ANX_EPERM)
		goto out;
	rc = ANX_OK;
out:
	if (caller)
		anx_cell_destroy(caller);
	if (call)
		anx_free(call);
	anx_external_unregister_handler("anxresearch019");
	retire_cap(next);
	retire_cap(ready);
	retire_cap(draft);
	retire_cap(candidate);
	retire_cap(incumbent);
	retire_cap(denied);
	return rc;
}
#endif
