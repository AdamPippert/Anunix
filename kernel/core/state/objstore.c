/*
 * objstore.c — Global in-memory State Object store.
 *
 * Objects are stored in a hash table keyed by OID. A secondary
 * index on content_hash enables content-addressable lookups.
 */

#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/hashtable.h>
#include <anx/arch.h>
#include <anx/kprintf.h>
#include <anx/crypto.h>

int anx_lifecycle_transition(struct anx_state_object *obj,
			     enum anx_object_state new_state);

#define OID_HASH_BITS	10	/* 1024 buckets */

static struct anx_htable oid_table;

void anx_objstore_init(void)
{
	anx_htable_init(&oid_table, OID_HASH_BITS);
}

struct anx_state_object *anx_objstore_lookup(const anx_oid_t *oid)
{
	uint64_t hash = anx_uuid_hash(oid);
	struct anx_list_head *pos;

	ANX_HTABLE_FOR_BUCKET(pos, &oid_table, hash) {
		struct anx_state_object *obj =
			ANX_LIST_ENTRY(pos, struct anx_state_object, oid_link);
		if (anx_uuid_compare(&obj->oid, oid) == 0) {
			obj->refcount++;
			return obj;
		}
	}
	return NULL;
}

void anx_objstore_release(struct anx_state_object *obj)
{
	if (!obj)
		return;
	if (obj->refcount > 0)
		obj->refcount--;
}

int anx_objstore_iterate(anx_objstore_iter_fn cb, void *arg)
{
	uint32_t bucket;
	int ret;

	for (bucket = 0; bucket < (1u << OID_HASH_BITS); bucket++) {
		struct anx_list_head *pos;
		struct anx_list_head *head;

		head = anx_htable_bucket(&oid_table, bucket);
		ANX_LIST_FOR_EACH(pos, head) {
			struct anx_state_object *obj;

			obj = ANX_LIST_ENTRY(pos, struct anx_state_object,
					     oid_link);
			ret = cb(obj, arg);
			if (ret != 0)
				return ret;
		}
	}
	return ANX_OK;
}

static void objstore_add(struct anx_state_object *obj)
{
	uint64_t hash = anx_uuid_hash(&obj->oid);
	anx_htable_add(&oid_table, &obj->oid_link, hash);
}

static void objstore_remove(struct anx_state_object *obj)
{
	anx_htable_del(&oid_table, &obj->oid_link);
}

static void compute_content_hash(struct anx_state_object *obj)
{
	if (obj->payload && obj->payload_size > 0)
		anx_sha256(obj->payload, (uint32_t)obj->payload_size,
			   obj->content_hash.bytes);
	else
		anx_memset(&obj->content_hash, 0, sizeof(obj->content_hash));
}

static void record_created_event(struct anx_state_object *obj)
{
	struct anx_prov_event ev;

	anx_memset(&ev, 0, sizeof(ev));
	ev.timestamp = arch_time_now();
	ev.event_type = ANX_PROV_CREATED;
	ev.actor_cell = obj->creator_cell;
	ev.reproducible = false;

	anx_prov_log_append(obj->provenance, &ev);
}

int anx_so_create(const struct anx_so_create_params *params,
		  struct anx_state_object **out)
{
	if (!params || !out)
		return ANX_EINVAL;
	if (params->object_type >= ANX_OBJ_TYPE_COUNT)
		return ANX_EINVAL;

	/* Schema required for structured_data */
	if (params->object_type == ANX_OBJ_STRUCTURED_DATA &&
	    !params->schema_uri)
		return ANX_EINVAL;

	struct anx_state_object *obj = anx_zalloc(sizeof(*obj));
	if (!obj)
		return ANX_ENOMEM;

	/* Identity */
	anx_uuid_generate(&obj->oid);
	obj->version = 1;

	/* Type */
	obj->object_type = params->object_type;
	if (params->schema_uri)
		anx_strlcpy(obj->schema_uri, params->schema_uri,
			    sizeof(obj->schema_uri));
	if (params->schema_version)
		anx_strlcpy(obj->schema_version, params->schema_version,
			    sizeof(obj->schema_version));

	/* Payload */
	if (params->payload && params->payload_size > 0) {
		obj->payload = anx_alloc(params->payload_size);
		if (!obj->payload) {
			anx_free(obj);
			return ANX_ENOMEM;
		}
		anx_memcpy(obj->payload, params->payload, params->payload_size);
		obj->payload_size = params->payload_size;
	}

	/* Content hash */
	compute_content_hash(obj);

	/* Metadata */
	obj->system_meta = anx_meta_create();
	obj->user_meta = anx_meta_create();
	if (!obj->system_meta || !obj->user_meta)
		goto cleanup;

	/* System metadata */
	anx_meta_set_i64(obj->system_meta, "sys.created_at", arch_time_now());
	anx_meta_set_i64(obj->system_meta, "sys.size_bytes", obj->payload_size);
	anx_meta_set_bool(obj->system_meta, "sys.sealed", false);

	/* Provenance */
	obj->provenance = anx_prov_log_create();
	if (!obj->provenance)
		goto cleanup;

	/* Governance defaults */
	struct anx_retention_policy defaults = ANX_RETENTION_DEFAULT;
	obj->retention = defaults;

	/* Parents */
	obj->creator_cell = params->creator_cell;
	if (params->parent_oids && params->parent_count > 0) {
		obj->parent_oids = anx_alloc(params->parent_count * sizeof(anx_oid_t));
		if (!obj->parent_oids)
			goto cleanup;
		anx_memcpy(obj->parent_oids, params->parent_oids,
			   params->parent_count * sizeof(anx_oid_t));
		obj->parent_count = params->parent_count;
	}

	/* Lifecycle */
	obj->state = ANX_OBJ_CREATING;
	anx_spin_init(&obj->lock);
	obj->refcount = 1;
	anx_list_init(&obj->oid_link);
	anx_list_init(&obj->content_link);

	/* Provenance: CREATED event */
	record_created_event(obj);

	/* Transition to ACTIVE and register */
	anx_lifecycle_transition(obj, ANX_OBJ_ACTIVE);
	objstore_add(obj);

	*out = obj;
	return ANX_OK;

cleanup:
	if (obj->payload)
		anx_free(obj->payload);
	if (obj->system_meta)
		anx_meta_destroy(obj->system_meta);
	if (obj->user_meta)
		anx_meta_destroy(obj->user_meta);
	if (obj->provenance)
		anx_prov_log_destroy(obj->provenance);
	if (obj->parent_oids)
		anx_free(obj->parent_oids);
	anx_free(obj);
	return ANX_ENOMEM;
}

int anx_so_open(const anx_oid_t *oid, enum anx_open_mode mode,
		struct anx_object_handle *handle)
{
	struct anx_state_object *obj = anx_objstore_lookup(oid);

	if (!obj)
		return ANX_ENOENT;
	if (obj->state != ANX_OBJ_ACTIVE && obj->state != ANX_OBJ_SEALED)
		return ANX_EINVAL;
	if (mode != ANX_OPEN_READ && obj->state == ANX_OBJ_SEALED)
		return ANX_EPERM;

	handle->obj = obj;
	handle->mode = mode;
	handle->open_version = obj->version;
	return ANX_OK;
}

void anx_so_close(struct anx_object_handle *handle)
{
	if (handle && handle->obj) {
		anx_objstore_release(handle->obj);
		handle->obj = NULL;
	}
}

int anx_so_seal(const anx_oid_t *oid)
{
	struct anx_state_object *obj = anx_objstore_lookup(oid);

	if (!obj)
		return ANX_ENOENT;

	anx_spin_lock(&obj->lock);

	int ret = anx_lifecycle_transition(obj, ANX_OBJ_SEALED);
	if (ret == ANX_OK) {
		anx_meta_set_bool(obj->system_meta, "sys.sealed", true);

		struct anx_prov_event ev;
		anx_memset(&ev, 0, sizeof(ev));
		ev.timestamp = arch_time_now();
		ev.event_type = ANX_PROV_SEALED;
		anx_prov_log_append(obj->provenance, &ev);
	}

	anx_spin_unlock(&obj->lock);
	anx_objstore_release(obj);
	return ret;
}

int anx_so_delete(const anx_oid_t *oid, bool force)
{
	struct anx_state_object *obj = anx_objstore_lookup(oid);

	if (!obj)
		return ANX_ENOENT;

	anx_spin_lock(&obj->lock);

	if (obj->retention.deletion_hold && !force) {
		anx_spin_unlock(&obj->lock);
		anx_objstore_release(obj);
		return ANX_EPERM;
	}

	int ret = anx_lifecycle_transition(obj, ANX_OBJ_DELETED);
	if (ret == ANX_OK)
		objstore_remove(obj);

	anx_spin_unlock(&obj->lock);
	anx_objstore_release(obj);
	return ret;
}

int anx_so_read_payload(struct anx_object_handle *handle,
			uint64_t offset, void *buf, uint64_t len)
{
	struct anx_state_object *obj = handle->obj;

	if (!obj || !buf)
		return ANX_EINVAL;
	if (offset >= obj->payload_size)
		return 0;
	if (offset + len > obj->payload_size)
		len = obj->payload_size - offset;

	anx_memcpy(buf, (uint8_t *)obj->payload + offset, len);
	return (int)len;
}

int anx_so_write_payload(struct anx_object_handle *handle,
			 uint64_t offset, const void *data, uint64_t len)
{
	struct anx_state_object *obj = handle->obj;

	if (!obj || !data)
		return ANX_EINVAL;
	if (handle->mode == ANX_OPEN_READ)
		return ANX_EPERM;
	if (obj->state == ANX_OBJ_SEALED)
		return ANX_EPERM;

	anx_spin_lock(&obj->lock);

	/* Grow payload if needed */
	if (offset + len > obj->payload_size) {
		uint64_t new_size = offset + len;
		void *new_buf = anx_alloc(new_size);
		if (!new_buf) {
			anx_spin_unlock(&obj->lock);
			return ANX_ENOMEM;
		}
		if (obj->payload) {
			anx_memcpy(new_buf, obj->payload, obj->payload_size);
			anx_free(obj->payload);
		}
		obj->payload = new_buf;
		obj->payload_size = new_size;
	}

	anx_memcpy((uint8_t *)obj->payload + offset, data, len);
	obj->version++;
	compute_content_hash(obj);

	/* Record mutation */
	struct anx_prov_event ev;
	anx_memset(&ev, 0, sizeof(ev));
	ev.timestamp = arch_time_now();
	ev.event_type = ANX_PROV_MUTATED;
	anx_prov_log_append(obj->provenance, &ev);

	anx_spin_unlock(&obj->lock);
	return (int)len;
}

int anx_so_replace_payload(struct anx_object_handle *handle,
			   const void *data, uint64_t len)
{
	struct anx_state_object *obj = handle->obj;

	if (!obj)
		return ANX_EINVAL;
	if (handle->mode == ANX_OPEN_READ)
		return ANX_EPERM;
	if (obj->state == ANX_OBJ_SEALED)
		return ANX_EPERM;

	anx_spin_lock(&obj->lock);

	void *new_buf = NULL;
	if (data && len > 0) {
		new_buf = anx_alloc(len);
		if (!new_buf) {
			anx_spin_unlock(&obj->lock);
			return ANX_ENOMEM;
		}
		anx_memcpy(new_buf, data, len);
	}

	if (obj->payload)
		anx_free(obj->payload);
	obj->payload = new_buf;
	obj->payload_size = len;
	obj->version++;
	compute_content_hash(obj);

	struct anx_prov_event ev;
	anx_memset(&ev, 0, sizeof(ev));
	ev.timestamp = arch_time_now();
	ev.event_type = ANX_PROV_MUTATED;
	anx_prov_log_append(obj->provenance, &ev);

	anx_spin_unlock(&obj->lock);
	return ANX_OK;
}
