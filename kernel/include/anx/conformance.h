/*
 * anx/conformance.h — Deterministic conformance/perf harness helpers.
 */

#ifndef ANX_CONFORMANCE_H
#define ANX_CONFORMANCE_H

#include <anx/types.h>

#define ANX_CONF_PROFILE_MAX 32
#define ANX_CONF_JSON_MAX    512

struct anx_conformance_report {
	char profile[ANX_CONF_PROFILE_MAX];
	uint32_t schema_version;
	uint32_t tests_passed;
	uint32_t tests_failed;
	uint32_t perf_budget_ms;
	uint32_t perf_measured_ms;
};

struct anx_conformance_delta {
	int32_t passed_delta;
	int32_t failed_delta;
	int32_t perf_delta_ms;
	bool threshold_breached;
};

int anx_conformance_report_validate(const struct anx_conformance_report *report);
int anx_conformance_diff(const struct anx_conformance_report *baseline,
			 const struct anx_conformance_report *current,
			 struct anx_conformance_delta *out);
int anx_conformance_gate(const struct anx_conformance_delta *delta,
			 uint32_t max_perf_regression_ms,
			 uint32_t max_failed_delta);
int anx_conformance_emit_report_json(const struct anx_conformance_report *report,
				     char *out, uint32_t out_len);
int anx_conformance_emit_delta_json(const struct anx_conformance_delta *delta,
				    char *out, uint32_t out_len);
int anx_conformance_parse_report_json(const char *json,
			      struct anx_conformance_report *out);

#endif /* ANX_CONFORMANCE_H */
