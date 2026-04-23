/*
 * event_queue.c — Interface Plane event queue (RFC-0012).
 *
 * Global ring buffer for all interface events.
 * Subscription table maps (surf_oid, cell_cid) pairs.
 * Cells poll for events via anx_iface_event_poll().
 *
 * This design avoids dynamic allocation and IPC until the full
 * cell messaging system is complete.
 *
 * P1-006: Event Queue QoS and Telemetry
 * - Priority classes (CRITICAL > NORMAL > LOW)
 * - Queue depth metrics, drop counters, latency histogram
 * - Backpressure policy with configurable thresholds
 */

#include <anx/interface_plane.h>
#include <anx/arch.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Event ring buffer                                                    */
/* ------------------------------------------------------------------ */

#define EV_RING_SIZE  ANX_IFACE_EVENT_RING_SIZE
#define EV_RING_MASK  (EV_RING_SIZE - 1)

static struct anx_event    ev_ring[EV_RING_SIZE];
static uint32_t            ev_write;   /* producer index */
static struct anx_spinlock ev_lock;

/* ------------------------------------------------------------------ */
/* Subscription table                                                   */
/* ------------------------------------------------------------------ */

#define EV_SUBS_MAX  64

struct anx_ev_sub {
	anx_oid_t  surf_oid;
	anx_cid_t  cell_cid;
	uint32_t   read_idx;  /* next ring index to deliver to this subscriber */
	bool       active;
};

static struct anx_ev_sub   subs[EV_SUBS_MAX];
static uint32_t            sub_count;
static struct anx_spinlock sub_lock;

/* ------------------------------------------------------------------ */
/* Telemetry                                                            */
/* ------------------------------------------------------------------ */

static uint64_t            ev_posted;
static uint64_t            ev_overflow_drops;
static uint64_t            ev_critical_posted;

/* Latency histogram: <1ms, 1-5ms, 5-10ms, >10ms (nanoseconds) */
static uint64_t            ev_latency_hist[ANX_LAT_BUCKETS];

/* Backpressure threshold: fraction of ring (1-255, default 230 = 90%) */
static uint32_t            ev_backpressure_threshold = 230;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/* Compute current ring depth accounting for wraparound */
static uint32_t ev_ring_depth(uint32_t write, uint32_t read)
{
	if (write >= read)
		return write - read;
	/* wrapped */
	return EV_RING_SIZE - (read - write);
}

/* Bucket a latency value into the histogram */
static void ev_bucket_latency(uint64_t latency_ns)
{
	if (latency_ns < ANX_LAT_BUCKET_0_NS)
		ev_latency_hist[0]++;
	else if (latency_ns < ANX_LAT_BUCKET_1_NS)
		ev_latency_hist[1]++;
	else if (latency_ns < ANX_LAT_BUCKET_2_NS)
		ev_latency_hist[2]++;
	else
		ev_latency_hist[3]++;
}

/* Determine if ring is in backpressure state (>threshold% full) */
static bool ev_under_backpressure(void)
{
	uint32_t depth = ev_ring_depth(ev_write, 0);
	uint32_t threshold = (uint32_t)ev_backpressure_threshold * EV_RING_SIZE / 256;
	return depth >= threshold;
}

/* Check if a priority should be rejected under backpressure */
static bool ev_priority_rejected_under_backpressure(enum anx_event_prio prio)
{
	/* CRITICAL always allowed; NORMAL and LOW rejected when backpressured */
	if (prio == ANX_EVENT_PRIO_CRITICAL)
		return false;
	return ev_under_backpressure();
}

void
anx_iface_event_reset(void)
{
	bool flags;
	uint32_t i;

	anx_spin_lock_irqsave(&ev_lock, &flags);
	anx_memset(ev_ring, 0, sizeof(ev_ring));
	ev_write = 0;
	ev_posted = 0;
	ev_overflow_drops = 0;
	ev_critical_posted = 0;
	for (i = 0; i < ANX_LAT_BUCKETS; i++)
		ev_latency_hist[i] = 0;
	anx_spin_unlock_irqrestore(&ev_lock, flags);

	anx_spin_lock_irqsave(&sub_lock, &flags);
	anx_memset(subs, 0, sizeof(subs));
	sub_count = 0;
	anx_spin_unlock_irqrestore(&sub_lock, flags);
}

void
anx_iface_event_stats(struct anx_iface_event_stats *out)
{
	bool flags;

	if (!out)
		return;

	anx_spin_lock_irqsave(&ev_lock, &flags);
	out->posted = ev_posted;
	out->overflow_drops = ev_overflow_drops;
	out->current_depth = 0;  /* legacy API, depth not available here */
	anx_memset(out->latency_histogram, 0, sizeof(out->latency_histogram));
	anx_spin_unlock_irqrestore(&ev_lock, flags);
}

void
anx_iface_event_stats_full(struct anx_iface_event_stats *out)
{
	bool flags;
	uint32_t i;
	uint32_t write_snap;
	uint32_t min_read;

	if (!out)
		return;

	anx_spin_lock_irqsave(&ev_lock, &flags);
	out->posted = ev_posted;
	out->overflow_drops = ev_overflow_drops;
	write_snap = ev_write;
	for (i = 0; i < ANX_LAT_BUCKETS; i++)
		out->latency_histogram[i] = ev_latency_hist[i];
	anx_spin_unlock_irqrestore(&ev_lock, flags);

	/* Depth = unread events for the most-behind active subscriber. */
	min_read = write_snap;
	anx_spin_lock_irqsave(&sub_lock, &flags);
	for (i = 0; i < sub_count; i++) {
		if (subs[i].active && subs[i].read_idx < min_read)
			min_read = subs[i].read_idx;
	}
	anx_spin_unlock_irqrestore(&sub_lock, flags);

	out->current_depth = write_snap - min_read;
}

void
anx_iface_event_set_backpressure_threshold(uint32_t fraction_of_ring)
{
	bool flags;

	/* Clamp to valid range 1-255 */
	if (fraction_of_ring == 0)
		fraction_of_ring = 1;
	if (fraction_of_ring > 255)
		fraction_of_ring = 255;

	anx_spin_lock_irqsave(&ev_lock, &flags);
	ev_backpressure_threshold = fraction_of_ring;
	anx_spin_unlock_irqrestore(&ev_lock, flags);
}

uint32_t
anx_iface_event_backpressure_threshold(void)
{
	bool flags;
	uint32_t val;

	anx_spin_lock_irqsave(&ev_lock, &flags);
	val = ev_backpressure_threshold;
	anx_spin_unlock_irqrestore(&ev_lock, flags);
	return val;
}

/* ------------------------------------------------------------------ */
/* Internal init                                                        */
/* ------------------------------------------------------------------ */

static void __attribute__((constructor)) event_queue_init(void)
{
	anx_spin_init(&ev_lock);
	anx_spin_init(&sub_lock);
	anx_iface_event_reset();
}

/* ------------------------------------------------------------------ */
/* anx_iface_event_post                                                 */
/* ------------------------------------------------------------------ */

int
anx_iface_event_post(struct anx_event *ev)
{
	bool flags;
	uint32_t slot;

	if (!ev)
		return ANX_EINVAL;

	/* Assign an OID and timestamp to this event */
	anx_uuid_generate(&ev->oid);
	ev->timestamp_ns = arch_time_now();

	/* Default priority to CRITICAL for any uninitialized or invalid value.
	 * This preserves backward compatibility: existing code that doesn't set
	 * priority (e.g., input.c) gets CRITICAL (input events) by default.
	 * Callers who want different behavior must explicitly set priority. */
	if (ev->priority > ANX_EVENT_PRIO_LOW)
		ev->priority = ANX_EVENT_PRIO_CRITICAL;

	anx_spin_lock_irqsave(&ev_lock, &flags);
	ev_posted++;
	if (ev->priority == ANX_EVENT_PRIO_CRITICAL)
		ev_critical_posted++;

	/* Ring overflow: CRITICAL events always write, overwriting old slots when full. */
	if (ev_write >= EV_RING_SIZE)
		ev_overflow_drops++;

	/* Backpressure: reject non-CRITICAL events when ring is near capacity. */
	if (ev_priority_rejected_under_backpressure(ev->priority)) {
		ev_overflow_drops++;
		anx_spin_unlock_irqrestore(&ev_lock, flags);
		return ANX_EFULL;
	}

	slot = ev_write & EV_RING_MASK;
	ev_ring[slot] = *ev;
	ev_write++;
	anx_spin_unlock_irqrestore(&ev_lock, flags);

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* anx_iface_event_poll_wm                                             */
/* ------------------------------------------------------------------ */

/*
 * Poll for the next WM-targeted event — i.e., any event whose
 * target_surf is the null OID {0,0}.  Used by anx_wm_run() to
 * receive pointer and key events without a surface subscription.
 * Maintains a separate read cursor so the WM never misses events.
 *
 * P1-006: latency histogram recorded on each delivery.
 */

static uint32_t wm_read_idx;

int
anx_iface_event_poll_wm(struct anx_event *out)
{
	bool flags;
	uint32_t write_snapshot, floor;
	uint64_t latency_ns;

	if (!out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&ev_lock, &flags);

	write_snapshot = ev_write;
	floor = (write_snapshot > EV_RING_SIZE)
		? (write_snapshot - EV_RING_SIZE) : 0;
	if (wm_read_idx < floor)
		wm_read_idx = floor;

	while (wm_read_idx != write_snapshot) {
		uint32_t slot = wm_read_idx & EV_RING_MASK;
		struct anx_event *ev = &ev_ring[slot];

		wm_read_idx++;

		if (ev->target_surf.hi == 0 && ev->target_surf.lo == 0) {
			latency_ns = arch_time_now() - ev->timestamp_ns;
			ev_bucket_latency(latency_ns);

			*out = *ev;
			anx_spin_unlock_irqrestore(&ev_lock, flags);
			return ANX_OK;
		}
	}

	anx_spin_unlock_irqrestore(&ev_lock, flags);
	return ANX_ENOENT;
}

/* ------------------------------------------------------------------ */
/* anx_iface_event_subscribe / unsubscribe                             */
/* ------------------------------------------------------------------ */

int
anx_iface_event_subscribe(anx_oid_t surf_oid, anx_cid_t cell_cid)
{
	uint32_t i;
	bool flags;

	anx_spin_lock_irqsave(&sub_lock, &flags);

	/* Avoid duplicate subscriptions */
	for (i = 0; i < sub_count; i++) {
		if (subs[i].active &&
		    anx_uuid_compare(&subs[i].surf_oid, &surf_oid) == 0 &&
		    anx_uuid_compare(&subs[i].cell_cid, &cell_cid) == 0) {
			anx_spin_unlock_irqrestore(&sub_lock, flags);
			return ANX_EEXIST;
		}
	}

	/* Reuse inactive slot before appending */
	for (i = 0; i < sub_count; i++) {
		if (!subs[i].active)
			goto found;
	}
	if (sub_count >= EV_SUBS_MAX) {
		anx_spin_unlock_irqrestore(&sub_lock, flags);
		return ANX_EFULL;
	}
	i = sub_count++;

found:
	subs[i].surf_oid = surf_oid;
	subs[i].cell_cid = cell_cid;
	subs[i].read_idx = ev_write;  /* start from current tail — no backlog */
	subs[i].active   = true;

	anx_spin_unlock_irqrestore(&sub_lock, flags);
	return ANX_OK;
}

int
anx_iface_event_unsubscribe(anx_oid_t surf_oid, anx_cid_t cell_cid)
{
	uint32_t i;
	bool flags;

	anx_spin_lock_irqsave(&sub_lock, &flags);
	for (i = 0; i < sub_count; i++) {
		if (subs[i].active &&
		    anx_uuid_compare(&subs[i].surf_oid, &surf_oid) == 0 &&
		    anx_uuid_compare(&subs[i].cell_cid, &cell_cid) == 0) {
			subs[i].active = false;
			anx_spin_unlock_irqrestore(&sub_lock, flags);
			return ANX_OK;
		}
	}
	anx_spin_unlock_irqrestore(&sub_lock, flags);
	return ANX_ENOENT;
}

/* ------------------------------------------------------------------ */
/* anx_iface_event_poll                                                 */
/* ------------------------------------------------------------------ */

/*
 * Poll for the next event addressed to cell_cid.
 * P1-006: latency histogram recorded on each delivery.
 * Priority is enforced at post time; poll returns in FIFO order.
 */

int
anx_iface_event_poll(anx_cid_t cell_cid, struct anx_event *out)
{
	uint32_t i;
	bool flags;
	uint32_t write_snapshot;
	uint32_t floor;
	uint64_t latency_ns;

	if (!out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&sub_lock, &flags);

	for (i = 0; i < sub_count; i++) {
		struct anx_ev_sub *sub = &subs[i];

		if (!sub->active)
			continue;
		if (anx_uuid_compare(&sub->cell_cid, &cell_cid) != 0)
			continue;

		write_snapshot = ev_write;
		floor = (write_snapshot > EV_RING_SIZE)
			? (write_snapshot - EV_RING_SIZE)
			: 0;
		if (sub->read_idx < floor)
			sub->read_idx = floor;

		/* Walk forward from sub->read_idx looking for a matching event */
		while (sub->read_idx != write_snapshot) {
			uint32_t slot = sub->read_idx & EV_RING_MASK;
			struct anx_event *ev = &ev_ring[slot];

			sub->read_idx++;

			/* Is this event addressed to a surface this cell subscribes to? */
			if (anx_uuid_compare(&ev->target_surf, &sub->surf_oid) == 0) {
				latency_ns = arch_time_now() - ev->timestamp_ns;
				ev_bucket_latency(latency_ns);
				*out = *ev;
				anx_spin_unlock_irqrestore(&sub_lock, flags);
				return ANX_OK;
			}
		}
	}

	anx_spin_unlock_irqrestore(&sub_lock, flags);
	return ANX_ENOENT;  /* no pending events */
}
