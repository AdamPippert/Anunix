/*
 * loop_shell.c — Shell command handler for IBAL loop commands (RFC-0020).
 *
 * Dispatches: loop create|run|step|status|halt|abort|commit|trace
 *
 * Float display uses integer arithmetic (FINT/FFRAC) because kprintf
 * does not support %f format specifiers.
 */

#include <anx/loop.h>
#include <anx/ibal.h>
#include <anx/memory.h>
#include <anx/cexl.h>
#include <anx/diag.h>
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include "loop_internal.h"

/* Integer/fractional parts for fixed-point display of floats */
#define FINT(f)  ((unsigned int)(f))
#define FFRAC(f) ((unsigned int)(((f) - (float)FINT(f)) * 10000.0f + 0.5f))

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
	kprintf("  run    [--world <uri>] [--goal <text>] [--max-iter N]\n");
	kprintf("  step   <session-id>\n");
	kprintf("  status <session-id>\n");
	kprintf("  halt   <session-id>\n");
	kprintf("  abort  <session-id>\n");
	kprintf("  commit <session-id>\n");
	kprintf("  trace  <session-id>\n");
	kprintf("  branch   <session-id>\n");
	kprintf("  merge    <session-id> <child-id>\n");
	kprintf("  branches <session-id>\n");
	kprintf("  cexl     <session-id> [world-uri]\n");
	kprintf("  jepa     <session-id> [world-uri]\n");
	kprintf("  diag                               Show diag trace timeline\n");
	kprintf("  pal      [world-uri]\n");
}

static void fill_params_from_args(struct anx_loop_create_params *params,
				  int argc, const char *const *argv,
				  int start)
{
	int i;

	for (i = start; i < argc - 1; i++) {
		if (anx_strcmp(argv[i], "--world") == 0 && i + 1 < argc)
			anx_strlcpy(params->world_uri, argv[++i],
				    sizeof(params->world_uri));
		else if (anx_strcmp(argv[i], "--goal") == 0 && i + 1 < argc)
			anx_strlcpy(params->goal_text, argv[++i],
				    sizeof(params->goal_text));
		else if (anx_strcmp(argv[i], "--max-iter") == 0 && i + 1 < argc)
			params->max_iterations = (uint32_t)anx_strtoul(
				argv[++i], NULL, 10);
		else if (anx_strcmp(argv[i], "--halt") == 0 && i + 1 < argc) {
			i++;
			if (anx_strcmp(argv[i], "convergence") == 0)
				params->halt_policy = ANX_LOOP_HALT_ON_CONVERGENCE;
			else if (anx_strcmp(argv[i], "confidence") == 0)
				params->halt_policy = ANX_LOOP_HALT_ON_CONFIDENCE;
			else if (anx_strcmp(argv[i], "manual") == 0)
				params->halt_policy = ANX_LOOP_HALT_MANUAL;
			else
				params->halt_policy = ANX_LOOP_HALT_ON_BUDGET;
		} else if (anx_strcmp(argv[i], "--threshold") == 0 &&
			   i + 1 < argc) {
			params->halt_threshold =
				(float)anx_strtoul(argv[++i], NULL, 10)
				/ 1000.0f;
		} else if (anx_strcmp(argv[i], "--branches") == 0 &&
			   i + 1 < argc) {
			params->branch_budget =
				(uint32_t)anx_strtoul(argv[++i], NULL, 10);
		}
	}
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

	(void)i;

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

		fill_params_from_args(&params, argc, argv, 2);

		rc = anx_loop_session_create(&params, &oid);
		if (rc != ANX_OK) {
			kprintf("loop: create failed (%d)\n", rc);
			return rc;
		}
		kprintf("session: %016llx\n", (unsigned long long)oid.lo);
		kprintf("world:   %s\n",
			params.world_uri[0] ? params.world_uri
					    : "anx:world/os-default");
		kprintf("halt:    %s (max %u iter)\n",
			halt_policy_name(params.halt_policy),
			params.max_iterations);
		return ANX_OK;
	}

	/* ---- loop run ---- */
	if (anx_strcmp(argv[1], "run") == 0) {
		struct anx_loop_create_params params;
		struct anx_loop_session_info  info;
		anx_oid_t oid;

		anx_memset(&params, 0, sizeof(params));
		params.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;
		params.max_iterations = 16;
		params.branch_budget  = 1;

		fill_params_from_args(&params, argc, argv, 2);

		rc = anx_ibal_run(&params, &oid);
		if (rc != ANX_OK) {
			kprintf("loop: run failed (%d)\n", rc);
			return rc;
		}

		rc = anx_loop_session_status_get(oid, &info);
		if (rc != ANX_OK) {
			kprintf("loop: run status unavailable (%d)\n", rc);
			return rc;
		}

		kprintf("session: %016llx\n", (unsigned long long)oid.lo);
		kprintf("status:  %s after %u iterations\n",
			session_status_name(info.status), info.iteration);
		kprintf("energy:  %u.%04u\n",
			FINT(info.last_best_energy),
			FFRAC(info.last_best_energy));
		return ANX_OK;
	}

	/* ---- loop pal ---- */
	if (anx_strcmp(argv[1], "pal") == 0) {
		const char *world = (argc > 2) ? argv[2]
					       : "anx:world/os-default";
		struct anx_pal_action_info stats[ANX_MEMORY_ACT_COUNT];
		uint32_t cnt = 0, j;

		rc = anx_pal_stats_get(world, stats,
				       ANX_MEMORY_ACT_COUNT, &cnt);
		if (rc == ANX_ENOENT) {
			kprintf("pal: no data for world %s\n", world);
			return ANX_OK;
		}
		if (rc != ANX_OK) {
			kprintf("pal: stats_get failed (%d)\n", rc);
			return rc;
		}

		kprintf("world    : %s\n", world);
		kprintf("sessions : %u\n", anx_pal_session_count(world));
		kprintf("action  avg_energy  win_rate  min_energy  samples\n");
		kprintf("------  ----------  --------  ----------  -------\n");
		for (j = 0; j < cnt; j++) {
			if (stats[j].sample_count == 0)
				continue;
			kprintf("%-6u  %4u.%04u  %3u.%04u  %4u.%04u  %u\n",
				j,
				FINT(stats[j].avg_energy),
				FFRAC(stats[j].avg_energy),
				FINT(stats[j].win_rate),
				FFRAC(stats[j].win_rate),
				FINT(stats[j].min_energy),
				FFRAC(stats[j].min_energy),
				stats[j].sample_count);
		}
		return ANX_OK;
	}

	/* ---- loop diag ---- (no session-id needed) */
	if (anx_strcmp(argv[1], "diag") == 0) {
		struct anx_trace_phase phases[ANX_TRACE_PHASE_MAX];
		uint32_t count = 0;
		uint32_t i;

		rc = anx_trace_get_phases(phases, ANX_TRACE_PHASE_MAX, &count);
		if (rc != ANX_OK) {
			kprintf("loop: diag get failed (%d)\n", rc);
			return rc;
		}
		kprintf("diag trace phases (%u):\n", count);
		for (i = 0; i < count; i++) {
			kprintf("  [%s] %s start=%llu end=%llu\n",
				phases[i].complete ? "done" : "open",
				phases[i].name,
				(unsigned long long)phases[i].start_ns,
				(unsigned long long)phases[i].end_ns);
		}
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
		{
			struct anx_loop_session_info info;

			if (anx_loop_session_status_get(session_oid, &info)
			    == ANX_OK)
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
		kprintf("energy   : %u.%04u  delta %u.%04u\n",
			FINT(info.last_best_energy),
			FFRAC(info.last_best_energy),
			FINT(info.last_delta),
			FFRAC(info.last_delta));
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

	/* ---- loop commit ---- */
	if (anx_strcmp(argv[1], "commit") == 0) {
		anx_oid_t null_oid;

		anx_memset(&null_oid, 0, sizeof(null_oid));
		rc = anx_loop_session_commit(session_oid, null_oid);
		if (rc != ANX_OK)
			kprintf("loop: commit failed (%d)\n", rc);
		else
			kprintf("session %s committed\n", argv[2]);
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
		kprintf("iter   energy      delta       candidates  best\n");
		kprintf("-----  ----------  ----------  ----------  ----------------\n");
		for (j = 0; j < found; j++) {
			kprintf("%-5u  %4u.%04u  %4u.%04u  %-10u  %016llx\n",
				hist[j].iteration,
				FINT(hist[j].best_energy),
				FFRAC(hist[j].best_energy),
				FINT(hist[j].delta),
				FFRAC(hist[j].delta),
				hist[j].candidate_count,
				(unsigned long long)hist[j].best_candidate.lo);
		}
		return ANX_OK;
	}

	/* ---- loop branch ---- */
	if (anx_strcmp(argv[1], "branch") == 0) {
		rc = anx_loop_branch_schedule_and_merge(session_oid);
		if (rc == ANX_EPERM)
			kprintf("loop: branch not allowed (branch session or no budget)\n");
		else if (rc != ANX_OK)
			kprintf("loop: branch failed (%d)\n", rc);
		else
			kprintf("loop: branches complete for session %s\n", argv[2]);
		return rc;
	}

	/* ---- loop merge ---- */
	if (anx_strcmp(argv[1], "merge") == 0) {
		anx_oid_t child_oid;

		if (argc < 4) {
			kprintf("loop: merge requires <session-id> <child-id>\n");
			return ANX_EINVAL;
		}
		child_oid = parse_session_oid(argv[3]);
		rc = anx_loop_branch_merge(session_oid, child_oid);
		if (rc != ANX_OK)
			kprintf("loop: merge failed (%d)\n", rc);
		else
			kprintf("loop: merged %s into %s\n", argv[3], argv[2]);
		return rc;
	}

	/* ---- loop branches ---- */
	if (anx_strcmp(argv[1], "branches") == 0) {
		anx_oid_t kids[ANX_LOOP_MAX_BRANCHES];
		uint32_t cnt = 0, j;

		rc = anx_loop_branch_list(session_oid, kids,
					  ANX_LOOP_MAX_BRANCHES, &cnt);
		if (rc != ANX_OK) {
			kprintf("loop: session not found\n");
			return rc;
		}
		if (cnt == 0) {
			kprintf("no branches for session %s\n", argv[2]);
			return ANX_OK;
		}
		kprintf("branches (%u):\n", cnt);
		for (j = 0; j < cnt; j++)
			kprintf("  [%u] %016llx\n", j,
				(unsigned long long)kids[j].lo);
		return ANX_OK;
	}

	/* ---- loop cexl ---- */
	if (anx_strcmp(argv[1], "cexl") == 0) {
		const char *world = (argc > 3) ? argv[3]
					       : "anx:world/os-default";

		rc = anx_loop_cexl_process(session_oid, world);
		if (rc == ANX_ENOENT) {
			kprintf("loop: no counterexamples for session %s\n",
				argv[2]);
			return ANX_OK;
		}
		if (rc != ANX_OK) {
			kprintf("loop: cexl failed (%d)\n", rc);
			return rc;
		}
		kprintf("loop: cexl complete for session %s world=%s\n",
			argv[2], world);
		return ANX_OK;
	}

	/* ---- loop jepa ---- */
	if (anx_strcmp(argv[1], "jepa") == 0) {
		const char *world = (argc > 3) ? argv[3]
					       : "anx:world/os-default";

		rc = anx_loop_jepa_ingest(session_oid, world);
		if (rc != ANX_OK) {
			kprintf("loop: jepa ingest failed (%d)\n", rc);
			return rc;
		}
		kprintf("loop: jepa ingested session %s world=%s\n",
			argv[2], world);
		return ANX_OK;
	}

	loop_usage();
	return ANX_EINVAL;
}
