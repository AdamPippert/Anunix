/*
 * external.c — External-call handler registry.
 *
 * Tiny linear registry keyed by URI scheme. Looked up on every
 * anx_external_invoke() call, so handlers stay cheap to register
 * and cheap to replace (tests register and tear down per case).
 */

#include <anx/types.h>
#include <anx/external_call.h>
#include <anx/external_operation.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>
#include <anx/cell.h>
#include <anx/sched_domain.h>
#include <anx/branch_group.h>
#include <anx/identity.h>
#include <anx/effect_fence.h>
#include <anx/string.h>

#define ANX_EXT_MAX_HANDLERS	16

struct anx_ext_slot {
	char scheme[ANX_EXT_SCHEME_MAX];
	anx_external_handler_fn fn;
	void *ctx;
	bool active;
	uint64_t generation;
};

static struct anx_ext_slot handlers[ANX_EXT_MAX_HANDLERS];
static bool initialized;
static uint64_t handler_sequence;

void anx_external_init(void)
{
	uint32_t i;
	if (anx_cell_current_id())
		return;

	for (i = 0; i < ANX_EXT_MAX_HANDLERS; i++) {
		handlers[i].scheme[0] = '\0';
		handlers[i].fn = NULL;
		handlers[i].ctx = NULL;
		handlers[i].active = false;
	}
	initialized = true;
}

/*
 * Extract the scheme prefix — characters up to the first ':' — into
 * `out`. Returns ANX_OK on success, ANX_EINVAL if the endpoint has no
 * scheme separator or the scheme is empty/too long.
 */
static int parse_scheme(const char *endpoint, char *out, uint32_t out_size)
{
	uint32_t i;

	if (!endpoint || !out || out_size == 0)
		return ANX_EINVAL;

	for (i = 0; endpoint[i] != '\0' && endpoint[i] != ':'; i++) {
		if (i + 1 >= out_size)
			return ANX_EINVAL;
		out[i] = endpoint[i];
	}
	if (i == 0 || endpoint[i] != ':')
		return ANX_EINVAL;
	out[i] = '\0';
	return ANX_OK;
}

static struct anx_ext_slot *find_slot(const char *scheme)
{
	uint32_t i;

	for (i = 0; i < ANX_EXT_MAX_HANDLERS; i++) {
		if (!handlers[i].active)
			continue;
		if (anx_strcmp(handlers[i].scheme, scheme) == 0)
			return &handlers[i];
	}
	return NULL;
}

int anx_external_register_handler(const char *scheme,
				  anx_external_handler_fn fn,
				  void *ctx)
{
	struct anx_ext_slot *slot;
	uint32_t i;

	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!scheme || !fn)
		return ANX_EINVAL;
	if (handler_sequence == ~(uint64_t)0) return ANX_EFULL;
	if (!initialized)
		anx_external_init();

	slot = find_slot(scheme);
	if (slot) {
		slot->fn = fn;
		slot->ctx = ctx;
		slot->generation = ++handler_sequence;
		return ANX_OK;
	}

	for (i = 0; i < ANX_EXT_MAX_HANDLERS; i++) {
		if (handlers[i].active)
			continue;
		anx_strlcpy(handlers[i].scheme, scheme,
			    sizeof(handlers[i].scheme));
		handlers[i].fn = fn;
		handlers[i].ctx = ctx;
		handlers[i].generation = ++handler_sequence;
		handlers[i].active = true;
		return ANX_OK;
	}
	return ANX_ENOMEM;
}

int anx_external_unregister_handler(const char *scheme)
{
	struct anx_ext_slot *slot;
	if (anx_cell_current_id())
		return ANX_EPERM;

	if (!scheme)
		return ANX_EINVAL;
	slot = find_slot(scheme);
	if (!slot)
		return ANX_ENOENT;
	slot->active = false;
	slot->fn = NULL;
	slot->ctx = NULL;
	slot->scheme[0] = '\0';
	return ANX_OK;
}

int anx_external_invoke(struct anx_external_call *call)
{
	char scheme[ANX_EXT_SCHEME_MAX];
	struct anx_ext_slot *slot;
	int ret;

	if (!call)
		return ANX_EINVAL;
	if (!initialized)
		anx_external_init();

	ret = parse_scheme(call->endpoint, scheme, sizeof(scheme));
	if (ret != ANX_OK)
		return ret;

	slot = find_slot(scheme);
	if (!slot)
		return ANX_ENOENT;

	call->response_size = 0;
	call->status_code = 0;
	if (anx_cell_current_id()) {
		struct anx_cell *caller = anx_cell_store_lookup(anx_cell_current_id());
		if (!caller)
			return ANX_EPERM;
		ret = anx_cell_check_contract(caller);
		if (ret == ANX_OK) ret = anx_branch_group_effect_check(&caller->cid);
		if (ret == ANX_OK) ret = anx_sched_domain_check(caller);
		if (ret == ANX_OK) {
			ret = ANX_EPERM;
			if (caller->execution.allow_side_effects && !anx_cell_status_terminal(caller->status) &&
			    anx_identity_admit(caller, NULL) == ANX_OK)
				ret = anx_effect_fence_check(caller, NULL, NULL);
		}
		if (ret == ANX_OK)
			ret = anx_effect_fence_check_sink(caller, NULL);
		if (ret == ANX_OK)
			ret = anx_tool_authorize_call(caller, call);
		anx_cell_store_release(caller);
		if (ret != ANX_OK)
			return ret;
	}
	return slot->fn(call, slot->ctx);
}

/* Explicit protected operations retain their outcome independently of a caller's descriptor. */
struct protected_operation {
	struct anx_external_operation_view view;
	struct anx_external_call call;
	uint8_t body[ANX_EXT_OPERATION_BODY_MAX];
	struct anx_pending_effect *effect;
	struct anx_cell *owner;
	anx_external_handler_fn provider;
	void *provider_context;
	uint64_t provider_generation;
};
static struct protected_operation *operations[ANX_EXT_OPERATION_MAX];
static struct anx_spinlock operation_lock = ANX_SPINLOCK_INIT;
static uint64_t operation_sequence;

static struct protected_operation *operation_find(const anx_oid_t *id, uint32_t *index)
{
	for (uint32_t i = 0; id && i < ANX_EXT_OPERATION_MAX; i++)
		if (operations[i] && !anx_uuid_compare(id, &operations[i]->view.id)) {
			if (index) *index = i;
			return operations[i];
		}
	return NULL;
}
static int operation_source(struct protected_operation *op, bool initial)
{
	if (anx_uuid_is_nil(&op->view.source)) return ANX_OK;
	struct anx_state_object *obj = anx_objstore_lookup(&op->view.source);
	if (!obj) return ANX_ENOENT;
	anx_spin_lock(&obj->lock);
	int ret = obj->state == ANX_OBJ_DELETED || obj->state == ANX_OBJ_TOMBSTONE ? ANX_ENOENT :
		anx_access_evaluate(&obj->access_policy, &op->view.owner, &obj->creator_cell, ANX_ACCESS_READ_PAYLOAD);
	if (ret == ANX_OK && initial) { op->view.source_version = obj->version; op->view.source_hash = obj->content_hash; }
	else if (ret == ANX_OK && (obj->version != op->view.source_version ||
		anx_memcmp(obj->content_hash.bytes, op->view.source_hash.bytes, 32))) ret = ANX_EBUSY;
	anx_spin_unlock(&obj->lock);
	anx_objstore_release(obj);
	return ret;
}
static bool operation_string(const char *text, uint32_t bound)
{
	if (!text) return false;
	for (uint32_t i = 0; i < bound; i++) if (!text[i]) return true;
	return false;
}
int anx_external_operation_prepare(const anx_cid_t *owner, const struct anx_external_call *call,
		const char *sink_name, const anx_oid_t *source, anx_oid_t *id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || anx_uuid_is_nil(owner) || !call || !id || call->request_size > ANX_EXT_OPERATION_BODY_MAX ||
	    (call->request_size && !call->request_body) ||
	    !operation_string(call->endpoint, sizeof(call->endpoint)) || !operation_string(call->method, sizeof(call->method))) return ANX_EINVAL;
	if ((!sink_name || !sink_name[0]) && (call->request_size || (source && !anx_uuid_is_nil(source)))) return ANX_EPERM;
	if (sink_name && !operation_string(sink_name, 64)) return ANX_EINVAL;
	struct anx_sink *sink = sink_name ? anx_sink_lookup(sink_name) : NULL;
	if (sink_name && (!sink || anx_strlen(sink_name) >= sizeof(((struct anx_external_operation_view *)0)->sink_name))) return ANX_ENOENT;
	char scheme[ANX_EXT_SCHEME_MAX];
	int ret = parse_scheme(call->endpoint, scheme, sizeof(scheme));
	if (ret != ANX_OK) return ret;
	struct anx_ext_slot *provider = find_slot(scheme);
	if (!provider) return ANX_ENOENT;
	struct protected_operation *op = anx_zalloc(sizeof(*op));
	if (!op) return ANX_ENOMEM;
	op->owner = anx_cell_store_lookup(owner);
	ret = op->owner ? anx_cell_check_contract(op->owner) : ANX_ENOENT;
	if (ret != ANX_OK) goto fail;
	op->view.owner = *owner; op->view.source = source ? *source : ANX_UUID_NIL;
	if (sink) anx_strlcpy(op->view.sink_name, sink_name, sizeof(op->view.sink_name));
	op->call = *call;
	if (call->request_size) anx_memcpy(op->body, call->request_body, call->request_size);
	op->call.request_body = op->body; op->call.response_size = 0; op->call.status_code = 0;
	anx_memset(op->call.response_buf, 0, sizeof(op->call.response_buf));
	struct anx_sha256_ctx hash;
	anx_sha256_init(&hash);
	anx_sha256_update(&hash, op->call.endpoint, sizeof(op->call.endpoint));
	anx_sha256_update(&hash, op->call.method, sizeof(op->call.method));
	anx_sha256_update(&hash, &op->call.tool_handle, sizeof(op->call.tool_handle));
	anx_sha256_update(&hash, op->body, op->call.request_size);
	anx_sha256_final(&hash, op->view.request_hash.bytes);
	ret = operation_source(op, true);
	if (ret == ANX_OK) ret = anx_tool_authorize_call(op->owner, &op->call);
	if (ret == ANX_OK) ret = anx_effect_prepare(*owner, sink, &op->view.source, &op->effect);
	if (ret != ANX_OK) goto fail;
	op->provider = provider->fn; op->provider_context = provider->ctx; op->provider_generation = provider->generation;
	op->view.phase = ANX_EFFECT_PREPARED;
	bool flags;
	ret = ANX_EFULL;
	anx_spin_lock_irqsave(&operation_lock, &flags);
	for (uint32_t i = 0; operation_sequence != ~(uint64_t)0 && i < ANX_EXT_OPERATION_MAX; i++) if (!operations[i]) {
		op->view.id = (anx_oid_t){ .hi = 0x414e584558544f50ULL, .lo = ++operation_sequence };
		operations[i] = op; *id = op->view.id; ret = ANX_OK; break;
	}
	anx_spin_unlock_irqrestore(&operation_lock, flags);
	if (ret == ANX_OK) return ret;
fail:
	if (op->owner) anx_cell_store_release(op->owner);
	anx_effect_destroy(op->effect); anx_free(op);
	return ret;
}
int anx_external_operation_get(const anx_oid_t *id, struct anx_external_operation_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&operation_lock, &flags);
	struct protected_operation *op = operation_find(id, NULL);
	const anx_cid_t *caller = anx_cell_current_id();
	int ret = !op ? ANX_ENOENT : caller && anx_uuid_compare(caller, &op->view.owner) ? ANX_EPERM : ANX_OK;
	if (ret == ANX_OK) *out = op->view;
	anx_spin_unlock_irqrestore(&operation_lock, flags);
	return ret;
}
int anx_external_operation_dispatch(const anx_oid_t *id, struct anx_external_call *response)
{
	if (!id || !response) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&operation_lock, &flags);
	struct protected_operation *op = operation_find(id, NULL);
	const anx_cid_t *caller = anx_cell_current_id();
	int ret = !op ? ANX_ENOENT : !caller || anx_uuid_compare(caller, &op->view.owner) ? ANX_EPERM : ANX_OK;
	if (ret == ANX_OK && op->view.phase != ANX_EFFECT_PREPARED) ret = ANX_EBUSY;
	struct anx_ext_slot *provider = NULL;
	char scheme[ANX_EXT_SCHEME_MAX];
	if (ret == ANX_OK) ret = parse_scheme(op->call.endpoint, scheme, sizeof(scheme));
	if (ret == ANX_OK) {
		provider = find_slot(scheme);
		if (!provider || provider->fn != op->provider || provider->ctx != op->provider_context ||
		    provider->generation != op->provider_generation) ret = ANX_EBUSY;
	}
	if (ret == ANX_OK) ret = anx_cell_check_contract(op->owner);
	if (ret == ANX_OK) ret = anx_sched_domain_check(op->owner);
	if (ret == ANX_OK) ret = anx_tool_authorize_call(op->owner, &op->call);
	if (ret == ANX_OK) ret = operation_source(op, false);
	if (ret == ANX_OK && op->view.sink_name[0] && anx_sink_lookup(op->view.sink_name) != op->effect->sink) ret = ANX_EPERM;
	if (ret == ANX_OK) ret = anx_effect_mark_dispatching(op->effect);
	if (ret == ANX_OK) op->view.phase = ANX_EFFECT_DISPATCHING;
	anx_spin_unlock_irqrestore(&operation_lock, flags);
	if (ret != ANX_OK) return ret;
	/* Registry state blocks reentry and cleanup while the actual provider is running. */
	ret = op->provider(&op->call, op->provider_context);
	if (op->call.response_size > ANX_EXT_RESPONSE_MAX) ret = ANX_EIO;
	anx_spin_lock_irqsave(&operation_lock, &flags);
	op->view.transport_result = ret;
	if (ret == ANX_OK) anx_effect_commit(op->effect);
	else anx_effect_mark_unknown(op->effect);
	op->view.phase = op->effect->phase;
	response->response_size = op->call.response_size <= ANX_EXT_RESPONSE_MAX ? op->call.response_size : 0;
	response->status_code = op->call.status_code;
	if (response->response_size) anx_memcpy(response->response_buf, op->call.response_buf, response->response_size);
	struct anx_cell *owner = op->owner; op->owner = NULL;
	anx_spin_unlock_irqrestore(&operation_lock, flags);
	anx_cell_store_release(owner);
	return ret;
}
int anx_external_operation_discard(const anx_oid_t *id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags;
	uint32_t index = 0;
	anx_spin_lock_irqsave(&operation_lock, &flags);
	struct protected_operation *op = operation_find(id, &index);
	int ret = op ? ANX_OK : ANX_ENOENT;
	if (ret == ANX_OK && op->view.phase != ANX_EFFECT_PREPARED && op->view.phase != ANX_EFFECT_COMMITTED) ret = ANX_EBUSY;
	if (ret == ANX_OK) operations[index] = NULL;
	anx_spin_unlock_irqrestore(&operation_lock, flags);
	if (ret == ANX_OK) {
		if (op->owner) anx_cell_store_release(op->owner);
		anx_effect_destroy(op->effect); anx_memset(op, 0, sizeof(*op)); anx_free(op);
	}
	return ret;
}
