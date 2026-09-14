/* Predictions reserve private bytes; verified publication uses staged mutation. */
#include <anx/speculation.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/arch.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct speculation_record {
	struct anx_speculation_view view;
	struct anx_cell *owner;
	void *result;
	uint8_t action[ANX_SPECULATION_ACTION_MAX], digest[32];
	uint32_t action_size;
	enum anx_object_type type;
	enum anx_sensitivity sensitivity;
};
static struct speculation_record records[ANX_SPECULATION_MAX];
static struct anx_spinlock speculation_lock = ANX_SPINLOCK_INIT;
static uint64_t reserved_bytes;

static struct speculation_record *lookup(const anx_oid_t *id)
{
	for (uint32_t i = 0; i < ANX_SPECULATION_MAX; i++)
		if (!anx_uuid_is_nil(&records[i].view.id) && !anx_uuid_compare(id, &records[i].view.id)) return &records[i];
	return NULL;
}
static int caller_check(const anx_cid_t *owner)
{
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, owner) ? ANX_EPERM : ANX_OK;
}
static void close_record(struct speculation_record *r, enum anx_speculation_state state, int reason)
{
	if (r->view.state == ANX_SPECULATION_PREPARED) {
		reserved_bytes -= r->view.result_size;
		anx_free(r->result); r->result = NULL;
		anx_cell_store_release(r->owner); r->owner = NULL;
	}
	r->view.state = state; r->view.reason = reason;
}
static void expire(anx_time_t now)
{
	for (uint32_t i = 0; i < ANX_SPECULATION_MAX; i++)
		if (records[i].view.state == ANX_SPECULATION_PREPARED && records[i].view.expires_at <= now)
			close_record(&records[i], ANX_SPECULATION_EXPIRED, ANX_ETIMEDOUT);
}
/* Caller holds the object's lock; capture committed bytes, never another stage. */
static int origin_digest(struct anx_state_object *obj, const anx_cid_t *owner, uint8_t digest[32])
{
	if (obj->state != ANX_OBJ_ACTIVE) return ANX_EPERM;
	if (obj->payload_size > ANX_SPECULATION_PAYLOAD_MAX || (obj->payload_size && !obj->payload)) return ANX_EINVAL;
	int ret = anx_access_evaluate(&obj->access_policy, owner, &obj->creator_cell, ANX_ACCESS_READ_PAYLOAD);
	if (ret != ANX_OK) return ret;
	struct anx_sha256_ctx hash;
	anx_sha256_init(&hash);
	anx_sha256_update(&hash, obj->schema_uri, sizeof(obj->schema_uri));
	anx_sha256_update(&hash, obj->schema_version, sizeof(obj->schema_version));
	anx_sha256_update(&hash, obj->payload, (uint32_t)obj->payload_size);
	anx_sha256_final(&hash, digest);
	return ANX_OK;
}

int anx_speculation_prepare(const struct anx_speculation_request *request, anx_oid_t *out)
{
	struct speculation_record candidate = {0};
	struct anx_object_handle h = {0};
	uint32_t owner_count = 0, slot;
	bool flags;
	anx_time_t now = arch_time_now();
	int ret;
	if (!request || !out || anx_uuid_is_nil(&request->owner) || anx_uuid_is_nil(&request->origin) ||
	    !request->action || !request->action_size || request->action_size > ANX_SPECULATION_ACTION_MAX ||
	    (!request->result && request->result_size) || request->result_size > ANX_SPECULATION_PAYLOAD_MAX ||
	    request->expires_at <= now || request->expires_at - now > ANX_SPECULATION_LIFETIME_MAX) return ANX_EINVAL;
	ret = caller_check(&request->owner);
	if (ret != ANX_OK) return ret;
	candidate.owner = anx_cell_store_lookup(&request->owner);
	if (!candidate.owner) return ANX_ENOENT;
	if (anx_cell_status_terminal(candidate.owner->status)) { ret = ANX_EPERM; goto done; }
	ret = anx_so_open(&request->origin, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) goto done;
	anx_spin_lock(&h.obj->lock);
	ret = h.obj->staged ? ANX_EBUSY : origin_digest(h.obj, &request->owner, candidate.digest);
	if (ret == ANX_OK) {
		candidate.view.origin_version = h.obj->version;
		candidate.type = h.obj->object_type; candidate.sensitivity = h.obj->sensitivity;
	}
	anx_spin_unlock(&h.obj->lock);
	if (ret != ANX_OK) goto done;
	candidate.view.owner = request->owner; candidate.view.origin = request->origin;
	candidate.view.expires_at = request->expires_at; candidate.view.result_size = request->result_size;
	candidate.action_size = request->action_size;
	anx_memcpy(candidate.action, request->action, request->action_size);
	anx_spin_lock_irqsave(&speculation_lock, &flags);
	expire(arch_time_now());
	for (slot = 0; slot < ANX_SPECULATION_MAX; slot++) if (anx_uuid_is_nil(&records[slot].view.id)) break;
	for (uint32_t i = 0; i < ANX_SPECULATION_MAX; i++)
		if (records[i].view.state == ANX_SPECULATION_PREPARED && !anx_uuid_compare(&records[i].view.owner, &request->owner)) owner_count++;
	if (slot == ANX_SPECULATION_MAX || owner_count >= ANX_SPECULATION_OWNER_MAX ||
	    request->result_size > ANX_SPECULATION_BYTES_MAX - reserved_bytes) ret = ANX_EFULL;
	else if (arch_time_now() >= request->expires_at) ret = ANX_ETIMEDOUT;
	else {
		if (request->result_size) candidate.result = anx_alloc(request->result_size);
		if (request->result_size && !candidate.result) ret = ANX_ENOMEM;
		else {
			if (request->result_size) anx_memcpy(candidate.result, request->result, request->result_size);
			anx_uuid_generate(&candidate.view.id); candidate.view.state = ANX_SPECULATION_PREPARED;
			records[slot] = candidate; reserved_bytes += request->result_size; *out = candidate.view.id;
			candidate.owner = NULL; candidate.result = NULL; ret = ANX_OK;
		}
	}
	anx_spin_unlock_irqrestore(&speculation_lock, flags);
done:
	anx_so_close(&h); anx_cell_store_release(candidate.owner); anx_free(candidate.result);
	return ret;
}

int anx_speculation_get(const anx_oid_t *id, struct anx_speculation_view *out)
{
	bool flags;
	int ret = ANX_ENOENT;
	if (!id || !out) return ANX_EINVAL;
	anx_spin_lock_irqsave(&speculation_lock, &flags);
	expire(arch_time_now());
	struct speculation_record *r = lookup(id);
	if (r) { ret = caller_check(&r->view.owner); if (ret == ANX_OK) *out = r->view; }
	anx_spin_unlock_irqrestore(&speculation_lock, flags);
	return ret;
}

static int origin_check(const struct speculation_record *r, struct anx_state_object *obj, bool own_stage)
{
	uint8_t digest[32];
	int ret = origin_digest(obj, &r->view.owner, digest);
	if (ret != ANX_OK) return ret;
	if ((!own_stage && obj->staged) || (own_stage && (!obj->staged ||
	    anx_uuid_compare(&obj->staged->staging_cell, &r->view.owner) || obj->staged->base_version != r->view.origin_version))) return ANX_EBUSY;
	return obj->version == r->view.origin_version && obj->object_type == r->type && obj->sensitivity == r->sensitivity &&
		!anx_memcmp(digest, r->digest, sizeof(digest)) ? ANX_OK : ANX_EBUSY;
}

int anx_speculation_commit(const anx_oid_t *id, const void *actual, uint32_t size)
{
	struct anx_object_handle h = {0};
	bool flags, staged = false;
	int ret = ANX_ENOENT;
	if (!id || !actual || !size || size > ANX_SPECULATION_ACTION_MAX) return ANX_EINVAL;
	anx_spin_lock_irqsave(&speculation_lock, &flags);
	expire(arch_time_now());
	struct speculation_record *r = lookup(id);
	if (!r) goto out;
	ret = caller_check(&r->view.owner);
	if (ret != ANX_OK) goto out;
	if (r->view.state != ANX_SPECULATION_PREPARED) { ret = r->view.reason ? r->view.reason : ANX_EBUSY; goto out; }
	if (size != r->action_size || anx_memcmp(actual, r->action, size)) { ret = ANX_ECANCELED; goto discard; }
	ret = anx_so_open(&r->view.origin, ANX_OPEN_READWRITE, &h);
	if (ret != ANX_OK) goto discard;
	anx_spin_lock(&h.obj->lock); ret = origin_check(r, h.obj, false); anx_spin_unlock(&h.obj->lock);
	if (ret != ANX_OK) goto discard;
	ret = anx_object_stage(&h, r->view.owner);
	if (ret != ANX_OK) goto discard;
	staged = true;
	anx_spin_lock(&h.obj->lock); ret = origin_check(r, h.obj, true); anx_spin_unlock(&h.obj->lock);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&h, r->result, r->view.result_size);
	if (ret == ANX_OK && arch_time_now() >= r->view.expires_at) ret = ANX_ETIMEDOUT;
	if (ret == ANX_OK) ret = anx_object_commit(&h);
	if (ret == ANX_OK) { close_record(r, ANX_SPECULATION_COMMITTED, ANX_OK); goto out; }
discard:
	if (staged) anx_object_abort(&h);
	close_record(r, ret == ANX_ETIMEDOUT ? ANX_SPECULATION_EXPIRED : ANX_SPECULATION_DISCARDED, ret);
out:
	anx_so_close(&h);
	anx_spin_unlock_irqrestore(&speculation_lock, flags);
	return ret;
}

static int remove_record(const anx_oid_t *id, bool release)
{
	bool flags;
	int ret = ANX_ENOENT;
	if (!id) return ANX_EINVAL;
	anx_spin_lock_irqsave(&speculation_lock, &flags);
	struct speculation_record *r = lookup(id);
	if (r) {
		ret = caller_check(&r->view.owner);
		if (ret == ANX_OK) {
			if (r->view.state == ANX_SPECULATION_PREPARED) close_record(r, ANX_SPECULATION_DISCARDED, ANX_ECANCELED);
			if (release) anx_memset(r, 0, sizeof(*r));
		}
	}
	anx_spin_unlock_irqrestore(&speculation_lock, flags);
	return ret;
}
int anx_speculation_discard(const anx_oid_t *id) { return remove_record(id, false); }
int anx_speculation_release(const anx_oid_t *id) { return remove_record(id, true); }
