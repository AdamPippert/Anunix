/*
 * test_ibal.c — Host-native tests for the IBAL subsystem (RFC-0020).
 *
 * Covers session lifecycle (phases 1-4), memory consolidation (phase 5),
 * goal alignment (phase 3), EBM state persistence (phase 9), per-action
 * JEPA divergences (phase 10), and workflow template registration (phase 11).
 *
 * JEPA is expected to be UNAVAILABLE in the test environment (no tensor
 * engine registered), so all JEPA-dependent paths fall through to the
 * no-op / zero-fill branches.
 */

#include <anx/types.h>
#include <anx/loop.h>
#include <anx/ibal.h>
#include <anx/jepa.h>
#include <anx/memory.h>
#include <anx/cexl.h>
#include <anx/workflow_library.h>
#include <anx/workflow.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/diag.h>
#include <anx/string.h>
#include <anx/uuid.h>

int test_ibal(void)
{
	struct anx_loop_create_params  params;
	struct anx_loop_session_info   info;
	anx_oid_t                      sid, sid2, null_oid;
	anx_oid_t                      belief_oid, proposal_oid, score_oid;
	int                            rc;

	/* ------------------------------------------------------------------ */
	/* Subsystem initialisation                                             */
	/* ------------------------------------------------------------------ */

	anx_objstore_init();
	anx_cell_store_init();
	anx_wf_init();

	/* Test 1: workflow library init registers built-ins */
	rc = anx_wf_lib_init();
	if (rc != ANX_OK) return -1;

	/* Test 2: JEPA init is non-fatal in test environment */
	rc = anx_jepa_init();
	if (rc != ANX_OK) return -2;

	/* Test 3: loop subsystem init */
	rc = anx_loop_init();
	if (rc != ANX_OK) return -3;

	anx_memset(&null_oid, 0, sizeof(null_oid));

	/* ------------------------------------------------------------------ */
	/* Phase 1: session CRUD                                               */
	/* ------------------------------------------------------------------ */

	/* Test 4: create a session with default params */
	anx_memset(&params, 0, sizeof(params));
	params.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;
	params.max_iterations = 4;
	params.branch_budget  = 1;

	rc = anx_loop_session_create(&params, &sid);
	if (rc != ANX_OK) return -4;

	/* Test 5: freshly created session is in PENDING state */
	rc = anx_loop_session_status_get(sid, &info);
	if (rc != ANX_OK) return -5;
	if (info.status != ANX_LOOP_PENDING) return -5;

	/* Test 6: advance once → moves to RUNNING */
	rc = anx_loop_session_advance(sid);
	if (rc != ANX_OK) return -6;
	rc = anx_loop_session_status_get(sid, &info);
	if (rc != ANX_OK) return -6;
	if (info.status != ANX_LOOP_RUNNING) return -6;

	/* Test 7: halt → transitions to HALTED */
	rc = anx_loop_session_halt(sid);
	if (rc != ANX_OK) return -7;
	rc = anx_loop_session_status_get(sid, &info);
	if (rc != ANX_OK) return -7;
	if (info.status != ANX_LOOP_HALTED) return -7;

	/* Test 8: create a second session and abort it */
	anx_memset(&params, 0, sizeof(params));
	params.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;
	params.max_iterations = 8;
	params.branch_budget  = 1;

	rc = anx_loop_session_create(&params, &sid2);
	if (rc != ANX_OK) return -8;
	rc = anx_loop_session_abort(sid2);
	if (rc != ANX_OK) return -8;
	rc = anx_loop_session_status_get(sid2, &info);
	if (rc != ANX_OK) return -8;
	if (info.status != ANX_LOOP_ABORTED) return -8;

	/* Test 9: advancing an ABORTED session returns error */
	rc = anx_loop_session_advance(sid2);
	if (rc == ANX_OK) return -9;

	/* Test 10: score history is empty before any iterations */
	{
		struct anx_loop_iter_score hist[4];
		uint32_t found = 99;

		rc = anx_loop_session_score_history(sid2, hist, 4, &found);
		if (rc != ANX_OK) return -10;
		if (found != 0) return -10;
	}

	/* ------------------------------------------------------------------ */
	/* Phase 2: belief, proposal, score objects                            */
	/* ------------------------------------------------------------------ */

	/* Create a fresh running session for phase 2 tests */
	anx_memset(&params, 0, sizeof(params));
	params.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;
	params.max_iterations = 16;
	params.branch_budget  = 1;

	rc = anx_loop_session_create(&params, &sid);
	if (rc != ANX_OK) return -11;
	rc = anx_loop_session_advance(sid);
	if (rc != ANX_OK) return -11;

	/* Test 11: belief create stores valid OID */
	rc = anx_loop_belief_create(sid, 1, null_oid, &belief_oid);
	if (rc != ANX_OK) return -11;
	if (belief_oid.lo == 0 && belief_oid.hi == 0) return -11;

	/* Test 12: proposal create from a null latent */
	rc = anx_loop_proposal_create_jepa(sid, 1, null_oid,
					   ANX_JEPA_ACT_ROUTE_LOCAL,
					   &proposal_oid);
	if (rc != ANX_OK) return -12;

	/* Test 13: score object creation */
	rc = anx_loop_score_create(sid, proposal_oid, "test-scorer",
				   0.4f, ANX_LOOP_SCORE_ACCEPT,
				   0.8f, &score_oid);
	if (rc != ANX_OK) return -13;

	/* ------------------------------------------------------------------ */
	/* Phase 3: goal alignment                                             */
	/* ------------------------------------------------------------------ */

	/* Test 14: empty goal gives neutral energy (0.5) */
	{
		float e = anx_loop_goal_alignment_energy("", ANX_JEPA_ACT_IDLE);

		if (e < 0.49f || e > 0.51f) return -14;
	}

	/* Test 15: routing goal keyword lowers energy for route actions */
	{
		float e_route = anx_loop_goal_alignment_energy(
			"route local traffic fast",
			ANX_JEPA_ACT_ROUTE_LOCAL);
		float e_other = anx_loop_goal_alignment_energy(
			"route local traffic fast",
			ANX_JEPA_ACT_IDLE);

		if (e_route > e_other) return -15;
	}

	/* ------------------------------------------------------------------ */
	/* Phase 5: memory consolidation                                       */
	/* ------------------------------------------------------------------ */

	/* Test 16: consolidate succeeds (action_stats may be empty) */
	{
		struct anx_loop_session_action_stats astats[ANX_MEMORY_ACT_COUNT];

		anx_memset(astats, 0, sizeof(astats));
		astats[0].total_proposals = 4;
		astats[0].win_count       = 2;
		astats[0].energy_sum      = 1.6f;
		astats[0].min_energy      = 0.3f;

		rc = anx_loop_consolidate(sid, astats, ANX_MEMORY_ACT_COUNT);
		if (rc != ANX_OK) return -16;
	}

	/* ------------------------------------------------------------------ */
	/* Phase 9: EBM state persistence                                      */
	/* ------------------------------------------------------------------ */

	/* Test 17: save_state creates a sealed object */
	{
		anx_oid_t state_oid;

		rc = anx_ibal_save_state(&state_oid);
		if (rc != ANX_OK) return -17;
		if (state_oid.lo == 0 && state_oid.hi == 0) return -17;

		/* Test 18: load_state successfully reads it back */
		rc = anx_ibal_load_state(state_oid);
		if (rc != ANX_OK) return -18;

		/* Test 19: loading a null OID returns error */
		rc = anx_ibal_load_state(null_oid);
		if (rc == ANX_OK) return -19;
	}

	/* ------------------------------------------------------------------ */
	/* Phase 10: per-action JEPA divergences                               */
	/* ------------------------------------------------------------------ */

	/* Test 20: returns the expected action count */
	{
		float divs[ANX_JEPA_ACT_COUNT];
		uint32_t n;

		anx_memset(divs, 0xff, sizeof(divs));
		n = anx_jepa_get_action_divergences(divs, ANX_JEPA_ACT_COUNT);

		if (n != (uint32_t)ANX_JEPA_ACT_COUNT) return -20;
		/* When JEPA unavailable, all divergences must be 0 */
		if (!anx_jepa_available()) {
			uint32_t k;

			for (k = 0; k < n; k++)
				if (divs[k] != 0.0f) return -20;
		}
	}

	/* Test 21: train step counter starts at 0, increments on record_winner */
	{
		uint32_t before = anx_jepa_get_train_step_count();

		anx_jepa_record_winner(ANX_JEPA_ACT_ROUTE_LOCAL);
		if (anx_jepa_get_train_step_count() != before + 1) return -21;
	}

	/* ------------------------------------------------------------------ */
	/* Phase 11: IBAL workflow templates                                   */
	/* ------------------------------------------------------------------ */

	/* Test 22: three IBAL templates are registered */
	{
		const struct anx_wf_template *t;

		t = anx_wf_lib_lookup("anx:workflow/ibal/default/v1");
		if (!t) return -22;

		t = anx_wf_lib_lookup("anx:workflow/ibal/lite/v1");
		if (!t) return -22;

		t = anx_wf_lib_lookup("anx:workflow/ibal/symbolic/v1");
		if (!t) return -22;
	}

	/* Test 23: each IBAL template has at least 2 nodes */
	{
		const char *uris[] = {
			"anx:workflow/ibal/default/v1",
			"anx:workflow/ibal/lite/v1",
			"anx:workflow/ibal/symbolic/v1",
		};
		uint32_t k;

		for (k = 0; k < 3; k++) {
			const struct anx_wf_template *t =
				anx_wf_lib_lookup(uris[k]);

			if (!t) return -23;
			if (t->node_count < 2) return -23;
			if (t->edge_count < 1) return -23;
		}
	}

	/* Test 24: keyword matching hits IBAL templates for "ibal loop plan" */
	{
		struct anx_wf_match matches[4];
		uint32_t n;

		n = anx_wf_lib_match("ibal loop plan", matches, 4);
		if (n == 0) return -24;
	}

	/* ------------------------------------------------------------------ */
	/* Phase 12: PAL accumulation and CEXL critic-loop                    */
	/* ------------------------------------------------------------------ */

	/* Test 25: PAL prior is neutral (0.5) before cold-start gate (3 sessions) */
	{
		struct anx_loop_memory_payload mp;
		float prior;
		const char *world = "anx:world/test-pal-unit";

		anx_memset(&mp, 0, sizeof(mp));
		anx_strlcpy(mp.world_uri, world, sizeof(mp.world_uri));
		mp.action_stats[0].total_updates = 5;
		mp.action_stats[0].avg_energy    = 0.1f;
		mp.action_stats[0].win_rate      = 0.9f;
		mp.action_stats[0].min_energy    = 0.05f;

		/* 2 sessions: below PAL_COLD_GATE of 3 */
		anx_pal_memory_update(world, &mp);
		anx_pal_memory_update(world, &mp);

		prior = anx_pal_action_prior(world, 0);
		if (prior < 0.49f || prior > 0.51f) return -25;
	}

	/* Test 26: PAL prior becomes active after cold-start gate */
	{
		struct anx_loop_memory_payload mp;
		float prior;
		const char *world = "anx:world/test-pal-unit";

		anx_memset(&mp, 0, sizeof(mp));
		anx_strlcpy(mp.world_uri, world, sizeof(mp.world_uri));
		mp.action_stats[0].total_updates = 5;
		mp.action_stats[0].avg_energy    = 0.1f;
		mp.action_stats[0].win_rate      = 0.9f;
		mp.action_stats[0].min_energy    = 0.05f;

		/* 3rd session: crosses PAL_COLD_GATE */
		anx_pal_memory_update(world, &mp);

		prior = anx_pal_action_prior(world, 0);
		/* After 3 sessions of avg_energy=0.1, prior must be well below 0.5 */
		if (prior >= 0.4f) return -26;
	}

	/* Test 27: PAL session count correct after updates */
	{
		uint32_t cnt = anx_pal_session_count("anx:world/test-pal-unit");

		if (cnt != 3) return -27;
	}

	/* Test 28: CEXL on session with no counterexamples returns ENOENT */
	{
		struct anx_loop_create_params cp;
		anx_oid_t new_sid;

		anx_memset(&cp, 0, sizeof(cp));
		anx_strlcpy(cp.world_uri, "anx:world/test-cexl",
			    sizeof(cp.world_uri));
		cp.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;
		cp.max_iterations = 1;
		cp.branch_budget  = 1;

		rc = anx_loop_session_create(&cp, &new_sid);
		if (rc != ANX_OK) return -28;

		rc = anx_loop_cexl_process(new_sid, "anx:world/test-cexl");
		if (rc != ANX_ENOENT) return -28;
	}

	/* ------------------------------------------------------------------ */
	/* Phase 13: Branch arbitration and PAL-biased action selection       */
	/* ------------------------------------------------------------------ */

	/* Test 29: branch_create produces a child with branch_depth=1 */
	{
		struct anx_loop_create_params cp;
		struct anx_loop_session_info  binfo;
		anx_oid_t parent_sid, child_oid;

		anx_memset(&cp, 0, sizeof(cp));
		anx_strlcpy(cp.world_uri, "anx:world/test-branch",
			    sizeof(cp.world_uri));
		cp.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;
		cp.max_iterations = 8;
		cp.branch_budget  = 2;

		rc = anx_loop_session_create(&cp, &parent_sid);
		if (rc != ANX_OK) return -29;

		rc = anx_loop_branch_create(parent_sid, 0, 4, &child_oid);
		if (rc != ANX_OK) return -29;

		rc = anx_loop_session_status_get(child_oid, &binfo);
		if (rc != ANX_OK) return -29;
		if (binfo.branch_depth != 1) return -29;
	}

	/* Test 30: branch_list shows the child just created */
	{
		struct anx_loop_create_params cp;
		anx_oid_t parent_sid, child1, child2;
		anx_oid_t kids[ANX_LOOP_MAX_BRANCHES];
		uint32_t  cnt = 0;

		anx_memset(&cp, 0, sizeof(cp));
		anx_strlcpy(cp.world_uri, "anx:world/test-branch-list",
			    sizeof(cp.world_uri));
		cp.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;
		cp.max_iterations = 8;
		cp.branch_budget  = 2;

		rc = anx_loop_session_create(&cp, &parent_sid);
		if (rc != ANX_OK) return -30;

		rc = anx_loop_branch_create(parent_sid, 0, 4, &child1);
		if (rc != ANX_OK) return -30;
		rc = anx_loop_branch_create(parent_sid, 1, 4, &child2);
		if (rc != ANX_OK) return -30;

		rc = anx_loop_branch_list(parent_sid, kids,
					  ANX_LOOP_MAX_BRANCHES, &cnt);
		if (rc != ANX_OK) return -30;
		if (cnt != 2) return -30;
	}

	/* Test 31: select_action_by_prior returns 0 for unknown world */
	{
		uint32_t act = anx_loop_select_action_by_prior(
			"anx:world/no-pal-data", ANX_MEMORY_ACT_COUNT);

		/* Cold-start: all priors are 0.5; function picks first (0) */
		if (act != 0) return -31;
	}

	/* Test 32: select_action_by_prior returns PAL-biased action */
	{
		uint32_t act;

		/*
		 * Tests 25-26 trained "test-pal-unit" with action 0 having
		 * avg_energy=0.1 for 3 sessions; all other actions are cold
		 * (prior=0.5).  So action 0 must have the lowest prior.
		 */
		act = anx_loop_select_action_by_prior(
			"anx:world/test-pal-unit", ANX_MEMORY_ACT_COUNT);
		if (act != 0) return -32;
	}

	/* Phase 17: IBAL → JEPA online training pipeline                     */

	/* Test 33: anx_loop_jepa_ingest on a completed session advances
	 * the train-step counter by 1. */
	{
		struct anx_loop_create_params p;
		anx_oid_t sid;
		uint32_t steps_before, steps_after;
		int rc;

		anx_memset(&p, 0, sizeof(p));
		anx_strlcpy(p.world_uri, "anx:world/test-jepa-ingest",
			     sizeof(p.world_uri));
		anx_strlcpy(p.goal_text, "observe", sizeof(p.goal_text));
		p.max_iterations  = 4;
		p.halt_policy     = ANX_LOOP_HALT_ON_BUDGET;
		p.halt_threshold  = 0.0f;
		p.confidence_min  = 0.0f;
		p.branch_budget   = 0;

		rc = anx_loop_session_create(&p, &sid);
		if (rc != ANX_OK) return -33;

		/* Run a few iterations to populate best_candidate */
		rc = anx_loop_session_advance(sid);
		if (rc != ANX_OK) return -33;
		rc = anx_loop_session_advance(sid);
		(void)rc;

		steps_before = anx_jepa_get_train_step_count();

		rc = anx_loop_jepa_ingest(sid, "anx:world/test-jepa-ingest");
		if (rc != ANX_OK) return -33;

		steps_after = anx_jepa_get_train_step_count();
		if (steps_after != steps_before + 1) return -33;
	}

	/* Test 34: anx_loop_jepa_ingest with a nil best_candidate still
	 * advances the step counter (falls back to action_id=0). */
	{
		struct anx_loop_create_params p;
		anx_oid_t sid;
		uint32_t steps_before, steps_after;
		int rc;

		anx_memset(&p, 0, sizeof(p));
		anx_strlcpy(p.world_uri, "anx:world/test-jepa-nil",
			     sizeof(p.world_uri));
		p.max_iterations  = 1;
		p.halt_policy     = ANX_LOOP_HALT_ON_BUDGET;

		rc = anx_loop_session_create(&p, &sid);
		if (rc != ANX_OK) return -34;

		/* Don't advance — best_candidate remains nil */
		steps_before = anx_jepa_get_train_step_count();
		rc = anx_loop_jepa_ingest(sid, "anx:world/test-jepa-nil");
		if (rc != ANX_OK) return -34;

		steps_after = anx_jepa_get_train_step_count();
		if (steps_after != steps_before + 1) return -34;
	}

	/* Test 35: nil session OID returns ANX_ENOENT */
	{
		anx_oid_t nil_sid = ANX_UUID_NIL;
		int rc;

		rc = anx_loop_jepa_ingest(nil_sid, "anx:world/os-default");
		if (rc == ANX_OK) return -35;
	}

	/* Phase 18: IBAL diagnostic trace integration                        */

	/* Test 36: anx_ibal_run records an "ibal.session" trace phase. */
	{
		struct anx_loop_create_params p;
		anx_oid_t sid;
		struct anx_trace_phase phases[ANX_TRACE_PHASE_MAX];
		uint32_t count = 0;
		uint32_t i;
		bool found = false;
		int rc;

		anx_trace_reset();

		anx_memset(&p, 0, sizeof(p));
		anx_strlcpy(p.world_uri, "anx:world/test-diag",
			     sizeof(p.world_uri));
		p.max_iterations = 4;
		p.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;

		rc = anx_ibal_run(&p, &sid);
		if (rc != ANX_OK) return -36;

		rc = anx_trace_get_phases(phases, ANX_TRACE_PHASE_MAX, &count);
		if (rc != ANX_OK) return -36;
		if (count == 0) return -36;

		for (i = 0; i < count; i++) {
			if (anx_strcmp(phases[i].name, "ibal.session") == 0) {
				found = true;
				break;
			}
		}
		if (!found) return -36;
	}

	/* Test 37: the "ibal.session" phase is marked complete with valid
	 * timing (end_ns >= start_ns). */
	{
		struct anx_trace_phase phases[ANX_TRACE_PHASE_MAX];
		uint32_t count = 0;
		uint32_t i;
		int rc;

		rc = anx_trace_get_phases(phases, ANX_TRACE_PHASE_MAX, &count);
		if (rc != ANX_OK) return -37;

		for (i = 0; i < count; i++) {
			if (anx_strcmp(phases[i].name, "ibal.session") != 0)
				continue;
			if (!phases[i].complete) return -37;
			if (phases[i].end_ns < phases[i].start_ns) return -37;
			break;
		}
	}

	/* Test 38: a second anx_ibal_run adds a second trace phase. */
	{
		struct anx_loop_create_params p;
		anx_oid_t sid;
		struct anx_trace_phase phases[ANX_TRACE_PHASE_MAX];
		uint32_t count_before = 0, count_after = 0;
		int rc;

		anx_trace_get_phases(phases, ANX_TRACE_PHASE_MAX, &count_before);

		anx_memset(&p, 0, sizeof(p));
		anx_strlcpy(p.world_uri, "anx:world/test-diag2",
			     sizeof(p.world_uri));
		p.max_iterations = 2;
		p.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;

		rc = anx_ibal_run(&p, &sid);
		if (rc != ANX_OK) return -38;

		anx_trace_get_phases(phases, ANX_TRACE_PHASE_MAX, &count_after);
		if (count_after != count_before + 1) return -38;
	}

	/* ------------------------------------------------------------------ */
	/* Tests 39-41: trajectory ring buffer API                             */
	/* ------------------------------------------------------------------ */

	/* Test 39: reset clears the buffer; export returns ENOENT on empty. */
	{
		uint8_t  buf[256];
		uint32_t written = 0;

		anx_jepa_traj_reset();
		rc = anx_jepa_export_trajectory(buf, sizeof(buf), &written);
		if (rc != ANX_ENOENT) return -39;
		if (written != 0)     return -39;
	}

	/* Test 40: traj_ingest with NULL obs returns EINVAL. */
	{
		rc = anx_jepa_traj_ingest(NULL, 0, "anx:world/os-default");
		if (rc != ANX_EINVAL) return -40;
	}

	/* Test 41: traj_ingest when JEPA unavailable is ANX_OK (non-fatal). */
	{
		struct anx_jepa_obs obs;

		anx_memset(&obs, 0, sizeof(obs));
		rc = anx_jepa_traj_ingest(&obs, 0, "anx:world/os-default");
		if (rc != ANX_OK) return -41;
	}

	return 0;
}
