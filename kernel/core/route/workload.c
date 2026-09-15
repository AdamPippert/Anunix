#include <anx/workload.h>
#include <anx/identity.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>
struct workload {
	struct anx_workload_view view;
	struct anx_workload_contract contract;
	struct anx_model_use_view samples[ANX_WORKLOAD_CANDIDATES];
	struct anx_cell *owner;
	anx_oid_t identity;
	anx_cid_t parent;
	bool busy;
};
static struct workload *records[ANX_WORKLOAD_MAX];
static struct anx_spinlock workload_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct workload *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_WORKLOAD_MAX; i++) if (records[i] && records[i]->view.id == id) return records[i];
	return NULL;
}
static int access(struct workload *w)
{
	if (!w) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, &w->view.owner) ? ANX_EPERM : ANX_OK;
}
static int owner_current(struct workload *w, bool capture)
{
	anx_oid_t identity;
	if (anx_uuid_compare(&w->owner->cid, &w->view.owner) || anx_uuid_compare(&w->owner->parent_cid, &w->parent)) return ANX_EPERM;
	int ret = anx_cell_check_scope(w->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(w->owner, &identity);
	if (ret == ANX_OK && !capture && anx_uuid_compare(&identity, &w->identity)) ret = ANX_EBUSY;
	if (ret == ANX_OK && capture) w->identity = identity;
	return ret;
}
static int quality(struct workload *w, const struct anx_anxml_response *response)
{
	return response->output_len < w->contract.minimum_output || response->output_len > w->contract.maximum_output ||
		!response->tokens_generated || response->tokens_generated > w->contract.maximum_tokens ||
		anx_memcmp(response->output, w->contract.prefix, w->contract.prefix_size) ? ANX_EAUDIT : ANX_OK;
}
static int sample_current(struct workload *w, uint32_t candidate, struct anx_anxml_response *response)
{
	struct anx_model_use_view now;
	const struct anx_model_use_view *sample = &w->samples[candidate];
	int ret = anx_model_use_get(sample->id, &now);
	if (ret == ANX_OK && (now.state != ANX_MODEL_USE_COMPLETED || now.epoch != sample->epoch || now.reused_from ||
	    !anx_model_use_same_request(&now, sample))) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = anx_model_use_read(now.id, response);
	if (ret == ANX_OK) ret = quality(w, response);
	return ret;
}
int anx_workload_create(const anx_cid_t *owner, const struct anx_workload_contract *contract, struct anx_workload_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !contract || !out || anx_uuid_is_nil(owner)) return ANX_EINVAL;
	struct workload *w = anx_zalloc(sizeof(*w));
	if (!w) return ANX_ENOMEM;
	w->contract = *contract; w->view.owner = *owner;
	int ret = !w->contract.minimum_output || w->contract.maximum_output < w->contract.minimum_output ||
		w->contract.maximum_output > 128 || !w->contract.maximum_tokens || w->contract.maximum_tokens > 128 ||
		!w->contract.prefix_size || w->contract.prefix_size > w->contract.minimum_output ? ANX_EINVAL : ANX_OK;
	if (ret == ANX_OK) {
		w->owner = anx_cell_store_lookup(&w->view.owner);
		if (w->owner) w->parent = w->owner->parent_cid;
		ret = w->owner ? owner_current(w, true) : ANX_ENOENT;
	}
	bool flags; anx_spin_lock_irqsave(&workload_lock, &flags);
	uint32_t slot;
	for (slot = 0; slot < ANX_WORKLOAD_MAX; slot++) if (!records[slot]) break;
	if (ret == ANX_OK && (slot == ANX_WORKLOAD_MAX || sequence == ~(uint64_t)0)) ret = ANX_EFULL;
	if (ret == ANX_OK) {
		w->view.id = ++sequence; w->view.epoch = 1; w->view.active = ANX_WORKLOAD_CANDIDATES;
		records[slot] = w; *out = w->view;
	}
	anx_spin_unlock_irqrestore(&workload_lock, flags);
	if (ret != ANX_OK) { if (w->owner) anx_cell_store_release(w->owner); anx_free(w); }
	return ret;
}
int anx_workload_add(uint64_t id, uint64_t sample_use, struct anx_workload_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !sample_use || !out) return ANX_EINVAL;
	struct anx_anxml_response *response = anx_zalloc(sizeof(*response));
	if (!response) return ANX_ENOMEM;
	bool flags; anx_spin_lock_irqsave(&workload_lock, &flags);
	struct workload *w = find(id); int ret = access(w);
	if (ret == ANX_OK && (w->busy || w->view.epoch == ~(uint64_t)0)) ret = ANX_EBUSY;
	if (ret == ANX_OK && w->view.candidates == ANX_WORKLOAD_CANDIDATES) ret = ANX_EFULL;
	if (ret == ANX_OK) ret = owner_current(w, false);
	struct anx_model_use_view sample;
	if (ret == ANX_OK) ret = anx_model_use_get(sample_use, &sample);
	if (ret == ANX_OK && (anx_uuid_compare(&sample.owner, &w->view.owner) || anx_uuid_compare(&sample.identity_record, &w->identity))) ret = ANX_EPERM;
	if (ret == ANX_OK && (sample.state != ANX_MODEL_USE_COMPLETED || sample.reused_from || sample.seed ||
	    sample.maximum_tokens > w->contract.maximum_tokens)) ret = ANX_EAUDIT;
	for (uint32_t i = 0; ret == ANX_OK && i < w->view.candidates; i++) if (w->samples[i].id == sample_use) ret = ANX_EEXIST;
	if (ret == ANX_OK) ret = anx_model_use_read(sample_use, response);
	if (ret == ANX_OK) ret = quality(w, response);
	if (ret == ANX_OK) { w->samples[w->view.candidates++] = sample; w->view.epoch++; *out = w->view; }
	anx_spin_unlock_irqrestore(&workload_lock, flags);
	anx_memset(response, 0, sizeof(*response)); anx_free(response); return ret;
}
int anx_workload_select(uint64_t id, uint64_t epoch, uint32_t candidate, uint32_t token_limit, struct anx_workload_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch || !out || !token_limit) return ANX_EINVAL;
	struct anx_anxml_response *response = anx_zalloc(sizeof(*response));
	if (!response) return ANX_ENOMEM;
	bool flags; anx_spin_lock_irqsave(&workload_lock, &flags);
	struct workload *w = find(id); int ret = access(w);
	if (ret == ANX_OK && (w->busy || w->view.epoch != epoch || epoch == ~(uint64_t)0)) ret = ANX_EBUSY;
	if (ret == ANX_OK && candidate >= w->view.candidates) ret = ANX_EINVAL;
	if (ret == ANX_OK && token_limit > w->contract.maximum_tokens) ret = ANX_EPERM;
	if (ret == ANX_OK && w->samples[candidate].maximum_tokens > token_limit) ret = ANX_ENOMEM;
	if (ret == ANX_OK) ret = owner_current(w, false);
	if (ret == ANX_OK) ret = sample_current(w, candidate, response);
	if (ret == ANX_OK) { w->view.active = candidate; w->view.token_limit = token_limit; w->view.epoch++; *out = w->view; }
	anx_spin_unlock_irqrestore(&workload_lock, flags);
	anx_memset(response, 0, sizeof(*response)); anx_free(response); return ret;
}
int anx_workload_execute(uint64_t id, uint64_t epoch, uint64_t use, struct anx_anxml_response *response, struct anx_workload_view *out)
{
	if (!id || !epoch || !use || !response || !out) return ANX_EINVAL;
	struct anx_anxml_response *result = anx_zalloc(sizeof(*result));
	if (!result) return ANX_ENOMEM;
	bool flags; anx_spin_lock_irqsave(&workload_lock, &flags);
	struct workload *w = find(id); int ret = access(w);
	if (ret == ANX_OK && (w->busy || w->view.epoch != epoch || epoch == ~(uint64_t)0 || w->view.active >= w->view.candidates)) ret = ANX_EBUSY;
	if (ret == ANX_OK && (w->view.executions == ~(uint64_t)0 || w->view.charged_tokens > ~(uint64_t)0 - w->contract.maximum_tokens)) ret = ANX_EFULL;
	if (ret == ANX_OK) ret = owner_current(w, false);
	if (ret == ANX_OK) ret = sample_current(w, w->view.active, result);
	struct anx_model_use_view view;
	if (ret == ANX_OK) ret = anx_model_use_get(use, &view);
	if (ret == ANX_OK && (view.state != ANX_MODEL_USE_READY || !anx_model_use_same_request(&view, &w->samples[w->view.active]) ||
	    view.maximum_tokens > w->view.token_limit)) ret = ANX_EPERM;
	if (ret == ANX_OK) w->busy = true;
	anx_spin_unlock_irqrestore(&workload_lock, flags);
	if (ret == ANX_OK) {
		ret = anx_model_use_execute(use, view.epoch, result, &view);
		bool produced = ret == ANX_OK;
		if (ret == ANX_OK) ret = quality(w, result);
		if (ret == ANX_OK) ret = owner_current(w, false);
		anx_spin_lock_irqsave(&workload_lock, &flags);
		w->busy = false; w->view.epoch++; w->view.last_result = ret;
		if (produced) { w->view.executions++; w->view.charged_tokens += result->tokens_generated; }
		if (ret == ANX_OK) { *response = *result; *out = w->view; }
		anx_spin_unlock_irqrestore(&workload_lock, flags);
	}
	anx_memset(result, 0, sizeof(*result)); anx_free(result); return ret;
}
int anx_workload_get(uint64_t id, struct anx_workload_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&workload_lock, &flags);
	struct workload *w = find(id); int ret = access(w);
	if (ret == ANX_OK && w->busy) ret = ANX_EBUSY;
	if (ret == ANX_OK) *out = w->view;
	anx_spin_unlock_irqrestore(&workload_lock, flags); return ret;
}
int anx_workload_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&workload_lock, &flags);
	struct workload *w = find(id); int ret = !w ? ANX_ENOENT : w->busy ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_WORKLOAD_MAX; i++) if (records[i] == w) records[i] = NULL;
		anx_cell_store_release(w->owner); anx_memset(w, 0, sizeof(*w)); anx_free(w);
	}
	anx_spin_unlock_irqrestore(&workload_lock, flags); return ret;
}
