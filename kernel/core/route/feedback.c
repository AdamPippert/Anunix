/*
 * feedback.c — Route outcome recording (ring buffer).
 *
 * Append-only ring buffer of the last 64 feedback records.
 * No learning logic yet — just recording for future use.
 */

#include <anx/types.h>
#include <anx/route_feedback.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/spinlock.h>

#define FEEDBACK_RING_SIZE	64

static struct anx_route_feedback ring[FEEDBACK_RING_SIZE];
static uint32_t ring_head;	/* next write position */
static uint32_t ring_count;	/* total written (saturates at SIZE) */
static struct anx_spinlock ring_lock;

void anx_route_feedback_init(void)
{
	anx_memset(ring, 0, sizeof(ring));
	ring_head = 0;
	ring_count = 0;
	anx_spin_init(&ring_lock);
}

int anx_route_record_feedback(const struct anx_route_feedback *fb)
{
	if (!fb)
		return ANX_EINVAL;

	anx_spin_lock(&ring_lock);

	ring[ring_head] = *fb;
	ring_head = (ring_head + 1) % FEEDBACK_RING_SIZE;
	if (ring_count < FEEDBACK_RING_SIZE)
		ring_count++;

	anx_spin_unlock(&ring_lock);
	return ANX_OK;
}

int anx_route_get_feedback(const anx_eid_t *engine_id,
			   struct anx_route_feedback *out,
			   uint32_t max_results,
			   uint32_t *found)
{
	uint32_t i, count = 0;

	if (!engine_id || !out || !found)
		return ANX_EINVAL;

	anx_spin_lock(&ring_lock);

	for (i = 0; i < ring_count && count < max_results; i++) {
		uint32_t idx = (ring_head + FEEDBACK_RING_SIZE - 1 - i)
			       % FEEDBACK_RING_SIZE;

		if (anx_uuid_compare(&ring[idx].engine_id, engine_id) == 0)
			out[count++] = ring[idx];
	}

	anx_spin_unlock(&ring_lock);

	*found = count;
	return ANX_OK;
}
