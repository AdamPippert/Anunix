/*
 * anx/diag.h — Crash diagnostics and performance observability (P2-005).
 *
 * Three facilities:
 *   1. Crash artifacts: structured fault records parseable by tooling.
 *   2. Trace timeline: named phase records for frame/input/network paths.
 *   3. Performance metrics: versioned counters exposed to monitoring tools.
 */

#ifndef ANX_DIAG_H
#define ANX_DIAG_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Crash artifact                                                       */
/* ------------------------------------------------------------------ */

#define ANX_DIAG_CRASH_MAGIC    0xAC120000u
#define ANX_DIAG_FAULT_MAX       64u
#define ANX_DIAG_SUBSYS_MAX      64u
#define ANX_DIAG_MSG_MAX        256u
#define ANX_DIAG_CRASH_POOL_MAX   4u

struct anx_crash_artifact {
	uint32_t magic;                     /* ANX_DIAG_CRASH_MAGIC if valid */
	uint32_t version;                   /* artifact format version (1)   */
	char     fault_type[ANX_DIAG_FAULT_MAX];
	uint64_t fault_addr;
	char     subsystem[ANX_DIAG_SUBSYS_MAX];
	char     message[ANX_DIAG_MSG_MAX];
	uint64_t timestamp_ns;
	bool     parsed;                    /* set by anx_diag_crash_parse() */
};

/* Record a fault into the next available pool slot.
 * Returns a pointer to the recorded artifact, or NULL if pool full. */
struct anx_crash_artifact *anx_diag_fault(const char *fault_type,
                                           uint64_t fault_addr,
                                           const char *subsystem,
                                           const char *message);

/* Validate an artifact (magic + version check); sets parsed = true.
 * Returns ANX_OK if valid, ANX_EINVAL if corrupted. */
int anx_diag_crash_parse(struct anx_crash_artifact *artifact);

/* Return the most recently recorded artifact, or NULL if none. */
struct anx_crash_artifact *anx_diag_last_crash(void);

/* Clear the crash pool. */
void anx_diag_crash_reset(void);

/* ------------------------------------------------------------------ */
/* Trace timeline                                                       */
/* ------------------------------------------------------------------ */

#define ANX_TRACE_NAME_MAX   64u
#define ANX_TRACE_PHASE_MAX  16u

struct anx_trace_phase {
	char     name[ANX_TRACE_NAME_MAX];
	uint64_t start_ns;
	uint64_t end_ns;
	bool     complete;
	bool     active;
};

/* Begin a named trace phase. Returns ANX_OK or ANX_EFULL. */
int anx_trace_begin(const char *name);

/* End a named trace phase. Returns ANX_OK or ANX_ENOENT. */
int anx_trace_end(const char *name);

/* Get all recorded phases (up to max); fills count_out. */
int anx_trace_get_phases(struct anx_trace_phase *out, uint32_t max,
                          uint32_t *count_out);

/* Clear all trace state. */
void anx_trace_reset(void);

/* ------------------------------------------------------------------ */
/* Performance metrics                                                  */
/* ------------------------------------------------------------------ */

#define ANX_METRICS_VERSION  1u

struct anx_perf_metrics {
	uint32_t schema_version;       /* must equal ANX_METRICS_VERSION   */
	uint64_t frame_time_ns;        /* last frame render time            */
	uint64_t input_latency_ns;     /* last input dispatch latency       */
	uint64_t memory_used_bytes;    /* kernel heap in use                */
	uint32_t cpu_load_pct;         /* 0-100                             */
	uint32_t active_surfaces;      /* surfaces currently visible        */
};

/* Snapshot current metrics into out. schema_version is always set. */
void anx_metrics_record(const struct anx_perf_metrics *in);

/* Read back the last recorded metrics snapshot. */
void anx_metrics_current(struct anx_perf_metrics *out);

/* Initialise / reset all diagnostics state. */
void anx_diag_init(void);

#endif /* ANX_DIAG_H */
