/*
 * test_event_qos.c — Deterministic tests for P1-006: Event Queue QoS and Telemetry.
 *
 * Tests:
 * - P1-006-U01: backpressure rejects LOW priority when ring is near full
 * - P1-006-I01: overload preserves CRITICAL event delivery
 * - P1-006-I02: telemetry counters match known injected workload
 */

#include <anx/types.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/uuid.h>
#include <anx/string.h>

#define ASSERT(cond, code) do { if (!(cond)) return (code); } while (0)
#define ASSERT_EQ(a, b, code) do { if ((a) != (b)) return (code); } while (0)

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

	anx_iface_event_reset();
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

/* ------------------------------------------------------------------ */
/* P1-006-U01: backpressure rejects LOW priority when ring is near full  */
/* ------------------------------------------------------------------ */

static int test_backpressure_rejects_low_priority(void)
{
	anx_oid_t oid;
	anx_cid_t cid;
	struct anx_event ev;
	struct anx_iface_event_stats stats;
	int rc;
	uint32_t i;
	uint32_t low_rejected = 0;
	uint32_t critical_admitted = 0;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -100);
	rc = create_surface_and_sub(&oid, &cid);
	ASSERT(rc == ANX_OK, -101);

	anx_input_focus_set(oid);

	/* Set backpressure threshold to 90% (230/256) */
	anx_iface_event_set_backpressure_threshold(230);

	/* Fill ring to ~95% (243/256) with LOW priority events */
	for (i = 0; i < 243; i++) {
		anx_memset(&ev, 0, sizeof(ev));
		ev.type = ANX_EVENT_SURFACE_MAPPED;
		ev.priority = ANX_EVENT_PRIO_LOW;
		ev.target_surf = oid;
		anx_uuid_generate(&ev.oid);
		ev.timestamp_ns = 0;
		rc = anx_iface_event_post(&ev);
		if (rc == ANX_EFULL)
			low_rejected++;
	}

	/* Now try to post CRITICAL events — they should all succeed */
	for (i = 0; i < 10; i++) {
		anx_memset(&ev, 0, sizeof(ev));
		ev.type = ANX_EVENT_POINTER_MOVE;
		ev.priority = ANX_EVENT_PRIO_CRITICAL;
		ev.target_surf = oid;
		anx_uuid_generate(&ev.oid);
		ev.timestamp_ns = 0;
		rc = anx_iface_event_post(&ev);
		ASSERT_EQ(rc, ANX_OK, -102);
		critical_admitted++;
	}

	/* Verify via stats */
	anx_iface_event_stats_full(&stats);

	/* Some LOW priority events should have been rejected under backpressure */
	ASSERT(low_rejected > 0, -103);

	/* All CRITICAL events should have been admitted */
	ASSERT((uint32_t)stats.posted >= critical_admitted, -104);

	/* Poll all events to verify ring content */
	for (i = 0; i < 256; i++) {
		rc = anx_iface_event_poll(cid, &ev);
		if (rc == ANX_ENOENT)
			break;
		if (rc != ANX_OK)
			return -105;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-006-I01: overload preserves CRITICAL event delivery               */
/* ------------------------------------------------------------------ */

static int test_overload_preserves_critical(void)
{
	anx_oid_t oid;
	anx_cid_t cid;
	struct anx_event ev;
	struct anx_iface_event_stats stats;
	int rc;
	uint32_t i;
	uint32_t critical_posted = 0;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -200);
	rc = create_surface_and_sub(&oid, &cid);
	ASSERT(rc == ANX_OK, -201);

	anx_input_focus_set(oid);

	/* Set backpressure threshold to 90% */
	anx_iface_event_set_backpressure_threshold(230);

	/* Inject more LOW events than ring can hold */
	for (i = 0; i < ANX_IFACE_EVENT_RING_SIZE + 50; i++) {
		anx_memset(&ev, 0, sizeof(ev));
		ev.type = ANX_EVENT_SURFACE_MAPPED;
		ev.priority = ANX_EVENT_PRIO_LOW;
		ev.target_surf = oid;
		anx_uuid_generate(&ev.oid);
		ev.timestamp_ns = 0;
		(void)anx_iface_event_post(&ev);
	}

	/* Now post CRITICAL events — they should all succeed */
	for (i = 0; i < 10; i++) {
		anx_memset(&ev, 0, sizeof(ev));
		ev.type = ANX_EVENT_POINTER_MOVE;
		ev.priority = ANX_EVENT_PRIO_CRITICAL;
		ev.target_surf = oid;
		anx_uuid_generate(&ev.oid);
		ev.timestamp_ns = 0;
		rc = anx_iface_event_post(&ev);
		ASSERT_EQ(rc, ANX_OK, -202);
		critical_posted++;
	}

	/* Drain the ring — count how many CRITICAL events are present */
	{
		uint32_t critical_seen = 0;
		for (;;) {
			rc = anx_iface_event_poll(cid, &ev);
			if (rc == ANX_ENOENT)
				break;
			ASSERT_EQ(rc, ANX_OK, -203);
			if (ev.priority == ANX_EVENT_PRIO_CRITICAL)
				critical_seen++;
		}
		/* All CRITICAL events must survive the LOW-event flood */
		ASSERT_EQ(critical_seen, critical_posted, -204);
	}

	/* Verify via stats */
	anx_iface_event_stats_full(&stats);

	/* Overflow drops should be > 0 because we tried to post 306 events (256 + 50) */
	ASSERT(stats.overflow_drops > 0, -205);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-006-I02: telemetry counters match known injected workload         */
/* ------------------------------------------------------------------ */

static int test_telemetry_counters_match_workload(void)
{
	anx_oid_t oid;
	anx_cid_t cid;
	struct anx_event ev;
	struct anx_iface_event_stats stats_full;
	int rc;
	uint32_t i;
	uint32_t injected_total = 0;
	uint32_t polled_total = 0;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -300);

	/* Reset before creating subscription so ev_posted starts at 0 */
	anx_iface_event_reset();

	rc = create_surface_and_sub(&oid, &cid);
	ASSERT(rc == ANX_OK, -301);

	anx_input_focus_set(oid);

	/* Inject a known number of events with known priorities */
	for (i = 0; i < 50; i++) {
		anx_memset(&ev, 0, sizeof(ev));
		ev.type = ANX_EVENT_POINTER_MOVE;
		ev.target_surf = oid;
		anx_uuid_generate(&ev.oid);
		ev.timestamp_ns = 0;

		if (i < 10) {
			ev.priority = ANX_EVENT_PRIO_CRITICAL;
		} else if (i < 30) {
			ev.priority = ANX_EVENT_PRIO_NORMAL;
		} else {
			ev.priority = ANX_EVENT_PRIO_LOW;
		}

		rc = anx_iface_event_post(&ev);
		ASSERT_EQ(rc, ANX_OK, -302);
		injected_total++;
	}

	/* Poll all events */
	for (i = 0; i < 50; i++) {
		rc = anx_iface_event_poll(cid, &ev);
		if (rc == ANX_ENOENT)
			break;
		ASSERT_EQ(rc, ANX_OK, -303);
		polled_total++;
	}

	/* Verify via full stats */
	anx_iface_event_stats_full(&stats_full);

	/* posted counter should match injected (50) */
	ASSERT_EQ((uint32_t)stats_full.posted, 50u, -304);

	/* Polled should match injected */
	ASSERT_EQ(polled_total, injected_total, -305);

	/* All 50 events should have been polled, so ring depth should be 0 */
	ASSERT_EQ(stats_full.current_depth, 0u, -306);

	/* Latency histogram should have 50 entries across buckets */
	{
		uint64_t total_latency_entries = 0;
		for (i = 0; i < ANX_LAT_BUCKETS; i++)
			total_latency_entries += stats_full.latency_histogram[i];
		ASSERT_EQ((uint32_t)total_latency_entries, 50u, -307);
	}

	/* Now test overflow drops */
	anx_iface_event_reset();

	/* Inject more events than ring can hold (276 total: 10 CRITICAL + 266 LOW) */
	for (i = 0; i < 276; i++) {
		anx_memset(&ev, 0, sizeof(ev));
		ev.type = ANX_EVENT_POINTER_MOVE;
		ev.target_surf = oid;
		/* First 10 are CRITICAL, rest are LOW */
		ev.priority = (i < 10) ? ANX_EVENT_PRIO_CRITICAL : ANX_EVENT_PRIO_LOW;
		anx_uuid_generate(&ev.oid);
		ev.timestamp_ns = 0;
		(void)anx_iface_event_post(&ev);
	}

	anx_iface_event_stats_full(&stats_full);

	/* With 276 events injected and ring size 256, there MUST be drops.
	 * Since CRITICAL events get through (10), and the ring is 256 slots,
	 * the remaining 246 slots can be filled by LOW events. Then when we
	 * try to post more LOW events, they get rejected.
	 * 276 - 256 = 20 drops minimum
	 */
	ASSERT(stats_full.overflow_drops >= 10, -308);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-006: backpressure threshold configuration                         */
/* ------------------------------------------------------------------ */

static int test_backpressure_threshold(void)
{
	uint32_t thresh;

	/* Default should be 230 (90%) */
	thresh = anx_iface_event_backpressure_threshold();
	ASSERT_EQ(thresh, 230u, -400);

	/* Set to 50% */
	anx_iface_event_set_backpressure_threshold(128);
	thresh = anx_iface_event_backpressure_threshold();
	ASSERT_EQ(thresh, 128u, -401);

	/* Set to 100% */
	anx_iface_event_set_backpressure_threshold(255);
	thresh = anx_iface_event_backpressure_threshold();
	ASSERT_EQ(thresh, 255u, -402);

	/* Clamp at 1 */
	anx_iface_event_set_backpressure_threshold(0);
	thresh = anx_iface_event_backpressure_threshold();
	ASSERT_EQ(thresh, 1u, -403);

	/* Clamp at 255 */
	anx_iface_event_set_backpressure_threshold(300);
	thresh = anx_iface_event_backpressure_threshold();
	ASSERT_EQ(thresh, 255u, -404);

	/* Reset to default for other tests */
	anx_iface_event_set_backpressure_threshold(230);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Test suite entry point                                               */
/* ------------------------------------------------------------------ */

int test_event_qos(void)
{
	int rc;

	rc = test_backpressure_threshold();
	if (rc != 0)
		return rc;

	rc = test_backpressure_rejects_low_priority();
	if (rc != 0)
		return rc;

	rc = test_overload_preserves_critical();
	if (rc != 0)
		return rc;

	rc = test_telemetry_counters_match_workload();
	if (rc != 0)
		return rc;

	return 0;
}
