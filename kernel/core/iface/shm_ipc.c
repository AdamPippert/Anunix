/*
 * shm_ipc.c — Shared-memory IPC v0 for Interface Plane.
 *
 * Deterministic fixed-pool shared buffers with explicit capability grants.
 */

#include <anx/interface_plane.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct anx_iface_shm_grant {
	anx_cid_t cid;
	uint32_t rights;
	bool active;
};

#define ANX_IFACE_SHM_GRANTS_MAX 8u

struct anx_iface_shm_slot {
	anx_oid_t oid;
	anx_cid_t owner_cid;
	uint8_t *bytes;
	uint32_t size_bytes;
	uint32_t payload_len;
	uint32_t sequence;
	bool in_use;
	struct anx_iface_shm_grant grants[ANX_IFACE_SHM_GRANTS_MAX];
};

static struct anx_iface_shm_slot shm_pool[ANX_IFACE_SHM_MAX];
static struct anx_spinlock shm_lock;

static struct anx_iface_shm_slot *shm_find_slot(anx_oid_t oid)
{
	uint32_t i;

	for (i = 0; i < ANX_IFACE_SHM_MAX; i++) {
		if (!shm_pool[i].in_use)
			continue;
		if (anx_uuid_compare(&shm_pool[i].oid, &oid) == 0)
			return &shm_pool[i];
	}
	return NULL;
}

static bool shm_has_right(struct anx_iface_shm_slot *slot, anx_cid_t cid,
			  uint32_t required)
{
	uint32_t i;

	if (anx_uuid_compare(&slot->owner_cid, &cid) == 0)
		return true;

	for (i = 0; i < ANX_IFACE_SHM_GRANTS_MAX; i++) {
		if (!slot->grants[i].active)
			continue;
		if (anx_uuid_compare(&slot->grants[i].cid, &cid) != 0)
			continue;
		if ((slot->grants[i].rights & required) == required)
			return true;
	}

	return false;
}

static void __attribute__((constructor)) anx_iface_shm_init(void)
{
	anx_spin_init(&shm_lock);
	anx_memset(shm_pool, 0, sizeof(shm_pool));
}

int anx_iface_shm_create(anx_cid_t owner_cid, uint32_t size_bytes, anx_oid_t *out_oid)
{
	uint32_t i;
	bool flags;

	if (!out_oid)
		return ANX_EINVAL;
	if (size_bytes == 0 || size_bytes > ANX_IFACE_SHM_MAX_BYTES)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&shm_lock, &flags);
	for (i = 0; i < ANX_IFACE_SHM_MAX; i++) {
		struct anx_iface_shm_slot *slot;

		if (shm_pool[i].in_use)
			continue;

		slot = &shm_pool[i];
		anx_memset(slot, 0, sizeof(*slot));
		slot->bytes = anx_zalloc(size_bytes);
		if (!slot->bytes) {
			anx_spin_unlock_irqrestore(&shm_lock, flags);
			return ANX_ENOMEM;
		}
		anx_uuid_generate(&slot->oid);
		slot->owner_cid = owner_cid;
		slot->size_bytes = size_bytes;
		slot->payload_len = 0;
		slot->sequence = 0;
		slot->in_use = true;

		*out_oid = slot->oid;
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_OK;
	}
	anx_spin_unlock_irqrestore(&shm_lock, flags);
	return ANX_EFULL;
}

int anx_iface_shm_grant(anx_oid_t shm_oid, anx_cid_t owner_cid,
			anx_cid_t grantee_cid, uint32_t rights_mask)
{
	struct anx_iface_shm_slot *slot;
	uint32_t i;
	bool flags;

	if ((rights_mask & (ANX_IFACE_SHM_RIGHT_PRODUCE | ANX_IFACE_SHM_RIGHT_CONSUME)) == 0)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&shm_lock, &flags);
	slot = shm_find_slot(shm_oid);
	if (!slot) {
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_ENOENT;
	}
	if (anx_uuid_compare(&slot->owner_cid, &owner_cid) != 0) {
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_EPERM;
	}

	for (i = 0; i < ANX_IFACE_SHM_GRANTS_MAX; i++) {
		if (!slot->grants[i].active)
			continue;
		if (anx_uuid_compare(&slot->grants[i].cid, &grantee_cid) == 0) {
			slot->grants[i].rights = rights_mask;
			anx_spin_unlock_irqrestore(&shm_lock, flags);
			return ANX_OK;
		}
	}

	for (i = 0; i < ANX_IFACE_SHM_GRANTS_MAX; i++) {
		if (slot->grants[i].active)
			continue;
		slot->grants[i].active = true;
		slot->grants[i].cid = grantee_cid;
		slot->grants[i].rights = rights_mask;
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_OK;
	}

	anx_spin_unlock_irqrestore(&shm_lock, flags);
	return ANX_EFULL;
}

int anx_iface_shm_map(anx_oid_t shm_oid, anx_cid_t cell_cid, uint32_t required_right,
		      void **out_ptr, uint32_t *out_size, uint32_t *out_sequence)
{
	struct anx_iface_shm_slot *slot;
	bool flags;

	if (!out_ptr || !out_size || !out_sequence)
		return ANX_EINVAL;
	if (required_right == 0)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&shm_lock, &flags);
	slot = shm_find_slot(shm_oid);
	if (!slot) {
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_ENOENT;
	}
	if (!shm_has_right(slot, cell_cid, required_right)) {
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_EPERM;
	}

	*out_ptr = slot->bytes;
	*out_size = slot->size_bytes;
	*out_sequence = slot->sequence;
	anx_spin_unlock_irqrestore(&shm_lock, flags);
	return ANX_OK;
}

int anx_iface_shm_publish(anx_oid_t shm_oid, anx_cid_t producer_cid,
			  const void *data, uint32_t len, uint32_t *out_sequence)
{
	struct anx_iface_shm_slot *slot;
	bool flags;

	if (!data || len == 0)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&shm_lock, &flags);
	slot = shm_find_slot(shm_oid);
	if (!slot) {
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_ENOENT;
	}
	if (!shm_has_right(slot, producer_cid, ANX_IFACE_SHM_RIGHT_PRODUCE)) {
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_EPERM;
	}
	if (len > slot->size_bytes) {
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_EFULL;
	}

	anx_memcpy(slot->bytes, data, len);
	slot->payload_len = len;
	slot->sequence++;
	if (out_sequence)
		*out_sequence = slot->sequence;

	anx_spin_unlock_irqrestore(&shm_lock, flags);
	return ANX_OK;
}

int anx_iface_shm_consume(anx_oid_t shm_oid, anx_cid_t consumer_cid,
			  void *out, uint32_t out_max,
			  uint32_t *out_len, uint32_t *out_sequence)
{
	struct anx_iface_shm_slot *slot;
	bool flags;

	if (!out || !out_len || !out_sequence)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&shm_lock, &flags);
	slot = shm_find_slot(shm_oid);
	if (!slot) {
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_ENOENT;
	}
	if (!shm_has_right(slot, consumer_cid, ANX_IFACE_SHM_RIGHT_CONSUME)) {
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_EPERM;
	}
	if (slot->payload_len > out_max) {
		anx_spin_unlock_irqrestore(&shm_lock, flags);
		return ANX_EFULL;
	}

	anx_memcpy(out, slot->bytes, slot->payload_len);
	*out_len = slot->payload_len;
	*out_sequence = slot->sequence;
	anx_spin_unlock_irqrestore(&shm_lock, flags);
	return ANX_OK;
}
