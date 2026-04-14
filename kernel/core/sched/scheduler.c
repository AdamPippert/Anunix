/*
 * scheduler.c — Unified Scheduler implementation.
 *
 * Priority-based queue scheduler. Each queue class has its own
 * list, and entries are ordered by priority (highest first).
 */

#include <anx/types.h>
#include <anx/sched.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/arch.h>
#include <anx/spinlock.h>

/* Per-queue state */
static struct {
	struct anx_list_head head;
	struct anx_spinlock lock;
	uint32_t depth;
} queues[ANX_QUEUE_CLASS_COUNT];

void anx_sched_init(void)
{
	uint32_t i;
	for (i = 0; i < ANX_QUEUE_CLASS_COUNT; i++) {
		anx_list_init(&queues[i].head);
		anx_spin_init(&queues[i].lock);
		queues[i].depth = 0;
	}
}

int anx_sched_enqueue(const anx_cid_t *cell_id,
		      enum anx_queue_class queue,
		      enum anx_sched_priority priority)
{
	struct anx_sched_entry *entry;
	struct anx_list_head *pos;

	if (!cell_id)
		return ANX_EINVAL;
	if ((int)queue < 0 || queue >= ANX_QUEUE_CLASS_COUNT)
		return ANX_EINVAL;

	entry = anx_zalloc(sizeof(*entry));
	if (!entry)
		return ANX_ENOMEM;

	entry->cell_id = *cell_id;
	entry->queue = queue;
	entry->priority = priority;
	entry->enqueued_at = arch_time_now();
	anx_list_init(&entry->queue_link);

	anx_spin_lock(&queues[queue].lock);

	/*
	 * Insert in priority order (highest priority at head).
	 * Walk from head, insert before the first lower-priority entry.
	 */
	pos = queues[queue].head.next;
	while (pos != &queues[queue].head) {
		struct anx_sched_entry *existing;

		existing = ANX_LIST_ENTRY(pos, struct anx_sched_entry,
					 queue_link);
		if (existing->priority < priority)
			break;
		pos = pos->next;
	}

	/* Insert before 'pos' (which is the first lower-priority entry) */
	entry->queue_link.next = pos;
	entry->queue_link.prev = pos->prev;
	pos->prev->next = &entry->queue_link;
	pos->prev = &entry->queue_link;

	queues[queue].depth++;
	anx_spin_unlock(&queues[queue].lock);

	return ANX_OK;
}

int anx_sched_dequeue(enum anx_queue_class queue,
		      anx_cid_t *cell_id_out)
{
	struct anx_sched_entry *entry;

	if (!cell_id_out)
		return ANX_EINVAL;
	if ((int)queue < 0 || queue >= ANX_QUEUE_CLASS_COUNT)
		return ANX_EINVAL;

	anx_spin_lock(&queues[queue].lock);

	if (anx_list_empty(&queues[queue].head)) {
		anx_spin_unlock(&queues[queue].lock);
		return ANX_ENOENT;
	}

	/* Take from head (highest priority) */
	entry = ANX_LIST_ENTRY(queues[queue].head.next,
			       struct anx_sched_entry, queue_link);
	anx_list_del(&entry->queue_link);
	queues[queue].depth--;

	anx_spin_unlock(&queues[queue].lock);

	*cell_id_out = entry->cell_id;
	anx_free(entry);
	return ANX_OK;
}

uint32_t anx_sched_queue_depth(enum anx_queue_class queue)
{
	if ((int)queue < 0 || queue >= ANX_QUEUE_CLASS_COUNT)
		return 0;
	return queues[queue].depth;
}

int anx_sched_cancel(const anx_cid_t *cell_id)
{
	uint32_t i;

	if (!cell_id)
		return ANX_EINVAL;

	for (i = 0; i < ANX_QUEUE_CLASS_COUNT; i++) {
		struct anx_list_head *pos, *tmp;

		anx_spin_lock(&queues[i].lock);

		ANX_LIST_FOR_EACH_SAFE(pos, tmp, &queues[i].head) {
			struct anx_sched_entry *entry;

			entry = ANX_LIST_ENTRY(pos, struct anx_sched_entry,
					       queue_link);
			if (anx_uuid_compare(&entry->cell_id, cell_id) == 0) {
				anx_list_del(&entry->queue_link);
				queues[i].depth--;
				anx_spin_unlock(&queues[i].lock);
				anx_free(entry);
				return ANX_OK;
			}
		}

		anx_spin_unlock(&queues[i].lock);
	}

	return ANX_ENOENT;
}
