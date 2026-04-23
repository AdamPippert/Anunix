/*
 * jepa_mem.c — JEPA memory plane integration hook.
 *
 * anx_jepa_mem_utility_hint() returns a 0-100 utility score for a State
 * Object.  The memory control plane's decay sweep uses this to suppress
 * demotion of objects that JEPA predicts will be accessed again soon.
 *
 * Current heuristic (pre-trained):
 *   - JEPA_LATENT and JEPA_TRACE objects: always useful (score 80)
 *   - JEPA_OBS and JEPA_WORLD_PROFILE objects: retain (score 70)
 *   - All other objects: neutral (score 50)
 *
 * Once a trained model is present the hint can be computed from the
 * cosine similarity between the object's embedding (if it has one in
 * the memory plane) and the current predicted future latent.
 */

#include "jepa_internal.h"
#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/memplane.h>

uint32_t anx_jepa_mem_utility_hint(const anx_oid_t *obj_oid)
{
	struct anx_state_object *so;
	enum anx_object_type obj_type;
	uint32_t score;

	if (!obj_oid || !anx_jepa_available())
		return 50;	/* neutral */

	so = anx_objstore_lookup(obj_oid);
	if (!so)
		return 50;

	obj_type = so->object_type;
	anx_objstore_release(so);

	/* Type-based heuristic until trained model is available */
	switch (obj_type) {
	case ANX_OBJ_JEPA_LATENT:
	case ANX_OBJ_JEPA_TRACE:
		score = 80;	/* training data — high retention value */
		break;
	case ANX_OBJ_JEPA_OBS:
	case ANX_OBJ_JEPA_WORLD_PROFILE:
		score = 70;	/* operational data — retain */
		break;
	case ANX_OBJ_CAPABILITY:
		score = 75;	/* capabilities expensive to revalidate */
		break;
	case ANX_OBJ_TENSOR:
		score = 65;	/* weights worth keeping warm */
		break;
	default:
		score = 50;	/* neutral for all other types */
		break;
	}

	return score;
}
