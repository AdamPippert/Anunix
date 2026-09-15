/* A compact procedure preserves evidence and needs an independent replay before use. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/procedure.h>
#include <anx/state_object.h>
#include <anx/icm.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/external_call.h>
#include <anx/kprintf.h>
struct fixture066 {
	struct anx_model_use_view uses[8];
	struct anx_procedure_view procedure;
	struct anx_anxml_response response, sentinel;
	struct anx_external_call call;
	struct anx_icm_view catalog;
	bool foreign;
	uint32_t calls;
};
static int denied066(struct fixture066 *f, const struct anx_procedure_view *p, uint32_t slot, int expected)
{
	struct anx_model_use_view before, after;
	struct anx_procedure_view view;
	int ret = anx_model_use_get(f->uses[slot].id, &before);
	if (ret != ANX_OK) return ret;
	anx_memset(&f->response, 0x55, sizeof(f->response)); f->sentinel = f->response;
	ret = anx_procedure_execute(p->id, p->epoch, before.id, before.epoch, &f->response);
	if (ret != expected) { kprintf("day066 denial expected=%d actual=%d\n", expected, ret); return -6602; }
	return !anx_memcmp(&f->response, &f->sentinel, sizeof(f->response)) &&
		anx_model_use_get(before.id, &after) == ANX_OK && !anx_memcmp(&before, &after, sizeof(before)) &&
		anx_procedure_get(p->id, &view) == ANX_OK && !anx_memcmp(&view, p, sizeof(view)) ? ANX_OK : -6603;
}
static int active066(struct anx_external_call *call, void *arg)
{
	struct fixture066 *f = arg;
	struct anx_procedure_view out, sentinel;
	(void)call; f->calls++;
	anx_memset(&out, 0x55, sizeof(out)); sentinel = out;
	if (anx_procedure_compile(&f->procedure.owner, f->uses[3].id, &f->procedure.knowledge, 0, &out) != ANX_EPERM ||
	    anx_procedure_validate(f->procedure.id, f->procedure.epoch, f->uses[4].id, &out) != ANX_EPERM ||
	    anx_procedure_destroy(f->procedure.id) != ANX_EPERM) return -6610;
	if (f->foreign) {
		anx_memset(&f->response, 0x55, sizeof(f->response)); f->sentinel = f->response;
		return anx_procedure_get(f->procedure.id, &out) == ANX_EPERM &&
			anx_procedure_execute(f->procedure.id, f->procedure.epoch, f->uses[5].id, f->uses[5].epoch, &f->response) == ANX_EPERM &&
			!anx_memcmp(&out, &sentinel, sizeof(out)) && !anx_memcmp(&f->response, &f->sentinel, sizeof(f->response)) ? ANX_OK : -6611;
	}
	int ret = anx_procedure_execute(f->procedure.id, f->procedure.epoch, f->uses[5].id, f->uses[5].epoch, &f->response);
	return ret == ANX_OK && f->response.output_len == 4 && !anx_memcmp(f->response.output, "BBBB", 4) ? ANX_OK : -6612;
}
static void erase066(const anx_oid_t *oid)
{
	if (!anx_uuid_is_nil(oid)) anx_so_delete(oid, false);
}
int anx_research_day066(void)
{
	struct fixture066 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *images[2] = {0}, *prompt = NULL, *knowledge[2] = {0}, *raw = NULL;
	struct anx_adapter_image adapter = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_model_use_spec model = { .maximum_tokens = 4 };
	struct anx_procedure_view procedures[2] = {0}, missing = {0}, output, sentinel;
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA,
		.schema_uri = ANX_MODEL_USE_SCHEMA, .schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-066", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	owner->execution.allow_network = owner->execution.allow_remote_models = false;
	for (uint32_t i = 0; i < 2; i++) {
		adapter.deltas[0].next = adapter.deltas[1].previous = adapter.deltas[1].next = (uint8_t)('A' + i);
		ret = anx_so_create(&params, &images[i]);
		if (ret == ANX_OK) ret = anx_so_seal(&images[i]->oid);
		if (ret != ANX_OK) goto out;
	}
	params = (struct anx_so_create_params){ .object_type = ANX_OBJ_BYTE_DATA, .payload = "~", .payload_size = 1 };
	ret = anx_so_create(&params, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	for (uint32_t i = 0; ret == ANX_OK && i < 2; i++) {
		params.payload = i ? "Use validated B for the fixed prompt." : "Use validated A for the fixed prompt.";
		params.payload_size = anx_strlen(params.payload);
		ret = anx_so_create(&params, &knowledge[i]);
		if (ret == ANX_OK) ret = anx_so_seal(&knowledge[i]->oid);
	}
	if (ret != ANX_OK) goto out;
	model.prompt = prompt->oid;
	for (uint32_t i = 0; i < 8; i++) {
		model.image = images[i >= 3 && i <= 5 ? 1 : 0]->oid;
		ret = anx_model_use_prepare(i == 7 ? &foreign->cid : &owner->cid, &model, &f->uses[i]);
		if (ret != ANX_OK) goto out;
	}
	ret = anx_model_use_execute(f->uses[0].id, f->uses[0].epoch, &f->response, &f->uses[0]);
	if (ret != ANX_OK) goto out;
	ret = -6601;
	if (anx_procedure_compile(&owner->cid, f->uses[0].id, &knowledge[0]->oid, 0, &procedures[0]) != ANX_OK) goto out;
	ret = -6604;
	if (procedures[0].state != ANX_PROCEDURE_DRAFT || procedures[0].epoch != 1 || procedures[0].version != 1 ||
	    anx_icm_read_view(&procedures[0].artifact, &f->catalog) != ANX_OK || anx_strcmp(f->catalog.domain, "procedural") ||
	    anx_strcmp(f->catalog.kind, "recipe")) goto out;
	anx_memset(&output, 0x55, sizeof(output)); sentinel = output;
	if (anx_procedure_compile(&owner->cid, 0, &knowledge[0]->oid, 0, &output) != ANX_EINVAL ||
	    anx_procedure_compile(&owner->cid, f->uses[2].id, &knowledge[0]->oid, 0, &output) != ANX_EINVAL ||
	    anx_procedure_compile(&owner->cid, f->uses[7].id, &knowledge[0]->oid, 0, &output) != ANX_EPERM ||
	    anx_procedure_compile(&owner->cid, f->uses[0].id, &ANX_UUID_NIL, 0, &output) != ANX_ENOENT ||
	    anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	/* Public catalog claims cannot promote a private draft or grant permissions. */
	ret = anx_icm_tag(&procedures[0].artifact, NULL, NULL, "all", "validated", NULL, NULL);
	if (ret == ANX_OK) ret = denied066(f, &procedures[0], 2, ANX_EPERM);
	if (ret != ANX_OK) goto out;
	ret = -6605;
	if (anx_procedure_validate(procedures[0].id, procedures[0].epoch, f->uses[0].id, &output) != ANX_EINVAL ||
	    anx_procedure_validate(procedures[0].id, procedures[0].epoch, f->uses[2].id, &output) != ANX_EINVAL ||
	    anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	ret = anx_model_use_execute(f->uses[1].id, f->uses[1].epoch, &f->response, &f->uses[1]);
	if (ret == ANX_OK) ret = anx_model_use_execute(f->uses[3].id, f->uses[3].epoch, &f->response, &f->uses[3]);
	if (ret != ANX_OK) goto out;
	ret = -6605;
	if (anx_procedure_validate(procedures[0].id, procedures[0].epoch, f->uses[3].id, &output) != ANX_ENOTSUP) goto out;
	/* Missing raw evidence is not replaced by a catalog label or another passing replay. */
	ret = anx_procedure_compile(&owner->cid, f->uses[0].id, &knowledge[0]->oid, 0, &missing);
	if (ret == ANX_OK) ret = anx_so_delete(&missing.evidence, false);
	if (ret != ANX_OK) goto out;
	ret = -6606;
	if (anx_procedure_validate(missing.id, missing.epoch, f->uses[1].id, &output) != ANX_ENOENT ||
	    anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	ret = anx_procedure_destroy(missing.id); missing.id = 0;
	if (ret != ANX_OK) goto out;
	/* The sealed source receipt remains inspectable after its runtime handle is released. */
	ret = anx_model_use_destroy(f->uses[0].id); f->uses[0].id = 0;
	if (ret != ANX_OK) goto out;
	raw = anx_objstore_lookup(&procedures[0].evidence);
	ret = -6607;
	if (!raw || raw->state != ANX_OBJ_SEALED || raw->parent_count != 2 ||
	    anx_strcmp(raw->schema_uri, ANX_PROCEDURE_EVIDENCE_SCHEMA)) goto out;
	anx_objstore_release(raw); raw = NULL;
	ret = anx_procedure_validate(procedures[0].id, procedures[0].epoch, f->uses[1].id, &procedures[0]);
	if (ret != ANX_OK) goto out;
	ret = -6607;
	if (procedures[0].state != ANX_PROCEDURE_VALIDATED || procedures[0].epoch != 2 ||
	    anx_uuid_is_nil(&procedures[0].validation_evidence) || owner->execution.allow_network || owner->execution.allow_remote_models) goto out;
	anx_oid_t dependencies[4] = { procedures[0].artifact, knowledge[0]->oid, procedures[0].evidence, procedures[0].validation_evidence };
	for (uint32_t i = 0; i < 4; i++) {
		raw = anx_objstore_lookup(&dependencies[i]);
		if (!raw) { ret = -6608; goto out; }
		raw->version++;
		ret = denied066(f, &procedures[0], 2, ANX_EBUSY); raw->version--;
		if (ret != ANX_OK) goto out;
		((uint8_t *)raw->payload)[0] ^= 1;
		ret = denied066(f, &procedures[0], 2, ANX_EBUSY); ((uint8_t *)raw->payload)[0] ^= 1;
		if (ret != ANX_OK) goto out;
		raw->access_policy.rule_count = 1; raw->access_policy.rules[0].effect = ANX_EFFECT_DENY;
		raw->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
		ret = denied066(f, &procedures[0], 2, ANX_EPERM); raw->access_policy.rule_count = 0;
		anx_objstore_release(raw); raw = NULL;
		if (ret != ANX_OK) goto out;
	}
	ret = denied066(f, &procedures[0], 5, ANX_ENOTSUP);
	if (ret == ANX_OK) ret = anx_procedure_execute(procedures[0].id, procedures[0].epoch, f->uses[2].id, f->uses[2].epoch, &f->response);
	if (ret != ANX_OK) goto out;
	ret = -6609;
	if (f->response.output_len != 4 || anx_memcmp(f->response.output, "AAAA", 4)) goto out;
	ret = anx_procedure_compile(&owner->cid, f->uses[3].id, &knowledge[1]->oid, procedures[0].id, &procedures[1]);
	if (ret == ANX_OK) ret = anx_model_use_execute(f->uses[4].id, f->uses[4].epoch, &f->response, &f->uses[4]);
	if (ret == ANX_OK) ret = anx_procedure_validate(procedures[1].id, procedures[1].epoch, f->uses[4].id, &procedures[1]);
	if (ret != ANX_OK) goto out;
	ret = -6609;
	if (procedures[1].version != 2 || procedures[1].predecessor != procedures[0].id ||
	    anx_procedure_destroy(procedures[0].id) != ANX_EBUSY) goto out;
	/* Explicitly selecting the previous validated version remains possible. */
	ret = anx_procedure_execute(procedures[0].id, procedures[0].epoch, f->uses[6].id, f->uses[6].epoch, &f->response);
	if (ret != ANX_OK) goto out;
	ret = -6609;
	if (f->response.output_len != 4 || anx_memcmp(f->response.output, "AAAA", 4)) goto out;
	f->procedure = procedures[1];
	anx_strlcpy(f->call.endpoint, "anxresearch066://procedure", sizeof(f->call.endpoint));
	ret = anx_external_register_handler("anxresearch066", active066, f);
	if (ret != ANX_OK) goto out;
	f->foreign = true; foreign->ext_call = &f->call;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	f->foreign = false; owner->ext_call = &f->call;
	ret = anx_cell_run(owner);
	if (ret != ANX_OK) goto out;
	ret = f->calls == 2 ? ANX_OK : -6613;
out:
	if (raw) anx_objstore_release(raw);
	anx_external_unregister_handler("anxresearch066");
	if (missing.id) anx_procedure_destroy(missing.id);
	erase066(&missing.artifact); erase066(&missing.evidence); erase066(&missing.validation_evidence);
	for (int i = 1; i >= 0; i--) {
		if (procedures[i].id && anx_procedure_destroy(procedures[i].id) != ANX_OK && ret == ANX_OK) ret = -6614;
		erase066(&procedures[i].artifact); erase066(&procedures[i].evidence); erase066(&procedures[i].validation_evidence);
	}
	if (f) for (uint32_t i = 0; i < 8; i++) if (f->uses[i].id && anx_model_use_destroy(f->uses[i].id) != ANX_OK && ret == ANX_OK) ret = -6615;
	if (foreign && anx_cell_destroy(foreign) != ANX_OK && ret == ANX_OK) ret = -6616;
	if (owner && anx_cell_destroy(owner) != ANX_OK && ret == ANX_OK) ret = -6617;
	for (uint32_t i = 0; i < 2; i++) {
		if (images[i]) { anx_so_delete(&images[i]->oid, false); anx_objstore_release(images[i]); }
		if (knowledge[i]) { anx_so_delete(&knowledge[i]->oid, false); anx_objstore_release(knowledge[i]); }
	}
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	anx_free(f);
	if (ret != ANX_OK) kprintf("day066 failure rc=%d\n", ret);
	return ret;
}
#endif
