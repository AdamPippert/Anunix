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

/* Defined in objstore.c; shared the way anx_lifecycle_transition is. */
void anx_so_compute_content_hash(struct anx_state_object *obj);

int anx_object_stage(struct anx_object_handle *handle, anx_cid_t staging_cell)
{
	struct anx_state_object *obj;
	struct anx_staged_mutation *stage;

	if (!handle || !handle->obj)
		return ANX_EINVAL;
	if (handle->mode == ANX_OPEN_READ)
		return ANX_EINVAL;

	obj = handle->obj;

	anx_spin_lock(&obj->lock);

	if (obj->state == ANX_OBJ_SEALED) {
		anx_spin_unlock(&obj->lock);
		return ANX_EPERM;
	}
	if (obj->staged) {
		anx_spin_unlock(&obj->lock);
		return ANX_EBUSY;
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

	if (!handle || !handle->obj)
		return ANX_EINVAL;

	obj = handle->obj;

	anx_spin_lock(&obj->lock);

	if (!obj->staged) {
		anx_spin_unlock(&obj->lock);
		return ANX_EINVAL;
	}

	stage = obj->staged;

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

	if (!handle || !handle->obj)
		return ANX_EINVAL;

	obj = handle->obj;

	anx_spin_lock(&obj->lock);

	if (!obj->staged) {
		anx_spin_unlock(&obj->lock);
		return ANX_EINVAL;
	}

	stage = obj->staged;

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
