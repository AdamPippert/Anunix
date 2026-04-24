/*
 * anx/conformance_gate.h — Graphical userspace conformance gate (P2-004).
 *
 * Curated fixture corpus runner with deterministic output, diff tracking,
 * and configurable pass/fail threshold enforcement.
 */

#ifndef ANX_CONFORMANCE_GATE_H
#define ANX_CONFORMANCE_GATE_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Fixture corpus                                                       */
/* ------------------------------------------------------------------ */

#define ANX_GATE_FIXTURE_NAME_MAX  64u
#define ANX_GATE_CORPUS_MAX        32u

typedef int (*anx_gate_fixture_fn)(void);   /* 0 = pass, non-0 = fail */

struct anx_gate_fixture {
	char                name[ANX_GATE_FIXTURE_NAME_MAX];
	anx_gate_fixture_fn fn;
	bool                active;
};

/* ------------------------------------------------------------------ */
/* Run result                                                           */
/* ------------------------------------------------------------------ */

struct anx_gate_fixture_result {
	char     name[ANX_GATE_FIXTURE_NAME_MAX];
	int      result_code;   /* 0 = pass */
	bool     passed;
};

#define ANX_GATE_REPORT_MAX  ANX_GATE_CORPUS_MAX

struct anx_gate_report {
	uint32_t                      total;
	uint32_t                      passed;
	uint32_t                      failed;
	struct anx_gate_fixture_result results[ANX_GATE_REPORT_MAX];
	uint32_t seed;           /* run seed for reproducibility */
};

/* ------------------------------------------------------------------ */
/* Diff tracking                                                        */
/* ------------------------------------------------------------------ */

#define ANX_GATE_DIFF_MAX  16u

struct anx_gate_diff_entry {
	char     fixture_name[ANX_GATE_FIXTURE_NAME_MAX];
	int      prev_code;
	int      curr_code;
	bool     regression;   /* was passing, now failing */
	bool     active;
};

struct anx_gate_diff {
	struct anx_gate_diff_entry entries[ANX_GATE_DIFF_MAX];
	uint32_t count;
};

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/* Initialise the conformance gate. */
void anx_gate_init(void);

/* Register a fixture. Returns ANX_EFULL if corpus is full. */
int anx_gate_register(const char *name, anx_gate_fixture_fn fn);

/* Run all registered fixtures with the given seed.
 * Same seed always produces the same report (deterministic). */
int anx_gate_run(uint32_t seed, struct anx_gate_report *report_out);

/* Compute diff between two reports; fills diff_out. */
void anx_gate_diff(const struct anx_gate_report *prev,
                    const struct anx_gate_report *curr,
                    struct anx_gate_diff *diff_out);

/* Check if the report breaches the allowed failure threshold.
 * max_failures: max number of failures before the gate fails.
 * Returns ANX_OK if within threshold, ANX_EPERM with reason if breached. */
int anx_gate_threshold_check(const struct anx_gate_report *report,
                               uint32_t max_failures,
                               char *reason_out, uint32_t reason_max);

#endif /* ANX_CONFORMANCE_GATE_H */
