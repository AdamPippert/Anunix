/*
 * batch.c — Parallel rollout runner.
 *
 * A batch owns N rollouts and drains them cooperatively through
 * the Unified Scheduler. Each rollout's root cell is enqueued with
 * the configured queue class; the drain loop dequeues a cell,
 * advances its rollout by one inference step, and re-enqueues it
 * if more work remains. This interleaves rollouts so an I/O-bound
 * adapter (remote model calls) does not serialize the whole batch.
 *
 * The implementation stays cooperative — there is no kernel
 * preemption yet — but the scheduler integration means throughput
 * scales directly once async inference is wired in.
 */

#include <anx/types.h>
#include <anx/rlm.h>
#include <anx/sched.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/arch.h>

int anx_rlm_batch_create(const anx_oid_t *prompt_oids, uint32_t count,
			 const struct anx_rlm_config *config,
			 struct anx_rlm_batch **out)
{
	struct anx_rlm_batch *batch;
	uint32_t i;
	int ret;

	if (!prompt_oids || !config || !out)
		return ANX_EINVAL;
	if (count == 0 || count > ANX_RLM_MAX_BATCH)
		return ANX_EINVAL;

	batch = anx_zalloc(sizeof(*batch));
	if (!batch)
		return ANX_ENOMEM;

	for (i = 0; i < count; i++) {
		ret = anx_rlm_rollout_create(&prompt_oids[i], config,
					     &batch->rollouts[i]);
		if (ret != ANX_OK)
			goto undo;
		batch->count++;
	}

	*out = batch;
	return ANX_OK;

undo:
	while (batch->count > 0) {
		batch->count--;
		anx_rlm_rollout_destroy(batch->rollouts[batch->count]);
	}
	anx_free(batch);
	return ret;
}

/* Locate the rollout whose root cell matches cid. Linear scan is
 * fine for bounded batch sizes (≤ ANX_RLM_MAX_BATCH). */
static struct anx_rlm_rollout *batch_find(struct anx_rlm_batch *b,
					  const anx_cid_t *cid)
{
	uint32_t i;

	for (i = 0; i < b->count; i++) {
		struct anx_rlm_rollout *r = b->rollouts[i];

		if (r && anx_uuid_compare(&r->root_cid, cid) == 0)
			return r;
	}
	return NULL;
}

static bool rollout_terminal(const struct anx_rlm_rollout *r)
{
	return r->status == ANX_RLM_COMPLETED ||
	       r->status == ANX_RLM_FAILED ||
	       r->status == ANX_RLM_CANCELLED;
}

int anx_rlm_batch_run(struct anx_rlm_batch *batch)
{
	uint32_t i;

	if (!batch)
		return ANX_EINVAL;

	batch->started_at = arch_time_now();
	batch->completed = 0;
	batch->failed = 0;

	/* Prime the scheduler with every rollout's root cell. */
	for (i = 0; i < batch->count; i++) {
		struct anx_rlm_rollout *r = batch->rollouts[i];

		if (!r || rollout_terminal(r))
			continue;
		anx_sched_enqueue(&r->root_cid,
				  r->config.queue,
				  r->config.priority);
	}

	/*
	 * Drain round-robin across every queue class the config may
	 * have selected. We iterate classes in priority-friendly
	 * order so batches with mixed classes still make progress.
	 */
	for (;;) {
		bool made_progress = false;
		enum anx_queue_class qc;

		for (qc = 0; qc < ANX_QUEUE_CLASS_COUNT; qc++) {
			anx_cid_t cid;
			struct anx_rlm_rollout *r;
			int ret;

			if (anx_sched_queue_depth(qc) == 0)
				continue;

			ret = anx_sched_dequeue(qc, &cid);
			if (ret != ANX_OK)
				continue;

			r = batch_find(batch, &cid);
			if (!r)
				continue;

			made_progress = true;

			if (rollout_terminal(r))
				continue;

			anx_rlm_rollout_step(r);

			if (!rollout_terminal(r)) {
				anx_sched_enqueue(&r->root_cid, r->config.queue,
						  r->config.priority);
			}
		}

		if (!made_progress)
			break;
	}

	/* Tally outcomes and finalize each rollout's trace. */
	for (i = 0; i < batch->count; i++) {
		struct anx_rlm_rollout *r = batch->rollouts[i];

		if (!r)
			continue;
		if (r->status == ANX_RLM_COMPLETED)
			batch->completed++;
		else if (r->status == ANX_RLM_FAILED)
			batch->failed++;
	}

	batch->completed_at = arch_time_now();

	return (batch->failed == 0) ? ANX_OK : ANX_EIO;
}

uint32_t anx_rlm_batch_tokens_per_second(const struct anx_rlm_batch *batch)
{
	uint64_t elapsed_ns;
	uint64_t total_tokens = 0;
	uint32_t i;

	if (!batch || batch->started_at == 0 || batch->completed_at == 0)
		return 0;
	if (batch->completed_at <= batch->started_at)
		return 0;

	elapsed_ns = (uint64_t)(batch->completed_at - batch->started_at);

	for (i = 0; i < batch->count; i++) {
		const struct anx_rlm_rollout *r = batch->rollouts[i];

		if (!r)
			continue;
		total_tokens += (uint64_t)r->total_output_tokens;
	}

	if (total_tokens == 0)
		return 0;

	/* tokens * 1e9 / elapsed_ns, guarded against overflow. */
	return (uint32_t)((total_tokens * 1000000000ULL) / elapsed_ns);
}

void anx_rlm_batch_destroy(struct anx_rlm_batch *batch)
{
	uint32_t i;

	if (!batch)
		return;

	for (i = 0; i < batch->count; i++) {
		if (batch->rollouts[i])
			anx_rlm_rollout_destroy(batch->rollouts[i]);
	}

	anx_free(batch);
}
