/*
 * conformance_gate.c — Graphical userspace conformance gate (P2-004).
 */

#include <anx/conformance_gate.h>
#include <anx/string.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Fixture corpus                                                       */
/* ------------------------------------------------------------------ */

static struct anx_gate_fixture corpus[ANX_GATE_CORPUS_MAX];
static uint32_t                corpus_count;

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

void
anx_gate_init(void)
{
	anx_memset(corpus, 0, sizeof(corpus));
	corpus_count = 0;
}

/* ------------------------------------------------------------------ */
/* Register                                                             */
/* ------------------------------------------------------------------ */

int
anx_gate_register(const char *name, anx_gate_fixture_fn fn)
{
	struct anx_gate_fixture *f;

	if (!name || !fn)
		return ANX_EINVAL;

	if (corpus_count >= ANX_GATE_CORPUS_MAX)
		return ANX_EFULL;

	f = &corpus[corpus_count++];
	anx_strlcpy(f->name, name, ANX_GATE_FIXTURE_NAME_MAX);
	f->fn     = fn;
	f->active = true;

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Run                                                                  */
/* ------------------------------------------------------------------ */

/*
 * Simple deterministic ordering: with a seed, we run fixtures in a
 * fixed permutation derived from the seed via a linear congruential step.
 * Same seed → same permutation → same report.
 */
static uint32_t
lcg_next(uint32_t state)
{
	return state * 1664525u + 1013904223u;
}

int
anx_gate_run(uint32_t seed, struct anx_gate_report *report_out)
{
	uint32_t order[ANX_GATE_CORPUS_MAX];
	uint32_t i, j, tmp, rng;
	int rc;

	if (!report_out)
		return ANX_EINVAL;

	anx_memset(report_out, 0, sizeof(*report_out));
	report_out->seed = seed;

	if (corpus_count == 0)
		return ANX_OK;

	/* build identity permutation then Fisher-Yates with LCG */
	for (i = 0; i < corpus_count; i++)
		order[i] = i;

	rng = seed;
	for (i = corpus_count - 1; i > 0; i--) {
		rng = lcg_next(rng);
		j = rng % (i + 1);
		tmp = order[i]; order[i] = order[j]; order[j] = tmp;
	}

	for (i = 0; i < corpus_count; i++) {
		struct anx_gate_fixture        *f  = &corpus[order[i]];
		struct anx_gate_fixture_result *r  = &report_out->results[i];

		if (!f->active)
			continue;

		anx_strlcpy(r->name, f->name, ANX_GATE_FIXTURE_NAME_MAX);
		rc = f->fn();
		r->result_code = rc;
		r->passed      = (rc == 0);

		report_out->total++;
		if (r->passed)
			report_out->passed++;
		else
			report_out->failed++;
	}

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Diff                                                                 */
/* ------------------------------------------------------------------ */

void
anx_gate_diff(const struct anx_gate_report *prev,
              const struct anx_gate_report *curr,
              struct anx_gate_diff *diff_out)
{
	uint32_t i, j;

	if (!prev || !curr || !diff_out)
		return;

	anx_memset(diff_out, 0, sizeof(*diff_out));

	/* for each result in curr, find matching name in prev */
	for (i = 0; i < curr->total && diff_out->count < ANX_GATE_DIFF_MAX; i++) {
		const struct anx_gate_fixture_result *cr = &curr->results[i];

		for (j = 0; j < prev->total; j++) {
			const struct anx_gate_fixture_result *pr = &prev->results[j];

			if (anx_strcmp(cr->name, pr->name) != 0)
				continue;

			if (cr->result_code == pr->result_code)
				break;   /* same result, no diff entry */

			/* result changed */
			struct anx_gate_diff_entry *d =
				&diff_out->entries[diff_out->count++];

			anx_strlcpy(d->fixture_name, cr->name,
			            ANX_GATE_FIXTURE_NAME_MAX);
			d->prev_code  = pr->result_code;
			d->curr_code  = cr->result_code;
			d->regression = (pr->passed && !cr->passed);
			d->active     = true;
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Threshold check                                                      */
/* ------------------------------------------------------------------ */

int
anx_gate_threshold_check(const struct anx_gate_report *report,
                          uint32_t max_failures,
                          char *reason_out, uint32_t reason_max)
{
	if (!report)
		return ANX_EINVAL;

	if (report->failed <= max_failures)
		return ANX_OK;

	if (reason_out && reason_max > 0) {
		anx_snprintf(reason_out, reason_max,
		             "%u failures exceed threshold of %u",
		             report->failed, max_failures);
	}

	return ANX_EPERM;
}
