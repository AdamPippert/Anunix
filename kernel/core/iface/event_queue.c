/*
 * event_queue.c — Interface Plane event queue (RFC-0012).
 *
 * Global ring buffer for all interface events.
 * Subscription table maps (surf_oid, cell_cid) pairs.
 * Cells poll for events via anx_iface_event_poll().
 *
 * This design avoids dynamic allocation and IPC until the full
 * cell messaging system is complete.
 */

#include <anx/interface_plane.h>
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

/* Telemetry */
static uint64_t            ev_posted;
static uint64_t            ev_overflow_drops;

/* ------------------------------------------------------------------ */
/* Surface cursor table (forward-declared for use in event_reset)      */
/* ------------------------------------------------------------------ */

#define SURF_POLL_MAX  32

struct anx_surf_poll_cursor {
	anx_oid_t  surf_oid;
	uint32_t   read_idx;
	bool       active;
};

static struct anx_surf_poll_cursor surf_cursors[SURF_POLL_MAX];
static struct anx_spinlock         sc_lock;

void
anx_iface_event_reset(void)
{
	bool flags;

	anx_spin_lock_irqsave(&ev_lock, &flags);
	anx_memset(ev_ring, 0, sizeof(ev_ring));
	ev_write = 0;
	ev_posted = 0;
	ev_overflow_drops = 0;
	anx_spin_unlock_irqrestore(&ev_lock, flags);

	anx_spin_lock_irqsave(&sub_lock, &flags);
	anx_memset(subs, 0, sizeof(subs));
	sub_count = 0;
	anx_spin_unlock_irqrestore(&sub_lock, flags);

	anx_spin_lock_irqsave(&sc_lock, &flags);
	anx_memset(surf_cursors, 0, sizeof(surf_cursors));
	anx_spin_unlock_irqrestore(&sc_lock, flags);
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
	anx_spin_unlock_irqrestore(&ev_lock, flags);
}

/* ------------------------------------------------------------------ */
/* Internal init                                                        */
/* ------------------------------------------------------------------ */

static void __attribute__((constructor)) event_queue_init(void)
{
	anx_spin_init(&ev_lock);
	anx_spin_init(&sub_lock);
	anx_spin_init(&sc_lock);
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

	/* Assign an OID to this event */
	anx_uuid_generate(&ev->oid);

	anx_spin_lock_irqsave(&ev_lock, &flags);
	ev_posted++;
	if (ev_write >= EV_RING_SIZE)
		ev_overflow_drops++;
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
 */

static uint32_t wm_read_idx;

int
anx_iface_event_poll_wm(struct anx_event *out)
{
	bool flags;
	uint32_t write_snapshot, floor;

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
			*out = *ev;
			anx_spin_unlock_irqrestore(&ev_lock, flags);
			return ANX_OK;
		}
	}

	anx_spin_unlock_irqrestore(&ev_lock, flags);
	return ANX_ENOENT;
}

/* ------------------------------------------------------------------ */
/* anx_iface_event_poll_surf                                           */
/* ------------------------------------------------------------------ */

int
anx_iface_event_poll_surf(anx_oid_t surf_oid, struct anx_event *out)
{
	bool flags;
	uint32_t i, write_snapshot, floor, slot;
	struct anx_surf_poll_cursor *cur = NULL;

	if (!out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&sc_lock, &flags);

	/* Find or create a cursor for this surface */
	for (i = 0; i < SURF_POLL_MAX; i++) {
		if (surf_cursors[i].active &&
		    surf_cursors[i].surf_oid.hi == surf_oid.hi &&
		    surf_cursors[i].surf_oid.lo == surf_oid.lo) {
			cur = &surf_cursors[i];
			break;
		}
	}
	if (!cur) {
		for (i = 0; i < SURF_POLL_MAX; i++) {
			if (!surf_cursors[i].active) {
				cur = &surf_cursors[i];
				cur->surf_oid = surf_oid;
				cur->read_idx = ev_write;
				cur->active   = true;
				break;
			}
		}
	}
	if (!cur) {
		anx_spin_unlock_irqrestore(&sc_lock, flags);
		return ANX_EFULL;
	}

	write_snapshot = ev_write;
	floor = (write_snapshot > EV_RING_SIZE) ? (write_snapshot - EV_RING_SIZE) : 0;
	if (cur->read_idx < floor)
		cur->read_idx = floor;

	while (cur->read_idx != write_snapshot) {
		slot = cur->read_idx & EV_RING_MASK;
		struct anx_event *ev = &ev_ring[slot];

		cur->read_idx++;

		if (ev->target_surf.hi == surf_oid.hi &&
		    ev->target_surf.lo == surf_oid.lo) {
			*out = *ev;
			anx_spin_unlock_irqrestore(&sc_lock, flags);
			return ANX_OK;
		}
	}

	anx_spin_unlock_irqrestore(&sc_lock, flags);
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

int
anx_iface_event_poll(anx_cid_t cell_cid, struct anx_event *out)
{
	uint32_t i;
	bool flags;
	uint32_t write_snapshot;
	uint32_t floor;

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
				*out = *ev;
				anx_spin_unlock_irqrestore(&sub_lock, flags);
				return ANX_OK;
			}
		}
	}

	anx_spin_unlock_irqrestore(&sub_lock, flags);
	return ANX_ENOENT;  /* no pending events */
}
