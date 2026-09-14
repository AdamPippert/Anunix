/* Private boot-local model bindings retain logical declarations across engine changes. */
#include <anx/route_binding.h>
#include <anx/model_server.h>
#include <anx/engine_lease.h>
#include <anx/state_object.h>
#include <anx/identity.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/arch.h>

struct binding_ref { anx_oid_t oid; uint64_t version; uint32_t sensitivity; uint8_t digest[32]; };
struct binding_record {
	struct anx_cell *owner;
	struct anx_route_binding_spec spec;
	struct anx_route_binding_view view;
	uint8_t declaration[32], engines[ANX_MAX_ROUTE_CANDIDATES][32];
	struct binding_ref model, inputs[ANX_MAX_CELL_INPUTS];
	bool private_data;
};
static struct binding_record *bindings[ANX_ROUTE_BINDING_MAX];
static struct anx_spinlock binding_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;

static void word(struct anx_sha256_ctx *hash, uint64_t value)
{
	uint8_t bytes[8];
	for (uint32_t i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (8 * i));
	anx_sha256_update(hash, bytes, sizeof(bytes));
}
static int declaration(const struct anx_cell *cell, uint8_t out[32])
{
	if (cell->input_count > ANX_MAX_CELL_INPUTS || cell->dep_count > ANX_MAX_CELL_DEPS) return ANX_EINVAL;
	struct anx_sha256_ctx h;
	anx_sha256_init(&h);
	/* These are boot-local declaration bytes, not a portable serialization format. */
#define FIELD(name) anx_sha256_update(&h, &cell->name, sizeof(cell->name))
	FIELD(cid); FIELD(cell_type); FIELD(intent); FIELD(constraints); FIELD(routing);
	FIELD(validation); FIELD(commit); FIELD(execution); FIELD(retry); FIELD(contract); FIELD(cognitive);
	FIELD(parent_cid); FIELD(identity_id); FIELD(effect_fence_id); FIELD(tool_namespace_id); FIELD(revision_lease_id);
	FIELD(recursion_depth); FIELD(input_count); FIELD(dep_count);
#undef FIELD
	anx_sha256_update(&h, cell->inputs, cell->input_count * sizeof(cell->inputs[0]));
	anx_sha256_update(&h, cell->dep_cids, cell->dep_count * sizeof(cell->dep_cids[0]));
	anx_sha256_final(&h, out);
	return ANX_OK;
}
static int object_ref(const anx_oid_t *oid, const anx_cid_t *owner, bool sealed, struct binding_ref *out)
{
	struct anx_state_object *object = anx_objstore_lookup(oid);
	struct binding_ref ref;
	bool irq;
	int ret = ANX_OK;
	if (!object) return ANX_ENOENT;
	anx_memset(&ref, 0, sizeof(ref));
	anx_spin_lock_irqsave(&object->lock, &irq);
	if (object->access_policy.rule_count > ANX_MAX_ACCESS_RULES ||
	    (uint32_t)object->sensitivity > ANX_SENSITIVITY_RESTRICTED ||
	    !object->version || !object->payload || !object->payload_size ||
	    object->payload_size > ANX_ROUTE_BINDING_OBJECT_MAX) ret = ANX_EINVAL;
	else if ((sealed && object->state != ANX_OBJ_SEALED) ||
		 (object->state != ANX_OBJ_ACTIVE && object->state != ANX_OBJ_SEALED)) ret = ANX_EBUSY;
	else ret = anx_access_evaluate(&object->access_policy, owner, &object->creator_cell, ANX_ACCESS_READ_PAYLOAD);
	if (ret == ANX_OK) {
		struct anx_sha256_ctx h;
		ref.oid = object->oid; ref.version = object->version; ref.sensitivity = object->sensitivity;
		anx_sha256_init(&h); word(&h, object->object_type); word(&h, object->payload_size);
		anx_sha256_update(&h, object->schema_uri, sizeof(object->schema_uri));
		anx_sha256_update(&h, object->schema_version, sizeof(object->schema_version));
		anx_sha256_update(&h, object->payload, (uint32_t)object->payload_size);
		anx_sha256_final(&h, ref.digest);
	}
	anx_spin_unlock_irqrestore(&object->lock, irq);
	anx_objstore_release(object);
	if (ret == ANX_OK) anx_memcpy(out, &ref, sizeof(ref));
	return ret;
}
static int engine_ref(const anx_eid_t *id, uint8_t out[32])
{
	struct anx_engine *engine = anx_engine_lookup(id);
	bool irq;
	if (!engine) return ANX_ENOENT;
	anx_spin_lock_irqsave(&engine->lock, &irq);
	if (engine->engine_class != ANX_ENGINE_LOCAL_MODEL && engine->engine_class != ANX_ENGINE_REMOTE_MODEL) {
		anx_spin_unlock_irqrestore(&engine->lock, irq); return ANX_EINVAL;
	}
	uint64_t fields[] = { engine->eid.hi, engine->eid.lo, engine->engine_class, engine->capabilities,
		engine->supports_private_data, engine->requires_network, engine->max_context_tokens, engine->is_local,
		engine->model.param_count, engine->model.quant, engine->model.context_window,
		engine->model.mem_footprint_bytes, engine->model.offline_capable };
	anx_spin_unlock_irqrestore(&engine->lock, irq);
	struct anx_sha256_ctx h;
	anx_sha256_init(&h);
	for (uint32_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) word(&h, fields[i]);
	anx_sha256_final(&h, out);
	return ANX_OK;
}
static struct binding_record *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_ROUTE_BINDING_MAX; i++) if (bindings[i] && bindings[i]->view.id == id) return bindings[i];
	return NULL;
}
static int access(struct binding_record *b)
{
	if (!b) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, &b->owner->cid) ? ANX_EPERM : ANX_OK;
}
static int logical_check(struct binding_record *b)
{
	uint8_t digest[32];
	struct binding_ref current;
	if (anx_cell_status_terminal(b->owner->status)) return ANX_EPERM;
	int ret = declaration(b->owner, digest);
	if (ret != ANX_OK) return ret;
	if (anx_memcmp(digest, b->declaration, sizeof(digest))) return ANX_EBUSY;
	ret = anx_cell_check_scope(b->owner);
	if (ret == ANX_OK) ret = anx_cell_check_contract(b->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(b->owner, NULL);
	if (ret != ANX_OK) return ret;
	ret = anx_cell_deps_satisfied(b->owner);
	if (ret != 1) return ret < 0 ? ret : ANX_EBUSY;
	ret = object_ref(&b->model.oid, &b->owner->cid, true, &current);
	if (ret != ANX_OK) return ret;
	if (anx_memcmp(&current, &b->model, sizeof(current))) return ANX_EBUSY;
	for (uint32_t i = 0; i < b->owner->input_count; i++) {
		if (anx_uuid_is_nil(&b->inputs[i].oid)) continue;
		ret = object_ref(&b->inputs[i].oid, &b->owner->cid, false, &current);
		if (ret != ANX_OK) return ret;
		if (anx_memcmp(&current, &b->inputs[i], sizeof(current))) return ANX_EBUSY;
	}
	return ANX_OK;
}
static bool ready(struct binding_record *b, uint32_t slot)
{
	uint8_t digest[32];
	const anx_eid_t *id = &b->spec.engines[slot];
	if (engine_ref(id, digest) != ANX_OK || anx_memcmp(digest, b->engines[slot], sizeof(digest))) return false;
	struct anx_engine *engine = anx_engine_lookup(id);
	struct anx_model_server *server = anx_msrv_lookup(id);
	struct anx_engine_lease *lease = anx_lease_lookup(id);
	if (!engine || !server || !lease || lease != engine->lease || lease->revoked ||
	    (engine->status != ANX_ENGINE_AVAILABLE && engine->status != ANX_ENGINE_DEGRADED) ||
	    (lease->expires_at && arch_time_now() >= lease->expires_at) ||
	    lease->mem_reserved_bytes < engine->model.mem_footprint_bytes ||
	    engine->max_context_tokens < b->spec.required_context_tokens ||
	    (b->private_data && !engine->supports_private_data)) return false;
	bool irq, serving;
	anx_spin_lock_irqsave(&server->lock, &irq);
	serving = server->msrv_status == ANX_MSRV_SERVING && server->pending_count < server->max_pending;
	anx_spin_unlock_irqrestore(&server->lock, irq);
	return serving;
}
static int choose(struct binding_record *b, anx_eid_t *engine_out, bool existing_only)
{
	struct anx_route_session session = {0};
	struct anx_route_result result;
	session.required_caps = b->spec.required_caps;
	for (uint32_t i = 0; i < b->spec.engine_count; i++) {
		if (existing_only && anx_uuid_compare(&b->spec.engines[i], &b->view.engine)) continue;
		if (!ready(b, i)) continue;
		session.eligible_engines[session.engine_count++] = b->spec.engines[i];
		if (!anx_uuid_compare(&b->spec.engines[i], &b->view.engine)) {
			session.selected_engine = b->view.engine; session.placement_count = 1;
		}
	}
	if (!session.engine_count) return ANX_ENODEV;
	int ret = anx_route_plan_session(b->owner, &session, &result);
	if (ret == ANX_OK) *engine_out = session.selected_engine;
	return ret;
}
int anx_route_binding_create(const anx_cid_t *cell, const struct anx_route_binding_spec *spec,
		struct anx_route_binding_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!cell || !spec || !out || spec->schema != 1 || anx_uuid_is_nil(&spec->model) ||
	    !spec->required_context_tokens || !spec->engine_count || spec->engine_count > ANX_MAX_ROUTE_CANDIDATES) return ANX_EINVAL;
	struct binding_record *b = anx_zalloc(sizeof(*b));
	if (!b) return ANX_ENOMEM;
	b->spec = *spec;
	spec = &b->spec;
	b->owner = anx_cell_store_lookup(cell);
	int ret = ANX_ENOENT;
	if (!b->owner) goto fail;
	if (anx_cell_status_terminal(b->owner->status)) { ret = ANX_EPERM; goto fail; }
	ret = declaration(b->owner, b->declaration);
	if (ret == ANX_OK) ret = anx_cell_check_scope(b->owner);
	if (ret == ANX_OK) ret = anx_cell_check_contract(b->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(b->owner, NULL);
	if (ret != ANX_OK) goto fail;
	if (b->owner->cognitive.max_tokens > spec->required_context_tokens) { ret = ANX_EINVAL; goto fail; }
	ret = object_ref(&spec->model, cell, true, &b->model);
	if (ret != ANX_OK) goto fail;
	b->private_data = b->model.sensitivity != ANX_SENSITIVITY_PUBLIC;
	for (uint32_t i = 0; i < b->owner->input_count; i++) {
		const struct anx_cell_input *input = &b->owner->inputs[i];
		if (input->mode != ANX_INPUT_READ) { ret = ANX_ENOTSUP; goto fail; }
		if (anx_uuid_is_nil(&input->state_object_ref)) {
			if (input->required) { ret = ANX_ENOENT; goto fail; }
			continue;
		}
		ret = object_ref(&input->state_object_ref, cell, false, &b->inputs[i]);
		if (ret != ANX_OK) goto fail;
		if (b->inputs[i].sensitivity != ANX_SENSITIVITY_PUBLIC) b->private_data = true;
	}
	for (uint32_t i = 0; i < spec->engine_count; i++) {
		if (anx_uuid_is_nil(&spec->engines[i])) { ret = ANX_EINVAL; goto fail; }
		for (uint32_t j = 0; j < i; j++) if (!anx_uuid_compare(&spec->engines[i], &spec->engines[j])) { ret = ANX_EINVAL; goto fail; }
		ret = engine_ref(&spec->engines[i], b->engines[i]);
		if (ret != ANX_OK) goto fail;
	}
	bool irq;
	anx_spin_lock_irqsave(&binding_lock, &irq);
	uint32_t slot;
	for (slot = 0; slot < ANX_ROUTE_BINDING_MAX; slot++) if (!bindings[slot]) break;
	if (slot == ANX_ROUTE_BINDING_MAX || sequence == ~(uint64_t)0) ret = ANX_EFULL;
	else {
		b->view.id = ++sequence; b->view.epoch = 1; b->view.cell = *cell;
		b->view.model = b->model.oid; b->view.model_version = b->model.version;
		bindings[slot] = b; *out = b->view;
	}
	anx_spin_unlock_irqrestore(&binding_lock, irq);
	if (ret == ANX_OK) return ret;
fail:
	anx_cell_store_release(b->owner); anx_free(b);
	return ret;
}
int anx_route_binding_select(uint64_t id, uint64_t epoch, struct anx_route_binding_view *out)
{
	if (!id || !epoch || !out) return ANX_EINVAL;
	bool irq;
	anx_spin_lock_irqsave(&binding_lock, &irq);
	struct binding_record *b = find(id);
	int ret = access(b);
	if (ret == ANX_OK && epoch != b->view.epoch) ret = ANX_EBUSY;
	if (ret == ANX_OK && b->view.epoch == ~(uint64_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK) ret = logical_check(b);
	anx_eid_t engine;
	if (ret == ANX_OK) ret = choose(b, &engine, false);
	if (ret == ANX_OK) { b->view.engine = engine; b->view.epoch++; *out = b->view; }
	anx_spin_unlock_irqrestore(&binding_lock, irq);
	return ret;
}
int anx_route_binding_get(uint64_t id, struct anx_route_binding_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool irq;
	anx_spin_lock_irqsave(&binding_lock, &irq);
	struct binding_record *b = find(id);
	int ret = access(b);
	if (ret == ANX_OK) *out = b->view;
	anx_spin_unlock_irqrestore(&binding_lock, irq);
	return ret;
}
int anx_route_binding_check(uint64_t id, uint64_t epoch)
{
	if (!id || !epoch) return ANX_EINVAL;
	bool irq;
	anx_spin_lock_irqsave(&binding_lock, &irq);
	struct binding_record *b = find(id);
	int ret = access(b);
	if (ret == ANX_OK && epoch != b->view.epoch) ret = ANX_EBUSY;
	if (ret == ANX_OK && anx_uuid_is_nil(&b->view.engine)) ret = ANX_ENODEV;
	if (ret == ANX_OK) ret = logical_check(b);
	anx_eid_t engine;
	if (ret == ANX_OK) ret = choose(b, &engine, true);
	anx_spin_unlock_irqrestore(&binding_lock, irq);
	return ret;
}
int anx_route_binding_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool irq;
	struct binding_record *b = NULL;
	anx_spin_lock_irqsave(&binding_lock, &irq);
	for (uint32_t i = 0; i < ANX_ROUTE_BINDING_MAX; i++) if (bindings[i] && bindings[i]->view.id == id) {
		b = bindings[i]; bindings[i] = NULL; break;
	}
	anx_spin_unlock_irqrestore(&binding_lock, irq);
	if (!b) return ANX_ENOENT;
	anx_cell_store_release(b->owner); anx_free(b);
	return ANX_OK;
}
