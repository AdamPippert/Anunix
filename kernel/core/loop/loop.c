/*
 * loop.c — Iterative Belief-Action Loop session registry (RFC-0020).
 *
 * Phase 1: session CRUD, iteration advance, halting, and introspection.
 * Static flat-array registry (pattern mirrors workflow_object.c).
 * EBM and JEPA integration deferred to Phase 2+.
 */

#include <anx/loop.h>
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/jepa.h>
#include "loop_internal.h"

/* ------------------------------------------------------------------ */
/* Global state (shared with loop_shell.c via loop_internal.h)        */
/* ------------------------------------------------------------------ */

struct anx_loop_session  g_loop_sessions[ANX_LOOP_MAX_SESSIONS];
struct anx_spinlock      g_loop_lock;
bool                     g_loop_initialized;

static uint64_t          g_loop_oid_seq;

/* ------------------------------------------------------------------ */
/* OID helpers                                                         */
/* ------------------------------------------------------------------ */

static anx_oid_t loop_oid_generate(void)
{
	anx_oid_t oid;

	oid.hi = 0x414e584c4f4f5053ULL;	/* "ANXLOOPS" */
	oid.lo = ++g_loop_oid_seq;
	return oid;
}

static bool loop_oid_eq(anx_oid_t a, anx_oid_t b)
{
	return a.hi == b.hi && a.lo == b.lo;
}

static bool loop_oid_zero(anx_oid_t a)
{
	return a.hi == 0 && a.lo == 0;
}

/* ------------------------------------------------------------------ */
/* Subsystem lifecycle                                                 */
/* ------------------------------------------------------------------ */

int anx_loop_init(void)
{
	anx_spin_init(&g_loop_lock);
	anx_memset(g_loop_sessions, 0, sizeof(g_loop_sessions));
	g_loop_oid_seq    = 0;
	g_loop_initialized = true;
	kprintf("[loop] session subsystem initialized (max %u sessions)\n",
		(uint32_t)ANX_LOOP_MAX_SESSIONS);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Internal lookup (caller must hold g_loop_lock if needed)           */
/* ------------------------------------------------------------------ */

struct anx_loop_session *anx_loop_session_get(anx_oid_t session_oid)
{
	uint32_t i;

	if (!g_loop_initialized || loop_oid_zero(session_oid))
		return NULL;

	for (i = 0; i < ANX_LOOP_MAX_SESSIONS; i++) {
		if (g_loop_sessions[i].in_use &&
		    loop_oid_eq(g_loop_sessions[i].session_oid, session_oid))
			return &g_loop_sessions[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Session CRUD                                                        */
/* ------------------------------------------------------------------ */

int anx_loop_session_create(const struct anx_loop_create_params *params,
			     anx_oid_t *session_oid_out)
{
	struct anx_loop_session *s;
	uint32_t i;

	if (!params || !session_oid_out)
		return ANX_EINVAL;

	anx_spin_lock(&g_loop_lock);

	/* Find a free slot; also reclaim committed/aborted sessions */
	s = NULL;
	for (i = 0; i < ANX_LOOP_MAX_SESSIONS; i++) {
		if (!g_loop_sessions[i].in_use) {
			s = &g_loop_sessions[i];
			break;
		}
		/* Reclaim terminal sessions that are no longer needed */
		if (g_loop_sessions[i].status == ANX_LOOP_COMMITTED ||
		    g_loop_sessions[i].status == ANX_LOOP_ABORTED  ||
		    g_loop_sessions[i].status == ANX_LOOP_HALTED) {
			g_loop_sessions[i].in_use = false;
			if (!s)
				s = &g_loop_sessions[i];
		}
	}
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOMEM;
	}

	anx_memset(s, 0, sizeof(*s));

	s->in_use           = true;
	s->session_oid      = loop_oid_generate();
	s->parent_task_oid  = params->parent_task_oid;
	s->halt_policy      = params->halt_policy;
	s->halt_threshold   = params->halt_threshold;
	s->confidence_min   = params->confidence_min;
	s->capability_scope = params->capability_scope;
	s->branch_budget    = (params->branch_budget > 0) ? params->branch_budget : 1;
	s->status           = ANX_LOOP_PENDING;
	s->iteration        = 0;

	s->max_iterations = (params->max_iterations > 0 &&
			     params->max_iterations <= ANX_LOOP_MAX_ITERATIONS)
			     ? params->max_iterations : ANX_LOOP_MAX_ITERATIONS;

	anx_strlcpy(s->world_uri,
		    (params->world_uri[0] != '\0') ? params->world_uri
						   : "anx:world/os-default",
		    sizeof(s->world_uri));

	anx_strlcpy(s->goal_text, params->goal_text, sizeof(s->goal_text));

	*session_oid_out = s->session_oid;

	anx_spin_unlock(&g_loop_lock);

	kprintf("[loop] session %016llx created world=%s max_iter=%u halt=%d goal=\"%.48s\"\n",
		(unsigned long long)s->session_oid.lo, s->world_uri,
		s->max_iterations, (int)s->halt_policy, s->goal_text);

	return ANX_OK;
}

int anx_loop_session_advance(anx_oid_t session_oid)
{
	struct anx_loop_session *s;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	if (s->status != ANX_LOOP_PENDING && s->status != ANX_LOOP_RUNNING) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_EPERM;
	}

	s->status = ANX_LOOP_RUNNING;
	s->iteration++;

	/* Clear per-iteration candidate list */
	anx_memset(s->candidates, 0,
		   sizeof(s->candidates[0]) * ANX_LOOP_MAX_CANDIDATES);
	s->candidate_count = 0;

	/* Budget-based auto-halt */
	if (s->halt_policy == ANX_LOOP_HALT_ON_BUDGET &&
	    s->iteration >= s->max_iterations) {
		s->status = ANX_LOOP_HALTED;
		kprintf("[loop] session %016llx halted on budget at iter %u\n",
			(unsigned long long)s->session_oid.lo, s->iteration);
	}

	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}

int anx_loop_session_halt(anx_oid_t session_oid)
{
	struct anx_loop_session *s;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}
	if (s->status == ANX_LOOP_COMMITTED || s->status == ANX_LOOP_ABORTED) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_EPERM;
	}

	s->status = ANX_LOOP_HALTED;
	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}

int anx_loop_session_abort(anx_oid_t session_oid)
{
	struct anx_loop_session *s;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}
	if (s->status == ANX_LOOP_COMMITTED) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_EPERM;
	}

	s->status = ANX_LOOP_ABORTED;
	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}

int anx_loop_session_commit(anx_oid_t session_oid, anx_oid_t plan_oid)
{
	struct anx_loop_session *s;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}
	if (s->status != ANX_LOOP_HALTED) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_EPERM;
	}

	s->best_candidate = plan_oid;
	s->status = ANX_LOOP_COMMITTED;
	anx_spin_unlock(&g_loop_lock);

	kprintf("[loop] session %016llx committed plan=%016llx after %u iter\n",
		(unsigned long long)s->session_oid.lo,
		(unsigned long long)plan_oid.lo,
		s->iteration);

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* State updates (called by loop cells)                               */
/* ------------------------------------------------------------------ */

int anx_loop_session_set_belief(anx_oid_t session_oid, anx_oid_t belief_oid)
{
	struct anx_loop_session *s;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}
	s->active_belief = belief_oid;
	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}

int anx_loop_session_add_candidate(anx_oid_t session_oid,
				   anx_oid_t candidate_oid)
{
	struct anx_loop_session *s;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}
	if (s->candidate_count >= ANX_LOOP_MAX_CANDIDATES) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOMEM;
	}
	s->candidates[s->candidate_count++] = candidate_oid;
	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}

int anx_loop_session_record_score(anx_oid_t session_oid, float best_energy,
				  anx_oid_t best_candidate_oid)
{
	struct anx_loop_session *s;
	struct anx_loop_iter_score *entry;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	s->best_candidate = best_candidate_oid;

	if (s->score_hist_count < ANX_LOOP_MAX_SCORE_HIST) {
		entry = &s->score_history[s->score_hist_count++];
		entry->iteration       = s->iteration;
		entry->best_energy     = best_energy;
		entry->candidate_count = s->candidate_count;
		entry->best_candidate  = best_candidate_oid;

		if (s->score_hist_count >= 2) {
			float prev = s->score_history[s->score_hist_count - 2].best_energy;
			entry->delta = best_energy - prev;
		} else {
			entry->delta = 0.0f;
		}

		/* Convergence-based auto-halt */
		if (s->halt_policy == ANX_LOOP_HALT_ON_CONVERGENCE &&
		    s->score_hist_count >= 2) {
			float d = entry->delta;
			if (d < 0.0f) d = -d;
			if (d < s->halt_threshold) {
				s->status = ANX_LOOP_HALTED;
				kprintf("[loop] session %016llx converged iter=%u delta=%.4f\n",
					(unsigned long long)s->session_oid.lo,
					s->iteration, entry->delta);

				/*
				 * Trigger a world model rebuild when the loop
				 * converges.  The model may have drifted; fresh
				 * training on recently accumulated traces keeps
				 * the JEPA predictions aligned with current
				 * system behaviour.  Non-fatal: failures are
				 * logged but do not block the halt.
				 */
				if (anx_jepa_available() &&
				    s->world_uri[0] != '\0') {
					anx_oid_t ckpt;
					int rrc = anx_jepa_world_rebuild(
						s->world_uri,
						64, &ckpt);

					if (rrc == ANX_OK)
						anx_jepa_world_activate(
							s->world_uri,
							&ckpt);
					else
						kprintf("[loop] world rebuild failed (%d)\n",
							rrc);
				}
			}
		}
	}

	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}

int anx_loop_session_apply_decision(anx_oid_t session_oid,
				    enum anx_loop_arb_decision decision)
{
	struct anx_loop_session *s;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	switch (decision) {
	case ANX_LOOP_ARB_CONTINUE:
		break;
	case ANX_LOOP_ARB_HALT:
		s->status = ANX_LOOP_HALTED;
		break;
	case ANX_LOOP_ARB_ABORT:
		s->status = ANX_LOOP_ABORTED;
		break;
	case ANX_LOOP_ARB_BRANCH:
		/* Phase 4: branch group scheduler. Treat as continue for now. */
		break;
	default:
		break;
	}

	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

int anx_loop_session_status_get(anx_oid_t session_oid,
				struct anx_loop_session_info *info_out)
{
	struct anx_loop_session *s;

	if (!info_out)
		return ANX_EINVAL;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	info_out->session_oid      = s->session_oid;
	info_out->status           = s->status;
	info_out->iteration        = s->iteration;
	info_out->max_iterations   = s->max_iterations;
	info_out->active_belief    = s->active_belief;
	info_out->best_candidate   = s->best_candidate;
	info_out->capability_scope = s->capability_scope;
	info_out->started_at_ns    = s->started_at_ns;
	info_out->halted_at_ns     = s->halted_at_ns;
	anx_strlcpy(info_out->goal_text, s->goal_text, sizeof(info_out->goal_text));

	if (s->score_hist_count > 0) {
		uint32_t last = s->score_hist_count - 1;
		info_out->last_best_energy = s->score_history[last].best_energy;
		info_out->last_delta       = s->score_history[last].delta;
	} else {
		info_out->last_best_energy = 0.0f;
		info_out->last_delta       = 0.0f;
	}

	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}

int anx_loop_session_candidates(anx_oid_t session_oid,
				anx_oid_t *oids_out, uint32_t max_oids,
				uint32_t *found_out)
{
	struct anx_loop_session *s;
	uint32_t n;

	if (!oids_out || !found_out)
		return ANX_EINVAL;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	n = (s->candidate_count < max_oids) ? s->candidate_count : max_oids;
	anx_memcpy(oids_out, s->candidates, n * sizeof(anx_oid_t));
	*found_out = s->candidate_count;

	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}

int anx_loop_session_score_history(anx_oid_t session_oid,
				   struct anx_loop_iter_score *out,
				   uint32_t max_entries, uint32_t *found_out)
{
	struct anx_loop_session *s;
	uint32_t n;

	if (!out || !found_out)
		return ANX_EINVAL;

	anx_spin_lock(&g_loop_lock);
	s = anx_loop_session_get(session_oid);
	if (!s) {
		anx_spin_unlock(&g_loop_lock);
		return ANX_ENOENT;
	}

	n = (s->score_hist_count < max_entries) ? s->score_hist_count : max_entries;
	anx_memcpy(out, s->score_history, n * sizeof(struct anx_loop_iter_score));
	*found_out = s->score_hist_count;

	anx_spin_unlock(&g_loop_lock);
	return ANX_OK;
}
