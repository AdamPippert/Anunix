/* Execute an evidence-based promotion workflow without resetting live stores. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workflow.h>
#include <anx/promotion_evidence.h>
#include <anx/engine.h>
#include <anx/string.h>
#include <anx/uuid.h>

static int store_evidence(struct anx_promotion_evidence *evidence, anx_oid_t *oid)
{
	struct anx_so_create_params params = {0};
	struct anx_state_object *object;
	int rc;

	params.object_type = ANX_OBJ_STRUCTURED_DATA;
	params.schema_uri = ANX_PROMOTION_EVIDENCE_SCHEMA;
	params.schema_version = "1";
	params.payload = evidence;
	params.payload_size = sizeof(*evidence);
	rc = anx_so_create(&params, &object);
	if (rc != ANX_OK)
		return rc;
	*oid = object->oid;
	anx_objstore_release(object);
	return ANX_OK;
}

static int check_decision(const anx_oid_t *oid, const anx_oid_t *source,
			  const struct anx_promotion_evidence *evidence, int result)
{
	struct anx_object_handle handle = {0};
	struct anx_promotion_decision decision;
	int rc = anx_so_open(oid, ANX_OPEN_READ, &handle);

	if (rc != ANX_OK)
		return rc;
	rc = -505;
	if (handle.obj->state == ANX_OBJ_SEALED &&
	    handle.obj->payload_size == sizeof(decision) &&
	    handle.obj->parent_count == 1 &&
	    anx_uuid_compare(&handle.obj->parent_oids[0], source) == 0 &&
	    anx_so_read_payload(&handle, 0, &decision, sizeof(decision)) == (int)sizeof(decision) &&
	    decision.magic == ANX_PROMOTION_DECISION_MAGIC && decision.version == 1 &&
	    decision.result == result && anx_uuid_compare(&decision.evidence_oid, source) == 0 &&
	    anx_memcmp(&decision.evidence, evidence, sizeof(*evidence)) == 0)
		rc = ANX_OK;
	anx_so_close(&handle);
	return rc;
}

int anx_research_day005(void)
{
	struct anx_capability *incumbent = NULL, *candidate = NULL;
	struct anx_promotion_evidence evidence = {0};
	struct anx_wf_node node = {0};
	struct anx_wf_object *wf = NULL;
	struct anx_object_handle handle = {0};
	anx_oid_t oid, source, original_engine;
	uint16_t id;
	int rc;

	rc = anx_cap_create("research-day-005-incumbent", "1", &incumbent);
	if (rc != ANX_OK)
		goto out;
	rc = anx_cap_validate(incumbent);
	if (rc != ANX_OK)
		goto out;
	rc = anx_cap_install(incumbent);
	if (rc != ANX_OK)
		goto out;
	original_engine = incumbent->installed_engine_id;
	rc = anx_cap_create("research-day-005-candidate", "2", &candidate);
	if (rc != ANX_OK)
		goto out;
	candidate->supersedes_oid = incumbent->cap_oid;
	rc = anx_cap_validate(candidate);
	if (rc != ANX_OK)
		goto out;
	evidence.magic = ANX_PROMOTION_EVIDENCE_MAGIC;
	evidence.version = 1;
	evidence.candidate_oid = candidate->cap_oid;
	evidence.incumbent_oid = incumbent->cap_oid;
	evidence.candidates_tried = 1;
	evidence.trial.n = 1;
	evidence.trial.incumbent_scores[0] = evidence.trial.candidate_scores[0] = 50;
	rc = store_evidence(&evidence, &source);
	if (rc != ANX_OK)
		goto out;
	rc = anx_wf_create("research-day-005", "promotion with evidence", &oid);
	if (rc != ANX_OK)
		goto out;
	wf = anx_wf_object_get(&oid);
	node.kind = ANX_WF_NODE_STATE_REF;
	node.params.state_ref.obj_oid = source;
	node.port_count = 1;
	node.ports[0].dir = ANX_WF_PORT_OUT;
	rc = anx_wf_node_add(&oid, &node, &id);
	if (rc != ANX_OK)
		goto out;
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_CAP_PROMOTION;
	node.port_count = 2;
	node.ports[0].dir = ANX_WF_PORT_IN;
	node.ports[1].dir = ANX_WF_PORT_OUT;
	rc = anx_wf_node_add(&oid, &node, &id);
	if (rc != ANX_OK)
		goto out;
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_OUTPUT;
	node.port_count = 1;
	node.ports[0].dir = ANX_WF_PORT_IN;
	rc = anx_wf_node_add(&oid, &node, &id);
	if (rc != ANX_OK)
		goto out;
	if (anx_wf_edge_add(&oid, 1, 0, 2, 0) != ANX_OK ||
	    anx_wf_edge_add(&oid, 2, 1, 3, 0) != ANX_OK) {
		rc = -500;
		goto out;
	}
	/* No implicit installation authority from the bundle or evidence. */
	if (wf->policy.allow_capability_install || anx_wf_run(&oid, NULL) != ANX_EPERM ||
	    candidate->status != ANX_CAP_VALIDATED) {
		rc = -501;
		goto out;
	}
	wf->policy.allow_capability_install = true;
	if (anx_wf_run(&oid, NULL) != ANX_EPERM || candidate->status != ANX_CAP_VALIDATED) {
		rc = -502;
		goto out;
	}
	rc = anx_so_seal(&source);
	if (rc != ANX_OK)
		goto out;
	if (anx_wf_run(&oid, NULL) != ANX_EPERM || wf->run_state != ANX_WF_RUN_SUSPENDED ||
	    candidate->status != ANX_CAP_VALIDATED || incumbent->status != ANX_CAP_INSTALLED ||
	    anx_uuid_compare(&incumbent->installed_engine_id, &original_engine) != 0 ||
	    !anx_engine_lookup(&original_engine) || wf->trace_entry_count != 2) {
		rc = -503;
		goto out;
	}
	rc = check_decision(&wf->trace_entries[1].trace_oid, &source, &evidence, ANX_EPERM);
	if (rc != ANX_OK)
		goto out;
	if (anx_so_open(&source, ANX_OPEN_WRITE, &handle) != ANX_EPERM) {
		anx_so_close(&handle);
		rc = -504;
		goto out;
	}
	/* New sealed evidence can justify a later installation. */
	evidence.trial.candidate_scores[0] = 75;
	anx_uuid_generate(&evidence.incumbent_oid);
	rc = store_evidence(&evidence, &source);
	if (rc != ANX_OK)
		goto out;
	rc = anx_so_seal(&source);
	if (rc != ANX_OK)
		goto out;
	wf->nodes[0].params.state_ref.obj_oid = source;
	if (anx_wf_run(&oid, NULL) != ANX_EINVAL || candidate->status != ANX_CAP_VALIDATED ||
	    wf->trace_entry_count != 2) {
		rc = -507;
		goto out;
	}
	rc = check_decision(&wf->trace_entries[1].trace_oid, &source, &evidence, ANX_EINVAL);
	if (rc != ANX_OK)
		goto out;
	evidence.incumbent_oid = incumbent->cap_oid;
	rc = store_evidence(&evidence, &source);
	if (rc != ANX_OK)
		goto out;
	rc = anx_so_seal(&source);
	if (rc != ANX_OK)
		goto out;
	wf->nodes[0].params.state_ref.obj_oid = source;
	if (anx_wf_run(&oid, NULL) != ANX_OK || wf->run_state != ANX_WF_RUN_COMPLETED ||
	    candidate->status != ANX_CAP_INSTALLED || wf->output_count != 1) {
		rc = -506;
		goto out;
	}
	rc = check_decision(&wf->output_oids[0], &source, &evidence, ANX_OK);
out:
	if (wf)
		anx_wf_destroy(&wf->oid);
	if (candidate) {
		if (candidate->status == ANX_CAP_INSTALLED)
			anx_cap_uninstall(candidate);
		anx_cap_transition(candidate, ANX_CAP_RETIRED);
	}
	if (incumbent) {
		if (incumbent->status == ANX_CAP_INSTALLED)
			anx_cap_uninstall(incumbent);
		anx_cap_transition(incumbent, ANX_CAP_RETIRED);
	}
	return rc;
}
#endif
