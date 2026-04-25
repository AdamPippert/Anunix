/*
 * loop_branch.c — Phase 4 branch/merge scheduler (RFC-0020).
 *
 * Creates child branch sessions that inherit the parent's world_uri,
 * goal_text, and capability_scope.  After a branch runs its budget,
 * anx_loop_branch_merge() adopts the branch result if it is strictly
 * better than the parent's current best.
 */

#include <anx/loop.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include "loop_internal.h"

/* ------------------------------------------------------------------ */
/* anx_loop_branch_create                                              */
/* ------------------------------------------------------------------ */

int anx_loop_branch_create(anx_oid_t parent_oid, uint32_t branch_id,
			   uint32_t max_iterations,
			   anx_oid_t *branch_oid_out)
{
	struct anx_loop_session *parent;
	struct anx_loop_create_params params;
	int rc;

	if (!branch_oid_out)
		return ANX_EINVAL;

	anx_spin_lock(&g_loop_lock);
	parent = anx_loop_session_get(parent_oid);
	if (!parent) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	if (parent->branch_child_count >= ANX_LOOP_MAX_BRANCHES) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOMEM;
	}

	/* Copy parent config into create params */
	anx_memset(&params, 0, sizeof(params));
	anx_strlcpy(params.world_uri, parent->world_uri, sizeof(params.world_uri));
	anx_strlcpy(params.goal_text, parent->goal_text, sizeof(params.goal_text));
	params.halt_policy      = ANX_LOOP_HALT_ON_BUDGET;
	params.halt_threshold   = parent->halt_threshold;
	params.confidence_min   = parent->confidence_min;
	params.capability_scope = parent->capability_scope;
	params.branch_budget    = 1;
	params.max_iterations   = (max_iterations > 0) ? max_iterations : 4;
	params.parent_task_oid  = parent->parent_task_oid;

	anx_spin_unlock(&g_loop_lock);

	/* Create the child session (uses lock internally) */
	rc = anx_loop_session_create(&params, branch_oid_out);
	if (rc != ANX_OK)
		return rc;

	/* Wire up parent/child relationship */
	anx_spin_lock(&g_loop_lock);

	/* Re-fetch parent (lock was dropped during create) */
	parent = anx_loop_session_get(parent_oid);
	if (parent && parent->branch_child_count < ANX_LOOP_MAX_BRANCHES) {
		parent->branch_child_oids[parent->branch_child_count++] = *branch_oid_out;
	}

	/* Set branch metadata on the child */
	{
		struct anx_loop_session *child = anx_loop_session_get(*branch_oid_out);
		if (child) {
			child->branch_from_oid = parent_oid;
			child->branch_depth    = (parent ? parent->branch_depth + 1 : 1);
			child->branch_id       = branch_id;
		}
	}

	anx_spin_unlock(&g_loop_lock);

	kprintf("[branch] created %016llx parent=%016llx id=%u max_iter=%u\n",
		(unsigned long long)branch_oid_out->lo,
		(unsigned long long)parent_oid.lo,
		branch_id, params.max_iterations);

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* anx_loop_branch_merge                                               */
/* ------------------------------------------------------------------ */

int anx_loop_branch_merge(anx_oid_t parent_oid, anx_oid_t branch_oid)
{
	struct anx_loop_session      *parent;
	struct anx_loop_session      *branch;
	struct anx_loop_session_info  binfo;
	float                         parent_energy;
	int rc;

	/* Read branch info without holding the lock (status_get acquires it) */
	rc = anx_loop_session_status_get(branch_oid, &binfo);
	if (rc != ANX_OK)
		return rc;

	anx_spin_lock(&g_loop_lock);

	parent = anx_loop_session_get(parent_oid);
	branch = anx_loop_session_get(branch_oid);

	if (!parent || !branch) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	/* Determine parent's current best energy */
	if (parent->score_hist_count > 0)
		parent_energy = parent->score_history[parent->score_hist_count - 1].best_energy;
	else
		parent_energy = 1.0f;

	/* Adopt branch result only when strictly better */
	if (binfo.last_best_energy < parent_energy &&
	    branch->score_hist_count > 0) {
		struct anx_loop_iter_score *entry;

		parent->best_candidate = branch->best_candidate;

		/* Record the improvement in the parent's score history */
		if (parent->score_hist_count < ANX_LOOP_MAX_SCORE_HIST) {
			entry = &parent->score_history[parent->score_hist_count++];
			entry->iteration       = parent->iteration;
			entry->best_energy     = binfo.last_best_energy;
			entry->delta           = binfo.last_best_energy - parent_energy;
			entry->candidate_count = 0;
			entry->best_candidate  = branch->best_candidate;
		}

		kprintf("[branch] merged %016llx→%016llx energy %.4f→%.4f\n",
			(unsigned long long)branch_oid.lo,
			(unsigned long long)parent_oid.lo,
			parent_energy, binfo.last_best_energy);
	} else {
		kprintf("[branch] merge %016llx skipped (branch=%.4f parent=%.4f)\n",
			(unsigned long long)branch_oid.lo,
			binfo.last_best_energy, parent_energy);
	}

	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* anx_loop_branch_list                                                */
/* ------------------------------------------------------------------ */

int anx_loop_branch_list(anx_oid_t parent_oid, anx_oid_t *oids_out,
			 uint32_t max_oids, uint32_t *found_out)
{
	struct anx_loop_session *parent;
	uint32_t n;

	if (!oids_out || !found_out)
		return ANX_EINVAL;

	*found_out = 0;

	anx_spin_lock(&g_loop_lock);
	parent = anx_loop_session_get(parent_oid);
	if (!parent) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	n = (parent->branch_child_count < max_oids)
		? parent->branch_child_count : max_oids;
	anx_memcpy(oids_out, parent->branch_child_oids, n * sizeof(anx_oid_t));
	*found_out = parent->branch_child_count;

	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}
