/*
 * stage.c — Staged State Object mutation (RFC-0002/RFC-0003 extension:
 * Execution Contracts).
 *
 * Gives a handle a way to accumulate a pending payload mutation
 * against a private shadow copy, then atomically commit or abort it,
 * instead of always writing the live payload in place. An aborted
 * stage is discarded without ever having been visible to readers, but
 * the attempt itself is still recorded in the provenance log — this
 * mirrors YoloFS's "rollback != erase evidence" principle from the
 * research this extension is drawn from.
 */

#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/arch.h>
#include <anx/cell.h>
#include <anx/identity.h>
#include <anx/effect_fence.h>
#include <anx/uuid.h>
#include <anx/branch_group.h>
#include <anx/epistemic.h>

/* Defined in objstore.c; shared the way anx_lifecycle_transition is. */
void anx_so_compute_content_hash(struct anx_state_object *obj);

/* Caller holds the object lock. Trusted control can resolve abandoned stages. */
int anx_object_stage_check_writer(const struct anx_state_object *obj)
{
	const anx_cid_t *active = anx_cell_current_id();
	if (obj->staged && active && anx_uuid_compare(active, &obj->staged->staging_cell))
		return ANX_EPERM;
	return ANX_OK;
}

static int writable_handle(const struct anx_object_handle *handle)
{
	if (!handle || !handle->obj) return ANX_EINVAL;
	if (handle->mode == ANX_OPEN_READ) return ANX_EPERM;
	return handle->mode == ANX_OPEN_WRITE || handle->mode == ANX_OPEN_READWRITE ? ANX_OK : ANX_EINVAL;
}

/* Preparation grants no publication authority; recheck the original actor now. */
static int commit_authority(struct anx_state_object *obj)
{
	struct anx_cell *owner;
	const anx_cid_t *actor = &obj->staged->staging_cell;
	int ret = anx_object_stage_check_writer(obj);
	if (ret != ANX_OK) return ret;
	ret = anx_access_evaluate(&obj->access_policy, actor, &obj->creator_cell, ANX_ACCESS_WRITE_PAYLOAD);
	if (ret != ANX_OK || anx_uuid_is_nil(actor)) return ret;
	owner = anx_cell_store_lookup(actor);
	if (!owner) return ANX_ENOENT;
	ret = owner->execution.allow_side_effects && !anx_cell_status_terminal(owner->status) ? ANX_OK : ANX_EPERM;
	if (ret == ANX_OK) ret = anx_identity_admit(owner, NULL);
	if (ret == ANX_OK) ret = anx_effect_fence_check(owner, NULL, NULL);
	if (ret == ANX_OK) ret = anx_branch_group_effect_check(actor);
	anx_cell_store_release(owner);
	return ret;
}

int anx_object_stage(struct anx_object_handle *handle, anx_cid_t staging_cell)
{
	const anx_cid_t *active = anx_cell_current_id();
	struct anx_state_object *obj;
	struct anx_staged_mutation *stage;
	int ret;

	if (!handle || !handle->obj)
		return ANX_EINVAL;
	if (handle->mode == ANX_OPEN_READ)
		return ANX_EINVAL;
	ret = writable_handle(handle);
	if (ret != ANX_OK) return ret;
	if (active && anx_uuid_compare(active, &staging_cell)) return ANX_EPERM;

	obj = handle->obj;

	anx_spin_lock(&obj->lock);

	if (obj->state == ANX_OBJ_SEALED || obj->state == ANX_OBJ_DELETED || obj->state == ANX_OBJ_TOMBSTONE) {
		anx_spin_unlock(&obj->lock);
		return ANX_EPERM;
	}
	if (obj->staged) {
		anx_spin_unlock(&obj->lock);
		return ANX_EBUSY;
	}
	ret = anx_access_evaluate(&obj->access_policy, &staging_cell, &obj->creator_cell, ANX_ACCESS_WRITE_PAYLOAD);
	if (ret != ANX_OK) {
		anx_spin_unlock(&obj->lock);
		return ret;
	}

	stage = anx_zalloc(sizeof(*stage));
	if (!stage) {
		anx_spin_unlock(&obj->lock);
		return ANX_ENOMEM;
	}

	if (obj->payload && obj->payload_size > 0) {
		stage->shadow_payload = anx_alloc(obj->payload_size);
		if (!stage->shadow_payload) {
			anx_free(stage);
			anx_spin_unlock(&obj->lock);
			return ANX_ENOMEM;
		}
		anx_memcpy(stage->shadow_payload, obj->payload,
			   obj->payload_size);
		stage->shadow_size = obj->payload_size;
	}

	stage->staging_cell = staging_cell;
	stage->base_version = obj->version;

	obj->staged = stage;

	anx_spin_unlock(&obj->lock);
	return ANX_OK;
}

/* Object locks remain held from validation through publication. */
static int commit_check(struct anx_state_object *obj)
{
	if (!obj->staged) return ANX_EINVAL;
	int ret = commit_authority(obj);
	if (ret == ANX_OK && (obj->state == ANX_OBJ_SEALED || obj->state == ANX_OBJ_DELETED ||
	    obj->state == ANX_OBJ_TOMBSTONE)) ret = ANX_EPERM;
	if (ret == ANX_OK && obj->version != obj->staged->base_version) ret = ANX_EBUSY;
	if (ret == ANX_OK && obj->version == ~(uint64_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK) ret = anx_epistemic_stage_check(obj);
	return ret;
}
static void commit_publish(struct anx_state_object *obj)
{
	struct anx_staged_mutation *stage = obj->staged;
	struct anx_prov_event ev;
	if (obj->payload) anx_free(obj->payload);
	obj->payload = stage->shadow_payload;
	obj->payload_size = stage->shadow_size;
	obj->version++;
	anx_so_compute_content_hash(obj);
	anx_memset(&ev, 0, sizeof(ev));
	ev.timestamp = arch_time_now();
	ev.event_type = ANX_PROV_MUTATED;
	ev.actor_cell = stage->staging_cell;
	anx_prov_log_append(obj->provenance, &ev);
	anx_epistemic_stage_resolve(obj, true);
	anx_free(stage);
	obj->staged = NULL;
}
static int abort_check(struct anx_state_object *obj)
{
	return obj->staged ? anx_object_stage_check_writer(obj) : ANX_EINVAL;
}
static void abort_discard(struct anx_state_object *obj)
{
	struct anx_staged_mutation *stage = obj->staged;
	struct anx_prov_event ev;
	anx_epistemic_stage_resolve(obj, false);
	if (stage->shadow_payload) anx_free(stage->shadow_payload);
	anx_memset(&ev, 0, sizeof(ev));
	ev.timestamp = arch_time_now();
	ev.event_type = ANX_PROV_STAGE_ABORTED;
	ev.actor_cell = stage->staging_cell;
	anx_prov_log_append(obj->provenance, &ev);
	anx_free(stage);
	obj->staged = NULL;
}
int anx_object_commit(struct anx_object_handle *handle)
{
	int ret = writable_handle(handle);
	if (ret != ANX_OK) return ret;
	struct anx_state_object *obj = handle->obj;
	anx_spin_lock(&obj->lock);
	ret = commit_check(obj);
	if (ret == ANX_OK) commit_publish(obj);
	anx_spin_unlock(&obj->lock);
	return ret;
}
int anx_object_abort(struct anx_object_handle *handle)
{
	int ret = writable_handle(handle);
	if (ret != ANX_OK) return ret;
	struct anx_state_object *obj = handle->obj;
	anx_spin_lock(&obj->lock);
	ret = abort_check(obj);
	if (ret == ANX_OK) abort_discard(obj);
	anx_spin_unlock(&obj->lock);
	return ret;
}
static int resolve_batch(struct anx_object_handle *const *handles, uint32_t count, bool commit)
{
	if (!handles || !count || count > ANX_STAGE_BATCH_MAX) return ANX_EINVAL;
	struct anx_state_object *objects[ANX_STAGE_BATCH_MAX];
	for (uint32_t i = 0; i < count; i++) {
		int ret = writable_handle(handles[i]);
		if (ret != ANX_OK) return ret;
		objects[i] = handles[i]->obj;
		for (uint32_t j = 0; j < i; j++)
			if (objects[i] == objects[j] || !anx_uuid_compare(&objects[i]->oid, &objects[j]->oid)) return ANX_EEXIST;
	}
	for (uint32_t i = 1; i < count; i++) {
		struct anx_state_object *object = objects[i];
		uint32_t j = i;
		while (j && anx_uuid_compare(&objects[j-1]->oid, &object->oid) > 0) { objects[j] = objects[j-1]; j--; }
		objects[j] = object;
	}
	bool flags;
	anx_spin_lock_irqsave(&objects[0]->lock, &flags);
	for (uint32_t i = 1; i < count; i++) anx_spin_lock(&objects[i]->lock);
	int ret = ANX_OK;
	for (uint32_t i = 0; i < count; i++) {
		if (!objects[i]->staged) { ret = ANX_EINVAL; break; }
		if (anx_uuid_is_nil(&objects[i]->staged->staging_cell) ||
		    anx_uuid_compare(&objects[0]->staged->staging_cell, &objects[i]->staged->staging_cell)) { ret = ANX_EPERM; break; }
	}
	for (uint32_t i = 0; ret == ANX_OK && i < count; i++)
		ret = commit ? commit_check(objects[i]) : abort_check(objects[i]);
	/* No fallible validation or external dispatch follows the first payload change. */
	if (ret == ANX_OK) for (uint32_t i = 0; i < count; i++) {
		if (commit) commit_publish(objects[i]); else abort_discard(objects[i]);
	}
	for (uint32_t i = count; i > 1; i--) anx_spin_unlock(&objects[i-1]->lock);
	anx_spin_unlock_irqrestore(&objects[0]->lock, flags);
	return ret;
}
int anx_object_commit_batch(struct anx_object_handle *const *handles, uint32_t count)
{
	return resolve_batch(handles, count, true);
}
int anx_object_abort_batch(struct anx_object_handle *const *handles, uint32_t count)
{
	return resolve_batch(handles, count, false);
}
