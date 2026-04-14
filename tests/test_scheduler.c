/*
 * test_scheduler.c — Tests for Unified Scheduler (RFC-0005).
 */

#include <anx/types.h>
#include <anx/sched.h>
#include <anx/uuid.h>

int test_scheduler(void)
{
	anx_cid_t id1, id2, id3;
	anx_cid_t dequeued;
	int ret;

	anx_sched_init();

	anx_uuid_generate(&id1);
	anx_uuid_generate(&id2);
	anx_uuid_generate(&id3);

	/* Enqueue with different priorities */
	ret = anx_sched_enqueue(&id1, ANX_QUEUE_INTERACTIVE, ANX_PRIO_LOW);
	if (ret != ANX_OK)
		return -1;

	ret = anx_sched_enqueue(&id2, ANX_QUEUE_INTERACTIVE, ANX_PRIO_HIGH);
	if (ret != ANX_OK)
		return -2;

	ret = anx_sched_enqueue(&id3, ANX_QUEUE_INTERACTIVE, ANX_PRIO_NORMAL);
	if (ret != ANX_OK)
		return -3;

	/* Queue depth */
	if (anx_sched_queue_depth(ANX_QUEUE_INTERACTIVE) != 3)
		return -4;

	/* Dequeue should return highest priority first (id2) */
	ret = anx_sched_dequeue(ANX_QUEUE_INTERACTIVE, &dequeued);
	if (ret != ANX_OK)
		return -5;
	if (anx_uuid_compare(&dequeued, &id2) != 0)
		return -6;

	/* Next should be normal priority (id3) */
	ret = anx_sched_dequeue(ANX_QUEUE_INTERACTIVE, &dequeued);
	if (ret != ANX_OK)
		return -7;
	if (anx_uuid_compare(&dequeued, &id3) != 0)
		return -8;

	/* Last should be low priority (id1) */
	ret = anx_sched_dequeue(ANX_QUEUE_INTERACTIVE, &dequeued);
	if (ret != ANX_OK)
		return -9;
	if (anx_uuid_compare(&dequeued, &id1) != 0)
		return -10;

	/* Queue should be empty now */
	ret = anx_sched_dequeue(ANX_QUEUE_INTERACTIVE, &dequeued);
	if (ret != ANX_ENOENT)
		return -11;

	/* Test cancel */
	anx_uuid_generate(&id1);
	anx_sched_enqueue(&id1, ANX_QUEUE_BATCH, ANX_PRIO_NORMAL);
	ret = anx_sched_cancel(&id1);
	if (ret != ANX_OK)
		return -12;
	if (anx_sched_queue_depth(ANX_QUEUE_BATCH) != 0)
		return -13;

	return 0;
}
