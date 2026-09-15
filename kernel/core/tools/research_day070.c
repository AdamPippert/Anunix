#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/external_operation.h>
#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/kprintf.h>
struct fixture070 {
	struct anx_external_call call, provider, response;
	struct anx_exposure_view ledger;
	anx_oid_t operation;
	uint64_t units;
	uint32_t mode, calls;
	bool foreign;
};
static int provider070(struct anx_external_call *call, void *arg)
{
	struct fixture070 *f = arg;
	struct anx_exposure_view view;
	struct anx_external_operation_view operation;
	f->calls++;
	if (f->calls != 1 || call->request_size != 4 || anx_memcmp(call->request_body, "data", 4)) return -7002;
	if (anx_exposure_get(f->ledger.id, &view) != ANX_OK || view.reserved != f->units || view.in_flight != 1 ||
	    view.closed || view.revoked != (f->mode == 3)) return -7003;
	if (anx_external_operation_get(&f->operation, &operation) != ANX_OK || operation.phase != ANX_EFFECT_DISPATCHING ||
	    operation.exposure_units != f->units || operation.exposure_ledger != f->ledger.id ||
	    anx_external_operation_dispatch(&f->operation, &f->response) != ANX_EBUSY) return -7004;
	if (f->mode == 1) return ANX_ETIMEDOUT;
	anx_memcpy(call->response_buf, "done", 4); call->response_size = 4; call->status_code = 200;
	return ANX_OK;
}
static int driver070(struct anx_external_call *call, void *arg)
{
	struct fixture070 *f = arg;
	struct anx_exposure_view view;
	struct anx_external_operation_view operation;
	(void)call;
	if (anx_exposure_create(NULL, 0, 1, &view) != ANX_EPERM ||
	    anx_exposure_revoke(f->ledger.id, &view) != ANX_EPERM ||
	    anx_exposure_destroy(f->ledger.id) != ANX_EPERM ||
	    anx_exposure_test_revoke_on_dispatch(0) != ANX_EPERM ||
	    anx_external_operation_prepare_budgeted(NULL, NULL, NULL, NULL, 1, 1, NULL) != ANX_EPERM) return -7005;
	if (f->foreign) return anx_exposure_get(f->ledger.id, &view) == ANX_EPERM &&
		anx_external_operation_dispatch(&f->operation, &f->response) == ANX_EPERM ? ANX_OK : -7006;
	int expected = f->mode == 1 ? ANX_ETIMEDOUT : f->mode == 2 ? ANX_EPERM : ANX_OK;
	int ret = anx_external_operation_dispatch(&f->operation, &f->response);
	if (ret != expected || f->calls != (f->mode == 2 ? 0U : 1U)) return -7007;
	if (anx_exposure_get(f->ledger.id, &view) != ANX_OK || anx_external_operation_get(&f->operation, &operation) != ANX_OK) return -7008;
	if (f->mode == 2) return operation.phase == ANX_EFFECT_PREPARED && view.reserved == f->units &&
		view.revoked && !view.closed && !view.in_flight ? ANX_OK : -7009;
	if (anx_external_operation_dispatch(&f->operation, &f->response) != ANX_EBUSY || f->calls != 1 ||
	    view.reserved || view.in_flight) return -7010;
	if (f->mode == 1) return operation.phase == ANX_EFFECT_UNKNOWN && view.uncertain == f->units && !view.closed ? ANX_OK : -7011;
	return operation.phase == ANX_EFFECT_COMMITTED && view.committed == f->units &&
		view.closed == (f->mode == 3) && f->response.response_size == 4 && !anx_memcmp(f->response.response_buf, "done", 4) ? ANX_OK : -7012;
}
static int closure070(struct fixture070 *f, const anx_oid_t *source, uint32_t mode)
{
	struct anx_cell *cell = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_exposure_view view;
	anx_oid_t operation = ANX_UUID_NIL;
	uint64_t ledger = 0;
	anx_strlcpy(intent.name, "research-day-070-closure", sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
	if (ret != ANX_OK) return ret;
	cell->execution.allow_side_effects = true; cell->ext_call = &f->call;
	ret = anx_exposure_create(&cell->cid, 0, 10, &view);
	if (ret == ANX_OK) { ledger = view.id; ret = anx_external_operation_prepare_budgeted(&cell->cid, &f->provider, "research-day-070", source, ledger, 10, &operation); }
	if (ret != ANX_OK) goto out;
	if (mode == 2) ret = anx_exposure_revoke(ledger, &view);
	else ret = anx_exposure_test_revoke_on_dispatch(ledger);
	if (ret != ANX_OK || (mode == 2 && view.closed)) { ret = -7013; goto out; }
	f->ledger = view; f->operation = operation; f->units = 10; f->mode = mode; f->calls = 0; f->foreign = false;
	ret = anx_cell_run(cell);
	if (ret != ANX_OK) goto out;
	ret = anx_external_operation_discard(&operation); operation = ANX_UUID_NIL;
	if (ret == ANX_OK) ret = anx_exposure_get(ledger, &view);
	if (ret != ANX_OK || !view.closed || view.reserved || view.uncertain || view.committed != (mode == 3 ? 10U : 0U)) { ret = -7014; goto out; }
	ret = anx_exposure_destroy(ledger); ledger = 0;
out:
	anx_exposure_test_revoke_on_dispatch(0);
	if (!anx_uuid_is_nil(&operation)) anx_external_operation_discard(&operation);
	if (ledger) { anx_exposure_revoke(ledger, &view); anx_exposure_destroy(ledger); }
	anx_cell_destroy(cell); return ret;
}
int anx_research_day070(void)
{
	struct fixture070 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *principal = NULL, *workflow = NULL, *agents[3] = {0}, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_exposure_view root = {0}, group = {0}, leaves[3] = {0}, view, sentinel;
	struct anx_state_object *source = NULL;
	struct anx_so_create_params params = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "data", .payload_size = 4 };
	anx_oid_t operations[3] = {0}, denied_id = { .hi = 0x55, .lo = 0x66 };
	bool retained_unknown = false;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-070", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &principal);
	if (ret != ANX_OK) goto out;
	principal->execution.allow_side_effects = principal->execution.allow_recursive_cells = true;
	ret = -7001;
	if (anx_exposure_create(&principal->cid, 0, 100, &root) != ANX_OK) goto out;
	ret = anx_cell_derive_child(principal, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &workflow);
	if (ret == ANX_OK) ret = anx_exposure_create(&workflow->cid, root.id, 80, &group);
	if (ret != ANX_OK) goto out;
	for (uint32_t i = 0; i < 3; i++) {
		ret = anx_cell_derive_child(workflow, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &agents[i]);
		if (ret == ANX_OK) ret = anx_exposure_create(&agents[i]->cid, group.id, 60, &leaves[i]);
		if (ret != ANX_OK) goto out;
	}
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	foreign->execution.allow_side_effects = true;
	anx_memset(&view, 0x55, sizeof(view)); sentinel = view;
	if (anx_exposure_create(&foreign->cid, root.id, 10, &view) != ANX_EPERM ||
	    anx_exposure_create(&workflow->cid, root.id, 101, &view) != ANX_EINVAL ||
	    anx_memcmp(&view, &sentinel, sizeof(view))) { ret = -7015; goto out; }
	ret = anx_so_create(&params, &source);
	if (ret == ANX_OK) ret = anx_so_seal(&source->oid);
	if (ret == ANX_OK) ret = anx_sink_register("research-day-070", ANX_SENSITIVITY_PUBLIC, NULL);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch070provider", provider070, f);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch070driver", driver070, f);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->provider.endpoint, "anxresearch070provider://effect", sizeof(f->provider.endpoint));
	f->provider.request_body = "data"; f->provider.request_size = 4;
	anx_strlcpy(f->call.endpoint, "anxresearch070driver://run", sizeof(f->call.endpoint));
	for (uint32_t i = 0; i < 2; i++) {
		ret = anx_external_operation_prepare_budgeted(&agents[i]->cid, &f->provider, "research-day-070", &source->oid, leaves[i].id, 40, &operations[i]);
		if (ret != ANX_OK) goto out;
	}
	if (anx_external_operation_prepare_budgeted(&agents[2]->cid, &f->provider, "research-day-070", &source->oid, leaves[2].id, 40, &denied_id) != ANX_EFULL ||
	    denied_id.hi != 0x55 || denied_id.lo != 0x66 ||
	    anx_external_operation_prepare_budgeted(&agents[2]->cid, &f->provider, "research-day-070", &source->oid, leaves[2].id, ~(uint64_t)0, &denied_id) != ANX_EFULL ||
	    anx_external_operation_prepare_budgeted(&agents[2]->cid, &f->provider, "research-day-070", &source->oid, leaves[0].id, 1, &denied_id) != ANX_EPERM) { ret = -7016; goto out; }
	ret = anx_exposure_get(root.id, &view);
	if (ret != ANX_OK || view.reserved != 80 || view.committed || view.uncertain || f->calls) { ret = -7017; goto out; }
	ret = anx_external_operation_discard(&operations[1]); operations[1] = ANX_UUID_NIL;
	if (ret == ANX_OK) ret = anx_exposure_get(group.id, &view);
	if (ret != ANX_OK || view.reserved != 40) { ret = -7018; goto out; }
	ret = anx_external_operation_prepare_budgeted(&agents[2]->cid, &f->provider, "research-day-070", &source->oid, leaves[2].id, 40, &operations[2]);
	if (ret != ANX_OK) goto out;
	f->ledger = leaves[0]; f->operation = operations[0]; f->units = 40; f->foreign = true;
	foreign->ext_call = &f->call; ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	f->foreign = false; agents[0]->ext_call = &f->call; ret = anx_cell_run(agents[0]);
	if (ret != ANX_OK) goto out;
	ret = anx_external_operation_discard(&operations[0]); operations[0] = ANX_UUID_NIL;
	if (ret == ANX_OK) ret = anx_exposure_get(root.id, &view);
	if (ret != ANX_OK || view.committed != 40 || view.reserved != 40) { ret = -7019; goto out; }
	f->ledger = leaves[2]; f->operation = operations[2]; f->mode = 1; f->calls = 0;
	agents[2]->ext_call = &f->call; ret = anx_cell_run(agents[2]);
	if (ret != ANX_OK) goto out;
	retained_unknown = true;
	ret = anx_exposure_revoke(root.id, &view);
	if (ret != ANX_OK || !view.revoked || view.closed || view.committed != 40 || view.uncertain != 40 || view.reserved || view.in_flight) { ret = -7020; goto out; }
	if (anx_external_operation_discard(&operations[2]) != ANX_EBUSY || anx_exposure_destroy(leaves[2].id) != ANX_EBUSY ||
	    anx_external_operation_prepare_budgeted(&agents[1]->cid, &f->provider, "research-day-070", &source->oid, leaves[1].id, 1, &denied_id) != ANX_EPERM) { ret = -7021; goto out; }
	ret = anx_exposure_destroy(leaves[0].id); leaves[0].id = 0;
	if (ret == ANX_OK) ret = anx_exposure_destroy(leaves[1].id);
	leaves[1].id = 0;
	if (ret == ANX_OK) ret = anx_exposure_get(group.id, &view);
	if (ret != ANX_OK || view.committed != 40 || view.uncertain != 40 || view.children != 1 || view.closed) { ret = -7022; goto out; }
	ret = closure070(f, &source->oid, 2);
	if (ret == ANX_OK) ret = closure070(f, &source->oid, 3);
	if (ret == ANX_OK) kprintf("day070 root_limit=100 workflow_limit=80 committed=40 uncertain=40 overdraft=0 closure_preserved=1\n");
out:
	anx_exposure_test_revoke_on_dispatch(0);
	for (uint32_t i = 0; i < 3; i++) if (!anx_uuid_is_nil(&operations[i])) anx_external_operation_discard(&operations[i]);
	if (root.id) anx_exposure_revoke(root.id, &view);
	for (uint32_t i = 0; i < 3; i++) if (leaves[i].id) anx_exposure_destroy(leaves[i].id);
	if (group.id) anx_exposure_destroy(group.id);
	if (root.id) anx_exposure_destroy(root.id);
	anx_external_unregister_handler("anxresearch070provider"); anx_external_unregister_handler("anxresearch070driver");
	if (source) { if (!retained_unknown) anx_so_delete(&source->oid, false); anx_objstore_release(source); }
	for (uint32_t i = 0; i < 3; i++) if (agents[i]) anx_cell_destroy(agents[i]);
	if (workflow) anx_cell_destroy(workflow);
	if (principal) anx_cell_destroy(principal);
	if (foreign) anx_cell_destroy(foreign);
	if (ret != ANX_OK) kprintf("day070 native failure rc=%d mode=%u calls=%u\n", ret, f->mode, f->calls);
	anx_free(f); return ret;
}
#endif
