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

int anx_object_commit(struct anx_object_handle *handle)
{
	struct anx_state_object *obj;
	struct anx_staged_mutation *stage;
	struct anx_prov_event ev;
	int ret = writable_handle(handle);

	if (ret != ANX_OK) return ret;

	obj = handle->obj;

	anx_spin_lock(&obj->lock);

	if (!obj->staged) {
		anx_spin_unlock(&obj->lock);
		return ANX_EINVAL;
	}

	stage = obj->staged;
	ret = commit_authority(obj);
	if (ret == ANX_OK && (obj->state == ANX_OBJ_SEALED || obj->state == ANX_OBJ_DELETED ||
	    obj->state == ANX_OBJ_TOMBSTONE)) ret = ANX_EPERM;
	if (ret == ANX_OK && obj->version != stage->base_version) ret = ANX_EBUSY;
	if (ret == ANX_OK && obj->version == ~(uint64_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK) ret = anx_epistemic_stage_check(obj);
	if (ret != ANX_OK) {
		anx_spin_unlock(&obj->lock);
		return ret;
	}

	if (obj->payload)
		anx_free(obj->payload);
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

	anx_spin_unlock(&obj->lock);
	return ANX_OK;
}

int anx_object_abort(struct anx_object_handle *handle)
{
	struct anx_state_object *obj;
	struct anx_staged_mutation *stage;
	struct anx_prov_event ev;
	int ret = writable_handle(handle);

	if (ret != ANX_OK) return ret;

	obj = handle->obj;

	anx_spin_lock(&obj->lock);

	if (!obj->staged) {
		anx_spin_unlock(&obj->lock);
		return ANX_EINVAL;
	}

	stage = obj->staged;
	ret = anx_object_stage_check_writer(obj);
	if (ret != ANX_OK) {
		anx_spin_unlock(&obj->lock);
		return ret;
	}

	anx_epistemic_stage_resolve(obj, false);
	if (stage->shadow_payload)
		anx_free(stage->shadow_payload);

	anx_memset(&ev, 0, sizeof(ev));
	ev.timestamp = arch_time_now();
	ev.event_type = ANX_PROV_STAGE_ABORTED;
	ev.actor_cell = stage->staging_cell;
	anx_prov_log_append(obj->provenance, &ev);

	anx_free(stage);
	obj->staged = NULL;

	anx_spin_unlock(&obj->lock);
	return ANX_OK;
}
