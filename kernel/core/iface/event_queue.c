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

#define EV_RING_SIZE  256u   /* must be power of two */
#define EV_RING_MASK  (EV_RING_SIZE - 1)

static struct anx_event  ev_ring[EV_RING_SIZE];
static uint32_t          ev_write;   /* producer index (never wraps to 0 mod SIZE) */
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

static struct anx_ev_sub subs[EV_SUBS_MAX];
static uint32_t          sub_count;
static struct anx_spinlock sub_lock;

/* ------------------------------------------------------------------ */
/* Internal init (called by anx_iface_init)                            */
/* ------------------------------------------------------------------ */

/* Not exported — iface.c calls via anx_iface_init() which calls this
 * implicitly through the static zero-init of ev_write and sub_count.
 * Locks are explicitly initialised here for clarity.                  */
static void __attribute__((constructor)) event_queue_init(void)
{
	anx_spin_init(&ev_lock);
	anx_spin_init(&sub_lock);
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
	slot = ev_write & EV_RING_MASK;
	ev_ring[slot] = *ev;
	ev_write++;
	anx_spin_unlock_irqrestore(&ev_lock, flags);

	return ANX_OK;
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

	if (!out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&sub_lock, &flags);

	for (i = 0; i < sub_count; i++) {
		struct anx_ev_sub *sub = &subs[i];

		if (!sub->active)
			continue;
		if (anx_uuid_compare(&sub->cell_cid, &cell_cid) != 0)
			continue;

		/* Walk forward from sub->read_idx looking for a matching event */
		while (sub->read_idx != ev_write) {
			uint32_t slot = sub->read_idx & EV_RING_MASK;
			struct anx_event *ev = &ev_ring[slot];

			sub->read_idx++;

			/* Is this event addressed to a surface this cell subscribes to? */
			if (anx_uuid_compare(&ev->target_surf,
			                     &sub->surf_oid) == 0) {
				*out = *ev;
				anx_spin_unlock_irqrestore(&sub_lock, flags);
				return ANX_OK;
			}
		}
	}

	anx_spin_unlock_irqrestore(&sub_lock, flags);
	return ANX_ENOENT;  /* no pending events */
}
