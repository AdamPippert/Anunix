/*
 * anx/sched.h — Unified Scheduler (RFC-0005 Section 15).
 *
 * The Unified Scheduler binds routed plans to resources:
 * CPU, GPU/NPU, memory tiers, queue classes, and network budgets.
 */

#ifndef ANX_SCHED_H
#define ANX_SCHED_H

#include <anx/types.h>
#include <anx/list.h>
#include <anx/cell.h>

/* --- Queue classes (RFC-0005 Section 15.3) --- */

enum anx_queue_class {
	ANX_QUEUE_INTERACTIVE,
	ANX_QUEUE_BACKGROUND,
	ANX_QUEUE_LATENCY_SENSITIVE,
	ANX_QUEUE_BATCH,
	ANX_QUEUE_VALIDATION,
	ANX_QUEUE_REPLICATION,
	ANX_QUEUE_CLASS_COUNT,
};

/* --- Priority levels --- */

enum anx_sched_priority {
	ANX_PRIO_LOW,
	ANX_PRIO_NORMAL,
	ANX_PRIO_HIGH,
	ANX_PRIO_CRITICAL,
};

/* --- Scheduler entry --- */

struct anx_sched_entry {
	anx_cid_t cell_id;
	enum anx_queue_class queue;
	enum anx_sched_priority priority;
	anx_time_t enqueued_at;
	struct anx_list_head queue_link;
};

/* --- Scheduler API --- */

/* Initialize the scheduler */
void anx_sched_init(void);

/* Enqueue a cell for execution */
int anx_sched_enqueue(const anx_cid_t *cell_id,
		      enum anx_queue_class queue,
		      enum anx_sched_priority priority);

/* Dequeue the highest priority cell from a queue class */
int anx_sched_dequeue(enum anx_queue_class queue,
		      anx_cid_t *cell_id_out);

/* Get the number of entries in a queue */
uint32_t anx_sched_queue_depth(enum anx_queue_class queue);

/* Remove a specific cell from any queue */
int anx_sched_cancel(const anx_cid_t *cell_id);

#endif /* ANX_SCHED_H */
