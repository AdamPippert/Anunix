#include <anx/execution_shape.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>
#include <anx/identity.h>
#include <anx/sched_domain.h>
struct shape_record {
	struct anx_shape_view view;
	struct anx_shape_spec spec;
	struct anx_model_use_view uses[ANX_SHAPE_NODES_MAX];
	struct anx_cell *owner;
};
static struct shape_record *records[ANX_SHAPE_MAX];
static struct anx_spinlock shape_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct shape_record *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_SHAPE_MAX; i++) if (records[i] && records[i]->view.id == id) return records[i];
	return NULL;
}
static int access(struct shape_record *s)
{
	if (!s) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, &s->view.owner) ? ANX_EPERM : ANX_OK;
}
static int owner_check(struct shape_record *s)
{
	if (anx_cell_status_terminal(s->owner->status)) return ANX_EPERM;
	int ret = anx_cell_check_scope(s->owner);
	if (ret == ANX_OK) ret = anx_cell_check_contract(s->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(s->owner, NULL);
	if (ret == ANX_OK) ret = anx_sched_domain_check(s->owner);
	return ret;
}
static int use_check(struct shape_record *s, uint32_t i, bool ready)
{
	struct anx_model_use_view current;
	int ret = anx_model_use_get(s->uses[i].id, &current);
	if (ret == ANX_OK && (current.epoch != s->uses[i].epoch ||
	    !anx_model_use_same_request(&current, &s->uses[i]) ||
	    current.state != (ready ? ANX_MODEL_USE_READY : ANX_MODEL_USE_COMPLETED))) ret = ANX_EBUSY;
	return ret;
}
int anx_shape_compile(const anx_cid_t *owner, const struct anx_shape_spec *spec, struct anx_shape_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !spec || !out) return ANX_EINVAL;
	struct shape_record *s = anx_zalloc(sizeof(*s));
	if (!s) return ANX_ENOMEM;
	s->spec = *spec; s->view.owner = *owner;
	int ret = ANX_OK;
	if (!s->spec.count || s->spec.count > ANX_SHAPE_NODES_MAX ||
	    s->spec.mode < ANX_SHAPE_LITERAL || s->spec.mode > ANX_SHAPE_REUSE) ret = ANX_EINVAL;
	if (ret == ANX_OK) {
		s->owner = anx_cell_store_lookup(owner);
		ret = s->owner ? owner_check(s) : ANX_ENOENT;
	}
	for (uint32_t i = 0; ret == ANX_OK && i < ANX_SHAPE_NODES_MAX; i++) {
		const struct anx_shape_node *n = &s->spec.nodes[i];
		if (i >= s->spec.count) {
			if (n->use || n->dependencies || n->opcode) ret = ANX_EINVAL;
			continue;
		}
		if (n->opcode != ANX_SHAPE_INFER) { ret = ANX_ENOTSUP; break; }
		if (!n->use || (n->dependencies & ~((1U << i) - 1))) { ret = ANX_EINVAL; break; }
		for (uint32_t j = 0; j < i; j++) if (s->spec.nodes[j].use == n->use) ret = ANX_EINVAL;
		if (ret != ANX_OK) break;
		ret = anx_model_use_get(n->use, &s->uses[i]);
		if (ret == ANX_OK && anx_uuid_compare(&s->uses[i].owner, owner)) ret = ANX_EPERM;
		if (ret == ANX_OK && s->uses[i].state != ANX_MODEL_USE_READY) ret = ANX_EBUSY;
		if (ret != ANX_OK) break;
		s->view.source_node[i] = i;
		if (s->spec.mode == ANX_SHAPE_REUSE && !s->uses[i].seed) {
			for (uint32_t j = 0; j < i; j++) {
				if (n->dependencies == s->spec.nodes[j].dependencies && anx_model_use_same_request(&s->uses[i], &s->uses[j])) {
					s->view.source_node[i] = s->view.source_node[j]; break;
				}
			}
		}
		if (s->view.source_node[i] == i) s->view.planned_physical_operations++;
	}
	if (ret == ANX_OK) {
		bool flags;
		anx_spin_lock_irqsave(&shape_lock, &flags);
		uint32_t i;
		for (i = 0; i < ANX_SHAPE_MAX; i++) if (!records[i]) break;
		if (i == ANX_SHAPE_MAX || sequence == ~(uint64_t)0) ret = ANX_EFULL;
		else {
			s->view.id = ++sequence; s->view.epoch = 1; s->view.logical_operations = s->spec.count;
			records[i] = s; *out = s->view;
		}
		anx_spin_unlock_irqrestore(&shape_lock, flags);
	}
	if (ret != ANX_OK) { if (s->owner) anx_cell_store_release(s->owner); anx_memset(s, 0, sizeof(*s)); anx_free(s); }
	return ret;
}
int anx_shape_get(uint64_t id, struct anx_shape_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&shape_lock, &flags);
	struct shape_record *s = find(id);
	int ret = access(s);
	if (ret == ANX_OK && s->view.state == ANX_SHAPE_RUNNING) ret = ANX_EBUSY;
	if (ret == ANX_OK) *out = s->view;
	anx_spin_unlock_irqrestore(&shape_lock, flags);
	return ret;
}
int anx_shape_run(uint64_t id, uint64_t epoch, struct anx_shape_view *out)
{
	if (!id || !epoch || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&shape_lock, &flags);
	struct shape_record *s = find(id);
	int ret = access(s);
	if (ret == ANX_OK && (s->view.epoch != epoch || s->view.state != ANX_SHAPE_READY)) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = owner_check(s);
	for (uint32_t i = 0; ret == ANX_OK && i < s->spec.count; i++) ret = use_check(s, i, true);
	struct anx_anxml_response *response = NULL;
	if (ret == ANX_OK) { response = anx_zalloc(sizeof(*response)); if (!response) ret = ANX_ENOMEM; }
	if (ret == ANX_OK) s->view.state = ANX_SHAPE_RUNNING;
	anx_spin_unlock_irqrestore(&shape_lock, flags);
	if (ret != ANX_OK) return ret;
	for (uint32_t i = 0; ret == ANX_OK && i < s->spec.count; i++) {
		const struct anx_shape_node *n = &s->spec.nodes[i];
		ret = owner_check(s);
		if (ret == ANX_OK && (n->dependencies & s->view.completed) != n->dependencies) ret = ANX_EBUSY;
		if (ret == ANX_OK) ret = use_check(s, i, true);
		if (ret != ANX_OK) break;
		uint32_t source = s->view.source_node[i];
		if (source == i) {
			ret = anx_model_use_execute(s->uses[i].id, s->uses[i].epoch, response, &s->uses[i]);
			if (ret == ANX_OK) s->view.physical_operations++;
		} else {
			if (!(s->view.completed & (1U << source))) ret = ANX_EBUSY;
			else ret = anx_model_use_reuse(s->uses[i].id, s->uses[i].epoch, s->uses[source].id, response, &s->uses[i]);
			if (ret == ANX_OK) s->view.reused_operations++;
		}
		if (ret == ANX_OK) { s->view.generated_tokens += response->tokens_generated; s->view.completed |= 1U << i; }
	}
	anx_spin_lock_irqsave(&shape_lock, &flags);
	s->view.epoch++; s->view.result = ret;
	s->view.state = ret == ANX_OK ? ANX_SHAPE_COMPLETED : ANX_SHAPE_FAILED;
	*out = s->view;
	anx_spin_unlock_irqrestore(&shape_lock, flags);
	anx_memset(response, 0, sizeof(*response)); anx_free(response);
	return ANX_OK;
}
int anx_shape_read(uint64_t id, uint32_t node, struct anx_anxml_response *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&shape_lock, &flags);
	struct shape_record *s = find(id);
	int ret = access(s);
	if (ret == ANX_OK && node >= s->spec.count) ret = ANX_EINVAL;
	if (ret == ANX_OK && s->view.state != ANX_SHAPE_COMPLETED) ret = ANX_EPERM;
	if (ret == ANX_OK) ret = use_check(s, node, false);
	if (ret == ANX_OK) ret = anx_model_use_read(s->uses[node].id, out);
	anx_spin_unlock_irqrestore(&shape_lock, flags);
	return ret;
}
int anx_shape_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&shape_lock, &flags);
	struct shape_record *s = find(id);
	int ret = !s ? ANX_ENOENT : s->view.state == ANX_SHAPE_RUNNING ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_SHAPE_MAX; i++) if (records[i] == s) records[i] = NULL;
		anx_cell_store_release(s->owner); anx_memset(s, 0, sizeof(*s)); anx_free(s);
	}
	anx_spin_unlock_irqrestore(&shape_lock, flags);
	return ret;
}
