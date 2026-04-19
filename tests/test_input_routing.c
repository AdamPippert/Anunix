/*
 * test_input_routing.c — Deterministic input routing/compositor-adjacent tests.
 *
 * Covers acceptance vectors for anxbrowser graphical userspace P0-004:
 * - key ordering
 * - focus gating
 * - monotonic timestamps
 * - focus-transfer routing
 * - queue overflow/drop policy telemetry
 */

#include <anx/types.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/uuid.h>

#define ASSERT(cond, code) do { if (!(cond)) return (code); } while (0)

static int noop_map(struct anx_surface *surf)
{
	(void)surf;
	return ANX_OK;
}

static int noop_commit(struct anx_surface *surf)
{
	(void)surf;
	return ANX_OK;
}

static void noop_damage(struct anx_surface *surf,
                        int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	(void)surf;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}

static void noop_unmap(struct anx_surface *surf)
{
	(void)surf;
}

static const struct anx_renderer_ops test_renderer_ops = {
	.map    = noop_map,
	.commit = noop_commit,
	.damage = noop_damage,
	.unmap  = noop_unmap,
};

static int test_setup(void)
{
	int rc;

	rc = anx_iface_init();
	if (rc != ANX_OK)
		return rc;

	rc = anx_iface_renderer_register(ANX_ENGINE_RENDERER_HEADLESS,
	                                 &test_renderer_ops,
	                                 "test-headless");
	if (rc != ANX_OK)
		return rc;

	anx_input_stats_reset();
	return ANX_OK;
}

static int create_surface_and_sub(anx_oid_t *oid_out, anx_cid_t *cid_out)
{
	struct anx_surface *surf;
	anx_cid_t cid;
	int rc;

	rc = anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS,
	                              NULL,
	                              0, 0, 100, 100,
	                              &surf);
	if (rc != ANX_OK)
		return rc;

	rc = anx_iface_surface_map(surf);
	if (rc != ANX_OK)
		return rc;

	anx_uuid_generate(&cid);
	rc = anx_iface_event_subscribe(surf->oid, cid);
	if (rc != ANX_OK)
		return rc;

	*oid_out = surf->oid;
	*cid_out = cid;
	return ANX_OK;
}

/* P0-004-U01 */
static int test_key_event_ordering(void)
{
	anx_oid_t oid;
	anx_cid_t cid;
	struct anx_event ev;
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -100);
	rc = create_surface_and_sub(&oid, &cid);
	ASSERT(rc == ANX_OK, -101);

	anx_input_focus_set(oid);

	anx_input_key_down(ANX_KEY_A, 0, 'a');
	anx_input_key_up(ANX_KEY_A, 0);
	anx_input_key_down(ANX_KEY_B, 0, 'b');
	anx_input_key_up(ANX_KEY_B, 0);

	rc = anx_iface_event_poll(cid, &ev);
	ASSERT(rc == ANX_OK && ev.type == ANX_EVENT_KEY_DOWN && ev.data.key.keycode == ANX_KEY_A, -102);
	rc = anx_iface_event_poll(cid, &ev);
	ASSERT(rc == ANX_OK && ev.type == ANX_EVENT_KEY_UP && ev.data.key.keycode == ANX_KEY_A, -103);
	rc = anx_iface_event_poll(cid, &ev);
	ASSERT(rc == ANX_OK && ev.type == ANX_EVENT_KEY_DOWN && ev.data.key.keycode == ANX_KEY_B, -104);
	rc = anx_iface_event_poll(cid, &ev);
	ASSERT(rc == ANX_OK && ev.type == ANX_EVENT_KEY_UP && ev.data.key.keycode == ANX_KEY_B, -105);

	return 0;
}

/* P0-004-U02 */
static int test_focus_gate(void)
{
	anx_oid_t oid;
	anx_cid_t cid;
	struct anx_event ev;
	struct anx_input_stats stats;
	anx_oid_t nil = {0, 0};
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -110);
	rc = create_surface_and_sub(&oid, &cid);
	ASSERT(rc == ANX_OK, -111);

	anx_input_focus_set(nil);
	anx_input_key_down(ANX_KEY_A, 0, 'a');

	anx_input_stats_get(&stats);
	ASSERT(stats.dropped_no_focus == 1, -112);

	rc = anx_iface_event_poll(cid, &ev);
	ASSERT(rc == ANX_ENOENT, -113);

	return 0;
}

/* P0-004-U03 */
static int test_monotonic_timestamps(void)
{
	anx_oid_t oid;
	anx_cid_t cid;
	struct anx_event ev;
	uint64_t prev = 0;
	int i;
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -120);
	rc = create_surface_and_sub(&oid, &cid);
	ASSERT(rc == ANX_OK, -121);

	anx_input_focus_set(oid);

	for (i = 0; i < 8; i++)
		anx_input_key_down(ANX_KEY_A, 0, 'a');

	for (i = 0; i < 8; i++) {
		rc = anx_iface_event_poll(cid, &ev);
		ASSERT(rc == ANX_OK, -122);
		ASSERT(ev.timestamp_ns > prev, -123);
		prev = ev.timestamp_ns;
	}

	return 0;
}

/* P0-004-I01 */
static int test_focus_transfer_routing(void)
{
	anx_oid_t oid_a, oid_b;
	anx_cid_t cid_a, cid_b;
	struct anx_event ev;
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -130);
	rc = create_surface_and_sub(&oid_a, &cid_a);
	ASSERT(rc == ANX_OK, -131);
	rc = create_surface_and_sub(&oid_b, &cid_b);
	ASSERT(rc == ANX_OK, -132);

	anx_input_focus_set(oid_a);
	anx_input_key_down(ANX_KEY_A, 0, 'a');

	rc = anx_iface_event_poll(cid_a, &ev);
	ASSERT(rc == ANX_OK && ev.target_surf.hi == oid_a.hi && ev.target_surf.lo == oid_a.lo, -133);
	rc = anx_iface_event_poll(cid_b, &ev);
	ASSERT(rc == ANX_ENOENT, -134);

	anx_input_focus_set(oid_b);
	anx_input_key_down(ANX_KEY_B, 0, 'b');

	rc = anx_iface_event_poll(cid_b, &ev);
	ASSERT(rc == ANX_OK && ev.target_surf.hi == oid_b.hi && ev.target_surf.lo == oid_b.lo, -135);
	rc = anx_iface_event_poll(cid_a, &ev);
	ASSERT(rc == ANX_ENOENT, -136);

	return 0;
}

/* P0-004-I02 */
static int test_queue_overflow_policy(void)
{
	anx_oid_t oid;
	anx_cid_t cid;
	struct anx_event ev;
	struct anx_iface_event_stats estats;
	uint32_t polled = 0;
	uint32_t inject_count = ANX_IFACE_EVENT_RING_SIZE + 20;
	int rc;
	uint32_t i;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -140);
	rc = create_surface_and_sub(&oid, &cid);
	ASSERT(rc == ANX_OK, -141);

	anx_input_focus_set(oid);
	for (i = 0; i < inject_count; i++)
		anx_input_key_down(ANX_KEY_A, 0, 'a');

	anx_iface_event_stats(&estats);
	ASSERT(estats.overflow_drops == 20, -142);

	while (anx_iface_event_poll(cid, &ev) == ANX_OK)
		polled++;

	ASSERT(polled == ANX_IFACE_EVENT_RING_SIZE, -143);
	return 0;
}

int test_input_routing(void)
{
	int rc;

	rc = test_key_event_ordering();
	if (rc != 0)
		return rc;
	rc = test_focus_gate();
	if (rc != 0)
		return rc;
	rc = test_monotonic_timestamps();
	if (rc != 0)
		return rc;
	rc = test_focus_transfer_routing();
	if (rc != 0)
		return rc;
	rc = test_queue_overflow_policy();
	if (rc != 0)
		return rc;

	return 0;
}
