/* A sensitive read in one branch constrains later effects across its run. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/effect_fence.h>
#include <anx/effect.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

static int read_handler(struct anx_external_call *call, void *context)
{
	struct anx_object_handle handle = {0};
	char payload[4] = {0};
	int ret = anx_so_open(context, ANX_OPEN_READ, &handle);
	(void)call;
	if (ret != ANX_OK)
		return ret;
	ret = -2802;
	if (anx_so_read_payload(&handle, 0, payload, 0) != 0 ||
	    anx_so_read_payload(&handle, 4, payload, sizeof(payload)) != 0 ||
	    anx_so_read_payload(&handle, 0, payload, sizeof(payload)) != 4 ||
	    anx_memcmp(payload, "data", 4))
		goto out;
	ret = -2806;
	if (anx_sink_register("research-day-028-public", ANX_SENSITIVITY_RESTRICTED, NULL) != ANX_EPERM)
		goto out;
	anx_sink_registry_init();
	if (!anx_sink_lookup("research-day-028-public") ||
	    anx_sink_lookup("research-day-028-public")->max_sensitivity != ANX_SENSITIVITY_PUBLIC)
		goto out;
	ret = ANX_OK;
out:
	anx_so_close(&handle);
	return ret;
}

static int make_root(struct anx_cell **out)
{
	struct anx_cell_intent intent = {0};
	int ret;
	anx_strlcpy(intent.name, "research-day-028-run", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, out);
	if (ret == ANX_OK) {
		(*out)->execution.allow_side_effects = true;
		(*out)->execution.allow_recursive_cells = true;
		(*out)->execution.max_recursion_depth = (*out)->constraints.max_recursion_depth = 3;
		(*out)->constraints.max_child_cells = 8;
	}
	return ret;
}

int anx_research_day028(void)
{
	struct anx_effect_fence_view fence, view, unrelated_fence;
	struct anx_cell *root = NULL, *reader = NULL, *sender = NULL, *denied = NULL;
	struct anx_cell *fresh = NULL, *unrelated = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *secret = NULL, *public = NULL;
	struct anx_so_create_params params = {0};
	struct anx_pending_effect *prepared = NULL, *inflight = NULL, *attempt = NULL;
	struct anx_external_call *call = NULL;
	struct anx_sink *public_sink = NULL, *approved_sink = NULL;
	anx_oid_t secret_id = ANX_UUID_NIL;
	int ret = anx_effect_fence_create(&fence);
	if (ret == ANX_OK) ret = make_root(&root);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(root, &fence.id);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(intent.name, "research-day-028-branch", sizeof(intent.name));
	ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &reader);
	if (ret == ANX_OK) ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXECUTION, &intent, &sender);
	if (ret == ANX_OK) ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &denied);
	if (ret != ANX_OK) goto out;
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = "data";
	params.payload_size = 4;
	params.sensitivity = ANX_SENSITIVITY_CONFIDENTIAL;
	params.creator_cell = reader->cid;
	ret = anx_so_create(&params, &secret);
	if (ret != ANX_OK) goto out;
	secret_id = secret->oid;
	secret->access_policy.rule_count = 2;
	secret->access_policy.rules[0].principal = reader->cid;
	secret->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	secret->access_policy.rules[0].effect = ANX_EFFECT_ALLOW;
	secret->access_policy.rules[1].operations = ANX_ACCESS_READ_PAYLOAD;
	secret->access_policy.rules[1].effect = ANX_EFFECT_DENY;
	params.sensitivity = ANX_SENSITIVITY_PUBLIC;
	ret = anx_so_create(&params, &public);
	if (ret == ANX_OK) ret = anx_sink_register("research-day-028-public", ANX_SENSITIVITY_PUBLIC, &public_sink);
	if (ret == ANX_OK) ret = anx_sink_register("research-day-028-approved", ANX_SENSITIVITY_CONFIDENTIAL, &approved_sink);
	if (ret == ANX_OK) ret = anx_effect_prepare(sender->cid, public_sink, &public->oid, &prepared);
	if (ret == ANX_OK) ret = anx_effect_prepare(sender->cid, public_sink, &public->oid, &inflight);
	if (ret == ANX_OK) ret = anx_effect_mark_dispatching(inflight);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch028://read", sizeof(call->endpoint));
	reader->ext_call = denied->ext_call = call;
	ret = anx_external_register_handler("anxresearch028", read_handler, &secret_id);
	if (ret != ANX_OK) goto out;
	ret = -2803;
	if (anx_cell_run(denied) != ANX_EPERM || anx_effect_fence_get(&fence.id, &view) != ANX_OK ||
	    view.read_count != 0 || view.read_sensitivity != ANX_SENSITIVITY_PUBLIC)
		goto out;
	ret = anx_cell_run(reader);
	if (ret != ANX_OK) goto out;
	ret = -2801;
	if (anx_effect_mark_dispatching(prepared) != ANX_EPERM || prepared->phase != ANX_EFFECT_PREPARED)
		goto out;
	ret = -2804;
	if (anx_effect_fence_get(&fence.id, &view) != ANX_OK || view.read_count != 1 ||
	    view.read_sensitivity != ANX_SENSITIVITY_CONFIDENTIAL || anx_uuid_compare(&view.read_origin, &secret_id) ||
	    view.epoch != fence.epoch || view.generation != fence.generation ||
	    anx_effect_prepare(sender->cid, public_sink, &public->oid, &attempt) != ANX_EPERM || attempt ||
	    anx_effect_prepare(sender->cid, NULL, NULL, &attempt) != ANX_EPERM || attempt ||
	    anx_effect_commit(inflight) != ANX_OK)
		goto out;
	ret = anx_effect_prepare(sender->cid, approved_sink, &public->oid, &attempt);
	if (ret == ANX_OK) ret = anx_effect_mark_dispatching(attempt);
	if (ret == ANX_OK) ret = anx_effect_commit(attempt);
	if (ret != ANX_OK) goto out;
	anx_effect_destroy(attempt); attempt = NULL;
	ret = anx_object_set_sensitivity(&secret_id, ANX_SENSITIVITY_PUBLIC);
	if (ret == ANX_OK) ret = anx_so_delete(&secret_id, false);
	if (ret == ANX_OK) ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXECUTION, &intent, &fresh);
	if (ret != ANX_OK) goto out;
	ret = -2805;
	if (anx_effect_prepare(fresh->cid, public_sink, &public->oid, &attempt) != ANX_EPERM || attempt ||
	    anx_effect_fence_hold(root) != ANX_OK || anx_effect_fence_get(&fence.id, &view) != ANX_OK ||
	    anx_effect_fence_transition(&fence.id, view.generation, ANX_FENCE_RUNNING) != ANX_OK ||
	    anx_effect_mark_dispatching(prepared) != ANX_EPERM)
		goto out;
	ret = anx_effect_fence_create(&unrelated_fence);
	if (ret == ANX_OK) ret = make_root(&unrelated);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(unrelated, &unrelated_fence.id);
	if (ret == ANX_OK) ret = anx_effect_prepare(unrelated->cid, public_sink, &public->oid, &attempt);
	if (ret == ANX_OK) ret = anx_effect_mark_dispatching(attempt);
	if (ret == ANX_OK) ret = anx_effect_commit(attempt);
out:
	if (prepared) anx_effect_destroy(prepared);
	if (inflight) anx_effect_destroy(inflight);
	if (attempt) anx_effect_destroy(attempt);
	if (reader) anx_cell_destroy(reader);
	if (sender) anx_cell_destroy(sender);
	if (denied) anx_cell_destroy(denied);
	if (fresh) anx_cell_destroy(fresh);
	if (root) anx_cell_destroy(root);
	if (unrelated) anx_cell_destroy(unrelated);
	if (secret) { anx_so_delete(&secret->oid, false); anx_objstore_release(secret); }
	if (public) { anx_so_delete(&public->oid, false); anx_objstore_release(public); }
	if (call) anx_free(call);
	anx_external_unregister_handler("anxresearch028");
	return ret;
}
#endif
