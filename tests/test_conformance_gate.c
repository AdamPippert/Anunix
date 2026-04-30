/*
 * test_conformance_gate.c — Tests for P2-004: conformance gate.
 *
 * Tests:
 * - P2-004-U01: runner is deterministic (same seed → same report)
 * - P2-004-U02: diff tracking detects regressions and improvements
 * - P2-004-U03: threshold enforcement rejects excess failures
 */

#include <anx/types.h>
#include <anx/conformance_gate.h>
#include <anx/string.h>

#define ASSERT(cond, code)    do { if (!(cond)) return (code); } while (0)
#define ASSERT_EQ(a, b, code) do { if ((a) != (b)) return (code); } while (0)

/* ------------------------------------------------------------------ */
/* Test fixtures                                                        */
/* ------------------------------------------------------------------ */

static int fixture_always_pass(void) { return 0; }
static int fixture_always_fail(void) { return 1; }

static int g_toggle_result;
static int fixture_toggle(void) { return g_toggle_result; }

/* ------------------------------------------------------------------ */
/* P2-004-U01: runner determinism                                      */
/* ------------------------------------------------------------------ */

static int test_runner_determinism(void)
{
	struct anx_gate_report r1, r2, r3;
	int rc;

	anx_gate_init();

	rc = anx_gate_register("always_pass", fixture_always_pass);
	ASSERT_EQ(rc, ANX_OK, -100);
	rc = anx_gate_register("always_fail", fixture_always_fail);
	ASSERT_EQ(rc, ANX_OK, -101);

	/* Same seed → same report twice. */
	rc = anx_gate_run(42, &r1);
	ASSERT_EQ(rc, ANX_OK, -102);
	rc = anx_gate_run(42, &r2);
	ASSERT_EQ(rc, ANX_OK, -103);

	ASSERT_EQ(r1.total,  r2.total,  -104);
	ASSERT_EQ(r1.passed, r2.passed, -105);
	ASSERT_EQ(r1.failed, r2.failed, -106);
	ASSERT_EQ(r1.seed,   r2.seed,   -107);

	/* Verify counts. */
	ASSERT_EQ(r1.total,  2u, -108);
	ASSERT_EQ(r1.passed, 1u, -109);
	ASSERT_EQ(r1.failed, 1u, -110);

	/* Different seed produces same counts (only 2 fixtures; order varies
	 * but pass/fail totals are the same). */
	rc = anx_gate_run(99, &r3);
	ASSERT_EQ(rc, ANX_OK, -111);
	ASSERT_EQ(r3.total,  2u, -112);
	ASSERT_EQ(r3.passed, 1u, -113);
	ASSERT_EQ(r3.failed, 1u, -114);

	/* Seeds are recorded. */
	ASSERT_EQ(r1.seed, 42u, -115);
	ASSERT_EQ(r3.seed, 99u, -116);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P2-004-U02: diff tracking                                           */
/* ------------------------------------------------------------------ */

static int test_diff_tracking(void)
{
	struct anx_gate_report prev, curr;
	struct anx_gate_diff diff;
	int rc;

	anx_gate_init();

	anx_gate_register("pass_fixture",   fixture_always_pass);
	anx_gate_register("toggle_fixture", fixture_toggle);

	/* First run: toggle passes. */
	g_toggle_result = 0;
	rc = anx_gate_run(1, &prev);
	ASSERT_EQ(rc, ANX_OK, -200);
	ASSERT_EQ(prev.passed, 2u, -201);

	/* Second run: toggle regresses. */
	g_toggle_result = 1;
	rc = anx_gate_run(1, &curr);
	ASSERT_EQ(rc, ANX_OK, -202);
	ASSERT_EQ(curr.passed, 1u, -203);
	ASSERT_EQ(curr.failed, 1u, -204);

	/* Diff should show one regression. */
	anx_gate_diff(&prev, &curr, &diff);
	ASSERT_EQ(diff.count, 1u, -205);
	ASSERT(diff.entries[0].regression == true, -206);
	ASSERT(anx_strcmp(diff.entries[0].fixture_name, "toggle_fixture") == 0,
	       -207);
	ASSERT_EQ(diff.entries[0].prev_code, 0, -208);
	ASSERT_EQ(diff.entries[0].curr_code, 1, -209);

	/* Reverse: toggle recovers → improvement, not regression. */
	g_toggle_result = 0;
	rc = anx_gate_run(1, &curr);
	ASSERT_EQ(rc, ANX_OK, -210);

	/* prev is the regression run (failed=1), curr is passing. */
	struct anx_gate_report old_curr = curr;
	(void)old_curr;

	/* Re-run with toggle failing to get a "prev" with failure. */
	g_toggle_result = 1;
	rc = anx_gate_run(1, &prev);
	ASSERT_EQ(rc, ANX_OK, -211);

	g_toggle_result = 0;
	rc = anx_gate_run(1, &curr);
	ASSERT_EQ(rc, ANX_OK, -212);

	anx_gate_diff(&prev, &curr, &diff);
	ASSERT_EQ(diff.count, 1u, -213);
	/* Improved: was failing, now passing — not a regression. */
	ASSERT(diff.entries[0].regression == false, -214);

	/* No diff when nothing changes. */
	rc = anx_gate_run(1, &prev);
	anx_gate_diff(&prev, &curr, &diff);
	ASSERT_EQ(diff.count, 0u, -215);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P2-004-U03: threshold enforcement                                   */
/* ------------------------------------------------------------------ */

static int test_threshold_enforcement(void)
{
	struct anx_gate_report report;
	char reason[128];
	int rc;

	anx_gate_init();

	anx_gate_register("p1", fixture_always_pass);
	anx_gate_register("p2", fixture_always_pass);
	anx_gate_register("f1", fixture_always_fail);
	anx_gate_register("f2", fixture_always_fail);

	rc = anx_gate_run(1, &report);
	ASSERT_EQ(rc, ANX_OK, -300);
	ASSERT_EQ(report.passed, 2u, -301);
	ASSERT_EQ(report.failed, 2u, -302);

	/* Threshold of 2: exactly 2 failures — OK. */
	rc = anx_gate_threshold_check(&report, 2, reason, sizeof(reason));
	ASSERT_EQ(rc, ANX_OK, -303);

	/* Threshold of 1: 2 failures > 1 — EPERM. */
	rc = anx_gate_threshold_check(&report, 1, reason, sizeof(reason));
	ASSERT_EQ(rc, ANX_EPERM, -304);

	/* Reason string is populated. */
	ASSERT(reason[0] != '\0', -305);

	/* Threshold of 0: any failure → EPERM. */
	rc = anx_gate_threshold_check(&report, 0, NULL, 0);
	ASSERT_EQ(rc, ANX_EPERM, -306);

	/* All-passing report with threshold 0: OK. */
	anx_gate_init();
	anx_gate_register("only_pass", fixture_always_pass);
	rc = anx_gate_run(1, &report);
	ASSERT_EQ(rc, ANX_OK, -307);
	rc = anx_gate_threshold_check(&report, 0, NULL, 0);
	ASSERT_EQ(rc, ANX_OK, -308);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int test_conformance_gate(void)
{
	int rc;

	rc = test_runner_determinism();
	if (rc) return rc;

	rc = test_diff_tracking();
	if (rc) return rc;

	rc = test_threshold_enforcement();
	if (rc) return rc;

	return 0;
}
