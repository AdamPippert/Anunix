/* Preserve the evidence and result of a workflow promotion attempt. */
#include <anx/promotion_evidence.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>

int anx_cap_install_evidence(const anx_oid_t *evidence_oid,
			     anx_oid_t *decision_oid_out)
{
	struct anx_object_handle input, output;
	struct anx_promotion_decision decision = {0};
	struct anx_promotion_evidence *evidence = &decision.evidence;
	struct anx_so_create_params params = {0};
	struct anx_state_object *record;
	struct anx_capability *candidate;
	int ret, written, sealed;

	if (!decision_oid_out)
		return ANX_EINVAL;
	*decision_oid_out = ANX_UUID_NIL;
	if (!evidence_oid)
		return ANX_EINVAL;
	ret = anx_so_open(evidence_oid, ANX_OPEN_READ, &input);
	if (ret != ANX_OK)
		return ret;
	if (input.obj->state != ANX_OBJ_SEALED)
		ret = ANX_EPERM;
	else if (input.obj->object_type != ANX_OBJ_STRUCTURED_DATA ||
		 input.obj->payload_size != sizeof(*evidence) ||
		 anx_strcmp(input.obj->schema_uri, ANX_PROMOTION_EVIDENCE_SCHEMA) != 0 ||
		 anx_strcmp(input.obj->schema_version, "1") != 0)
		ret = ANX_EINVAL;
	else {
		ret = anx_so_read_payload(&input, 0, evidence, sizeof(*evidence));
		ret = ret == (int)sizeof(*evidence) ? ANX_OK : ANX_EINVAL;
	}
	anx_so_close(&input);
	if (ret != ANX_OK)
		return ret;
	if (evidence->magic != ANX_PROMOTION_EVIDENCE_MAGIC || evidence->version != 1)
		return ANX_EINVAL;

	decision.magic = ANX_PROMOTION_DECISION_MAGIC;
	decision.version = 1;
	decision.evidence_oid = *evidence_oid;
	decision.result = ANX_EBUSY; /* Prepared; no installation has run yet. */
	params.object_type = ANX_OBJ_EXECUTION_TRACE;
	params.schema_uri = ANX_PROMOTION_DECISION_SCHEMA;
	params.schema_version = "1";
	params.payload = &decision;
	params.payload_size = sizeof(decision);
	params.parent_oids = evidence_oid;
	params.parent_count = 1;
	ret = anx_so_create(&params, &record);
	if (ret != ANX_OK)
		return ret;
	*decision_oid_out = record->oid;
	/* Reserve the full record and acquire its write handle before mutation. */
	ret = anx_so_open(&record->oid, ANX_OPEN_WRITE, &output);
	if (ret != ANX_OK) {
		anx_objstore_release(record);
		return ret;
	}
	candidate = anx_cap_lookup(&evidence->candidate_oid);
	if (!candidate)
		ret = ANX_ENOENT;
	else if (anx_uuid_compare(&candidate->supersedes_oid, &evidence->incumbent_oid) != 0)
		ret = ANX_EINVAL;
	else
		ret = anx_cap_install_gated(candidate, &evidence->trial, evidence->candidates_tried);
	decision.result = ret;
	written = anx_so_write_payload(&output, 0, &decision, sizeof(decision));
	anx_so_close(&output);
	sealed = anx_so_seal(&record->oid);
	anx_objstore_release(record);
	if (written != (int)sizeof(decision))
		return written < 0 ? written : ANX_EIO;
	return sealed == ANX_OK ? ret : sealed;
}
