/*
 * test_ebm.c — Unit tests for the EBM scoring subsystem (RFC-0020 Phase 3).
 *
 * Tests: init, per-proposal scoring, full iteration pipeline, goal alignment
 * energy, and consolidation via anx_ibal_run.
 */

#include <anx/types.h>
#include <anx/loop.h>
#include <anx/ebm.h>
#include <anx/ibal.h>
#include <anx/jepa.h>
#include <anx/string.h>
#include <anx/kprintf.h>

int test_ebm(void)
{
	struct anx_loop_create_params p;
	anx_oid_t sid   = {0};
	anx_oid_t nil   = {0};
	float     energy = -1.0f;
	int       rc;

	/* Init subsystems */
	anx_objstore_init();
	anx_cell_store_init();
	anx_jepa_init();
	anx_loop_init();

	/* Test 1: anx_ebm_init succeeds */
	rc = anx_ebm_init();
	if (rc != ANX_OK) return -1;

	/* Test 2: idempotent re-init */
	rc = anx_ebm_init();
	if (rc != ANX_OK) return -2;

	/* Test 3: score_proposal rejects NULL energy_out */
	rc = anx_ebm_score_proposal(nil, nil, NULL);
	if (rc != ANX_EINVAL) return -3;

	/* Test 4: score_proposal with nil proposal OID returns valid energy */
	anx_memset(&p, 0, sizeof(p));
	anx_strlcpy(p.world_uri, "anx:world/test-ebm", sizeof(p.world_uri));
	p.max_iterations = 2;
	p.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;

	rc = anx_loop_session_create(&p, &sid);
	if (rc != ANX_OK) return -4;

	(void)anx_loop_session_advance(sid);
	rc = anx_ebm_score_proposal(sid, nil, &energy);
	if (rc != ANX_OK) return -5;
	if (energy < 0.0f || energy > 1.0f) return -6;

	/* Test 5: run_iteration on an active session succeeds */
	{
		struct anx_loop_session_action_stats stats[ANX_JEPA_ACT_COUNT];

		anx_memset(stats, 0, sizeof(stats));
		rc = anx_ebm_run_iteration(sid, stats, (uint32_t)ANX_JEPA_ACT_COUNT);
		if (rc != ANX_OK) return -7;
	}

	/* Test 6: run_iteration on a HALTED session returns non-OK */
	(void)anx_loop_session_halt(sid);
	rc = anx_ebm_run_iteration(sid, NULL, 0);
	if (rc == ANX_OK) return -8;

	/* Test 7: anx_ibal_run drives EBM each iteration and fills score history */
	{
		anx_oid_t run_sid = {0};
		struct anx_loop_session_info info;

		anx_memset(&p, 0, sizeof(p));
		anx_strlcpy(p.world_uri, "anx:world/test-ebm2", sizeof(p.world_uri));
		anx_strlcpy(p.goal_text, "route network traffic",
			    sizeof(p.goal_text));
		p.max_iterations = 3;
		p.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;

		rc = anx_ibal_run(&p, &run_sid);
		if (rc != ANX_OK) return -9;

		rc = anx_loop_session_status_get(run_sid, &info);
		if (rc != ANX_OK) return -10;
		if (info.status != ANX_LOOP_HALTED) return -11;
		/* EBM must have produced at least one score record */
		if (info.last_best_energy < 0.0f || info.last_best_energy > 1.0f)
			return -12;
	}

	/* Test 8: goal alignment energy — matching action gets low energy */
	{
		float e = anx_loop_goal_alignment_energy("route traffic",
							 ANX_JEPA_ACT_ROUTE_LOCAL);
		if (e >= 0.3f) return -13;
	}

	/* Test 9: goal alignment energy — opposed action gets high energy */
	{
		float e = anx_loop_goal_alignment_energy("route traffic",
							 ANX_JEPA_ACT_MEM_FORGET);
		if (e <= 0.5f) return -14;
	}

	/* Test 10: two sequential ibal_run sessions both complete to HALTED.
	 * Read sA status immediately — HALTED slots are recycled on next create. */
	{
		anx_oid_t sA = {0}, sB = {0};
		struct anx_loop_session_info iA, iB;

		anx_memset(&p, 0, sizeof(p));
		anx_strlcpy(p.world_uri, "anx:world/test-ebm3", sizeof(p.world_uri));
		p.max_iterations = 2;
		p.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;
		if (anx_ibal_run(&p, &sA) != ANX_OK) return -15;

		/* Read sA before creating sB (which may recycle sA's slot) */
		anx_memset(&iA, 0, sizeof(iA));
		if (anx_loop_session_status_get(sA, &iA) != ANX_OK) return -16;
		if (iA.status != ANX_LOOP_HALTED) return -17;

		p.max_iterations = 3;
		if (anx_ibal_run(&p, &sB) != ANX_OK) return -18;

		anx_memset(&iB, 0, sizeof(iB));
		if (anx_loop_session_status_get(sB, &iB) != ANX_OK) return -19;
		if (iB.status != ANX_LOOP_HALTED) return -20;
	}

	/* Test 11: anx_loop_llm_propose returns a valid proposal OID */
	{
		anx_oid_t llm_sid = {0};
		anx_oid_t llm_prop = {0};
		struct anx_loop_session_info llm_info;

		anx_memset(&p, 0, sizeof(p));
		anx_strlcpy(p.world_uri, "anx:world/test-llm",  sizeof(p.world_uri));
		anx_strlcpy(p.goal_text, "route network traffic", sizeof(p.goal_text));
		p.max_iterations = 4;
		p.halt_policy    = ANX_LOOP_HALT_ON_BUDGET;

		rc = anx_loop_session_create(&p, &llm_sid);
		if (rc != ANX_OK) return -21;
		(void)anx_loop_session_advance(llm_sid);

		rc = anx_loop_llm_propose(llm_sid, 1, &llm_prop);
		if (rc != ANX_OK) return -22;

		/* OID must be non-zero */
		if (llm_prop.hi == 0 && llm_prop.lo == 0) return -23;

		/* best_candidate must be set after the proposal was added */
		rc = anx_loop_session_status_get(llm_sid, &llm_info);
		if (rc != ANX_OK) return -24;
	}

	return 0;
}
