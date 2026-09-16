#include <anx/execution_shape.h>
#include <anx/physical_plan.h>
#include <anx/resource_shape.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>
#include <anx/identity.h>
#include <anx/sched_domain.h>
#include <anx/phase.h>
#include <anx/crypto.h>
#include <anx/arch.h>
struct shape_record {
	struct anx_shape_view view;
	struct anx_shape_spec spec;
	struct anx_model_use_view uses[ANX_SHAPE_NODES_MAX];
	struct anx_cell *owner;
	bool logical;
	uint32_t plan_refs;
	uint8_t program_digest[32];
};
static struct shape_record *records[ANX_SHAPE_MAX];
static struct anx_spinlock shape_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;

struct physical_record {
	struct anx_physical_plan_view view;
	struct anx_phase_view phase;
	struct shape_record *graph;
};
static struct physical_record *plans[ANX_PHYSICAL_PLAN_MAX];
static uint64_t plan_sequence;
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
	if (ret == ANX_OK && s->logical) ret = ANX_ENOTSUP;
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
	int ret = !s ? ANX_ENOENT : s->view.state == ANX_SHAPE_RUNNING || s->plan_refs ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_SHAPE_MAX; i++) if (records[i] == s) records[i] = NULL;
		anx_cell_store_release(s->owner); anx_memset(s, 0, sizeof(*s)); anx_free(s);
	}
	anx_spin_unlock_irqrestore(&shape_lock, flags);
	return ret;
}

static void logical_view(struct shape_record *s, struct anx_logical_graph_view *out)
{
	struct anx_logical_graph_view view = { .id = s->view.id, .epoch = s->view.epoch, .owner = s->view.owner,
		.count = s->spec.count, .completed = s->view.completed, .physical_operations = s->view.physical_operations,
		.reused_operations = s->view.reused_operations, .state = s->view.state, .result = s->view.result };
	anx_memcpy(view.program_digest, s->program_digest, 32); *out = view;
}
static int logical_access(struct shape_record *s)
{
	int ret = access(s);
	if (ret == ANX_OK && !s->logical) ret = ANX_EINVAL;
	if (ret == ANX_OK && s->view.state == ANX_SHAPE_RUNNING) ret = ANX_EBUSY;
	return ret;
}
int anx_logical_graph_create(const anx_cid_t *owner, const struct anx_logical_graph_spec *spec, struct anx_logical_graph_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !spec || !out) return ANX_EINVAL;
	struct anx_shape_spec input = { .count = spec->count, .mode = ANX_SHAPE_LITERAL };
	anx_memcpy(input.nodes, spec->nodes, sizeof(input.nodes));
	struct anx_shape_view view;
	int ret = anx_shape_compile(owner, &input, &view);
	if (ret != ANX_OK) return ret;
	bool flags; anx_spin_lock_irqsave(&shape_lock, &flags);
	struct shape_record *s = find(view.id);
	s->logical = true;
	struct anx_sha256_ctx hash;
	anx_sha256_init(&hash);
	const char domain[] = "anx-logical-graph-v1";
	anx_sha256_update(&hash, domain, sizeof(domain) - 1);
	anx_sha256_update(&hash, &s->view.owner, sizeof(s->view.owner));
	anx_sha256_update(&hash, &s->spec.count, sizeof(s->spec.count));
	for (uint32_t i = 0; i < s->spec.count; i++) {
		anx_sha256_update(&hash, &s->spec.nodes[i].use, sizeof(s->spec.nodes[i].use));
		anx_sha256_update(&hash, &s->spec.nodes[i].dependencies, sizeof(s->spec.nodes[i].dependencies));
		anx_sha256_update(&hash, &s->spec.nodes[i].opcode, sizeof(s->spec.nodes[i].opcode));
		anx_sha256_update(&hash, &s->uses[i], sizeof(s->uses[i]));
	}
	anx_sha256_final(&hash, s->program_digest); logical_view(s, out);
	anx_spin_unlock_irqrestore(&shape_lock, flags); return ANX_OK;
}
int anx_logical_graph_get(uint64_t id, struct anx_logical_graph_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&shape_lock, &flags);
	struct shape_record *s = find(id); int ret = logical_access(s);
	if (ret == ANX_OK) logical_view(s, out);
	anx_spin_unlock_irqrestore(&shape_lock, flags); return ret;
}
int anx_logical_graph_read(uint64_t id, uint32_t node, struct anx_anxml_response *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&shape_lock, &flags);
	struct shape_record *s = find(id); int ret = logical_access(s);
	if (ret == ANX_OK && (node >= s->spec.count || !(s->view.completed & (1U << node)))) ret = ANX_EPERM;
	if (ret == ANX_OK) ret = use_check(s, node, false);
	if (ret == ANX_OK) ret = anx_model_use_read(s->uses[node].id, out);
	anx_spin_unlock_irqrestore(&shape_lock, flags); return ret;
}
int anx_logical_graph_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	bool flags; anx_spin_lock_irqsave(&shape_lock, &flags);
	struct shape_record *s = find(id); int ret = logical_access(s);
	anx_spin_unlock_irqrestore(&shape_lock, flags);
	return ret == ANX_OK ? anx_shape_destroy(id) : ret;
}
static struct physical_record *plan_find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_PHYSICAL_PLAN_MAX; i++) if (plans[i] && plans[i]->view.id == id) return plans[i];
	return NULL;
}
static int plan_phase(struct physical_record *p, bool capture)
{
	struct anx_phase_view phase;
	int ret = anx_phase_get(&p->graph->view.owner, &phase);
	if (ret == ANX_OK && (phase.epoch != p->view.phase_epoch || phase.parked || phase.phase != ANX_PHASE_INFERENCE)) ret = ANX_EBUSY;
	if (ret == ANX_OK && !capture && (phase.memory_bytes != p->phase.memory_bytes || phase.tier != p->phase.tier ||
	    phase.accelerator != p->phase.accelerator || phase.accelerator_pct != p->phase.accelerator_pct ||
	    anx_uuid_compare(&phase.lease_id, &p->phase.lease_id))) ret = ANX_EBUSY;
	if (ret == ANX_OK) {
		struct anx_engine_lease *lease = anx_lease_lookup(&phase.lease_id);
		if (!lease || lease->revoked || (lease->expires_at && lease->expires_at <= arch_time_now()) ||
		    lease->mem_reserved_bytes != phase.memory_bytes || lease->mem_tier != phase.tier ||
		    lease->accel != phase.accelerator || lease->accel_pct != phase.accelerator_pct) ret = ANX_EBUSY;
	}
	if (ret == ANX_OK && capture) p->phase = phase;
	if (ret == ANX_OK && !capture && p->view.resource_shape)
		ret = anx_resource_shape_check(p->view.resource_shape, p->view.resource_epoch, p->view.replica,
			&p->graph->view.owner, &p->graph->uses[p->view.node].image);
	return ret;
}
static int physical_compile(uint64_t graph, uint64_t logical_epoch, uint64_t phase_epoch,
		enum anx_physical_mode preference, uint64_t resource_shape, uint64_t resource_epoch, uint32_t replica,
		struct anx_physical_plan_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!graph || !logical_epoch || !phase_epoch || !out || preference < ANX_PHYSICAL_DIRECT || preference > ANX_PHYSICAL_REUSE) return ANX_EINVAL;
	struct physical_record *p = anx_zalloc(sizeof(*p));
	if (!p) return ANX_ENOMEM;
	bool flags; anx_spin_lock_irqsave(&shape_lock, &flags);
	struct shape_record *s = find(graph); int ret = logical_access(s);
	if (ret == ANX_OK && (s->view.epoch != logical_epoch || s->view.state != ANX_SHAPE_READY)) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = owner_check(s);
	uint32_t node = 0;
	if (ret == ANX_OK) {
		for (; node < s->spec.count; node++) if (!(s->view.completed & (1U << node))) break;
		if (node == s->spec.count || (s->spec.nodes[node].dependencies & s->view.completed) != s->spec.nodes[node].dependencies) ret = ANX_EBUSY;
	}
	if (ret == ANX_OK) ret = use_check(s, node, true);
	if (ret == ANX_OK) {
		p->graph = s; p->view.logical_graph = graph; p->view.logical_epoch = logical_epoch; p->view.phase_epoch = phase_epoch;
		p->view.node = p->view.source_node = node;
		p->view.resource_shape = resource_shape; p->view.resource_epoch = resource_epoch; p->view.replica = replica;
		ret = plan_phase(p, true);
	}
	if (ret == ANX_OK && preference == ANX_PHYSICAL_REUSE && !s->uses[node].seed) {
		for (uint32_t i = 0; i < node; i++) if ((s->view.completed & (1U << i)) &&
		    s->spec.nodes[i].dependencies == s->spec.nodes[node].dependencies && anx_model_use_same_request(&s->uses[i], &s->uses[node])) {
			ret = use_check(s, i, false);
			if (ret == ANX_OK) { p->view.source_node = i; p->view.mode = ANX_PHYSICAL_REUSE; }
			break;
		}
	}
	if (ret == ANX_OK) {
		uint32_t slot;
		for (slot = 0; slot < ANX_PHYSICAL_PLAN_MAX; slot++) if (!plans[slot]) break;
		if (slot == ANX_PHYSICAL_PLAN_MAX || plan_sequence == ~(uint64_t)0) ret = ANX_EFULL;
		else {
			if (resource_shape) ret = anx_resource_shape_bind(resource_shape, resource_epoch, replica, &s->view.owner, &s->uses[node].image);
			if (ret == ANX_OK) { p->view.id = ++plan_sequence; plans[slot] = p; s->plan_refs++; *out = p->view; }
		}
	}
	anx_spin_unlock_irqrestore(&shape_lock, flags);
	if (ret != ANX_OK) anx_free(p);
	return ret;
}
int anx_physical_plan_compile(uint64_t graph, uint64_t logical_epoch, uint64_t phase_epoch,
		enum anx_physical_mode preference, struct anx_physical_plan_view *out)
{ return physical_compile(graph, logical_epoch, phase_epoch, preference, 0, 0, 0, out); }
int anx_physical_plan_compile_shaped(uint64_t graph, uint64_t logical_epoch, uint64_t phase_epoch,
		uint64_t resource_shape, uint64_t resource_epoch, uint32_t replica, struct anx_physical_plan_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!resource_shape || !resource_epoch) return ANX_EINVAL;
	return physical_compile(graph, logical_epoch, phase_epoch, ANX_PHYSICAL_DIRECT, resource_shape, resource_epoch, replica, out);
}
int anx_physical_plan_commit(uint64_t id, struct anx_anxml_response *response, struct anx_logical_graph_view *out)
{
	if (!id || !response || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&shape_lock, &flags);
	struct physical_record *p = plan_find(id);
	struct shape_record *s = p ? p->graph : NULL;
	int ret = logical_access(s);
	if (ret == ANX_OK && (p->view.state != ANX_PHYSICAL_ISSUED || p->view.logical_epoch != s->view.epoch ||
	    s->view.state != ANX_SHAPE_READY || s->view.epoch == ~(uint64_t)0)) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = owner_check(s);
	if (ret == ANX_OK) ret = plan_phase(p, false);
	if (ret == ANX_OK) ret = use_check(s, p->view.node, true);
	if (ret == ANX_OK && p->view.mode == ANX_PHYSICAL_REUSE) ret = use_check(s, p->view.source_node, false);
	struct anx_anxml_response *result = NULL;
	if (ret == ANX_OK) { result = anx_zalloc(sizeof(*result)); if (!result) ret = ANX_ENOMEM; }
	if (ret == ANX_OK) { p->view.state = ANX_PHYSICAL_RUNNING; s->view.state = ANX_SHAPE_RUNNING; }
	anx_spin_unlock_irqrestore(&shape_lock, flags);
	if (ret != ANX_OK) return ret;
	uint32_t node = p->view.node;
	struct anx_model_use_view use;
	if (p->view.mode == ANX_PHYSICAL_REUSE)
		ret = anx_model_use_reuse(s->uses[node].id, s->uses[node].epoch, s->uses[p->view.source_node].id, result, &use);
	else if (p->view.resource_shape)
		ret = anx_resource_shape_execute(p->view.resource_shape, p->view.resource_epoch, p->view.replica,
			s->uses[node].id, s->uses[node].epoch, result, &use);
	else ret = anx_model_use_execute(s->uses[node].id, s->uses[node].epoch, result, &use);
	bool produced = ret == ANX_OK;
	if (ret == ANX_OK) ret = owner_check(s);
	if (ret == ANX_OK) ret = plan_phase(p, false);
	anx_spin_lock_irqsave(&shape_lock, &flags);
	p->view.result = ret; p->view.state = ret == ANX_OK ? ANX_PHYSICAL_COMMITTED : ANX_PHYSICAL_FAILED;
	if (ret == ANX_OK) {
		s->uses[node] = use; s->view.completed |= 1U << node; s->view.epoch++;
		s->view.source_node[node] = p->view.source_node; s->view.generated_tokens += result->tokens_generated;
		if (p->view.mode == ANX_PHYSICAL_REUSE) s->view.reused_operations++;
		else s->view.physical_operations++;
		s->view.state = s->view.completed == ((1U << s->spec.count) - 1) ? ANX_SHAPE_COMPLETED : ANX_SHAPE_READY;
		*response = *result; logical_view(s, out);
	} else if (produced) { s->view.state = ANX_SHAPE_FAILED; s->view.result = ret; }
	else s->view.state = ANX_SHAPE_READY;
	anx_spin_unlock_irqrestore(&shape_lock, flags);
	anx_memset(result, 0, sizeof(*result)); anx_free(result); return ret;
}
int anx_physical_plan_get(uint64_t id, struct anx_physical_plan_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&shape_lock, &flags);
	struct physical_record *p = plan_find(id);
	int ret = !p ? ANX_ENOENT : access(p->graph);
	if (ret == ANX_OK && p->view.state == ANX_PHYSICAL_RUNNING) ret = ANX_EBUSY;
	if (ret == ANX_OK) *out = p->view;
	anx_spin_unlock_irqrestore(&shape_lock, flags); return ret;
}
int anx_physical_plan_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&shape_lock, &flags);
	struct physical_record *p = plan_find(id);
	int ret = !p ? ANX_ENOENT : p->view.state == ANX_PHYSICAL_RUNNING ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK && p->view.resource_shape) ret = anx_resource_shape_unbind(p->view.resource_shape);
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_PHYSICAL_PLAN_MAX; i++) if (plans[i] == p) plans[i] = NULL;
		p->graph->plan_refs--; anx_memset(p, 0, sizeof(*p)); anx_free(p);
	}
	anx_spin_unlock_irqrestore(&shape_lock, flags); return ret;
}
