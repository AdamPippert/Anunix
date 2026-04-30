/*
 * test_diag.c — Tests for P2-005: crash diagnostics and observability.
 *
 * Tests:
 * - P2-005-U01: synthetic fault produces a parseable crash artifact
 * - P2-005-U02: trace phases recorded in order, start < end
 * - P2-005-U03: metrics schema version is backward compatible
 */

#include <anx/types.h>
#include <anx/diag.h>
#include <anx/string.h>

#define ASSERT(cond, code)    do { if (!(cond)) return (code); } while (0)
#define ASSERT_EQ(a, b, code) do { if ((a) != (b)) return (code); } while (0)

/* ------------------------------------------------------------------ */
/* P2-005-U01: synthetic fault produces a parseable crash artifact     */
/* ------------------------------------------------------------------ */

static int test_crash_artifact(void)
{
	struct anx_crash_artifact *a;
	int rc;

	anx_diag_init();

	/* No crash yet. */
	ASSERT(anx_diag_last_crash() == NULL, -100);

	a = anx_diag_fault("null_deref", 0xDEADBEEF, "iface",
	                    "null pointer in surface lookup");
	ASSERT(a != NULL, -101);

	/* Artifact is the most recent. */
	ASSERT(anx_diag_last_crash() == a, -102);

	/* Magic and version must be set. */
	ASSERT_EQ(a->magic,   ANX_DIAG_CRASH_MAGIC, -103);
	ASSERT_EQ(a->version, 1u, -104);

	/* Fields populated. */
	ASSERT(anx_strcmp(a->fault_type, "null_deref") == 0, -105);
	ASSERT_EQ(a->fault_addr, (uint64_t)0xDEADBEEF, -106);
	ASSERT(anx_strcmp(a->subsystem, "iface") == 0, -107);
	ASSERT(a->parsed == false, -108);

	/* Parse succeeds and sets parsed flag. */
	rc = anx_diag_crash_parse(a);
	ASSERT_EQ(rc, ANX_OK, -109);
	ASSERT(a->parsed == true, -110);

	/* Corrupt magic fails parse. */
	a->magic = 0;
	rc = anx_diag_crash_parse(a);
	ASSERT_EQ(rc, ANX_EINVAL, -111);

	/* Reset clears pool. */
	anx_diag_crash_reset();
	ASSERT(anx_diag_last_crash() == NULL, -112);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P2-005-U02: trace phases recorded in order, start < end             */
/* ------------------------------------------------------------------ */

static int test_trace_phases(void)
{
	struct anx_trace_phase phases[4];
	uint32_t count;
	int rc;

	anx_diag_init();

	/* Begin three phases. */
	rc = anx_trace_begin("render");
	ASSERT_EQ(rc, ANX_OK, -200);

	rc = anx_trace_begin("input");
	ASSERT_EQ(rc, ANX_OK, -201);

	rc = anx_trace_begin("network");
	ASSERT_EQ(rc, ANX_OK, -202);

	/* Duplicate open phase is EBUSY. */
	rc = anx_trace_begin("render");
	ASSERT_EQ(rc, ANX_EBUSY, -203);

	/* End phases in order. */
	rc = anx_trace_end("input");
	ASSERT_EQ(rc, ANX_OK, -204);

	rc = anx_trace_end("render");
	ASSERT_EQ(rc, ANX_OK, -205);

	/* End non-existent phase. */
	rc = anx_trace_end("missing");
	ASSERT_EQ(rc, ANX_ENOENT, -206);

	/* Get all phases. */
	rc = anx_trace_get_phases(phases, 4, &count);
	ASSERT_EQ(rc, ANX_OK, -207);
	ASSERT_EQ(count, 3u, -208);

	/* render is complete; network is still open */
	ASSERT(phases[0].complete == true,  -209);   /* render */
	ASSERT(phases[1].active   == true,  -210);   /* input  */
	ASSERT(phases[2].complete == false, -211);   /* network still open */

	/* completed phases have end >= start */
	ASSERT(phases[0].end_ns >= phases[0].start_ns, -212);
	ASSERT(phases[1].end_ns >= phases[1].start_ns, -213);

	/* Reset clears everything. */
	anx_trace_reset();
	rc = anx_trace_get_phases(phases, 4, &count);
	ASSERT_EQ(rc, ANX_OK, -214);
	ASSERT_EQ(count, 0u, -215);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P2-005-U03: metrics schema version is backward compatible            */
/* ------------------------------------------------------------------ */

static int test_metrics_schema(void)
{
	struct anx_perf_metrics m;
	struct anx_perf_metrics out;

	anx_diag_init();

	/* Fresh init: schema version is always ANX_METRICS_VERSION. */
	anx_metrics_current(&out);
	ASSERT_EQ(out.schema_version, ANX_METRICS_VERSION, -300);

	/* Record a snapshot. */
	anx_memset(&m, 0, sizeof(m));
	m.schema_version    = 0;   /* caller value overridden by implementation */
	m.frame_time_ns     = 16666666ULL;
	m.input_latency_ns  = 1000000ULL;
	m.memory_used_bytes = 4096ULL;
	m.cpu_load_pct      = 42;
	m.active_surfaces   = 3;

	anx_metrics_record(&m);

	/* Read back. */
	anx_metrics_current(&out);

	/* schema_version is always ANX_METRICS_VERSION regardless of input */
	ASSERT_EQ(out.schema_version, ANX_METRICS_VERSION, -301);
	ASSERT_EQ(out.frame_time_ns,    m.frame_time_ns,    -302);
	ASSERT_EQ(out.input_latency_ns, m.input_latency_ns, -303);
	ASSERT_EQ(out.cpu_load_pct,     42u,                -304);
	ASSERT_EQ(out.active_surfaces,  3u,                 -305);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int test_diag(void)
{
	int rc;

	rc = test_crash_artifact();
	if (rc) return rc;

	rc = test_trace_phases();
	if (rc) return rc;

	rc = test_metrics_schema();
	if (rc) return rc;

	return 0;
}
