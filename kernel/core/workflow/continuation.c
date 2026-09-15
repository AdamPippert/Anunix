#include <anx/continuation.h>
#include <anx/external_operation.h>
#include <anx/state_object.h>
#include <anx/effect_fence.h>
#include <anx/identity.h>
#include <anx/sched_domain.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>

struct continuation_ref { anx_oid_t oid; uint64_t version; uint32_t size, sensitivity; uint8_t digest[32]; };
struct continuation_policy {
	anx_oid_t identity, identity_record, fence, tools, revision;
	struct anx_execution_policy execution;
	struct anx_cell_constraints constraints;
	struct anx_cognitive_envelope cognitive;
	struct anx_execution_contract contract;
};
struct continuation_operation {
	uint64_t key;
	uint32_t slot, kind;
	anx_oid_t operation;
	struct continuation_ref source, result;
};
struct continuation_record {
	struct anx_continuation_view view;
	struct anx_cell *owner, *workers[ANX_CONTINUATION_CAPABILITIES];
	struct continuation_policy policy;
	struct anx_continuation_event events[ANX_CONTINUATION_EVENTS];
	struct continuation_ref journal[ANX_CONTINUATION_EVENTS];
	struct continuation_operation operations[ANX_CONTINUATION_OPERATIONS];
	struct anx_external_call *response;
	uint32_t active_operation;
	bool busy;
};
static struct continuation_record *records[ANX_CONTINUATION_MAX];
static struct anx_spinlock continuation_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
static bool drop_reply;
int anx_continuation_test_drop_reply(bool enabled)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	drop_reply = enabled; return ANX_OK;
}
#endif
static struct continuation_record *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_CONTINUATION_MAX; i++) if (records[i] && records[i]->view.id == id) return records[i];
	return NULL;
}
static void capture_policy(const struct anx_cell *c, struct continuation_policy *p)
{
	anx_memset(p, 0, sizeof(*p));
	p->identity = c->identity_id; p->fence = c->effect_fence_id; p->tools = c->tool_namespace_id; p->revision = c->revision_lease_id;
	p->execution = c->execution; p->constraints = c->constraints; p->cognitive = c->cognitive; p->contract = c->contract;
}
static int owner_check(struct continuation_record *r)
{
	struct continuation_policy now;
	capture_policy(r->owner, &now);
	int ret = anx_identity_admit(r->owner, &now.identity_record);
	if (ret == ANX_OK && anx_memcmp(&now, &r->policy, sizeof(now))) ret = ANX_EBUSY;
	if (ret == ANX_OK && anx_cell_status_terminal(r->owner->status)) ret = ANX_EPERM;
	if (ret == ANX_OK) ret = anx_cell_check_scope(r->owner);
	if (ret == ANX_OK) ret = anx_cell_check_contract(r->owner);
	if (ret == ANX_OK) ret = anx_sched_domain_check(r->owner);
	if (ret == ANX_OK) ret = anx_effect_fence_check(r->owner, NULL, NULL);
	return ret;
}
static int access(struct continuation_record *r)
{
	if (!r) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	if (caller && anx_uuid_compare(caller, &r->view.owner)) return ANX_EPERM;
	return r->busy ? ANX_EBUSY : owner_check(r);
}
/* This internal read uses the logical owner's ACL even for trusted controller calls. */
static int object_read(struct continuation_record *r, struct continuation_ref *ref, const char *schema,
		void *out, uint32_t exact, bool capture)
{
	struct anx_state_object *o = anx_objstore_lookup(&ref->oid);
	if (!o) return ANX_ENOENT;
	struct continuation_ref now = { .oid = ref->oid };
	anx_spin_lock(&o->lock);
	int ret = o->state != ANX_OBJ_SEALED || !o->version || !o->payload || !o->payload_size || o->payload_size > 8192 ||
		o->access_policy.rule_count > ANX_MAX_ACCESS_RULES || o->sensitivity > ANX_SENSITIVITY_RESTRICTED ||
		(exact && o->payload_size != exact) ? ANX_EINVAL : ANX_OK;
	if (ret == ANX_OK && (o->object_type != (schema ? ANX_OBJ_STRUCTURED_DATA : ANX_OBJ_BYTE_DATA) ||
	    (schema && (anx_strcmp(o->schema_uri, schema) || anx_strcmp(o->schema_version, "1"))))) ret = ANX_EINVAL;
	if (ret == ANX_OK) ret = anx_access_evaluate(&o->access_policy, &r->view.owner, &o->creator_cell, ANX_ACCESS_READ_PAYLOAD);
	if (ret == ANX_OK) {
		now.version = o->version; now.size = o->payload_size; now.sensitivity = o->sensitivity;
		anx_sha256(o->payload, now.size, now.digest);
		if (!capture && (now.version != ref->version || now.size != ref->size || now.sensitivity != ref->sensitivity ||
		    anx_memcmp(now.digest, ref->digest, 32))) ret = ANX_EBUSY;
	}
	if (ret == ANX_OK) ret = anx_effect_fence_observe_read(&o->oid, o->sensitivity);
	if (ret == ANX_OK) {
		if (out) anx_memcpy(out, o->payload, now.size);
		if (capture) *ref = now;
	}
	anx_spin_unlock(&o->lock); anx_objstore_release(o);
	return ret;
}
static int store_object(struct continuation_record *r, const char *schema, const void *payload, uint32_t size,
		const anx_oid_t *parents, uint32_t count, struct continuation_ref *ref)
{
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA, .schema_uri = schema, .schema_version = "1",
		.payload = payload, .payload_size = size, .parent_oids = parents, .parent_count = count,
		.creator_cell = r->view.owner, .sensitivity = ANX_SENSITIVITY_RESTRICTED };
	struct anx_state_object *o = NULL;
	int ret = anx_so_create(&params, &o);
	if (ret == ANX_OK) {
		o->access_policy.rule_count = 2;
		o->access_policy.rules[0] = (struct anx_access_rule){ .principal = r->view.owner, .operations = 0xff, .effect = ANX_EFFECT_ALLOW };
		o->access_policy.rules[1] = (struct anx_access_rule){ .operations = 0xff, .effect = ANX_EFFECT_DENY };
		ret = anx_so_seal(&o->oid);
	}
	if (ret == ANX_OK) { ref->oid = o->oid; ret = object_read(r, ref, schema, NULL, size, true); }
	if (o) { if (ret != ANX_OK) anx_so_delete(&o->oid, false); anx_objstore_release(o); }
	return ret;
}
static int journal_check(struct continuation_record *r)
{
	for (uint32_t i = 0; i < r->view.events; i++) {
		int ret = object_read(r, &r->journal[i], ANX_CONTINUATION_SCHEMA, NULL, sizeof(r->events[i]), false);
		if (ret != ANX_OK) return ret;
	}
	return ANX_OK;
}
static int append(struct continuation_record *r, struct anx_continuation_event *event)
{
	if (r->view.events == ANX_CONTINUATION_EVENTS || r->view.epoch == ~(uint64_t)0) return ANX_EFULL;
	event->continuation = r->view.id; event->epoch = r->view.epoch + 1; event->previous = r->view.head;
	if (r->view.events) anx_memcpy(event->previous_digest, r->journal[r->view.events - 1].digest, 32);
	anx_oid_t parents[2]; uint32_t count = 0;
	if (!anx_uuid_is_nil(&event->previous)) parents[count++] = event->previous;
	if (!anx_uuid_is_nil(&event->result_object)) parents[count++] = event->result_object;
	else if (!anx_uuid_is_nil(&event->source)) parents[count++] = event->source;
	struct continuation_ref ref = {0};
	int ret = store_object(r, ANX_CONTINUATION_SCHEMA, event, sizeof(*event), parents, count, &ref);
	if (ret == ANX_OK) {
		r->events[r->view.events] = *event; r->journal[r->view.events++] = ref;
		r->view.head = ref.oid; r->view.epoch = event->epoch;
	}
	return ret;
}
static int worker_check(struct continuation_record *r, struct anx_cell *worker)
{
	if (!worker) return ANX_ENOENT;
	if (anx_uuid_compare(&worker->parent_cid, &r->view.owner) || worker->cell_type != ANX_CELL_TASK_EXTERNAL_CALL) return ANX_EPERM;
	if (worker->status != ANX_CELL_CREATED || worker->runtime_active || worker->ext_call) return ANX_EBUSY;
	int ret = anx_cell_check_scope(worker);
	if (ret == ANX_OK) ret = anx_cell_check_contract(worker);
	if (ret == ANX_OK) ret = anx_identity_admit(worker, NULL);
	if (ret == ANX_OK) ret = anx_sched_domain_check(worker);
	if (ret == ANX_OK) ret = anx_effect_fence_check(worker, NULL, NULL);
	return ret;
}
int anx_continuation_create(const anx_cid_t *owner, struct anx_continuation_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !out || anx_uuid_is_nil(owner)) return ANX_EINVAL;
	struct continuation_record *r = anx_zalloc(sizeof(*r));
	if (!r) return ANX_ENOMEM;
	r->owner = anx_cell_store_lookup(owner); r->view.owner = *owner;
	int ret = r->owner ? ANX_OK : ANX_ENOENT;
	if (ret == ANX_OK && !anx_uuid_is_nil(&r->owner->tool_namespace_id)) ret = ANX_ENOTSUP;
	if (ret == ANX_OK) { capture_policy(r->owner, &r->policy); ret = anx_identity_admit(r->owner, &r->policy.identity_record); }
	if (ret == ANX_OK) ret = owner_check(r);
	if (ret == ANX_OK && (!r->owner->execution.allow_recursive_cells || !r->owner->execution.allow_side_effects)) ret = ANX_EPERM;
	bool flags;
	anx_spin_lock_irqsave(&continuation_lock, &flags);
	if (ret == ANX_OK) {
		ret = ANX_EFULL;
		for (uint32_t i = 0; sequence != ~(uint64_t)0 && i < ANX_CONTINUATION_MAX; i++) if (!records[i]) {
			r->view.id = ++sequence; r->view.epoch = 1; records[i] = r; *out = r->view; ret = ANX_OK; break;
		}
	}
	anx_spin_unlock_irqrestore(&continuation_lock, flags);
	if (ret != ANX_OK) { if (r->owner) anx_cell_store_release(r->owner); anx_free(r); }
	return ret;
}
int anx_continuation_bind(uint64_t id, uint64_t epoch, uint32_t slot, const anx_cid_t *worker, struct anx_continuation_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch || slot >= ANX_CONTINUATION_CAPABILITIES || !worker || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&continuation_lock, &flags);
	struct continuation_record *r = find(id);
	int ret = access(r);
	if (ret == ANX_OK && r->view.epoch != epoch) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = journal_check(r);
	struct anx_cell *next = NULL;
	if (ret == ANX_OK) { next = anx_cell_store_lookup(worker); ret = worker_check(r, next); }
	if (ret == ANX_OK && r->workers[slot] && !anx_cell_status_terminal(r->workers[slot]->status)) ret = ANX_EBUSY;
	if (ret == ANX_OK) for (uint32_t i = 0; i < ANX_CONTINUATION_MAX; i++) if (records[i])
		for (uint32_t j = 0; j < ANX_CONTINUATION_CAPABILITIES; j++)
			if (records[i]->workers[j] == next) ret = ANX_EEXIST;
	if (ret == ANX_OK && r->view.generations[slot] == ~(uint64_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK) {
		struct anx_continuation_event event = { .kind = ANX_CONT_BIND, .slot = slot, .worker = *worker,
			.generation = r->view.generations[slot] + 1 };
		ret = append(r, &event);
		if (ret == ANX_OK) {
			if (r->workers[slot]) anx_cell_store_release(r->workers[slot]);
			r->workers[slot] = next; next = NULL; r->view.workers[slot] = *worker;
			r->view.generations[slot] = event.generation; *out = r->view;
		}
	}
	anx_spin_unlock_irqrestore(&continuation_lock, flags);
	if (next) anx_cell_store_release(next);
	return ret;
}
/* Only a running, privately selected worker can enter the protected operation. */
static int driver(struct anx_external_call *call, void *unused)
{
	(void)call; (void)unused;
	const anx_cid_t *caller = anx_cell_current_id();
	if (!caller) return ANX_EPERM;
	bool flags;
	anx_spin_lock_irqsave(&continuation_lock, &flags);
	struct continuation_record *r = NULL;
	for (uint32_t i = 0; i < ANX_CONTINUATION_MAX; i++) if (records[i] && records[i]->busy && records[i]->response) {
		struct continuation_record *candidate = records[i];
		uint32_t slot = candidate->operations[candidate->active_operation].slot;
		if (!anx_uuid_compare(caller, &candidate->view.workers[slot])) { r = candidate; break; }
	}
	int ret = r ? owner_check(r) : ANX_EPERM;
	anx_oid_t operation = r ? r->operations[r->active_operation].operation : ANX_UUID_NIL;
	struct anx_external_call *response = r ? r->response : NULL;
	anx_spin_unlock_irqrestore(&continuation_lock, flags);
	return ret == ANX_OK ? anx_external_operation_dispatch(&operation, response) : ret;
}
int anx_continuation_dispatch(uint64_t id, uint64_t epoch, uint32_t slot, uint64_t key,
		const struct anx_external_call *call, const char *sink, const anx_oid_t *source, struct anx_continuation_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch || slot >= ANX_CONTINUATION_CAPABILITIES || !key || !call || !source || !out ||
	    !call->request_size || call->request_size > ANX_EXT_OPERATION_BODY_MAX || !call->request_body) return ANX_EINVAL;
	struct anx_external_call *bridge = anx_zalloc(sizeof(*bridge)), *response = anx_zalloc(sizeof(*response));
	struct anx_continuation_result *result = anx_zalloc(sizeof(*result));
	if (!bridge || !response || !result) { anx_free(bridge); anx_free(response); anx_free(result); return ANX_ENOMEM; }
	struct continuation_operation op = { .key = key, .slot = slot, .kind = ANX_CONT_INTENT, .source.oid = *source };
	struct anx_continuation_event event = { .kind = ANX_CONT_INTENT, .slot = slot, .key = key, .source = *source };
	bool flags;
	anx_spin_lock_irqsave(&continuation_lock, &flags);
	struct continuation_record *r = find(id);
	int ret = access(r);
	if (ret == ANX_OK && r->view.epoch != epoch) ret = ANX_EBUSY;
	if (ret == ANX_OK) for (uint32_t i = 0; i < r->view.operations; i++) if (r->operations[i].key == key) ret = ANX_EEXIST;
	if (ret == ANX_OK && (r->view.operations == ANX_CONTINUATION_OPERATIONS || r->view.events > ANX_CONTINUATION_EVENTS - 2 ||
	    r->view.epoch > ~(uint64_t)0 - 2)) ret = ANX_EFULL;
	if (ret == ANX_OK) ret = journal_check(r);
	if (ret == ANX_OK) ret = worker_check(r, r->workers[slot]);
	if (ret == ANX_OK) ret = object_read(r, &op.source, NULL, NULL, call->request_size, true);
	uint8_t digest[32];
	if (ret == ANX_OK) {
		anx_sha256(call->request_body, call->request_size, digest);
		if (anx_memcmp(digest, op.source.digest, 32)) ret = ANX_EINVAL;
	}
	if (ret == ANX_OK) ret = anx_external_operation_prepare(&r->view.workers[slot], call, sink, source, &op.operation);
	struct anx_external_operation_view prepared;
	if (ret == ANX_OK) ret = anx_external_operation_get(&op.operation, &prepared);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxcontinuation", driver, NULL);
	if (ret == ANX_OK) {
		event.worker = r->view.workers[slot]; event.generation = r->view.generations[slot];
		anx_memcpy(event.request_digest, prepared.request_hash.bytes, 32);
		ret = append(r, &event);
	}
	struct anx_cell *worker = NULL;
	uint32_t index = 0;
	if (ret == ANX_OK) {
		index = r->view.operations++; r->operations[index] = op; r->active_operation = index;
		r->busy = true; r->response = response; worker = r->workers[slot];
		anx_strlcpy(bridge->endpoint, "anxcontinuation://dispatch", sizeof(bridge->endpoint));
		worker->ext_call = bridge;
	}
	anx_spin_unlock_irqrestore(&continuation_lock, flags);
	if (ret != ANX_OK) { if (!anx_uuid_is_nil(&op.operation)) anx_external_operation_discard(&op.operation); goto out; }
	int run_result = anx_cell_run(worker);
	worker->ext_call = NULL;
	struct anx_external_operation_view settled;
	int settled_ret = anx_external_operation_get(&op.operation, &settled);
	uint32_t kind = settled_ret != ANX_OK ? ANX_CONT_UNCERTAIN : settled.phase == ANX_EFFECT_COMMITTED ? ANX_CONT_COMMITTED :
		settled.phase == ANX_EFFECT_PREPARED ? ANX_CONT_REJECTED : ANX_CONT_UNCERTAIN;
	anx_spin_lock_irqsave(&continuation_lock, &flags);
	r->response = NULL;
	event.kind = kind; event.result = run_result;
	ret = owner_check(r);
	if (ret == ANX_OK) ret = journal_check(r);
	if (ret == ANX_OK) ret = object_read(r, &op.source, NULL, NULL, op.source.size, false);
	if (ret == ANX_OK && kind == ANX_CONT_COMMITTED && run_result == ANX_OK) {
		result->key = key; result->status_code = response->status_code; result->size = response->response_size;
		anx_memcpy(result->bytes, response->response_buf, result->size);
		ret = store_object(r, ANX_CONTINUATION_RESULT_SCHEMA, result, sizeof(*result), &r->view.head, 1, &op.result);
		if (ret == ANX_OK) { event.result_object = op.result.oid; anx_memcpy(event.result_digest, op.result.digest, 32); }
	}
	if (ret == ANX_OK && kind == ANX_CONT_COMMITTED && run_result != ANX_OK) ret = run_result;
	if (ret == ANX_OK) ret = append(r, &event);
	if (ret != ANX_OK) kind = ANX_CONT_UNCERTAIN;
	r->operations[index].result = op.result; r->operations[index].kind = kind;
	if (kind == ANX_CONT_COMMITTED) r->view.completed++;
	if (kind == ANX_CONT_UNCERTAIN) r->view.uncertain++;
	r->busy = false;
	if (ret == ANX_OK) {
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
		if (drop_reply && kind == ANX_CONT_COMMITTED) { drop_reply = false; ret = ANX_ETIMEDOUT; }
		else
#endif
		{ *out = r->view; ret = run_result; }
	}
	anx_spin_unlock_irqrestore(&continuation_lock, flags);
	if (settled_ret == ANX_OK && (settled.phase == ANX_EFFECT_PREPARED || settled.phase == ANX_EFFECT_COMMITTED))
		anx_external_operation_discard(&op.operation);
out:
	anx_memset(bridge, 0, sizeof(*bridge)); anx_memset(response, 0, sizeof(*response)); anx_memset(result, 0, sizeof(*result));
	anx_free(bridge); anx_free(response); anx_free(result);
	return ret;
}
int anx_continuation_get(uint64_t id, struct anx_continuation_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&continuation_lock, &flags);
	struct continuation_record *r = find(id); int ret = access(r);
	if (ret == ANX_OK) ret = journal_check(r);
	if (ret == ANX_OK) *out = r->view;
	anx_spin_unlock_irqrestore(&continuation_lock, flags); return ret;
}
int anx_continuation_event_get(uint64_t id, uint32_t index, struct anx_continuation_event *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&continuation_lock, &flags);
	struct continuation_record *r = find(id); int ret = access(r);
	if (ret == ANX_OK && index >= r->view.events) ret = ANX_ENOENT;
	if (ret == ANX_OK) ret = journal_check(r);
	if (ret == ANX_OK) *out = r->events[index];
	anx_spin_unlock_irqrestore(&continuation_lock, flags); return ret;
}
int anx_continuation_read(uint64_t id, uint64_t key, struct anx_continuation_result *out)
{
	if (!id || !key || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&continuation_lock, &flags);
	struct continuation_record *r = find(id); int ret = access(r);
	struct continuation_operation *op = NULL;
	if (ret == ANX_OK) ret = journal_check(r);
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < r->view.operations; i++) if (r->operations[i].key == key) op = &r->operations[i];
		ret = !op ? ANX_ENOENT : op->kind != ANX_CONT_COMMITTED ? ANX_EBUSY : ANX_OK;
	}
	if (ret == ANX_OK) ret = object_read(r, &op->source, NULL, NULL, op->source.size, false);
	if (ret == ANX_OK) ret = object_read(r, &op->result, ANX_CONTINUATION_RESULT_SCHEMA, out, sizeof(*out), false);
	anx_spin_unlock_irqrestore(&continuation_lock, flags); return ret;
}
int anx_continuation_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&continuation_lock, &flags);
	struct continuation_record *r = find(id);
	int ret = !r ? ANX_ENOENT : r->busy || r->view.uncertain ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_CONTINUATION_MAX; i++) if (records[i] == r) records[i] = NULL;
		for (uint32_t i = 0; i < ANX_CONTINUATION_CAPABILITIES; i++) if (r->workers[i]) anx_cell_store_release(r->workers[i]);
		anx_cell_store_release(r->owner); anx_memset(r, 0, sizeof(*r)); anx_free(r);
	}
	anx_spin_unlock_irqrestore(&continuation_lock, flags); return ret;
}

int anx_continuation_suspend_configure(uint64_t id, uint64_t epoch, uint64_t phase_epoch,
		const anx_oid_t *model, struct anx_continuation_view *out)
{
	(void)id; (void)epoch; (void)phase_epoch; (void)model; (void)out; return ANX_ENOSYS;
}
