/*
 * conformance.c — Deterministic conformance/perf report helpers.
 */

#include <anx/types.h>
#include <anx/conformance.h>
#include <anx/string.h>

static int append_char(char *out, uint32_t out_len, uint32_t *idx, char c)
{
	if (*idx + 1 >= out_len)
		return ANX_EFULL;
	out[*idx] = c;
	(*idx)++;
	out[*idx] = '\0';
	return ANX_OK;
}

static int append_str(char *out, uint32_t out_len, uint32_t *idx, const char *s)
{
	uint32_t i = 0;

	while (s[i]) {
		int rc = append_char(out, out_len, idx, s[i]);
		if (rc != ANX_OK)
			return rc;
		i++;
	}
	return ANX_OK;
}

static int append_u32(char *out, uint32_t out_len, uint32_t *idx, uint32_t v)
{
	char tmp[16];
	uint32_t n = 0;
	uint32_t i;

	if (v == 0)
		return append_char(out, out_len, idx, '0');

	while (v > 0 && n < sizeof(tmp)) {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	if (v != 0)
		return ANX_EFULL;

	for (i = 0; i < n; i++) {
		int rc = append_char(out, out_len, idx, tmp[n - 1 - i]);
		if (rc != ANX_OK)
			return rc;
	}
	return ANX_OK;
}

static int append_i32(char *out, uint32_t out_len, uint32_t *idx, int32_t v)
{
	if (v < 0) {
		int rc = append_char(out, out_len, idx, '-');
		if (rc != ANX_OK)
			return rc;
		return append_u32(out, out_len, idx, (uint32_t)(-v));
	}
	return append_u32(out, out_len, idx, (uint32_t)v);
}

static int parse_u32(const char *p, uint32_t *out)
{
	uint32_t v = 0;
	uint32_t i = 0;

	if (!p || !out || p[0] < '0' || p[0] > '9')
		return ANX_EINVAL;

	while (p[i] >= '0' && p[i] <= '9') {
		v = v * 10 + (uint32_t)(p[i] - '0');
		i++;
	}
	*out = v;
	return ANX_OK;
}

static const char *find_substr(const char *haystack, const char *needle)
{
	uint32_t i;
	uint32_t nlen;

	if (!haystack || !needle)
		return NULL;
	nlen = (uint32_t)anx_strlen(needle);
	if (nlen == 0)
		return haystack;

	for (i = 0; haystack[i]; i++) {
		if (anx_strncmp(&haystack[i], needle, nlen) == 0)
			return &haystack[i];
	}
	return NULL;
}

static int parse_quoted_value(const char *json, const char *key, char *out, uint32_t out_len)
{
	const char *p;
	uint32_t i = 0;

	p = find_substr(json, key);
	if (!p)
		return ANX_ENOENT;
	p = find_substr(p, "\"");
	if (!p)
		return ANX_EINVAL;
	p++;
	while (p[i] && p[i] != '"') {
		if (i + 1 >= out_len)
			return ANX_EFULL;
		out[i] = p[i];
		i++;
	}
	if (p[i] != '"')
		return ANX_EINVAL;
	out[i] = '\0';
	return ANX_OK;
}

static int parse_u32_value(const char *json, const char *key, uint32_t *out)
{
	const char *p;

	p = find_substr(json, key);
	if (!p)
		return ANX_ENOENT;
	p = find_substr(p, ":");
	if (!p)
		return ANX_EINVAL;
	p++;
	while (*p == ' ')
		p++;
	return parse_u32(p, out);
}

int anx_conformance_report_validate(const struct anx_conformance_report *report)
{
	if (!report)
		return ANX_EINVAL;
	if (report->profile[0] == '\0')
		return ANX_EINVAL;
	if (report->schema_version == 0)
		return ANX_EINVAL;
	if (report->perf_budget_ms == 0)
		return ANX_EINVAL;
	if (report->perf_measured_ms == 0)
		return ANX_EINVAL;
	return ANX_OK;
}

int anx_conformance_diff(const struct anx_conformance_report *baseline,
			 const struct anx_conformance_report *current,
			 struct anx_conformance_delta *out)
{
	if (!baseline || !current || !out)
		return ANX_EINVAL;

	out->passed_delta = (int32_t)current->tests_passed - (int32_t)baseline->tests_passed;
	out->failed_delta = (int32_t)current->tests_failed - (int32_t)baseline->tests_failed;
	out->perf_delta_ms = (int32_t)current->perf_measured_ms - (int32_t)baseline->perf_measured_ms;
	out->threshold_breached = false;
	return ANX_OK;
}

int anx_conformance_gate(const struct anx_conformance_delta *delta,
			 uint32_t max_perf_regression_ms,
			 uint32_t max_failed_delta)
{
	if (!delta)
		return ANX_EINVAL;
	if (delta->failed_delta > (int32_t)max_failed_delta)
		return ANX_EPERM;
	if (delta->perf_delta_ms > (int32_t)max_perf_regression_ms)
		return ANX_EPERM;
	return ANX_OK;
}

int anx_conformance_emit_report_json(const struct anx_conformance_report *report,
				     char *out, uint32_t out_len)
{
	uint32_t idx = 0;
	int rc;

	if (!report || !out || out_len == 0)
		return ANX_EINVAL;
	out[0] = '\0';

	rc = append_str(out, out_len, &idx, "{\"schema_version\":");
	if (rc != ANX_OK)
		return rc;
	rc = append_u32(out, out_len, &idx, report->schema_version);
	if (rc != ANX_OK)
		return rc;
	rc = append_str(out, out_len, &idx, ",\"profile\":\"");
	if (rc != ANX_OK)
		return rc;
	rc = append_str(out, out_len, &idx, report->profile);
	if (rc != ANX_OK)
		return rc;
	rc = append_str(out, out_len, &idx, "\",\"tests_passed\":");
	if (rc != ANX_OK)
		return rc;
	rc = append_u32(out, out_len, &idx, report->tests_passed);
	if (rc != ANX_OK)
		return rc;
	rc = append_str(out, out_len, &idx, ",\"tests_failed\":");
	if (rc != ANX_OK)
		return rc;
	rc = append_u32(out, out_len, &idx, report->tests_failed);
	if (rc != ANX_OK)
		return rc;
	rc = append_str(out, out_len, &idx, ",\"perf_budget_ms\":");
	if (rc != ANX_OK)
		return rc;
	rc = append_u32(out, out_len, &idx, report->perf_budget_ms);
	if (rc != ANX_OK)
		return rc;
	rc = append_str(out, out_len, &idx, ",\"perf_measured_ms\":");
	if (rc != ANX_OK)
		return rc;
	rc = append_u32(out, out_len, &idx, report->perf_measured_ms);
	if (rc != ANX_OK)
		return rc;
	return append_char(out, out_len, &idx, '}');
}

int anx_conformance_emit_delta_json(const struct anx_conformance_delta *delta,
				    char *out, uint32_t out_len)
{
	uint32_t idx = 0;
	int rc;

	if (!delta || !out || out_len == 0)
		return ANX_EINVAL;
	out[0] = '\0';

	rc = append_str(out, out_len, &idx, "{\"passed_delta\":");
	if (rc != ANX_OK)
		return rc;
	rc = append_i32(out, out_len, &idx, delta->passed_delta);
	if (rc != ANX_OK)
		return rc;
	rc = append_str(out, out_len, &idx, ",\"failed_delta\":");
	if (rc != ANX_OK)
		return rc;
	rc = append_i32(out, out_len, &idx, delta->failed_delta);
	if (rc != ANX_OK)
		return rc;
	rc = append_str(out, out_len, &idx, ",\"perf_delta_ms\":");
	if (rc != ANX_OK)
		return rc;
	rc = append_i32(out, out_len, &idx, delta->perf_delta_ms);
	if (rc != ANX_OK)
		return rc;
	rc = append_str(out, out_len, &idx, ",\"threshold_breached\":");
	if (rc != ANX_OK)
		return rc;
	rc = append_str(out, out_len, &idx,
			delta->threshold_breached ? "true" : "false");
	if (rc != ANX_OK)
		return rc;
	return append_char(out, out_len, &idx, '}');
}

int anx_conformance_parse_report_json(const char *json,
			      struct anx_conformance_report *out)
{
	int rc;

	if (!json || !out)
		return ANX_EINVAL;
	anx_memset(out, 0, sizeof(*out));

	rc = parse_u32_value(json, "\"schema_version\"", &out->schema_version);
	if (rc != ANX_OK)
		return rc;
	rc = parse_quoted_value(json, "\"profile\"", out->profile,
				 sizeof(out->profile));
	if (rc != ANX_OK)
		return rc;
	rc = parse_u32_value(json, "\"tests_passed\"", &out->tests_passed);
	if (rc != ANX_OK)
		return rc;
	rc = parse_u32_value(json, "\"tests_failed\"", &out->tests_failed);
	if (rc != ANX_OK)
		return rc;
	rc = parse_u32_value(json, "\"perf_budget_ms\"", &out->perf_budget_ms);
	if (rc != ANX_OK)
		return rc;
	rc = parse_u32_value(json, "\"perf_measured_ms\"", &out->perf_measured_ms);
	if (rc != ANX_OK)
		return rc;

	return anx_conformance_report_validate(out);
}
