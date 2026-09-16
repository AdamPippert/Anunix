/* Semantic drift must not consume or edit a suspended continuation. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workflow_semantic.h>
#include <anx/state_object.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/alloc.h>

int anx_research_day043(void)
{
	struct anx_state_object *objects[3] = {0}, *manifest_obj = NULL;
	const char *names[] = {"policy", "embedding", "index"};
	const char *payloads[] = {"policy-v1", "embed-v1", "index-v1"};
	enum anx_semantic_kind kinds[] = {ANX_SEMANTIC_POLICY, ANX_SEMANTIC_EMBEDDING, ANX_SEMANTIC_INDEX};
	struct anx_so_create_params p = {0};
	struct anx_object_handle h = {0};
	struct anx_wf_node node = { .kind = ANX_WF_NODE_CAP_PROMOTION }, replacement = { .kind = ANX_WF_NODE_TRIGGER };
	struct anx_semantic_spec *spec = NULL;
	anx_oid_t oid = ANX_UUID_NIL, manifests[3] = {0}, checkpoint = ANX_UUID_NIL, resolved = ANX_UUID_NIL;
	uint16_t id;
	uint8_t before[32], after[32];
	int ret = ANX_ENOMEM;
	spec = anx_zalloc(sizeof(*spec));
	if (!spec) goto out;
	spec->resource_count = 3; spec->requires_count = 1;
	for (uint32_t i = 0; i < 3; i++) {
		p.object_type = ANX_OBJ_BYTE_DATA; p.payload = payloads[i]; p.payload_size = anx_strlen(payloads[i]);
		ret = anx_so_create(&p, &objects[i]);
		if (ret == ANX_OK && i) ret = anx_so_seal(&objects[i]->oid);
		if (ret != ANX_OK) goto out;
		anx_strlcpy(spec->resources[i].name, names[i], ANX_SEMANTIC_NAME_MAX);
		spec->resources[i].kind = kinds[i]; spec->resources[i].oid = objects[i]->oid; spec->resources[i].version = objects[i]->version;
	}
	spec->requires[0] = (struct anx_semantic_requires){2, 1, objects[1]->oid, objects[1]->version};
	ret = anx_so_open(&objects[0]->oid, ANX_OPEN_READWRITE, &h);
	if (ret == ANX_OK) ret = anx_wf_create("research-day-043", NULL, &oid);
	if (ret == ANX_OK) ret = anx_wf_node_add(&oid, &node, &id);
	if (ret != ANX_OK) goto out;
	struct anx_wf_object *wf = anx_wf_object_get(&oid);
	spec->requires[0].expected_oid = objects[0]->oid;
	ret = -4302;
	if (anx_wf_semantic_bind(&oid, spec, &manifests[0]) != ANX_EPERM || !anx_uuid_is_nil(&manifests[0])) goto out;
	spec->requires[0].expected_oid = objects[1]->oid;
	spec->requires[0].expected_version++;
	ret = -4302;
	if (anx_wf_semantic_bind(&oid, spec, &manifests[0]) != ANX_EPERM || !anx_uuid_is_nil(&manifests[0])) goto out;
	spec->resources[1].version++;
	if (anx_wf_semantic_bind(&oid, spec, &manifests[0]) != ANX_EBUSY || !anx_uuid_is_nil(&manifests[0])) goto out;
	spec->resources[1].version--; spec->requires[0].expected_version--;
	ret = anx_wf_semantic_bind(&oid, spec, &manifests[0]);
	if (ret != ANX_OK) goto out;
	ret = -4303;
	if (anx_wf_semantic_resolve(&oid, "policy", &resolved) != ANX_OK || anx_uuid_compare(&resolved, &objects[0]->oid)) goto out;
	resolved = ANX_UUID_NIL;
	if (anx_wf_semantic_resolve(&oid, "unknown", &resolved) != ANX_ENOENT || !anx_uuid_is_nil(&resolved)) goto out;
	/* Trusted fixture corruption must not turn a public manifest into an issued one. */
	manifest_obj = anx_objstore_lookup(&manifests[0]);
	if (!manifest_obj || manifest_obj->state != ANX_OBJ_SEALED) goto out;
	((uint8_t *)manifest_obj->payload)[0] ^= 1;
	int corrupted = anx_wf_semantic_check(wf);
	((uint8_t *)manifest_obj->payload)[0] ^= 1;
	if (corrupted != ANX_EPERM || anx_wf_semantic_check(wf) != ANX_OK) goto out;
	anx_objstore_release(manifest_obj); manifest_obj = NULL;
	ret = -4300;
	if (anx_wf_run(&oid, NULL) != ANX_EPERM || !wf->continuation) goto out;
	struct anx_wf_continuation *saved = wf->continuation;
	struct anx_wf_trace_entry *trace = wf->trace_entries;
	uint32_t trace_count = wf->trace_entry_count;
	anx_sha256(saved, sizeof(*saved), before);
	objects[0]->access_policy.rule_count = 1;
	objects[0]->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	objects[0]->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	ret = -4304;
	if (anx_wf_resume(&oid, ANX_WF_RESUME_SKIP, NULL) != ANX_EPERM || wf->continuation != saved) goto out;
	objects[0]->access_policy.rule_count = 0;
	ret = anx_so_replace_payload(&h, "policy-v2", 9);
	if (ret != ANX_OK) goto out;
	ret = -4301;
	if (anx_wf_resume(&oid, ANX_WF_RESUME_SKIP, NULL) != ANX_EBUSY ||
	    anx_wf_resume(&oid, ANX_WF_RESUME_RETRY, NULL) != ANX_EBUSY ||
	    anx_wf_resume(&oid, ANX_WF_RESUME_REPLACE, &replacement) != ANX_EBUSY) goto out;
	anx_sha256(wf->continuation, sizeof(*saved), after);
	ret = -4305;
	if (wf->continuation != saved || anx_memcmp(before, after, sizeof(before)) ||
	    wf->trace_entries != trace || wf->trace_entry_count != trace_count ||
	    wf->run_state != ANX_WF_RUN_SUSPENDED || wf->nodes[0].kind != ANX_WF_NODE_CAP_PROMOTION ||
	    anx_wf_checkpoint_save(&oid, &checkpoint) != ANX_EBUSY || !anx_uuid_is_nil(&checkpoint)) goto out;
	spec->resources[0].version = objects[0]->version;
	if (anx_wf_semantic_bind(&oid, spec, &manifests[1]) != ANX_EBUSY || !anx_uuid_is_nil(&manifests[1])) goto out;
	ret = anx_wf_resume(&oid, ANX_WF_RESUME_ABORT, NULL);
	if (ret == ANX_OK) ret = anx_wf_semantic_bind(&oid, spec, &manifests[1]);
	if (ret != ANX_OK) goto out;
	ret = -4306;
	if (anx_wf_run(&oid, NULL) != ANX_EPERM ||
	    anx_wf_resume(&oid, ANX_WF_RESUME_REPLACE, &replacement) != ANX_EPERM ||
	    wf->nodes[0].kind != ANX_WF_NODE_CAP_PROMOTION) goto out;
	ret = anx_wf_checkpoint_save(&oid, &checkpoint);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&h, "policy-v3", 9);
	if (ret != ANX_OK) goto out;
	ret = -4307;
	if (anx_wf_checkpoint_restore(&oid, &checkpoint) != ANX_EBUSY || wf->continuation || wf->trace_entries ||
	    wf->checkpoint_consumed || anx_uuid_compare(&checkpoint, &wf->checkpoint_oid)) goto out;
	ret = anx_wf_resume(&oid, ANX_WF_RESUME_ABORT, NULL);
	if (ret != ANX_OK) goto out;
	anx_so_delete(&checkpoint, false); checkpoint = ANX_UUID_NIL;
	spec->resources[0].version = objects[0]->version;
	ret = anx_wf_semantic_bind(&oid, spec, &manifests[2]);
	if (ret != ANX_OK) goto out;
	ret = -4308;
	if (anx_wf_run(&oid, NULL) != ANX_EPERM) goto out;
	ret = anx_wf_checkpoint_save(&oid, &checkpoint);
	if (ret == ANX_OK) ret = anx_wf_checkpoint_restore(&oid, &checkpoint);
	if (ret == ANX_OK) ret = anx_wf_resume(&oid, ANX_WF_RESUME_SKIP, NULL);
	if (ret != ANX_OK) goto out;
	ret = -4309;
	if (wf->run_state != ANX_WF_RUN_COMPLETED || wf->continuation) goto out;
	wf->nodes[0].label[0] = 'X';
	if (anx_wf_run(&oid, NULL) != ANX_EBUSY || wf->run_state != ANX_WF_RUN_COMPLETED) goto out;
	wf->nodes[0].label[0] = 0;
	ret = anx_so_delete(&manifests[2], false);
	if (ret != ANX_OK) goto out;
	ret = -4310;
	if (anx_wf_semantic_check(wf) != ANX_ENOENT) goto out;
	ret = ANX_OK;
out:
	if (manifest_obj) anx_objstore_release(manifest_obj);
	if (!anx_uuid_is_nil(&oid)) anx_wf_destroy(&oid);
	if (!anx_uuid_is_nil(&checkpoint)) anx_so_delete(&checkpoint, false);
	for (uint32_t i = 0; i < 3; i++) if (!anx_uuid_is_nil(&manifests[i])) anx_so_delete(&manifests[i], false);
	anx_so_close(&h);
	for (uint32_t i = 0; i < 3; i++) if (objects[i]) { anx_so_delete(&objects[i]->oid, false); anx_objstore_release(objects[i]); }
	anx_free(spec);
	return ret;
}
#endif
