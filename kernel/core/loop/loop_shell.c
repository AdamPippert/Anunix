/*
 * loop_shell.c — Shell command handler for IBAL loop commands (RFC-0020).
 *
 * Dispatches: loop create|step|status|halt|abort|commit|trace
 */

#include <anx/loop.h>
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include "loop_internal.h"

static const char *session_status_name(enum anx_loop_session_status s)
{
	switch (s) {
	case ANX_LOOP_PENDING:   return "pending";
	case ANX_LOOP_RUNNING:   return "running";
	case ANX_LOOP_HALTED:    return "halted";
	case ANX_LOOP_COMMITTED: return "committed";
	case ANX_LOOP_ABORTED:   return "aborted";
	default:                 return "unknown";
	}
}

static const char *halt_policy_name(enum anx_loop_halt_policy p)
{
	switch (p) {
	case ANX_LOOP_HALT_ON_CONVERGENCE: return "convergence";
	case ANX_LOOP_HALT_ON_BUDGET:      return "budget";
	case ANX_LOOP_HALT_ON_CONFIDENCE:  return "confidence";
	case ANX_LOOP_HALT_MANUAL:         return "manual";
	default:                           return "unknown";
	}
}

static void loop_usage(void)
{
	kprintf("usage: loop <subcommand> [args]\n");
	kprintf("  create [--world <uri>] [--goal <text>] [--max-iter N]\n");
	kprintf("         [--halt budget|convergence|confidence|manual]\n");
	kprintf("         [--threshold F] [--branches N]\n");
	kprintf("  step   <session-id>\n");
	kprintf("  status <session-id>\n");
	kprintf("  halt   <session-id>\n");
	kprintf("  abort  <session-id>\n");
	kprintf("  trace  <session-id>\n");
}

static anx_oid_t parse_session_oid(const char *s)
{
	anx_oid_t oid;

	oid.hi = 0x414e584c4f4f5053ULL;	/* "ANXLOOPS" */
	oid.lo = anx_strtoull(s, NULL, 16);
	return oid;
}

int anx_loop_shell_dispatch(int argc, const char *const *argv)
{
	anx_oid_t session_oid;
	int i, rc;

	if (argc < 2) {
		loop_usage();
		return 0;
	}

	/* ---- loop create ---- */
	if (anx_strcmp(argv[1], "create") == 0) {
		struct anx_loop_create_params params;
		anx_oid_t oid;

		anx_memset(&params, 0, sizeof(params));
		params.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;
		params.max_iterations = 16;
		params.branch_budget  = 1;

		for (i = 2; i < argc - 1; i++) {
			if (anx_strcmp(argv[i], "--world") == 0 && i + 1 < argc)
				anx_strlcpy(params.world_uri, argv[++i],
					    sizeof(params.world_uri));
			else if (anx_strcmp(argv[i], "--goal") == 0 && i + 1 < argc)
				anx_strlcpy(params.goal_text, argv[++i],
					    sizeof(params.goal_text));
			else if (anx_strcmp(argv[i], "--max-iter") == 0 && i + 1 < argc)
				params.max_iterations = (uint32_t)anx_strtoul(
					argv[++i], NULL, 10);
			else if (anx_strcmp(argv[i], "--halt") == 0 && i + 1 < argc) {
				i++;
				if (anx_strcmp(argv[i], "convergence") == 0)
					params.halt_policy = ANX_LOOP_HALT_ON_CONVERGENCE;
				else if (anx_strcmp(argv[i], "confidence") == 0)
					params.halt_policy = ANX_LOOP_HALT_ON_CONFIDENCE;
				else if (anx_strcmp(argv[i], "manual") == 0)
					params.halt_policy = ANX_LOOP_HALT_MANUAL;
				else
					params.halt_policy = ANX_LOOP_HALT_ON_BUDGET;
			} else if (anx_strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
				/* Simple float parse: integer part only for now */
				params.halt_threshold = (float)anx_strtoul(argv[++i], NULL, 10) / 1000.0f;
			} else if (anx_strcmp(argv[i], "--branches") == 0 && i + 1 < argc) {
				params.branch_budget = (uint32_t)anx_strtoul(argv[++i], NULL, 10);
			}
		}

		rc = anx_loop_session_create(&params, &oid);
		if (rc != ANX_OK) {
			kprintf("loop: create failed (%d)\n", rc);
			return rc;
		}
		kprintf("session: %016llx\n", (unsigned long long)oid.lo);
		kprintf("world:   %s\n", params.world_uri[0] ? params.world_uri : "anx:world/os-default");
		kprintf("halt:    %s (max %u iter)\n", halt_policy_name(params.halt_policy),
			params.max_iterations);
		return ANX_OK;
	}

	/* All remaining subcommands take a session-id */
	if (argc < 3) {
		loop_usage();
		return ANX_EINVAL;
	}

	session_oid = parse_session_oid(argv[2]);

	/* ---- loop step ---- */
	if (anx_strcmp(argv[1], "step") == 0) {
		rc = anx_loop_session_advance(session_oid);
		if (rc != ANX_OK) {
			kprintf("loop: step failed (%d)\n", rc);
			return rc;
		}
		/* Print new iteration count */
		{
			struct anx_loop_session_info info;
			if (anx_loop_session_status_get(session_oid, &info) == ANX_OK)
				kprintf("session %s: iter %u status=%s\n",
					argv[2], info.iteration,
					session_status_name(info.status));
		}
		return ANX_OK;
	}

	/* ---- loop status ---- */
	if (anx_strcmp(argv[1], "status") == 0) {
		struct anx_loop_session_info info;

		rc = anx_loop_session_status_get(session_oid, &info);
		if (rc != ANX_OK) {
			kprintf("loop: session not found\n");
			return rc;
		}
		kprintf("session  : %016llx\n",
			(unsigned long long)info.session_oid.lo);
		kprintf("status   : %s\n", session_status_name(info.status));
		kprintf("iter     : %u / %u\n",
			info.iteration, info.max_iterations);
		kprintf("energy   : %.4f  delta %.4f\n",
			info.last_best_energy, info.last_delta);
		kprintf("belief   : %016llx\n",
			(unsigned long long)info.active_belief.lo);
		kprintf("best     : %016llx\n",
			(unsigned long long)info.best_candidate.lo);
		if (info.goal_text[0])
			kprintf("goal     : %s\n", info.goal_text);
		return ANX_OK;
	}

	/* ---- loop halt ---- */
	if (anx_strcmp(argv[1], "halt") == 0) {
		rc = anx_loop_session_halt(session_oid);
		if (rc != ANX_OK)
			kprintf("loop: halt failed (%d)\n", rc);
		else
			kprintf("session %s halted\n", argv[2]);
		return rc;
	}

	/* ---- loop abort ---- */
	if (anx_strcmp(argv[1], "abort") == 0) {
		rc = anx_loop_session_abort(session_oid);
		if (rc != ANX_OK)
			kprintf("loop: abort failed (%d)\n", rc);
		else
			kprintf("session %s aborted\n", argv[2]);
		return rc;
	}

	/* ---- loop trace ---- */
	if (anx_strcmp(argv[1], "trace") == 0) {
		struct anx_loop_iter_score hist[ANX_LOOP_MAX_SCORE_HIST];
		uint32_t found = 0, j;

		rc = anx_loop_session_score_history(session_oid, hist,
						    ANX_LOOP_MAX_SCORE_HIST,
						    &found);
		if (rc != ANX_OK) {
			kprintf("loop: session not found\n");
			return rc;
		}
		if (found == 0) {
			kprintf("no score history yet\n");
			return ANX_OK;
		}
		kprintf("iter   energy    delta     candidates  best\n");
		kprintf("-----  --------  --------  ----------  ----------------\n");
		for (j = 0; j < found; j++) {
			kprintf("%-5u  %-8.4f  %-8.4f  %-10u  %016llx\n",
				hist[j].iteration,
				hist[j].best_energy,
				hist[j].delta,
				hist[j].candidate_count,
				(unsigned long long)hist[j].best_candidate.lo);
		}
		return ANX_OK;
	}

	loop_usage();
	return ANX_EINVAL;
}
