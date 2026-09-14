#include <anx/branch_group.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/sched.h>
#include <anx/arch.h>

struct branch_group {
	struct anx_branch_view view;
	struct anx_branch_spec spec;
	struct anx_cell *owner, *children[ANX_BRANCH_MAX];
	uint64_t prompt_version;
	uint32_t prompt_size, prompt_sensitivity, active;
	uint8_t prompt_hash[32];
	char results[ANX_BRANCH_MAX][129];
	uint32_t result_sizes[ANX_BRANCH_MAX];
};
static struct branch_group *groups[ANX_BRANCH_GROUP_MAX];
static struct anx_spinlock group_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;

static struct branch_group *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_BRANCH_GROUP_MAX; i++)
		if (groups[i] && groups[i]->view.id == id) return groups[i];
	return NULL;
}
static struct branch_group *member(const anx_cid_t *cid, uint32_t *index)
{
	for (uint32_t i = 0; i < ANX_BRANCH_GROUP_MAX; i++) if (groups[i])
		for (uint32_t j = 0; j < groups[i]->spec.count; j++)
			if (groups[i]->children[j] && !anx_uuid_compare(cid, &groups[i]->view.branches[j])) {
				if (index) *index = j;
				return groups[i];
			}
	return NULL;
}
static int access(struct branch_group *g)
{
	if (!g) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, &g->view.owner) ? ANX_EPERM : ANX_OK;
}
/* Read through the real object API, retaining its normal fence read tracking. */
static int prompt_read(struct branch_group *g, const anx_cid_t *actor, char *bytes, bool capture)
{
	struct anx_object_handle handle = {0};
	int ret = anx_so_open(&g->spec.prompt, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK) return ret;
	struct anx_state_object *o = handle.obj;
	uint32_t size = 0, sensitivity = 0;
	uint64_t version = 0;
	uint8_t digest[32];
	anx_spin_lock(&o->lock);
	if (o->state != ANX_OBJ_SEALED || o->object_type != ANX_OBJ_BYTE_DATA || !o->payload ||
	    !o->payload_size || o->payload_size > ANX_ANXML_PROMPT_MAX || !o->version ||
	    o->access_policy.rule_count > ANX_MAX_ACCESS_RULES || (uint32_t)o->sensitivity > ANX_SENSITIVITY_RESTRICTED) ret = ANX_EINVAL;
	else ret = anx_access_evaluate(&o->access_policy, actor, &o->creator_cell, ANX_ACCESS_READ_PAYLOAD);
	if (ret == ANX_OK) {
		size = (uint32_t)o->payload_size; version = o->version; sensitivity = o->sensitivity;
		if (!capture && (size != g->prompt_size || version != g->prompt_version || sensitivity != g->prompt_sensitivity)) ret = ANX_EBUSY;
	}
	anx_spin_unlock(&o->lock);
	if (ret == ANX_OK) {
		ret = anx_so_read_payload(&handle, 0, bytes, size);
		if (ret == (int)size) ret = ANX_OK;
		else if (ret >= 0) ret = ANX_EIO;
	}
	if (ret == ANX_OK) {
		anx_sha256(bytes, size, digest);
		anx_spin_lock(&o->lock);
		if (o->state != ANX_OBJ_SEALED || o->object_type != ANX_OBJ_BYTE_DATA || o->version != version ||
		    o->payload_size != size || (uint32_t)o->sensitivity != sensitivity ||
		    o->access_policy.rule_count > ANX_MAX_ACCESS_RULES) ret = ANX_EBUSY;
		else ret = anx_access_evaluate(&o->access_policy, actor, &o->creator_cell, ANX_ACCESS_READ_PAYLOAD);
		anx_spin_unlock(&o->lock);
		if (ret == ANX_OK && !capture && anx_memcmp(digest, g->prompt_hash, 32)) ret = ANX_EBUSY;
		if (ret == ANX_OK && capture) {
			g->prompt_size = size; g->prompt_version = version; g->prompt_sensitivity = sensitivity;
			anx_memcpy(g->prompt_hash, digest, 32);
		}
	}
	anx_so_close(&handle);
	return ret;
}

static int spec_check(const struct anx_branch_spec *s)
{
	if (!s || s->schema != 1 || !s->count || s->count > ANX_BRANCH_MAX ||
	    !s->token_budget || s->token_budget > ANX_BRANCH_TOKEN_MAX || anx_uuid_is_nil(&s->prompt) ||
	    (s->semantics != ANX_BRANCH_REQUIRED && s->semantics != ANX_BRANCH_TRIAL)) return ANX_EINVAL;
	uint32_t sum = 0;
	for (uint32_t i = 0; i < s->count; i++) {
		const struct anx_branch_candidate *c = &s->candidates[i];
		if (!c->maximum_tokens || c->maximum_tokens > 128 || !c->expected_size ||
		    c->expected_size > c->maximum_tokens || anx_adapter_image_check(&c->image) != ANX_OK) return ANX_EINVAL;
		sum += c->maximum_tokens;
	}
	if (s->semantics == ANX_BRANCH_REQUIRED && sum > s->token_budget) return ANX_ENOMEM;
	return ANX_OK;
}
int anx_branch_group_create(const anx_cid_t *owner, const struct anx_branch_spec *spec, struct anx_branch_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !out) return ANX_EINVAL;
	int ret = spec_check(spec);
	if (ret != ANX_OK) return ret;
	struct branch_group *g = anx_zalloc(sizeof(*g));
	char *prefix = anx_alloc(ANX_ANXML_PROMPT_MAX);
	if (!g || !prefix) { anx_free(g); anx_free(prefix); return ANX_ENOMEM; }
	g->spec = *spec; g->active = ~(uint32_t)0; g->view.winner = ~(uint32_t)0;
	spec = &g->spec;
	ret = spec_check(spec);
	g->owner = anx_cell_store_lookup(owner);
	if (ret == ANX_OK) ret = g->owner ? anx_cell_check_scope(g->owner) : ANX_ENOENT;
	if (ret == ANX_OK && (anx_cell_status_terminal(g->owner->status) || g->owner->runtime_active)) ret = ANX_EPERM;
	if (ret == ANX_OK) ret = prompt_read(g, owner, prefix, true);
	anx_memset(prefix, 0, ANX_ANXML_PROMPT_MAX); anx_free(prefix);
	if (ret != ANX_OK) goto fail;
	for (uint32_t i = 0; i < spec->count; i++) {
		if (g->owner->cognitive.max_tokens && spec->candidates[i].maximum_tokens > g->owner->cognitive.max_tokens) {
			ret = ANX_EPERM; goto fail;
		}
		struct anx_cell_intent intent = {0};
		anx_strlcpy(intent.name, "anxml-branch", sizeof(intent.name));
		ret = anx_cell_derive_child(g->owner, ANX_CELL_TASK_EXECUTION, &intent, &g->children[i]);
		if (ret != ANX_OK) goto fail;
		struct anx_cell *c = g->children[i];
		anx_cell_store_lookup(&c->cid); /* Registry pin prevents caller destruction. */
		c->execution.allow_recursive_cells = c->execution.allow_side_effects = false;
		c->execution.allow_network = c->execution.allow_remote_models = false;
		c->routing.strategy = ANX_ROUTE_DIRECT; c->routing.decomposition = ANX_DECOMP_NONE;
		c->cognitive.max_tokens = spec->candidates[i].maximum_tokens;
		c->input_count = 1; c->inputs[0].state_object_ref = spec->prompt;
		c->inputs[0].mode = ANX_INPUT_READ; c->inputs[0].required = true;
		g->view.branches[i] = c->cid;
	}
	bool flags;
	anx_spin_lock_irqsave(&group_lock, &flags);
	uint32_t slot;
	for (slot = 0; slot < ANX_BRANCH_GROUP_MAX; slot++) if (!groups[slot]) break;
	if (slot == ANX_BRANCH_GROUP_MAX || sequence == ~(uint64_t)0) ret = ANX_EFULL;
	else {
		g->view.id = ++sequence; g->view.epoch = 1; g->view.owner = *owner;
		g->view.count = spec->count; g->view.state = ANX_BRANCH_READY;
		groups[slot] = g; *out = g->view;
	}
	anx_spin_unlock_irqrestore(&group_lock, flags);
	if (ret == ANX_OK) return ret;
fail:
	for (uint32_t i = 0; i < ANX_BRANCH_MAX; i++) if (g->children[i]) {
		anx_cell_store_release(g->children[i]); anx_cell_destroy(g->children[i]);
	}
	if (g->owner) anx_cell_store_release(g->owner);
	anx_memset(g, 0, sizeof(*g)); anx_free(g);
	return ret;
}

static int scope(struct branch_group *g, uint32_t i, struct anx_cell *c)
{
	if (g->view.state != ANX_BRANCH_RUNNING || g->active != i ||
	    anx_cell_status_terminal(g->owner->status) || c != g->children[i] ||
	    anx_uuid_compare(&c->parent_cid, &g->view.owner) || c->cell_type != ANX_CELL_TASK_EXECUTION || c->ext_call ||
	    c->execution.allow_side_effects || c->execution.allow_network || c->execution.allow_remote_models ||
	    c->execution.allow_recursive_cells || c->routing.strategy != ANX_ROUTE_DIRECT || c->routing.decomposition != ANX_DECOMP_NONE ||
	    c->input_count != 1 || !c->inputs[0].required || c->inputs[0].mode != ANX_INPUT_READ ||
	    anx_uuid_compare(&c->inputs[0].state_object_ref, &g->spec.prompt) || c->dep_count ||
	    c->cognitive.max_tokens != g->spec.candidates[i].maximum_tokens) return ANX_EPERM;
	return ANX_OK;
}
int anx_branch_group_check(struct anx_cell *cell)
{
	if (!cell) return ANX_EINVAL;
	bool flags;
	uint32_t i;
	anx_spin_lock_irqsave(&group_lock, &flags);
	struct branch_group *g = member(&cell->cid, &i);
	int ret = g ? scope(g, i, cell) : ANX_OK;
	anx_spin_unlock_irqrestore(&group_lock, flags);
	return ret;
}
int anx_branch_group_effect_check(const anx_cid_t *cell)
{
	if (!cell) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&group_lock, &flags);
	int ret = member(cell, NULL) ? ANX_EPERM : ANX_OK;
	anx_spin_unlock_irqrestore(&group_lock, flags);
	return ret;
}

int anx_branch_group_execute(struct anx_cell *cell, bool *handled)
{
	if (!cell || !handled) return ANX_EINVAL;
	bool flags;
	uint32_t index;
	anx_spin_lock_irqsave(&group_lock, &flags);
	struct branch_group *g = member(&cell->cid, &index);
	*handled = g != NULL;
	int ret = g ? scope(g, index, cell) : ANX_OK;
	const anx_cid_t *active = anx_cell_current_id();
	if (g && (!active || anx_uuid_compare(active, &cell->cid) || cell->status != ANX_CELL_RUNNING)) ret = ANX_EPERM;
	anx_spin_unlock_irqrestore(&group_lock, flags);
	if (!g || ret != ANX_OK) return ret;
	/* RUNNING pins the group against abort/removal until its synchronous run settles. */
	struct anx_anxml_request *request = anx_zalloc(sizeof(*request));
	struct anx_anxml_response *response = anx_zalloc(sizeof(*response));
	if (!request || !response) { anx_free(request); anx_free(response); return ANX_ENOMEM; }
	ret = prompt_read(g, &cell->cid, request->prompt, false);
	request->prompt_len = g->prompt_size;
	request->max_tokens = g->spec.candidates[index].maximum_tokens;
	if (ret == ANX_OK) ret = anx_anxml_generate_image(request, &g->spec.candidates[index].image, response);
	const struct anx_branch_candidate *candidate = &g->spec.candidates[index];
	if (ret == ANX_OK && (response->output_len != candidate->expected_size ||
	    anx_memcmp(response->output, candidate->expected, candidate->expected_size))) ret = ANX_EIO;
	if (ret == ANX_OK) {
		anx_spin_lock_irqsave(&group_lock, &flags);
		ret = scope(g, index, cell);
		if (ret == ANX_OK) {
			anx_memcpy(g->results[index], response->output, response->output_len);
			g->result_sizes[index] = response->output_len;
		}
		anx_spin_unlock_irqrestore(&group_lock, flags);
	}
	anx_memset(request, 0, sizeof(*request)); anx_memset(response, 0, sizeof(*response));
	anx_free(request); anx_free(response);
	return ret;
}

/* These leaf Cells have no effects. Do not close their shared parent run fence. */
static int cancel_pending(struct branch_group *g)
{
	for (uint32_t i = 0; i < g->spec.count; i++) {
		struct anx_cell *c = g->children[i];
		if (!c || anx_cell_status_terminal(c->status)) continue;
		if (c->runtime_active || c->child_count || c->status != ANX_CELL_CREATED) return ANX_EBUSY;
		int ret = anx_cell_transition(c, ANX_CELL_CANCELLED);
		if (ret != ANX_OK) return ret;
		c->completed_at = arch_time_now(); c->error_code = ANX_ECANCELED;
		anx_sched_cancel(&c->cid);
		g->view.cancelled |= 1U << i;
	}
	return ANX_OK;
}
int anx_branch_group_get(uint64_t id, struct anx_branch_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&group_lock, &flags);
	struct branch_group *g = find(id);
	int ret = access(g);
	if (ret == ANX_OK) *out = g->view;
	anx_spin_unlock_irqrestore(&group_lock, flags);
	return ret;
}
int anx_branch_group_run(uint64_t id, uint64_t epoch, struct anx_branch_view *out)
{
	if (!id || !epoch || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&group_lock, &flags);
	struct branch_group *g = find(id);
	int ret = access(g);
	if (ret == ANX_OK && (g->view.epoch != epoch || g->view.state != ANX_BRANCH_READY)) ret = ANX_EBUSY;
	if (ret == ANX_OK && anx_cell_status_terminal(g->owner->status)) ret = ANX_EPERM;
	if (ret == ANX_OK) for (uint32_t i = 0; i < g->spec.count; i++)
		if (g->children[i]->status != ANX_CELL_CREATED || g->children[i]->runtime_active) { ret = ANX_EBUSY; break; }
	if (ret == ANX_OK) { g->view.state = ANX_BRANCH_RUNNING; g->view.epoch++; }
	anx_spin_unlock_irqrestore(&group_lock, flags);
	if (ret != ANX_OK) return ret;
	for (uint32_t i = 0; i < g->spec.count; i++) {
		uint32_t cost = g->spec.candidates[i].maximum_tokens;
		if (cost > g->spec.token_budget - g->view.charged_tokens) { ret = ANX_EFULL; break; }
		g->active = i; g->view.charged_tokens += cost; g->view.attempted |= 1U << i;
		ret = anx_cell_run(g->children[i]);
		g->active = ~(uint32_t)0;
		if (ret == ANX_OK) {
			g->view.accepted |= 1U << i;
			if (g->spec.semantics == ANX_BRANCH_TRIAL) { g->view.winner = i; break; }
		} else {
			anx_memset(g->results[i], 0, sizeof(g->results[i])); g->result_sizes[i] = 0;
			if (g->spec.semantics == ANX_BRANCH_REQUIRED || ret != ANX_EIO) break;
		}
	}
	anx_spin_lock_irqsave(&group_lock, &flags);
	bool complete = g->spec.semantics == ANX_BRANCH_TRIAL ? g->view.winner < g->spec.count :
		g->view.accepted == (1U << g->spec.count) - 1;
	if (g->spec.semantics == ANX_BRANCH_TRIAL) {
		int cancelled = cancel_pending(g);
		if (cancelled != ANX_OK) { ret = cancelled; complete = false; }
	}
	g->view.state = complete ? ANX_BRANCH_COMPLETED : ANX_BRANCH_FAILED;
	g->view.result = complete ? ANX_OK : ret == ANX_OK ? ANX_EIO : ret;
	g->view.epoch++; *out = g->view;
	anx_spin_unlock_irqrestore(&group_lock, flags);
	return ANX_OK;
}
int anx_branch_group_read(uint64_t id, uint32_t branch, void *bytes, uint32_t capacity, uint32_t *size)
{
	if (!id || !bytes || !size) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&group_lock, &flags);
	struct branch_group *g = find(id);
	int ret = access(g);
	if (ret == ANX_OK && branch >= g->spec.count) ret = ANX_EINVAL;
	if (ret == ANX_OK && (g->view.state != ANX_BRANCH_COMPLETED || !(g->view.accepted & (1U << branch)) ||
	    (g->spec.semantics == ANX_BRANCH_TRIAL && g->view.winner != branch))) ret = ANX_EPERM;
	if (ret == ANX_OK && capacity < g->result_sizes[branch]) ret = ANX_ENOMEM;
	if (ret == ANX_OK) {
		char prefix[ANX_ANXML_PROMPT_MAX];
		ret = prompt_read(g, &g->view.owner, prefix, false);
		anx_memset(prefix, 0, sizeof(prefix));
		if (ret == ANX_OK) { anx_memcpy(bytes, g->results[branch], g->result_sizes[branch]); *size = g->result_sizes[branch]; }
	}
	anx_spin_unlock_irqrestore(&group_lock, flags);
	return ret;
}
int anx_branch_group_abort(uint64_t id, uint64_t epoch)
{
	if (!id || !epoch) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&group_lock, &flags);
	struct branch_group *g = find(id);
	int ret = access(g);
	if (ret == ANX_OK && (g->view.epoch != epoch || (g->view.state != ANX_BRANCH_READY && g->view.state != ANX_BRANCH_FAILED))) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = cancel_pending(g);
	if (ret == ANX_OK) { g->view.state = ANX_BRANCH_ABORTED; g->view.result = ANX_ECANCELED; g->view.epoch++; }
	anx_spin_unlock_irqrestore(&group_lock, flags);
	return ret;
}
int anx_branch_group_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&group_lock, &flags);
	struct branch_group *g = find(id);
	int ret = !g ? ANX_ENOENT : g->view.state == ANX_BRANCH_RUNNING ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) for (uint32_t i = 0; i < g->spec.count; i++) if (g->children[i]) {
		struct anx_cell *c = g->children[i];
		if (c->runtime_active || c->child_count || c->refcount > 2) { ret = ANX_EBUSY; break; }
	}
	if (ret == ANX_OK) ret = cancel_pending(g);
	if (ret == ANX_OK) for (uint32_t i = 0; i < g->spec.count; i++) if (g->children[i]) {
		struct anx_cell *c = g->children[i];
		anx_cell_store_release(c);
		ret = anx_cell_destroy(c);
		if (ret != ANX_OK) { anx_cell_store_lookup(&c->cid); break; }
		g->children[i] = NULL;
	}
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_BRANCH_GROUP_MAX; i++) if (groups[i] == g) groups[i] = NULL;
		anx_cell_store_release(g->owner);
		anx_memset(g, 0, sizeof(*g)); anx_free(g);
	}
	anx_spin_unlock_irqrestore(&group_lock, flags);
	return ret;
}
