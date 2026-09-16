/* Private boot-local adapter images and atomic serving-version transitions. */
#include <anx/adapter.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>

struct adapter_record {
	struct anx_cell *owner;
	struct anx_adapter_view view;
	struct anx_adapter_image current, prior, candidate;
	uint64_t candidate_token, candidate_base;
	bool verified, have_prior;
};
static struct adapter_record adapters[ANX_ADAPTER_MAX];
static struct anx_spinlock adapter_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;

int anx_adapter_image_check(const struct anx_adapter_image *image)
{
	if (!image || image->format != 1 || !image->count || image->count > ANX_ADAPTER_DELTA_MAX) return ANX_EINVAL;
	for (uint32_t i = 0; i < ANX_ADAPTER_DELTA_MAX; i++) {
		const struct anx_adapter_delta *d = &image->deltas[i];
		if (i >= image->count) {
			if (d->previous || d->next || d->boost) return ANX_EINVAL;
			continue;
		}
		if (d->previous >= 128 || d->next >= 128 || !d->boost || d->boost > 4096) return ANX_EINVAL;
		for (uint32_t j = 0; j < i; j++)
			if (image->deltas[j].previous == d->previous && image->deltas[j].next == d->next) return ANX_EINVAL;
	}
	return ANX_OK;
}
static struct adapter_record *find(const anx_oid_t *id)
{
	for (uint32_t i = 0; id && i < ANX_ADAPTER_MAX; i++)
		if (adapters[i].owner && !anx_uuid_compare(id, &adapters[i].view.id)) return &adapters[i];
	return NULL;
}
static int access(struct adapter_record *a, bool changing)
{
	if (!a) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	if (caller && (changing || anx_uuid_compare(caller, &a->owner->cid))) return ANX_EPERM;
	if (changing && anx_cell_status_terminal(a->owner->status)) return ANX_EPERM;
	return ANX_OK;
}
static void clear_candidate(struct adapter_record *a)
{
	anx_memset(&a->candidate, 0, sizeof(a->candidate));
	a->candidate_token = a->candidate_base = 0; a->verified = false;
}
static int request_check(const struct anx_anxml_request *r)
{
	return !r || r->model_name[0] || r->prompt_len > ANX_ANXML_PROMPT_MAX || !r->max_tokens || r->max_tokens > 128 ? ANX_EINVAL : ANX_OK;
}
static void request_hash(const struct anx_anxml_request *r, struct anx_hash *hash)
{
	struct anx_sha256_ctx ctx;
	anx_sha256_init(&ctx);
	anx_sha256_update(&ctx, "anxml-toy-adapter-cache-v1", sizeof("anxml-toy-adapter-cache-v1") - 1);
	anx_sha256_update(&ctx, &r->prompt_len, sizeof(r->prompt_len));
	anx_sha256_update(&ctx, r->prompt, r->prompt_len);
	anx_sha256_update(&ctx, &r->max_tokens, sizeof(r->max_tokens));
	anx_sha256_update(&ctx, &r->seed, sizeof(r->seed));
	anx_sha256_final(&ctx, hash->bytes);
}
int anx_adapter_create(const anx_cid_t *owner, const struct anx_adapter_image *image, struct anx_adapter_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || anx_uuid_is_nil(owner) || !image || !out) return ANX_EINVAL;
	struct anx_adapter_image copy = *image;
	int ret = anx_adapter_image_check(&copy);
	if (ret != ANX_OK) return ret;
	struct anx_cell *cell = anx_cell_store_lookup(owner);
	if (!cell) return ANX_ENOENT;
	if (anx_cell_status_terminal(cell->status)) { anx_cell_store_release(cell); return ANX_EPERM; }
	bool flags;
	ret = ANX_EFULL;
	anx_spin_lock_irqsave(&adapter_lock, &flags);
	for (uint32_t i = 0; sequence != ~(uint64_t)0 && i < ANX_ADAPTER_MAX; i++) if (!adapters[i].owner) {
		struct adapter_record *a = &adapters[i];
		a->owner = cell; a->current = copy; a->view.generation = ++sequence;
		a->view.id = (anx_oid_t){ .hi = 0x414e584144415054ULL, .lo = sequence };
		anx_sha256(&copy, sizeof(copy), a->view.image_hash.bytes);
		*out = a->view; ret = ANX_OK; break;
	}
	anx_spin_unlock_irqrestore(&adapter_lock, flags);
	if (ret != ANX_OK) anx_cell_store_release(cell);
	return ret;
}
int anx_adapter_get(const anx_oid_t *id, struct anx_adapter_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&adapter_lock, &flags);
	struct adapter_record *a = find(id);
	int ret = access(a, false);
	if (ret == ANX_OK) *out = a->view;
	anx_spin_unlock_irqrestore(&adapter_lock, flags);
	return ret;
}
int anx_adapter_stage(const anx_oid_t *id, uint64_t generation, const struct anx_adapter_image *image, uint64_t *candidate)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !generation || !image || !candidate) return ANX_EINVAL;
	struct anx_adapter_image copy = *image;
	int ret = anx_adapter_image_check(&copy);
	if (ret != ANX_OK) return ret;
	bool flags;
	anx_spin_lock_irqsave(&adapter_lock, &flags);
	struct adapter_record *a = find(id);
	ret = access(a, true);
	if (ret == ANX_OK && (a->candidate_token || a->view.generation != generation)) ret = ANX_EBUSY;
	if (ret == ANX_OK && sequence == ~(uint64_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK) {
		a->candidate = copy; a->candidate_base = generation; a->candidate_token = ++sequence;
		a->verified = false; *candidate = a->candidate_token;
	}
	anx_spin_unlock_irqrestore(&adapter_lock, flags);
	return ret;
}
int anx_adapter_verify(const anx_oid_t *id, uint64_t token, const struct anx_adapter_trial *trials, uint32_t count)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !token || !trials || !count || count > ANX_ADAPTER_TRIAL_MAX) return ANX_EINVAL;
	struct anx_adapter_trial *copy = anx_alloc(count * sizeof(*copy));
	struct anx_anxml_response *response = anx_alloc(sizeof(*response));
	if (!copy || !response) { anx_free(copy); anx_free(response); return ANX_ENOMEM; }
	anx_memcpy(copy, trials, count * sizeof(*copy));
	bool flags;
	anx_spin_lock_irqsave(&adapter_lock, &flags);
	struct adapter_record *a = find(id);
	int ret = access(a, true);
	if (ret == ANX_OK && a->candidate_token != token) ret = ANX_EBUSY;
	if (ret == ANX_OK) {
		a->verified = false;
		for (uint32_t i = 0; i < count; i++) {
			struct anx_adapter_trial *t = &copy[i];
			ret = request_check(&t->request);
			if (ret == ANX_OK && (t->request.seed || !t->expected_len || t->expected_len > t->request.max_tokens)) ret = ANX_EINVAL;
			if (ret == ANX_OK) ret = anx_anxml_generate_image(&t->request, &a->candidate, response);
			if (ret == ANX_OK && (response->output_len != t->expected_len ||
			    anx_memcmp(response->output, t->expected, t->expected_len))) ret = ANX_EIO;
			if (ret != ANX_OK) break;
		}
		if (ret == ANX_OK) a->verified = true;
	}
	anx_spin_unlock_irqrestore(&adapter_lock, flags);
	anx_free(response); anx_free(copy);
	return ret;
}
int anx_adapter_publish(const anx_oid_t *id, uint64_t token, uint64_t generation, struct anx_adapter_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !token || !generation || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&adapter_lock, &flags);
	struct adapter_record *a = find(id);
	int ret = access(a, true);
	if (ret == ANX_OK && (a->candidate_token != token || a->view.generation != generation || a->candidate_base != generation)) ret = ANX_EBUSY;
	if (ret == ANX_OK && !a->verified) ret = ANX_EPERM;
	if (ret == ANX_OK && sequence == ~(uint64_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK) {
		a->prior = a->current; a->have_prior = true; a->current = a->candidate;
		a->view.generation = ++sequence;
		anx_sha256(&a->current, sizeof(a->current), a->view.image_hash.bytes);
		clear_candidate(a); *out = a->view;
	}
	anx_spin_unlock_irqrestore(&adapter_lock, flags);
	return ret;
}
int anx_adapter_abort(const anx_oid_t *id, uint64_t token)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !token) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&adapter_lock, &flags);
	struct adapter_record *a = find(id);
	int ret = a ? ANX_OK : ANX_ENOENT;
	if (ret == ANX_OK && a->candidate_token != token) ret = ANX_EBUSY;
	if (ret == ANX_OK) clear_candidate(a);
	anx_spin_unlock_irqrestore(&adapter_lock, flags);
	return ret;
}
int anx_adapter_rollback(const anx_oid_t *id, uint64_t generation, struct anx_adapter_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !generation || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&adapter_lock, &flags);
	struct adapter_record *a = find(id);
	int ret = access(a, true);
	if (ret == ANX_OK && (a->candidate_token || !a->have_prior || a->view.generation != generation)) ret = ANX_EBUSY;
	if (ret == ANX_OK && sequence == ~(uint64_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK) {
		a->current = a->prior; a->have_prior = false; anx_memset(&a->prior, 0, sizeof(a->prior));
		a->view.generation = ++sequence; anx_sha256(&a->current, sizeof(a->current), a->view.image_hash.bytes); *out = a->view;
	}
	anx_spin_unlock_irqrestore(&adapter_lock, flags);
	return ret;
}
int anx_adapter_destroy(const anx_oid_t *id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags;
	struct anx_cell *owner = NULL;
	anx_spin_lock_irqsave(&adapter_lock, &flags);
	struct adapter_record *a = find(id);
	int ret = a ? ANX_OK : ANX_ENOENT;
	if (ret == ANX_OK && a->candidate_token) ret = ANX_EBUSY;
	if (ret == ANX_OK) { owner = a->owner; anx_memset(a, 0, sizeof(*a)); }
	anx_spin_unlock_irqrestore(&adapter_lock, flags);
	if (owner) anx_cell_store_release(owner);
	return ret;
}
int anx_adapter_generate(const anx_oid_t *id, uint64_t generation, const struct anx_anxml_request *request,
		struct anx_anxml_response *response, struct anx_adapter_cache_key *key)
{
	if (!id || !generation || !request || !response || !key) return ANX_EINVAL;
	struct anx_anxml_request input = *request;
	int ret = request_check(&input);
	if (ret == ANX_OK) ret = anx_cell_cognitive_limit(input.max_tokens, &input.max_tokens);
	if (ret != ANX_OK) return ret;
	struct anx_adapter_image image;
	struct anx_adapter_cache_key result = {0};
	bool flags;
	anx_spin_lock_irqsave(&adapter_lock, &flags);
	struct adapter_record *a = find(id);
	ret = access(a, false);
	if (ret == ANX_OK && a->view.generation != generation) ret = ANX_EBUSY;
	if (ret == ANX_OK) { image = a->current; result.version = a->view; }
	anx_spin_unlock_irqrestore(&adapter_lock, flags);
	if (ret == ANX_OK) ret = anx_anxml_generate_image(&input, &image, response);
	if (ret == ANX_OK) { request_hash(&input, &result.request_hash); *key = result; }
	return ret;
}
int anx_adapter_cache_check(const anx_oid_t *id, const struct anx_anxml_request *request, const struct anx_adapter_cache_key *key)
{
	if (!id || !request || !key) return ANX_EINVAL;
	struct anx_anxml_request input = *request;
	int ret = request_check(&input);
	if (ret == ANX_OK) ret = anx_cell_cognitive_limit(input.max_tokens, &input.max_tokens);
	if (ret != ANX_OK) return ret;
	struct anx_adapter_view view;
	ret = anx_adapter_get(id, &view);
	if (ret != ANX_OK) return ret;
	struct anx_hash hash;
	request_hash(&input, &hash);
	return anx_uuid_compare(&view.id, &key->version.id) || view.generation != key->version.generation ||
		anx_memcmp(view.image_hash.bytes, key->version.image_hash.bytes, 32) ||
		anx_memcmp(hash.bytes, key->request_hash.bytes, 32) ? ANX_EBUSY : ANX_OK;
}
