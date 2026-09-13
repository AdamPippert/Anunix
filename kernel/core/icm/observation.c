/* Versioned observations retain history while refusing stale context reads. */
#include <anx/observation.h>
#include <anx/cell.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct observation_ref {
	anx_oid_t oid;
	uint64_t version;
	enum anx_object_type type;
	enum anx_sensitivity sensitivity;
	uint8_t digest[32];
};
struct observation_record {
	struct anx_observation_view view;
	struct observation_ref subject, content, provenance;
};
static struct observation_record catalog[ANX_OBSERVATION_MAX];
static struct anx_spinlock catalog_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;

static struct observation_record *lookup(const anx_oid_t *id)
{
	for (uint32_t i = 0; i < ANX_OBSERVATION_MAX; i++)
		if (!anx_uuid_is_nil(&catalog[i].view.id) && !anx_uuid_compare(&catalog[i].view.id, id)) return &catalog[i];
	return NULL;
}

static int copy_record(const anx_oid_t *id, struct observation_record *out)
{
	const anx_cid_t *caller = anx_cell_current_id();
	bool flags;
	int ret = ANX_ENOENT;
	if (!id || !out || anx_uuid_is_nil(id)) return ANX_EINVAL;
	anx_spin_lock_irqsave(&catalog_lock, &flags);
	struct observation_record *r = lookup(id);
	if (r) {
		if (caller && anx_uuid_compare(caller, &r->view.spec.owner)) ret = ANX_EPERM;
		else { *out = *r; ret = ANX_OK; }
	}
	anx_spin_unlock_irqrestore(&catalog_lock, flags);
	return ret;
}

static int capture_ref(const anx_oid_t *oid, bool sealed, struct observation_ref *out)
{
	struct anx_object_handle h = {0};
	struct observation_ref ref = {0};
	int ret = anx_so_open(oid, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	anx_spin_lock(&h.obj->lock);
	struct anx_state_object *obj = h.obj;
	if (obj->staged) ret = ANX_EBUSY;
	else if ((sealed && obj->state != ANX_OBJ_SEALED) ||
		 (obj->state != ANX_OBJ_ACTIVE && obj->state != ANX_OBJ_SEALED)) ret = ANX_EPERM;
	else if (obj->payload_size > ANX_OBSERVATION_OBJECT_MAX || (obj->payload_size && !obj->payload) ||
		 (int)obj->sensitivity < 0 || obj->sensitivity > ANX_SENSITIVITY_RESTRICTED) ret = ANX_EINVAL;
	else {
		ref.oid = obj->oid; ref.version = obj->version; ref.type = obj->object_type; ref.sensitivity = obj->sensitivity;
		anx_sha256(obj->payload, (uint32_t)obj->payload_size, ref.digest);
	}
	anx_spin_unlock(&obj->lock);
	anx_so_close(&h);
	if (ret == ANX_OK) *out = ref;
	return ret;
}

static int check_ref(const struct observation_ref *expected, bool sealed)
{
	struct observation_ref current;
	int ret = capture_ref(&expected->oid, sealed, &current);
	if (ret != ANX_OK) return ret;
	return current.version == expected->version && current.type == expected->type &&
		current.sensitivity == expected->sensitivity && !anx_memcmp(current.digest, expected->digest, sizeof(current.digest)) ?
		ANX_OK : ANX_EBUSY;
}

static int current_record(const struct observation_record *r)
{
	if (!anx_uuid_is_nil(&r->view.superseded_by)) return ANX_EBUSY;
	int ret = check_ref(&r->subject, false);
	if (ret == ANX_OK) ret = check_ref(&r->content, true);
	if (ret == ANX_OK) ret = check_ref(&r->provenance, true);
	return ret;
}

int anx_observation_publish(const struct anx_observation_spec *spec, anx_oid_t *out)
{
	struct observation_record record = {0};
	struct anx_cell *owner;
	bool flags;
	int ret;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!spec || !out || anx_uuid_is_nil(&spec->owner) || anx_uuid_is_nil(&spec->subject) ||
	    anx_uuid_is_nil(&spec->content) || anx_uuid_is_nil(&spec->provenance) ||
	    (spec->kind != ANX_OBSERVATION_VISUAL && spec->kind != ANX_OBSERVATION_STRUCTURED)) return ANX_EINVAL;
	record.view.spec = *spec;
	owner = anx_cell_store_lookup(&spec->owner);
	if (!owner) return ANX_ENOENT;
	ret = anx_cell_status_terminal(owner->status) ? ANX_EPERM : ANX_OK;
	anx_cell_store_release(owner);
	if (ret != ANX_OK) return ret;
	ret = capture_ref(&spec->subject, false, &record.subject);
	if (ret == ANX_OK) ret = capture_ref(&spec->content, true, &record.content);
	if (ret == ANX_OK) ret = capture_ref(&spec->provenance, true, &record.provenance);
	if (ret != ANX_OK) return ret;
	if (record.provenance.type != ANX_OBJ_EXECUTION_TRACE || record.content.sensitivity < record.subject.sensitivity ||
	    record.content.sensitivity < record.provenance.sensitivity) return ANX_EPERM;
	record.view.subject_version = record.subject.version;
	anx_uuid_generate(&record.view.id);
	ret = ANX_EFULL;
	anx_spin_lock_irqsave(&catalog_lock, &flags);
	if (sequence != ~(uint64_t)0) {
		for (uint32_t i = 0; i < ANX_OBSERVATION_MAX; i++) if (anx_uuid_is_nil(&catalog[i].view.id)) {
			record.view.sequence = ++sequence;
			catalog[i] = record;
			*out = record.view.id;
			ret = ANX_OK;
			break;
		}
	}
	anx_spin_unlock_irqrestore(&catalog_lock, flags);
	return ret;
}

int anx_observation_describe(const anx_oid_t *id, struct anx_observation_view *out)
{
	struct observation_record record;
	if (!out) return ANX_EINVAL;
	int ret = copy_record(id, &record);
	if (ret == ANX_OK) *out = record.view;
	return ret;
}

int anx_observation_supersede(const anx_oid_t *previous, const anx_oid_t *replacement)
{
	struct observation_record old, next;
	bool flags;
	if (anx_cell_current_id()) return ANX_EPERM;
	int ret = copy_record(previous, &old);
	if (ret == ANX_OK) ret = copy_record(replacement, &next);
	if (ret != ANX_OK) return ret;
	if (!anx_uuid_is_nil(&old.view.superseded_by)) return ANX_EBUSY;
	if (next.view.sequence <= old.view.sequence || next.view.subject_version < old.view.subject_version ||
	    !next.view.spec.full_coverage || next.view.spec.kind != ANX_OBSERVATION_STRUCTURED ||
	    anx_uuid_compare(&old.view.spec.owner, &next.view.spec.owner) ||
	    anx_uuid_compare(&old.view.spec.subject, &next.view.spec.subject)) return ANX_EPERM;
	ret = current_record(&next);
	if (ret != ANX_OK) return ret;
	anx_spin_lock_irqsave(&catalog_lock, &flags);
	struct observation_record *a = lookup(previous), *b = lookup(replacement);
	if (!a || !b) ret = ANX_ENOENT;
	else if (!anx_uuid_is_nil(&a->view.superseded_by) || !anx_uuid_is_nil(&b->view.superseded_by)) ret = ANX_EBUSY;
	else a->view.superseded_by = *replacement;
	anx_spin_unlock_irqrestore(&catalog_lock, flags);
	return ret;
}

int anx_observation_read(const anx_oid_t *id, uint64_t offset, void *out, uint64_t bytes)
{
	struct observation_record record;
	struct anx_object_handle h = {0};
	bool flags;
	if (!out) return ANX_EINVAL;
	int ret = copy_record(id, &record);
	if (ret == ANX_OK) ret = current_record(&record);
	if (ret == ANX_OK) ret = anx_so_open(&record.view.spec.content, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	/* Keep release and supersession from changing the catalog during this copy. */
	anx_spin_lock_irqsave(&catalog_lock, &flags);
	struct observation_record *current = lookup(id);
	if (!current) ret = ANX_ENOENT;
	else if (!anx_uuid_is_nil(&current->view.superseded_by)) ret = ANX_EBUSY;
	else ret = anx_so_read_payload(&h, offset, out, bytes);
	anx_spin_unlock_irqrestore(&catalog_lock, flags);
	anx_so_close(&h);
	return ret;
}

int anx_observation_release(const anx_oid_t *id)
{
	bool flags;
	int ret = ANX_ENOENT;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || anx_uuid_is_nil(id)) return ANX_EINVAL;
	anx_spin_lock_irqsave(&catalog_lock, &flags);
	struct observation_record *r = lookup(id);
	if (r) { anx_memset(r, 0, sizeof(*r)); ret = ANX_OK; }
	anx_spin_unlock_irqrestore(&catalog_lock, flags);
	return ret;
}
