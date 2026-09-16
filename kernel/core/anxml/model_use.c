#include <anx/model_use.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>
#include <anx/identity.h>
#include <anx/sched_domain.h>
#include <anx/resource_view.h>

struct model_use {
	struct anx_model_use_view view;
	struct anx_cell *owner;
	anx_cid_t parent;
	struct anx_anxml_response response;
};
struct materialization {
	struct anx_adapter_image image;
	struct anx_anxml_request request;
	struct anx_anxml_response response;
};
static struct model_use *records[ANX_MODEL_USE_MAX];
static struct anx_spinlock use_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct model_use *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_MODEL_USE_MAX; i++) if (records[i] && records[i]->view.id == id) return records[i];
	return NULL;
}
static int access(struct model_use *u)
{
	if (!u) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, &u->view.owner) ? ANX_EPERM : ANX_OK;
}
static int owner_check(struct model_use *u, bool capture)
{
	anx_oid_t identity;
	if (anx_cell_status_terminal(u->owner->status) || anx_uuid_compare(&u->owner->cid, &u->view.owner) ||
	    anx_uuid_compare(&u->owner->parent_cid, &u->parent)) return ANX_EPERM;
	int ret = anx_cell_check_scope(u->owner);
	if (ret == ANX_OK) ret = anx_cell_check_contract(u->owner);
	if (ret == ANX_OK) ret = anx_sched_domain_check(u->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(u->owner, &identity);
	if (ret == ANX_OK && !capture && anx_uuid_compare(&identity, &u->view.identity_record)) ret = ANX_EBUSY;
	if (ret == ANX_OK && u->owner->cognitive.max_tokens && u->view.maximum_tokens > u->owner->cognitive.max_tokens) ret = ANX_EPERM;
	if (ret == ANX_OK && capture) u->view.identity_record = identity;
	return ret;
}
static int metadata(struct anx_state_object *o, bool model, const anx_cid_t *owner)
{
	if (!o->version || o->state != ANX_OBJ_SEALED || !o->payload || !o->payload_size ||
	    o->access_policy.rule_count > ANX_MAX_ACCESS_RULES || o->sensitivity > ANX_SENSITIVITY_RESTRICTED) return ANX_EINVAL;
	if (model) {
		if (o->object_type != ANX_OBJ_STRUCTURED_DATA || o->payload_size != sizeof(struct anx_adapter_image) ||
		    anx_strcmp(o->schema_uri, ANX_MODEL_USE_SCHEMA) || anx_strcmp(o->schema_version, "1")) return ANX_EINVAL;
	} else if (o->object_type != ANX_OBJ_BYTE_DATA || o->payload_size > ANX_ANXML_PROMPT_MAX) return ANX_EINVAL;
	return anx_access_evaluate(&o->access_policy, owner, &o->creator_cell, ANX_ACCESS_READ_PAYLOAD);
}
static int source_read(struct model_use *u, struct anx_model_use_source *source, bool model, bool capture, void *bytes)
{
	struct anx_object_handle h = {0};
	struct anx_model_use_source now = { .oid = source->oid };
	int ret = anx_so_open(&source->oid, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	anx_spin_lock(&h.obj->lock);
	ret = metadata(h.obj, model, &u->view.owner);
	if (ret == ANX_OK) {
		now.version = h.obj->version; now.size = h.obj->payload_size; now.sensitivity = h.obj->sensitivity;
		if (!capture && (now.version != source->version || now.size != source->size || now.sensitivity != source->sensitivity)) ret = ANX_EBUSY;
	}
	anx_spin_unlock(&h.obj->lock);
	if (ret == ANX_OK) {
		ret = anx_so_read_payload(&h, 0, bytes, now.size);
		if (ret == (int)now.size) ret = ANX_OK;
		else if (ret >= 0) ret = ANX_EIO;
	}
	if (ret == ANX_OK) {
		anx_sha256(bytes, now.size, now.digest);
		anx_spin_lock(&h.obj->lock);
		ret = metadata(h.obj, model, &u->view.owner);
		if (ret == ANX_OK && (h.obj->version != now.version || h.obj->payload_size != now.size ||
		    (uint32_t)h.obj->sensitivity != now.sensitivity)) ret = ANX_EBUSY;
		anx_spin_unlock(&h.obj->lock);
		if (ret == ANX_OK && !capture && anx_memcmp(now.digest, source->digest, 32)) ret = ANX_EBUSY;
		if (ret == ANX_OK && capture) *source = now;
	}
	anx_so_close(&h);
	return ret;
}
static void request_digest(const struct anx_anxml_request *request, uint8_t digest[32])
{
	struct anx_sha256_ctx hash;
	anx_sha256_init(&hash);
	anx_sha256_update(&hash, "anx-model-use-request-v1", 24);
	anx_sha256_update(&hash, &request->prompt_len, sizeof(request->prompt_len));
	anx_sha256_update(&hash, request->prompt, request->prompt_len);
	anx_sha256_update(&hash, &request->max_tokens, sizeof(request->max_tokens));
	anx_sha256_update(&hash, &request->seed, sizeof(request->seed));
	anx_sha256_final(&hash, digest);
}
static int materialize(struct model_use *u, struct materialization *m, bool capture)
{
	int ret = source_read(u, &u->view.image, true, capture, &m->image);
	if (ret == ANX_OK) ret = anx_adapter_image_check(&m->image);
	if (ret == ANX_OK) ret = source_read(u, &u->view.prompt, false, capture, m->request.prompt);
	if (ret == ANX_OK) {
		m->request.prompt_len = u->view.prompt.size; m->request.max_tokens = u->view.maximum_tokens;
		m->request.seed = u->view.seed;
		uint8_t digest[32];
		request_digest(&m->request, digest);
		if (capture) anx_memcpy(u->view.request_digest, digest, 32);
		else if (anx_memcmp(digest, u->view.request_digest, 32)) ret = ANX_EBUSY;
	}
	return ret;
}
int anx_model_use_prepare(const anx_cid_t *owner, const struct anx_model_use_spec *spec, struct anx_model_use_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !spec || !out) return ANX_EINVAL;
	struct anx_model_use_spec copy = *spec;
	if (!copy.maximum_tokens || copy.maximum_tokens > 128 || anx_uuid_is_nil(&copy.image) || anx_uuid_is_nil(&copy.prompt)) return ANX_EINVAL;
	struct model_use *u = anx_zalloc(sizeof(*u));
	struct materialization *m = anx_zalloc(sizeof(*m));
	int ret = ANX_ENOMEM;
	if (!u || !m) goto out;
	u->view.owner = *owner; u->view.image.oid = copy.image; u->view.prompt.oid = copy.prompt;
	u->view.maximum_tokens = copy.maximum_tokens; u->view.seed = copy.seed;
	u->owner = anx_cell_store_lookup(owner);
	if (u->owner) u->parent = u->owner->parent_cid;
	ret = u->owner ? owner_check(u, true) : ANX_ENOENT;
	if (ret == ANX_OK) ret = materialize(u, m, true);
	if (ret == ANX_OK) {
		bool flags;
		anx_spin_lock_irqsave(&use_lock, &flags);
		uint32_t i;
		for (i = 0; i < ANX_MODEL_USE_MAX; i++) if (!records[i]) break;
		if (i == ANX_MODEL_USE_MAX || sequence == ~(uint64_t)0) ret = ANX_EFULL;
		else { u->view.id = ++sequence; u->view.epoch = 1; records[i] = u; *out = u->view; }
		anx_spin_unlock_irqrestore(&use_lock, flags);
	}
out:
	if (m) { anx_memset(m, 0, sizeof(*m)); anx_free(m); }
	if (ret != ANX_OK && u) { if (u->owner) anx_cell_store_release(u->owner); anx_memset(u, 0, sizeof(*u)); anx_free(u); }
	return ret;
}
int anx_model_use_get(uint64_t id, struct anx_model_use_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&use_lock, &flags);
	struct model_use *u = find(id);
	int ret = access(u);
	if (ret == ANX_OK && u->view.state == ANX_MODEL_USE_BUSY) ret = ANX_EBUSY;
	if (ret == ANX_OK) *out = u->view;
	anx_spin_unlock_irqrestore(&use_lock, flags);
	return ret;
}
static int execute(uint64_t id, uint64_t epoch, const anx_oid_t *image_view,
		struct anx_anxml_response *response, struct anx_model_use_view *out)
{
	if (!id || !epoch || !response || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&use_lock, &flags);
	struct model_use *u = find(id);
	int ret = access(u);
	if (ret == ANX_OK && (u->view.epoch != epoch || u->view.state != ANX_MODEL_USE_READY)) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = owner_check(u, false);
	if (ret == ANX_OK) u->view.state = ANX_MODEL_USE_BUSY;
	anx_spin_unlock_irqrestore(&use_lock, flags);
	if (ret != ANX_OK) return ret;
	struct materialization *m = anx_zalloc(sizeof(*m));
	ret = m ? materialize(u, m, false) : ANX_ENOMEM;
	if (ret == ANX_OK && image_view) {
		struct anx_resource_view_info info;
		ret = anx_resource_view_info(image_view, &info);
		if (ret == ANX_OK && (anx_uuid_compare(&info.owner, &u->view.owner) || info.bytes != sizeof(m->image))) ret = ANX_EPERM;
		if (ret == ANX_OK) {
			ret = anx_resource_view_read(image_view, 0, &m->image, sizeof(m->image));
			if (ret == sizeof(m->image)) ret = ANX_OK;
			else if (ret >= 0) ret = ANX_EIO;
		}
	}
	if (ret == ANX_OK) ret = anx_anxml_generate_verified(&m->request, &m->image, u->view.image.digest, &m->response);
	/* No bytes are exposed until the source and owner still match at completion. */
	if (ret == ANX_OK) ret = owner_check(u, false);
	if (ret == ANX_OK) ret = materialize(u, m, false);
	anx_spin_lock_irqsave(&use_lock, &flags);
	if (ret == ANX_OK) {
		u->view.state = ANX_MODEL_USE_COMPLETED; u->view.epoch++;
		u->view.output_size = m->response.output_len; u->view.tokens_generated = m->response.tokens_generated;
		anx_memcpy(u->view.consumed_image_digest, u->view.image.digest, 32);
		anx_sha256(m->response.output, m->response.output_len, u->view.output_digest);
		u->response = m->response;
		*response = m->response; *out = u->view;
	} else u->view.state = ANX_MODEL_USE_READY;
	anx_spin_unlock_irqrestore(&use_lock, flags);
	if (m) { anx_memset(m, 0, sizeof(*m)); anx_free(m); }
	return ret;
}
int anx_model_use_execute(uint64_t id, uint64_t epoch, struct anx_anxml_response *response, struct anx_model_use_view *out)
{ return execute(id, epoch, NULL, response, out); }
int anx_model_use_execute_view(uint64_t id, uint64_t epoch, const anx_oid_t *image_view,
		struct anx_anxml_response *response, struct anx_model_use_view *out)
{
	if (!image_view || anx_uuid_is_nil(image_view)) return ANX_EINVAL;
	anx_oid_t copy = *image_view;
	return execute(id, epoch, &copy, response, out);
}
int anx_model_use_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&use_lock, &flags);
	struct model_use *u = find(id);
	int ret = !u ? ANX_ENOENT : u->view.state == ANX_MODEL_USE_BUSY ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_MODEL_USE_MAX; i++) if (records[i] == u) records[i] = NULL;
		anx_cell_store_release(u->owner); anx_memset(u, 0, sizeof(*u)); anx_free(u);
	}
	anx_spin_unlock_irqrestore(&use_lock, flags);
	return ret;
}

static bool same_source(const struct anx_model_use_source *a, const struct anx_model_use_source *b)
{
	return !anx_uuid_compare(&a->oid, &b->oid) && a->version == b->version && a->size == b->size &&
		a->sensitivity == b->sensitivity && !anx_memcmp(a->digest, b->digest, 32);
}
bool anx_model_use_same_request(const struct anx_model_use_view *a, const struct anx_model_use_view *b)
{
	return a && b && !anx_uuid_compare(&a->owner, &b->owner) && !anx_uuid_compare(&a->identity_record, &b->identity_record) &&
		a->maximum_tokens == b->maximum_tokens && a->seed == b->seed &&
		same_source(&a->image, &b->image) && same_source(&a->prompt, &b->prompt) &&
		!anx_memcmp(a->request_digest, b->request_digest, 32);
}
static int response_check(struct model_use *u)
{
	uint8_t digest[32];
	if (!u->response.output_len || u->response.output_len >= ANX_ANXML_OUTPUT_MAX ||
	    u->response.output_len != u->view.output_size) return ANX_EIO;
	anx_sha256(u->response.output, u->response.output_len, digest);
	return anx_memcmp(digest, u->view.output_digest, 32) ? ANX_EIO : ANX_OK;
}
int anx_model_use_reuse(uint64_t id, uint64_t epoch, uint64_t source, struct anx_anxml_response *response, struct anx_model_use_view *out)
{
	if (!id || !epoch || !source || id == source || !response || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&use_lock, &flags);
	struct model_use *u = find(id), *s = find(source);
	int ret = access(u);
	if (ret == ANX_OK) ret = access(s);
	if (ret == ANX_OK && (u->view.epoch != epoch || u->view.state != ANX_MODEL_USE_READY || s->view.state != ANX_MODEL_USE_COMPLETED)) ret = ANX_EBUSY;
	if (ret == ANX_OK && !anx_model_use_same_request(&u->view, &s->view)) ret = ANX_EPERM;
	/* Explicitly seeded randomness remains an execution request, even if its toy output is reproducible. */
	if (ret == ANX_OK && u->view.seed) ret = ANX_ENOTSUP;
	if (ret == ANX_OK) ret = owner_check(u, false);
	if (ret == ANX_OK) ret = owner_check(s, false);
	if (ret == ANX_OK) ret = response_check(s);
	struct materialization *m = NULL;
	if (ret == ANX_OK) { m = anx_zalloc(sizeof(*m)); ret = m ? materialize(u, m, false) : ANX_ENOMEM; }
	if (ret == ANX_OK) {
		u->response = s->response; u->response.tokens_generated = 0;
		u->view.state = ANX_MODEL_USE_COMPLETED; u->view.epoch++; u->view.reused_from = source;
		u->view.output_size = s->view.output_size; u->view.tokens_generated = 0;
		anx_memcpy(u->view.consumed_image_digest, s->view.consumed_image_digest, 32);
		anx_memcpy(u->view.output_digest, s->view.output_digest, 32);
		*response = u->response; *out = u->view;
	}
	anx_spin_unlock_irqrestore(&use_lock, flags);
	if (m) { anx_memset(m, 0, sizeof(*m)); anx_free(m); }
	return ret;
}
int anx_model_use_read(uint64_t id, struct anx_anxml_response *response)
{
	if (!id || !response) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&use_lock, &flags);
	struct model_use *u = find(id);
	int ret = access(u);
	if (ret == ANX_OK && u->view.state != ANX_MODEL_USE_COMPLETED) ret = ANX_EPERM;
	if (ret == ANX_OK) ret = response_check(u);
	struct materialization *m = NULL;
	anx_oid_t identity;
	if (ret == ANX_OK) ret = anx_identity_admit(u->owner, &identity);
	if (ret == ANX_OK && anx_uuid_compare(&identity, &u->view.identity_record)) ret = ANX_EBUSY;
	if (ret == ANX_OK) { m = anx_zalloc(sizeof(*m)); ret = m ? materialize(u, m, false) : ANX_ENOMEM; }
	if (ret == ANX_OK) *response = u->response;
	anx_spin_unlock_irqrestore(&use_lock, flags);
	if (m) { anx_memset(m, 0, sizeof(*m)); anx_free(m); }
	return ret;
}
