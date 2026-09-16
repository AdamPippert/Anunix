#include <anx/identity.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/alloc.h>

struct identity_entry {
	struct anx_identity_view current;
	struct identity_entry *next;
};

static struct identity_entry *identities;
static uint32_t identity_count;
static struct anx_spinlock identity_lock = ANX_SPINLOCK_INIT;
static bool writing;

/* Writers reserve the registry without holding a spinlock during signing or allocation. */
static int begin_write(void)
{
	bool flags;
	int ret = ANX_OK;
	anx_spin_lock_irqsave(&identity_lock, &flags);
	if (writing)
		ret = ANX_EBUSY;
	else
		writing = true;
	anx_spin_unlock_irqrestore(&identity_lock, flags);
	return ret;
}

static void end_write(void)
{
	bool flags;
	anx_spin_lock_irqsave(&identity_lock, &flags);
	writing = false;
	anx_spin_unlock_irqrestore(&identity_lock, flags);
}

/* Callers hold the lock or own the writer reservation. Entries never disappear. */
static struct identity_entry *find_entry(const anx_oid_t *id)
{
	for (struct identity_entry *entry = identities; entry; entry = entry->next)
		if (!anx_uuid_compare(&entry->current.commitment.identity_id, id))
			return entry;
	return NULL;
}

static void put64(uint8_t *out, uint64_t value)
{
	for (uint32_t i = 0; i < 8; i++)
		out[i] = (uint8_t)(value >> (8 * i));
}

static int encode(const struct anx_identity_commitment *c, uint8_t out[ANX_IDENTITY_WIRE_BYTES])
{
	if (!c || c->schema != 1 || anx_uuid_is_nil(&c->identity_id) || !c->generation ||
	    (c->authority & ~ANX_CAP_AUTH_ALL))
		return ANX_EINVAL;
	anx_memcpy(out, "ANX-IDENTITY-v1", 16);
	put64(out + 16, c->identity_id.hi);
	put64(out + 24, c->identity_id.lo);
	put64(out + 32, c->generation);
	for (uint32_t i = 0; i < 4; i++)
		out[40 + i] = (uint8_t)(c->authority >> (8 * i));
	anx_memcpy(out + 44, c->operator_key, 32);
	anx_memcpy(out + 76, c->previous_digest, 32);
	return ANX_OK;
}

int anx_identity_digest(const struct anx_identity_commitment *c, uint8_t digest[32])
{
	uint8_t bytes[ANX_IDENTITY_WIRE_BYTES];
	if (!digest || encode(c, bytes) != ANX_OK)
		return ANX_EINVAL;
	anx_sha256(bytes, sizeof(bytes), digest);
	return ANX_OK;
}

static int make_record(const struct anx_identity_commitment *c, const uint8_t signature[64],
		       const anx_oid_t *parent, struct anx_identity_view *view)
{
	struct anx_so_create_params params = {0};
	struct anx_state_object *object;
	uint8_t payload[ANX_IDENTITY_RECORD_BYTES];
	int ret = encode(c, payload);
	if (ret != ANX_OK)
		return ret;
	anx_memcpy(payload + ANX_IDENTITY_WIRE_BYTES, signature, 64);
	params.object_type = ANX_OBJ_STRUCTURED_DATA;
	params.schema_uri = ANX_IDENTITY_SCHEMA;
	params.schema_version = "1";
	params.payload = payload;
	params.payload_size = sizeof(payload);
	params.parent_oids = parent;
	params.parent_count = parent ? 1 : 0;
	ret = anx_so_create(&params, &object);
	if (ret != ANX_OK)
		return ret;
	ret = anx_so_seal(&object->oid);
	if (ret == ANX_OK) {
		object->retention.deletion_hold = true;
		anx_memset(view, 0, sizeof(*view));
		view->commitment = *c;
		anx_sha256(payload, ANX_IDENTITY_WIRE_BYTES, view->digest);
		view->record_oid = object->oid;
	} else {
		anx_so_delete(&object->oid, true);
	}
	anx_objstore_release(object);
	return ret;
}

int anx_identity_create(const struct anx_identity_commitment *root, const uint8_t signature[64])
{
	struct identity_entry *entry;
	uint8_t digest[32], zero[32] = {0};
	bool flags;
	int ret;
	if (!signature || anx_identity_digest(root, digest) != ANX_OK ||
	    root->generation != 1 || anx_memcmp(root->previous_digest, zero, 32))
		return ANX_EINVAL;
	if (anx_cell_current_id() || !anx_memcmp(root->operator_key, zero, 32) ||
	    anx_ed25519_verify(signature, digest, 32, root->operator_key))
		return ANX_EPERM;
	ret = begin_write();
	if (ret != ANX_OK)
		return ret;
	if (find_entry(&root->identity_id)) {
		ret = ANX_EEXIST;
		goto out;
	}
	if (identity_count == ANX_IDENTITY_MAX) {
		ret = ANX_EFULL;
		goto out;
	}
	entry = anx_zalloc(sizeof(*entry));
	if (!entry) {
		ret = ANX_ENOMEM;
		goto out;
	}
	ret = make_record(root, signature, NULL, &entry->current);
	if (ret != ANX_OK) {
		anx_free(entry);
		goto out;
	}
	anx_spin_lock_irqsave(&identity_lock, &flags);
	entry->next = identities;
	identities = entry;
	identity_count++;
	anx_spin_unlock_irqrestore(&identity_lock, flags);
out:
	end_write();
	return ret;
}

int anx_identity_transition(const struct anx_identity_commitment *next, const uint8_t signature[64])
{
	struct identity_entry *entry;
	struct anx_identity_view view;
	uint8_t digest[32];
	bool flags;
	int ret;
	if (!signature || anx_identity_digest(next, digest) != ANX_OK)
		return ANX_EINVAL;
	ret = begin_write();
	if (ret != ANX_OK)
		return ret;
	entry = find_entry(&next->identity_id);
	if (!entry) {
		ret = ANX_ENOENT;
		goto out;
	}
	if (entry->current.commitment.generation == ~(uint64_t)0 ||
	    next->generation != entry->current.commitment.generation + 1 ||
	    anx_memcmp(next->previous_digest, entry->current.digest, 32)) {
		ret = ANX_EBUSY;
		goto out;
	}
	if (anx_memcmp(next->operator_key, entry->current.commitment.operator_key, 32) ||
	    anx_ed25519_verify(signature, digest, 32, entry->current.commitment.operator_key)) {
		ret = ANX_EPERM;
		goto out;
	}
	ret = make_record(next, signature, &entry->current.record_oid, &view);
	if (ret == ANX_OK) {
		anx_spin_lock_irqsave(&identity_lock, &flags);
		entry->current = view;
		anx_spin_unlock_irqrestore(&identity_lock, flags);
	}
out:
	end_write();
	return ret;
}

int anx_identity_get(const anx_oid_t *identity_id, struct anx_identity_view *out)
{
	struct identity_entry *entry;
	bool flags;
	if (!identity_id || anx_uuid_is_nil(identity_id) || !out)
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&identity_lock, &flags);
	entry = find_entry(identity_id);
	if (entry)
		*out = entry->current;
	anx_spin_unlock_irqrestore(&identity_lock, flags);
	return entry ? ANX_OK : ANX_ENOENT;
}

int anx_identity_bind(struct anx_cell *cell, const anx_oid_t *identity_id)
{
	struct anx_identity_view view;
	struct anx_cell *registered;
	int ret;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!cell)
		return ANX_EINVAL;
	ret = anx_identity_get(identity_id, &view);
	if (ret != ANX_OK)
		return ret;
	registered = anx_cell_store_lookup(&cell->cid);
	if (!registered)
		return ANX_ENOENT;
	if (registered != cell) {
		anx_cell_store_release(registered);
		return ANX_EPERM;
	}
	anx_spin_lock(&cell->lock);
	if (cell->status != ANX_CELL_CREATED || cell->runtime_active)
		ret = ANX_EBUSY;
	else
		cell->identity_id = *identity_id;
	anx_spin_unlock(&cell->lock);
	anx_cell_store_release(registered);
	return ret;
}

int anx_identity_admit(const struct anx_cell *cell, anx_oid_t *record_out)
{
	struct anx_identity_view view;
	uint32_t required = 0;
	int ret;
	if (record_out)
		*record_out = ANX_UUID_NIL;
	if (!cell)
		return ANX_EINVAL;
	if (anx_uuid_is_nil(&cell->identity_id))
		return ANX_OK;
	ret = anx_identity_get(&cell->identity_id, &view);
	if (ret != ANX_OK)
		return ret;
	if (record_out)
		*record_out = view.record_oid;
	if (cell->execution.allow_network)
		required |= ANX_CAP_AUTH_NETWORK;
	if (cell->execution.allow_remote_models)
		required |= ANX_CAP_AUTH_REMOTE_MODEL;
	if (cell->execution.allow_recursive_cells)
		required |= ANX_CAP_AUTH_DERIVE_CELL;
	if (cell->execution.allow_side_effects)
		required |= ANX_CAP_AUTH_SIDE_EFFECT;
	return required & ~view.commitment.authority ? ANX_EPERM : ANX_OK;
}
