/*
 * flow.c — Information-flow labels for State Objects (RFC-0028).
 *
 * A capability grant answers "can this actor invoke this operation."
 * It says nothing about whether the data that operation would move is
 * safe to send where it's going. This file gives State Objects a
 * sensitivity classification, independent of the capability system,
 * so the two questions stay two separate checks (see anx_sink_check_send
 * in kernel/core/cap/sink.c for the other half).
 */

#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/uuid.h>

int anx_object_set_sensitivity(const anx_oid_t *oid, enum anx_sensitivity level)
{
	struct anx_state_object *obj;

	if (!oid)
		return ANX_EINVAL;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_ENOENT;

	anx_spin_lock(&obj->lock);
	obj->sensitivity = level;
	obj->sensitivity_origin = ANX_UUID_NIL;
	anx_spin_unlock(&obj->lock);

	anx_objstore_release(obj);
	return ANX_OK;
}

enum anx_sensitivity anx_object_get_sensitivity(const anx_oid_t *oid)
{
	struct anx_state_object *obj;
	enum anx_sensitivity level;

	if (!oid)
		return ANX_SENSITIVITY_PUBLIC;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_SENSITIVITY_PUBLIC;

	anx_spin_lock(&obj->lock);
	level = obj->sensitivity;
	anx_spin_unlock(&obj->lock);

	anx_objstore_release(obj);
	return level;
}
