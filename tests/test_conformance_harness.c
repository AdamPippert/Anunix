/*
 * test_conformance_harness.c — P0-008 deterministic conformance/perf harness tests.
 */

#include <anx/types.h>
#include <anx/conformance.h>
#include <anx/string.h>

#define ASSERT(cond, code) do { if (!(cond)) return (code); } while (0)

static const char *find_substr(const char *haystack, const char *needle)
{
	uint32_t i;
	uint32_t nlen;

	nlen = (uint32_t)anx_strlen(needle);
	for (i = 0; haystack[i]; i++) {
		if (anx_strncmp(&haystack[i], needle, nlen) == 0)
			return &haystack[i];
	}
	return NULL;
}

/* P0-008-U01 */
static int test_report_schema_validation(void)
{
	struct anx_conformance_report report;
	char json[ANX_CONF_JSON_MAX];
	int rc;

	anx_memset(&report, 0, sizeof(report));
	anx_strlcpy(report.profile, "host-ci", sizeof(report.profile));
	report.schema_version = 1;
	report.tests_passed = 10;
	report.tests_failed = 0;
	report.perf_budget_ms = 40;
	report.perf_measured_ms = 31;

	rc = anx_conformance_report_validate(&report);
	ASSERT(rc == ANX_OK, -100);
	rc = anx_conformance_emit_report_json(&report, json, sizeof(json));
	ASSERT(rc == ANX_OK, -101);
	ASSERT(anx_conformance_parse_report_json(json, &report) == ANX_OK, -102);
	return 0;
}

/* P0-008-U02 */
static int test_deterministic_diff_engine(void)
{
	struct anx_conformance_report baseline;
	struct anx_conformance_report current;
	struct anx_conformance_delta d1;
	struct anx_conformance_delta d2;
	char j1[ANX_CONF_JSON_MAX];
	char j2[ANX_CONF_JSON_MAX];
	int rc;

	anx_memset(&baseline, 0, sizeof(baseline));
	anx_memset(&current, 0, sizeof(current));
	anx_strlcpy(baseline.profile, "host-ci", sizeof(baseline.profile));
	anx_strlcpy(current.profile, "host-ci", sizeof(current.profile));
	baseline.schema_version = current.schema_version = 1;
	baseline.tests_passed = 20;
	current.tests_passed = 19;
	baseline.tests_failed = 0;
	current.tests_failed = 1;
	baseline.perf_budget_ms = current.perf_budget_ms = 40;
	baseline.perf_measured_ms = 30;
	current.perf_measured_ms = 36;

	rc = anx_conformance_diff(&baseline, &current, &d1);
	ASSERT(rc == ANX_OK, -110);
	rc = anx_conformance_diff(&baseline, &current, &d2);
	ASSERT(rc == ANX_OK, -111);
	rc = anx_conformance_emit_delta_json(&d1, j1, sizeof(j1));
	ASSERT(rc == ANX_OK, -112);
	rc = anx_conformance_emit_delta_json(&d2, j2, sizeof(j2));
	ASSERT(rc == ANX_OK, -113);
	ASSERT(anx_memcmp(j1, j2, anx_strlen(j1) + 1) == 0, -114);
	return 0;
}

/* P0-008-I01 */
static int test_weekly_run_artifact_semantics(void)
{
	struct anx_conformance_report prev;
	struct anx_conformance_report curr;
	struct anx_conformance_delta delta;
	char drift[ANX_CONF_JSON_MAX];
	int rc;

	anx_memset(&prev, 0, sizeof(prev));
	anx_memset(&curr, 0, sizeof(curr));
	anx_strlcpy(prev.profile, "host-ci", sizeof(prev.profile));
	anx_strlcpy(curr.profile, "host-ci", sizeof(curr.profile));
	prev.schema_version = curr.schema_version = 1;
	prev.tests_passed = curr.tests_passed = 64;
	prev.tests_failed = curr.tests_failed = 0;
	prev.perf_budget_ms = curr.perf_budget_ms = 40;
	prev.perf_measured_ms = curr.perf_measured_ms = 38;

	rc = anx_conformance_diff(&prev, &curr, &delta);
	ASSERT(rc == ANX_OK, -120);
	rc = anx_conformance_emit_delta_json(&delta, drift, sizeof(drift));
	ASSERT(rc == ANX_OK, -121);
	ASSERT(find_substr(drift, "\"passed_delta\":0") != NULL, -122);
	ASSERT(find_substr(drift, "\"failed_delta\":0") != NULL, -123);
	return 0;
}

/* P0-008-I02 */
static int test_threshold_gate(void)
{
	struct anx_conformance_delta delta;
	int rc;

	delta.passed_delta = -1;
	delta.failed_delta = 1;
	delta.perf_delta_ms = 8;
	delta.threshold_breached = false;

	rc = anx_conformance_gate(&delta, 5, 0);
	ASSERT(rc == ANX_EPERM, -130);

	rc = anx_conformance_gate(&delta, 10, 1);
	ASSERT(rc == ANX_OK, -131);
	return 0;
}

int test_conformance_harness(void)
{
	int rc;

	rc = test_report_schema_validation();
	if (rc != 0)
		return rc;
	rc = test_deterministic_diff_engine();
	if (rc != 0)
		return rc;
	rc = test_weekly_run_artifact_semantics();
	if (rc != 0)
		return rc;
	rc = test_threshold_gate();
	if (rc != 0)
		return rc;
	return 0;
}
