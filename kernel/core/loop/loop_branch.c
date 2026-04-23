/*
 * loop_branch.c — Branch/merge scheduler for IBAL (RFC-0020 Phase 4).
 *
 * When the belief-action loop plateaus (no energy improvement for N
 * iterations), the supervisor spawns ANX_LOOP_MAX_BRANCHES child sessions
 * that each explore a different JEPA action seed.  After their budget runs
 * out, the child with the lowest final energy is merged back into the
 * parent session.
 *
 * A child branch is an ordinary loop session with:
 *   branch_depth  > 0  (prevents recursive branching)
 *   branch_from_oid    set to parent OID
 *
 * All locking follows the same pattern as loop.c: hold g_loop_lock
 * while reading or writing session fields, release before calling
 * functions that re-acquire it.
 */

#include <anx/loop.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include "loop_internal.h"

/* ------------------------------------------------------------------ */
/* anx_loop_branch_create                                              */
/* ------------------------------------------------------------------ */

int anx_loop_branch_create(anx_oid_t parent_oid, uint32_t branch_id,
			   uint32_t branch_max_iterations,
			   anx_oid_t *child_oid_out)
{
	struct anx_loop_session    *parent;
	struct anx_loop_session    *child;
	struct anx_loop_create_params child_params;
	char    world_uri[128];
	char    goal_text[512];
	float   halt_threshold;
	float   confidence_min;
	anx_oid_t capability_scope;
	anx_oid_t child_oid;
	int rc;

	if (!child_oid_out)
		return ANX_EINVAL;

	/* --- read parent config under lock --- */
	anx_spin_lock(&g_loop_lock);
	parent = anx_loop_session_get(parent_oid);
	if (!parent) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}
	if (parent->branch_child_count >= ANX_LOOP_MAX_BRANCHES) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_EFULL;
	}
	if (parent->branch_depth > 0) {
		/* Prevent recursive branching */
		anx_spin_unlock(&g_loop_lock);
		return ANX_EPERM;
	}

	anx_strlcpy(world_uri,   parent->world_uri,  sizeof(world_uri));
	anx_strlcpy(goal_text,   parent->goal_text,  sizeof(goal_text));
	halt_threshold   = parent->halt_threshold;
	confidence_min   = parent->confidence_min;
	capability_scope = parent->capability_scope;
	anx_spin_unlock(&g_loop_lock);

	/* --- create child session (acquires lock internally) --- */
	anx_memset(&child_params, 0, sizeof(child_params));
	anx_strlcpy(child_params.world_uri,  world_uri,  sizeof(child_params.world_uri));
	anx_strlcpy(child_params.goal_text,  goal_text,  sizeof(child_params.goal_text));
	child_params.max_iterations = branch_max_iterations;
	child_params.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;
	child_params.halt_threshold = halt_threshold;
	child_params.confidence_min = confidence_min;
	child_params.capability_scope = capability_scope;

	rc = anx_loop_session_create(&child_params, &child_oid);
	if (rc != ANX_OK)
		return rc;

	/* --- register child in parent, tag child with branch metadata --- */
	anx_spin_lock(&g_loop_lock);
	parent = anx_loop_session_get(parent_oid);	/* re-lookup */
	if (parent && parent->branch_child_count < ANX_LOOP_MAX_BRANCHES)
		parent->branch_child_oids[parent->branch_child_count++] = child_oid;

	child = anx_loop_session_get(child_oid);
	if (child) {
		child->branch_from_oid = parent_oid;
		child->branch_depth    = 1;
		child->branch_id       = branch_id;
	}
	anx_spin_unlock(&g_loop_lock);

	kprintf("[loop] branch %u created %016llx from parent %016llx\n",
		branch_id,
		(unsigned long long)child_oid.lo,
		(unsigned long long)parent_oid.lo);

	*child_oid_out = child_oid;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* anx_loop_branch_merge                                               */
/* ------------------------------------------------------------------ */

int anx_loop_branch_merge(anx_oid_t parent_oid, anx_oid_t child_oid)
{
	struct anx_loop_session    *parent;
	struct anx_loop_session    *child;
	struct anx_loop_iter_score *entry;
	float   child_energy;
	float   parent_energy;

	anx_spin_lock(&g_loop_lock);
	parent = anx_loop_session_get(parent_oid);
	child  = anx_loop_session_get(child_oid);

	if (!parent || !child) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	/* Nothing to merge if child ran zero iterations */
	if (child->score_hist_count == 0) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_OK;
	}
	child_energy = child->score_history[child->score_hist_count - 1].best_energy;

	parent_energy = (parent->score_hist_count > 0)
		? parent->score_history[parent->score_hist_count - 1].best_energy
		: 1.0f;

	if (child_energy < parent_energy) {
		/*
		 * Child found a better trajectory — adopt its best candidate
		 * and inject its energy into the parent's history ring so
		 * convergence detection sees the improvement.
		 */
		parent->best_candidate = child->best_candidate;

		if (parent->score_hist_count < ANX_LOOP_MAX_SCORE_HIST) {
			entry = &parent->score_history[parent->score_hist_count++];
			entry->iteration       = parent->iteration;
			entry->best_energy     = child_energy;
			entry->delta           = child_energy - parent_energy;
			entry->candidate_count = child->candidate_count;
			entry->best_candidate  = child->best_candidate;
		}

		kprintf("[loop] merge branch %016llx → parent %016llx"
			" energy %.4f→%.4f\n",
			(unsigned long long)child_oid.lo,
			(unsigned long long)parent_oid.lo,
			parent_energy, child_energy);
	}

	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* anx_loop_branch_list                                                */
/* ------------------------------------------------------------------ */

int anx_loop_branch_list(anx_oid_t parent_oid,
			 anx_oid_t *oids_out, uint32_t max_count,
			 uint32_t *count_out)
{
	struct anx_loop_session *parent;
	uint32_t i, n;

	if (!oids_out || !count_out)
		return ANX_EINVAL;

	anx_spin_lock(&g_loop_lock);
	parent = anx_loop_session_get(parent_oid);
	if (!parent) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	n = (parent->branch_child_count < max_count)
		? parent->branch_child_count : max_count;
	for (i = 0; i < n; i++)
		oids_out[i] = parent->branch_child_oids[i];
	*count_out = parent->branch_child_count;

	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}
