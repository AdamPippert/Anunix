#include <anx/procedure.h>
#include <anx/state_object.h>
#include <anx/icm.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/crypto.h>
#include <anx/spinlock.h>
#include <anx/identity.h>
#include <anx/sched_domain.h>
struct procedure_recipe {
	uint32_t abi, reserved;
	uint64_t version;
	anx_oid_t predecessor, evidence;
	struct anx_model_use_view source;
	struct anx_model_use_source knowledge;
};
struct procedure_validation { anx_oid_t artifact; struct anx_model_use_view replay; };
struct procedure_record {
	struct anx_procedure_view view;
	struct procedure_recipe recipe;
	struct anx_model_use_source artifact, evidence, validation, predecessor;
	struct anx_cell *owner;
	uint32_t children;
	bool busy;
};
static struct procedure_record *records[ANX_PROCEDURE_MAX];
static struct anx_spinlock procedure_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct procedure_record *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_PROCEDURE_MAX; i++) if (records[i] && records[i]->view.id == id) return records[i];
	return NULL;
}
static int access(struct procedure_record *p)
{
	if (!p) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, &p->view.owner) ? ANX_EPERM : ANX_OK;
}
static int owner_check(struct procedure_record *p)
{
	if (anx_cell_status_terminal(p->owner->status)) return ANX_EPERM;
	int ret = anx_cell_check_scope(p->owner);
	if (ret == ANX_OK) ret = anx_cell_check_contract(p->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(p->owner, NULL);
	if (ret == ANX_OK) ret = anx_sched_domain_check(p->owner);
	return ret;
}
static int object_metadata(struct anx_state_object *o, const anx_cid_t *owner,
		enum anx_object_type type, const char *schema, uint32_t exact)
{
	if (!o->version || o->state != ANX_OBJ_SEALED || o->object_type != type || !o->payload || !o->payload_size ||
	    o->payload_size > 1024 || (exact && o->payload_size != exact) ||
	    o->access_policy.rule_count > ANX_MAX_ACCESS_RULES || o->sensitivity > ANX_SENSITIVITY_RESTRICTED) return ANX_EINVAL;
	if (schema && (anx_strcmp(o->schema_uri, schema) || anx_strcmp(o->schema_version, "1"))) return ANX_EINVAL;
	return anx_access_evaluate(&o->access_policy, owner, &o->creator_cell, ANX_ACCESS_READ_PAYLOAD);
}
static int snapshot(struct procedure_record *p, struct anx_model_use_source *source, enum anx_object_type type,
		const char *schema, uint32_t exact, bool capture)
{
	uint8_t bytes[1024];
	struct anx_object_handle h = {0};
	struct anx_model_use_source now = { .oid = source->oid };
	int ret = anx_so_open(&source->oid, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	anx_spin_lock(&h.obj->lock);
	ret = object_metadata(h.obj, &p->view.owner, type, schema, exact);
	if (ret == ANX_OK) { now.version = h.obj->version; now.size = h.obj->payload_size; now.sensitivity = h.obj->sensitivity; }
	anx_spin_unlock(&h.obj->lock);
	if (ret == ANX_OK && !capture && (now.version != source->version || now.size != source->size || now.sensitivity != source->sensitivity)) ret = ANX_EBUSY;
	if (ret == ANX_OK) {
		ret = anx_so_read_payload(&h, 0, bytes, now.size);
		if (ret == (int)now.size) ret = ANX_OK;
		else if (ret >= 0) ret = ANX_EIO;
	}
	if (ret == ANX_OK) {
		anx_sha256(bytes, now.size, now.digest);
		anx_spin_lock(&h.obj->lock);
		ret = object_metadata(h.obj, &p->view.owner, type, schema, exact);
		if (ret == ANX_OK && (h.obj->version != now.version || h.obj->payload_size != now.size ||
		    (uint32_t)h.obj->sensitivity != now.sensitivity)) ret = ANX_EBUSY;
		anx_spin_unlock(&h.obj->lock);
		if (ret == ANX_OK && !capture && anx_memcmp(now.digest, source->digest, 32)) ret = ANX_EBUSY;
		if (ret == ANX_OK && capture) *source = now;
	}
	anx_memset(bytes, 0, sizeof(bytes)); anx_so_close(&h);
	return ret;
}
static int dependencies(struct procedure_record *p, bool validated)
{
	int ret = snapshot(p, &p->artifact, ANX_OBJ_STRUCTURED_DATA, ANX_PROCEDURE_SCHEMA, sizeof(p->recipe), false);
	if (ret == ANX_OK) ret = snapshot(p, &p->recipe.knowledge, ANX_OBJ_BYTE_DATA, NULL, 0, false);
	if (ret == ANX_OK) ret = snapshot(p, &p->evidence, ANX_OBJ_EXECUTION_TRACE, ANX_PROCEDURE_EVIDENCE_SCHEMA, sizeof(p->recipe.source), false);
	if (ret == ANX_OK && p->view.predecessor) ret = snapshot(p, &p->predecessor, ANX_OBJ_STRUCTURED_DATA, ANX_PROCEDURE_SCHEMA, sizeof(p->recipe), false);
	if (ret == ANX_OK && validated) ret = snapshot(p, &p->validation, ANX_OBJ_EXECUTION_TRACE, ANX_PROCEDURE_VALIDATION_SCHEMA, sizeof(struct procedure_validation), false);
	return ret;
}
static int create_object(struct procedure_record *p, enum anx_object_type type, const char *schema,
		const void *bytes, uint32_t size, const anx_oid_t *parents, uint32_t count, struct anx_state_object **out)
{
	struct anx_so_create_params params = { .object_type = type, .schema_uri = schema, .schema_version = "1",
		.payload = bytes, .payload_size = size, .parent_oids = parents, .parent_count = count, .creator_cell = p->view.owner };
	int ret = anx_so_create(&params, out);
	if (ret == ANX_OK) ret = anx_so_seal(&(*out)->oid);
	return ret;
}
static bool executed(const struct anx_model_use_view *use)
{
	return use->state == ANX_MODEL_USE_COMPLETED && !use->reused_from && !use->seed && use->tokens_generated &&
		!anx_memcmp(use->image.digest, use->consumed_image_digest, 32);
}
int anx_procedure_compile(const anx_cid_t *owner, uint64_t source_use, const anx_oid_t *knowledge,
		uint64_t predecessor, struct anx_procedure_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !source_use || !knowledge || !out) return ANX_EINVAL;
	struct procedure_record *p = anx_zalloc(sizeof(*p));
	struct anx_anxml_response *response = anx_zalloc(sizeof(*response));
	struct anx_state_object *evidence = NULL, *artifact = NULL;
	int ret = ANX_ENOMEM;
	if (!p || !response) goto out;
	p->view.owner = *owner; p->view.knowledge = *knowledge; p->view.predecessor = predecessor;
	p->recipe.abi = 1; p->recipe.version = 1; p->recipe.knowledge.oid = *knowledge;
	p->owner = anx_cell_store_lookup(owner);
	ret = p->owner ? owner_check(p) : ANX_ENOENT;
	if (ret == ANX_OK) ret = anx_model_use_get(source_use, &p->recipe.source);
	if (ret == ANX_OK && anx_uuid_compare(&p->recipe.source.owner, owner)) ret = ANX_EPERM;
	if (ret == ANX_OK && !executed(&p->recipe.source)) ret = ANX_EINVAL;
	if (ret == ANX_OK) ret = anx_model_use_read(source_use, response);
	if (ret == ANX_OK) ret = snapshot(p, &p->recipe.knowledge, ANX_OBJ_BYTE_DATA, NULL, 0, true);
	if (ret != ANX_OK) goto out;
	bool flags;
	anx_spin_lock_irqsave(&procedure_lock, &flags);
	struct procedure_record *prior = predecessor ? find(predecessor) : NULL;
	if (predecessor) {
		if (!prior) ret = ANX_ENOENT;
		else if (anx_uuid_compare(&prior->view.owner, owner)) ret = ANX_EPERM;
		else if (prior->view.state != ANX_PROCEDURE_VALIDATED || prior->busy) ret = ANX_EBUSY;
		else if (prior->view.version == ~(uint64_t)0) ret = ANX_EFULL;
		else {
			p->recipe.version = prior->view.version + 1; p->recipe.predecessor = prior->view.artifact;
			p->predecessor = prior->artifact;
			ret = snapshot(p, &p->predecessor, ANX_OBJ_STRUCTURED_DATA, ANX_PROCEDURE_SCHEMA, sizeof(p->recipe), false);
		}
	}
	uint32_t slot;
	for (slot = 0; slot < ANX_PROCEDURE_MAX; slot++) if (!records[slot]) break;
	if (ret == ANX_OK && (slot == ANX_PROCEDURE_MAX || sequence == ~(uint64_t)0)) ret = ANX_EFULL;
	anx_oid_t parents[3] = { p->recipe.source.image.oid, p->recipe.source.prompt.oid };
	if (ret == ANX_OK) ret = create_object(p, ANX_OBJ_EXECUTION_TRACE, ANX_PROCEDURE_EVIDENCE_SCHEMA,
		&p->recipe.source, sizeof(p->recipe.source), parents, 2, &evidence);
	if (ret == ANX_OK) {
		p->evidence.oid = evidence->oid;
		p->recipe.evidence = evidence->oid;
		ret = snapshot(p, &p->evidence, ANX_OBJ_EXECUTION_TRACE, ANX_PROCEDURE_EVIDENCE_SCHEMA, sizeof(p->recipe.source), true);
	}
	parents[0] = evidence ? evidence->oid : ANX_UUID_NIL; parents[1] = *knowledge; parents[2] = p->recipe.predecessor;
	if (ret == ANX_OK) ret = create_object(p, ANX_OBJ_STRUCTURED_DATA, ANX_PROCEDURE_SCHEMA,
		&p->recipe, sizeof(p->recipe), parents, predecessor ? 3 : 2, &artifact);
	if (ret == ANX_OK) ret = anx_icm_tag(&artifact->oid, "procedural", "recipe", "evidence", NULL, "anxml-toy", ANX_PROCEDURE_SCHEMA);
	if (ret == ANX_OK) {
		p->artifact.oid = artifact->oid;
		ret = snapshot(p, &p->artifact, ANX_OBJ_STRUCTURED_DATA, ANX_PROCEDURE_SCHEMA, sizeof(p->recipe), true);
	}
	if (ret == ANX_OK) {
		p->view.id = ++sequence; p->view.epoch = 1; p->view.version = p->recipe.version;
		p->view.artifact = artifact->oid; p->view.evidence = evidence->oid;
		records[slot] = p; if (prior) prior->children++; *out = p->view;
	}
	anx_spin_unlock_irqrestore(&procedure_lock, flags);
out:
	if (artifact) { if (ret != ANX_OK) anx_so_delete(&artifact->oid, false); anx_objstore_release(artifact); }
	if (evidence) { if (ret != ANX_OK) anx_so_delete(&evidence->oid, false); anx_objstore_release(evidence); }
	if (response) { anx_memset(response, 0, sizeof(*response)); anx_free(response); }
	if (ret != ANX_OK && p) { if (p->owner) anx_cell_store_release(p->owner); anx_memset(p, 0, sizeof(*p)); anx_free(p); }
	return ret;
}
int anx_procedure_validate(uint64_t id, uint64_t epoch, uint64_t replay_use, struct anx_procedure_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch || !replay_use || !out) return ANX_EINVAL;
	struct anx_anxml_response *response = anx_zalloc(sizeof(*response));
	if (!response) return ANX_ENOMEM;
	struct procedure_validation proof = {0};
	struct anx_state_object *evidence = NULL;
	bool flags;
	anx_spin_lock_irqsave(&procedure_lock, &flags);
	struct procedure_record *p = find(id);
	int ret = !p ? ANX_ENOENT : p->busy || p->view.epoch != epoch || p->view.state != ANX_PROCEDURE_DRAFT ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) ret = owner_check(p);
	if (ret == ANX_OK) ret = dependencies(p, false);
	if (ret == ANX_OK) ret = anx_model_use_get(replay_use, &proof.replay);
	if (ret == ANX_OK && (!executed(&proof.replay) || proof.replay.id <= p->recipe.source.id)) ret = ANX_EINVAL;
	if (ret == ANX_OK && !anx_model_use_same_request(&proof.replay, &p->recipe.source)) ret = ANX_ENOTSUP;
	if (ret == ANX_OK) ret = anx_model_use_read(replay_use, response);
	if (ret == ANX_OK && (proof.replay.output_size != p->recipe.source.output_size ||
	    anx_memcmp(proof.replay.output_digest, p->recipe.source.output_digest, 32))) ret = ANX_EIO;
	if (ret == ANX_OK) {
		proof.artifact = p->view.artifact;
		ret = create_object(p, ANX_OBJ_EXECUTION_TRACE, ANX_PROCEDURE_VALIDATION_SCHEMA, &proof, sizeof(proof), &proof.artifact, 1, &evidence);
	}
	if (ret == ANX_OK) {
		struct anx_model_use_source stamp = { .oid = evidence->oid };
		ret = snapshot(p, &stamp, ANX_OBJ_EXECUTION_TRACE, ANX_PROCEDURE_VALIDATION_SCHEMA, sizeof(proof), true);
		if (ret == ANX_OK) {
			p->validation = stamp; p->view.validation_evidence = stamp.oid;
			p->view.state = ANX_PROCEDURE_VALIDATED; p->view.epoch++; *out = p->view;
		}
	}
	anx_spin_unlock_irqrestore(&procedure_lock, flags);
	if (evidence) { if (ret != ANX_OK) anx_so_delete(&evidence->oid, false); anx_objstore_release(evidence); }
	anx_memset(response, 0, sizeof(*response)); anx_free(response);
	return ret;
}
int anx_procedure_get(uint64_t id, struct anx_procedure_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&procedure_lock, &flags);
	struct procedure_record *p = find(id);
	int ret = access(p);
	if (ret == ANX_OK && p->busy) ret = ANX_EBUSY;
	if (ret == ANX_OK) *out = p->view;
	anx_spin_unlock_irqrestore(&procedure_lock, flags);
	return ret;
}
int anx_procedure_execute(uint64_t id, uint64_t epoch, uint64_t use, uint64_t use_epoch, struct anx_anxml_response *out)
{
	if (!id || !epoch || !use || !use_epoch || !out) return ANX_EINVAL;
	struct anx_model_use_view current;
	bool flags;
	anx_spin_lock_irqsave(&procedure_lock, &flags);
	struct procedure_record *p = find(id);
	int ret = access(p);
	if (ret == ANX_OK && (p->busy || p->view.epoch != epoch)) ret = ANX_EBUSY;
	if (ret == ANX_OK && p->view.state != ANX_PROCEDURE_VALIDATED) ret = ANX_EPERM;
	if (ret == ANX_OK) ret = owner_check(p);
	if (ret == ANX_OK) ret = dependencies(p, true);
	if (ret == ANX_OK) ret = anx_model_use_get(use, &current);
	if (ret == ANX_OK && (current.epoch != use_epoch || current.state != ANX_MODEL_USE_READY)) ret = ANX_EBUSY;
	if (ret == ANX_OK && !anx_model_use_same_request(&current, &p->recipe.source)) ret = ANX_ENOTSUP;
	struct anx_anxml_response *response = NULL;
	if (ret == ANX_OK) { response = anx_zalloc(sizeof(*response)); if (!response) ret = ANX_ENOMEM; }
	if (ret == ANX_OK) p->busy = true;
	anx_spin_unlock_irqrestore(&procedure_lock, flags);
	if (ret != ANX_OK) return ret;
	ret = anx_model_use_execute(use, use_epoch, response, &current);
	if (ret == ANX_OK && (current.output_size != p->recipe.source.output_size ||
	    anx_memcmp(current.output_digest, p->recipe.source.output_digest, 32))) ret = ANX_EIO;
	if (ret == ANX_OK) ret = dependencies(p, true);
	anx_spin_lock_irqsave(&procedure_lock, &flags);
	p->busy = false;
	if (ret == ANX_OK) *out = *response;
	anx_spin_unlock_irqrestore(&procedure_lock, flags);
	anx_memset(response, 0, sizeof(*response)); anx_free(response);
	return ret;
}
int anx_procedure_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&procedure_lock, &flags);
	struct procedure_record *p = find(id);
	int ret = !p ? ANX_ENOENT : p->busy || p->children ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) {
		struct procedure_record *prior = find(p->view.predecessor);
		if (prior) prior->children--;
		for (uint32_t i = 0; i < ANX_PROCEDURE_MAX; i++) if (records[i] == p) records[i] = NULL;
		anx_cell_store_release(p->owner); anx_memset(p, 0, sizeof(*p)); anx_free(p);
	}
	anx_spin_unlock_irqrestore(&procedure_lock, flags);
	return ret;
}
