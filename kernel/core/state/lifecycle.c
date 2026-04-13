/*
 * lifecycle.c — State Object lifecycle state machine.
 *
 * Enforces the valid transitions defined in RFC-0002 Section 10.1.
 */

#include <anx/types.h>
#include <anx/state_object.h>

/* Transition table: valid[from][to] */
static const bool valid_transitions[][6] = {
	/* from CREATING: */ { false, true, false, false, false, false },
	/* from ACTIVE:   */ { false, false, true, false, true, false },
	/* from SEALED:   */ { false, false, false, true, true, false },
	/* from EXPIRED:  */ { false, false, false, false, true, false },
	/* from DELETED:  */ { false, false, false, false, false, true },
	/* from TOMBSTONE:*/ { false, false, false, false, false, false },
};

int anx_lifecycle_transition(struct anx_state_object *obj,
			     enum anx_object_state new_state)
{
	enum anx_object_state old_state = obj->state;

	if ((int)old_state < 0 || old_state >= ANX_OBJ_TOMBSTONE + 1)
		return ANX_EINVAL;
	if ((int)new_state < 0 || new_state >= ANX_OBJ_TOMBSTONE + 1)
		return ANX_EINVAL;
	if (!valid_transitions[old_state][new_state])
		return ANX_EINVAL;

	obj->state = new_state;
	return ANX_OK;
}
