/*
 * diag.c — Crash diagnostics and performance observability (P2-005).
 */

#include <anx/diag.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/arch.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Crash pool                                                           */
/* ------------------------------------------------------------------ */

static struct anx_crash_artifact crash_pool[ANX_DIAG_CRASH_POOL_MAX];
static uint32_t                  crash_head;   /* next slot to write */
static uint32_t                  crash_count;
static struct anx_spinlock       diag_lock;

struct anx_crash_artifact *
anx_diag_fault(const char *fault_type, uint64_t fault_addr,
               const char *subsystem, const char *message)
{
	struct anx_crash_artifact *a;
	bool flags;

	anx_spin_lock_irqsave(&diag_lock, &flags);

	a = &crash_pool[crash_head % ANX_DIAG_CRASH_POOL_MAX];
	crash_head++;
	if (crash_count < ANX_DIAG_CRASH_POOL_MAX)
		crash_count++;

	anx_memset(a, 0, sizeof(*a));
	a->magic        = ANX_DIAG_CRASH_MAGIC;
	a->version      = 1;
	a->fault_addr   = fault_addr;
	a->timestamp_ns = arch_time_now();
	a->parsed       = false;

	anx_strlcpy(a->fault_type, fault_type, ANX_DIAG_FAULT_MAX);
	anx_strlcpy(a->subsystem,  subsystem,  ANX_DIAG_SUBSYS_MAX);
	anx_strlcpy(a->message,    message,    ANX_DIAG_MSG_MAX);

	anx_spin_unlock_irqrestore(&diag_lock, flags);
	return a;
}

int
anx_diag_crash_parse(struct anx_crash_artifact *artifact)
{
	if (!artifact)
		return ANX_EINVAL;
	if (artifact->magic != ANX_DIAG_CRASH_MAGIC || artifact->version != 1)
		return ANX_EINVAL;
	artifact->parsed = true;
	return ANX_OK;
}

struct anx_crash_artifact *
anx_diag_last_crash(void)
{
	bool flags;
	struct anx_crash_artifact *a;

	anx_spin_lock_irqsave(&diag_lock, &flags);
	if (crash_count == 0) {
		anx_spin_unlock_irqrestore(&diag_lock, flags);
		return NULL;
	}
	/* crash_head points to the *next* write slot; last written is head-1 */
	a = &crash_pool[(crash_head - 1) % ANX_DIAG_CRASH_POOL_MAX];
	anx_spin_unlock_irqrestore(&diag_lock, flags);
	return a;
}

void
anx_diag_crash_reset(void)
{
	bool flags;

	anx_spin_lock_irqsave(&diag_lock, &flags);
	anx_memset(crash_pool, 0, sizeof(crash_pool));
	crash_head  = 0;
	crash_count = 0;
	anx_spin_unlock_irqrestore(&diag_lock, flags);
}

/* ------------------------------------------------------------------ */
/* Trace timeline                                                       */
/* ------------------------------------------------------------------ */

static struct anx_trace_phase trace_phases[ANX_TRACE_PHASE_MAX];
static uint32_t               trace_count;
static struct anx_spinlock    trace_lock;

int
anx_trace_begin(const char *name)
{
	uint32_t i;
	bool flags;

	if (!name)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&trace_lock, &flags);

	/* reject duplicate open phase */
	for (i = 0; i < trace_count; i++) {
		if (trace_phases[i].active && !trace_phases[i].complete &&
		    anx_strcmp(trace_phases[i].name, name) == 0) {
			anx_spin_unlock_irqrestore(&trace_lock, flags);
			return ANX_EBUSY;
		}
	}

	if (trace_count >= ANX_TRACE_PHASE_MAX) {
		anx_spin_unlock_irqrestore(&trace_lock, flags);
		return ANX_EFULL;
	}

	anx_memset(&trace_phases[trace_count], 0, sizeof(trace_phases[0]));
	anx_strlcpy(trace_phases[trace_count].name, name, ANX_TRACE_NAME_MAX);
	trace_phases[trace_count].start_ns = arch_time_now();
	trace_phases[trace_count].complete = false;
	trace_phases[trace_count].active   = true;
	trace_count++;

	anx_spin_unlock_irqrestore(&trace_lock, flags);
	return ANX_OK;
}

int
anx_trace_end(const char *name)
{
	uint32_t i;
	bool flags;

	if (!name)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&trace_lock, &flags);

	for (i = 0; i < trace_count; i++) {
		if (trace_phases[i].active && !trace_phases[i].complete &&
		    anx_strcmp(trace_phases[i].name, name) == 0) {
			trace_phases[i].end_ns   = arch_time_now();
			trace_phases[i].complete = true;
			anx_spin_unlock_irqrestore(&trace_lock, flags);
			return ANX_OK;
		}
	}

	anx_spin_unlock_irqrestore(&trace_lock, flags);
	return ANX_ENOENT;
}

int
anx_trace_get_phases(struct anx_trace_phase *out, uint32_t max,
                     uint32_t *count_out)
{
	uint32_t n, i;
	bool flags;

	if (!out || !count_out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&trace_lock, &flags);

	n = (trace_count < max) ? trace_count : max;
	for (i = 0; i < n; i++)
		out[i] = trace_phases[i];
	*count_out = trace_count;

	anx_spin_unlock_irqrestore(&trace_lock, flags);
	return ANX_OK;
}

void
anx_trace_reset(void)
{
	bool flags;

	anx_spin_lock_irqsave(&trace_lock, &flags);
	anx_memset(trace_phases, 0, sizeof(trace_phases));
	trace_count = 0;
	anx_spin_unlock_irqrestore(&trace_lock, flags);
}

/* ------------------------------------------------------------------ */
/* Performance metrics                                                  */
/* ------------------------------------------------------------------ */

static struct anx_perf_metrics metrics_snapshot;
static struct anx_spinlock     metrics_lock;

void
anx_metrics_record(const struct anx_perf_metrics *in)
{
	bool flags;

	if (!in)
		return;

	anx_spin_lock_irqsave(&metrics_lock, &flags);
	metrics_snapshot = *in;
	metrics_snapshot.schema_version = ANX_METRICS_VERSION;
	anx_spin_unlock_irqrestore(&metrics_lock, flags);
}

void
anx_metrics_current(struct anx_perf_metrics *out)
{
	bool flags;

	if (!out)
		return;

	anx_spin_lock_irqsave(&metrics_lock, &flags);
	*out = metrics_snapshot;
	anx_spin_unlock_irqrestore(&metrics_lock, flags);
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

void
anx_diag_init(void)
{
	anx_spin_init(&diag_lock);
	anx_spin_init(&trace_lock);
	anx_spin_init(&metrics_lock);

	anx_memset(crash_pool,      0, sizeof(crash_pool));
	anx_memset(trace_phases,    0, sizeof(trace_phases));
	anx_memset(&metrics_snapshot, 0, sizeof(metrics_snapshot));

	crash_head  = 0;
	crash_count = 0;
	trace_count = 0;

	metrics_snapshot.schema_version = ANX_METRICS_VERSION;
}
