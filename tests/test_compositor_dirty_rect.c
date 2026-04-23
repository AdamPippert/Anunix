/*
 * test_compositor_dirty_rect.c — Deterministic tests for P1-001: dirty-rect
 * compositor and frame pacing.
 *
 * Tests:
 * - P1-001-U01: damage region merge correctness
 * - P1-001-I01: unchanged frame triggers zero-render fast path
 * - P1-001-I02: commit count decreases under partial updates (only dirty
 *               surfaces are committed per cycle)
 */

#include <anx/types.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/uuid.h>
#include <anx/string.h>

#define ASSERT(cond, code)        do { if (!(cond)) return (code); } while (0)
#define ASSERT_EQ(a, b, code)     do { if ((a) != (b)) return (code); } while (0)

static int noop_map(struct anx_surface *s)    { (void)s; return ANX_OK; }
static int noop_commit(struct anx_surface *s) { (void)s; return ANX_OK; }
static void noop_damage(struct anx_surface *s,
                         int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	(void)s; (void)x; (void)y; (void)w; (void)h;
}
static void noop_unmap(struct anx_surface *s) { (void)s; }

static const struct anx_renderer_ops test_ops = {
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
	                                  &test_ops, "test-headless");
	if (rc != ANX_OK)
		return rc;

	/* compositor_start requires a pre-existing environment */
	rc = anx_iface_env_define("visual-desktop", "anx:schema/env/v1", 0);
	if (rc != ANX_OK && rc != ANX_EEXIST)
		return rc;

	anx_iface_event_reset();
	anx_input_stats_reset();
	return ANX_OK;
}

static int make_surface(uint32_t w, uint32_t h, struct anx_surface **out)
{
	int rc;
	rc = anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS,
	                               NULL, 0, 0, w, h, out);
	if (rc != ANX_OK)
		return rc;
	return anx_iface_surface_map(*out);
}

/* ------------------------------------------------------------------ */
/* P1-001-U01: damage region merge correctness                          */
/* ------------------------------------------------------------------ */

static int test_damage_region_merge(void)
{
	struct anx_surface *surf;
	int32_t  dx, dy;
	uint32_t dw, dh;
	bool     valid;
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -100);

	rc = make_surface(200, 200, &surf);
	ASSERT(rc == ANX_OK, -101);

	/* After map the surface already has full-surface damage.
	 * Clear it by running a compositor cycle. */
	rc = anx_iface_compositor_start("visual-desktop");
	ASSERT(rc == ANX_OK || rc == ANX_EEXIST, -102);
	anx_iface_compositor_tick("visual-desktop", NULL);

	/* Confirm damage cleared after commit */
	anx_iface_surface_damage_query(surf, NULL, NULL, NULL, NULL, &valid);
	ASSERT(!valid, -103);

	/* Post first damage rect: (10, 20, 50, 30) */
	rc = anx_iface_surface_damage(surf, 10, 20, 50, 30);
	ASSERT(rc == ANX_OK, -104);
	anx_iface_surface_damage_query(surf, &dx, &dy, &dw, &dh, &valid);
	ASSERT(valid,      -105);
	ASSERT_EQ(dx, 10,  -106);
	ASSERT_EQ(dy, 20,  -107);
	ASSERT_EQ(dw, 50u, -108);
	ASSERT_EQ(dh, 30u, -109);

	/* Post overlapping second rect: (30, 10, 60, 50) — right and up */
	rc = anx_iface_surface_damage(surf, 30, 10, 60, 50);
	ASSERT(rc == ANX_OK, -110);
	anx_iface_surface_damage_query(surf, &dx, &dy, &dw, &dh, &valid);

	/*
	 * Expected union:
	 *   x: min(10, 30) = 10
	 *   y: min(20, 10) = 10
	 *   right:  max(10+50, 30+60) = max(60, 90) = 90  → w = 90-10 = 80
	 *   bottom: max(20+30, 10+50) = max(50, 60) = 60  → h = 60-10 = 50
	 */
	ASSERT(valid,       -111);
	ASSERT_EQ(dx,  10,  -112);
	ASSERT_EQ(dy,  10,  -113);
	ASSERT_EQ(dw,  80u, -114);
	ASSERT_EQ(dh,  50u, -115);

	/* Post a third non-overlapping rect: (150, 150, 20, 20) */
	rc = anx_iface_surface_damage(surf, 150, 150, 20, 20);
	ASSERT(rc == ANX_OK, -116);
	anx_iface_surface_damage_query(surf, &dx, &dy, &dw, &dh, &valid);

	/*
	 * New union of (10,10,80,50) with (150,150,20,20):
	 *   x: min(10,150) = 10
	 *   y: min(10,150) = 10
	 *   right:  max(10+80, 150+20) = max(90, 170) = 170 → w = 160
	 *   bottom: max(10+50, 150+20) = max(60, 170) = 170 → h = 160
	 */
	ASSERT(valid,        -117);
	ASSERT_EQ(dx,   10,  -118);
	ASSERT_EQ(dy,   10,  -119);
	ASSERT_EQ(dw,  160u, -120);
	ASSERT_EQ(dh,  160u, -121);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-001-I01: unchanged frame triggers zero-render fast path           */
/* ------------------------------------------------------------------ */

static int test_zero_render_fast_path(void)
{
	struct anx_surface *surf;
	struct anx_iface_compositor_stats stats;
	uint32_t committed_a, committed_b;
	bool valid;
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -200);
	rc = make_surface(100, 100, &surf);
	ASSERT(rc == ANX_OK, -201);

	rc = anx_iface_compositor_start("visual-desktop");
	ASSERT(rc == ANX_OK, -202);

	/* First tick: surface has initial full-surface damage — should commit. */
	rc = anx_iface_compositor_tick("visual-desktop", &committed_a);
	ASSERT(rc == ANX_OK, -203);
	ASSERT(committed_a >= 1u, -204);

	/* Surface should now be clean. */
	anx_iface_surface_damage_query(surf, NULL, NULL, NULL, NULL, &valid);
	ASSERT(!valid, -205);

	/* Second tick: no damage posted — should be zero commits (fast path). */
	rc = anx_iface_compositor_tick("visual-desktop", &committed_b);
	ASSERT(rc == ANX_OK, -206);
	ASSERT_EQ(committed_b, 0u, -207);

	/* Verify last_cycle_commits reflects the fast path. */
	rc = anx_iface_compositor_stats("visual-desktop", &stats);
	ASSERT(rc == ANX_OK, -208);
	ASSERT_EQ(stats.last_cycle_commits, 0u, -209);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-001-I02: only dirty surfaces committed per cycle                  */
/* ------------------------------------------------------------------ */

static int test_partial_update_efficiency(void)
{
	struct anx_surface *surf_a, *surf_b;
	uint32_t committed;
	uint32_t count_before_a, count_after_a;
	bool valid;
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -300);
	rc = make_surface(100, 100, &surf_a);
	ASSERT(rc == ANX_OK, -301);
	rc = make_surface(100, 100, &surf_b);
	ASSERT(rc == ANX_OK, -302);

	rc = anx_iface_compositor_start("visual-desktop");
	ASSERT(rc == ANX_OK, -303);

	/* Drain initial damage on both surfaces. */
	anx_iface_compositor_tick("visual-desktop", NULL);

	/* Both surfaces are now clean. */
	anx_iface_surface_damage_query(surf_a, NULL, NULL, NULL, NULL, &valid);
	ASSERT(!valid, -304);
	anx_iface_surface_damage_query(surf_b, NULL, NULL, NULL, NULL, &valid);
	ASSERT(!valid, -305);

	/* Damage only surf_a. */
	rc = anx_iface_surface_damage(surf_a, 0, 0, 50, 50);
	ASSERT(rc == ANX_OK, -306);

	count_before_a = surf_a->commit_count;

	/* Run one cycle — only surf_a should be committed. */
	rc = anx_iface_compositor_tick("visual-desktop", &committed);
	ASSERT(rc == ANX_OK, -307);
	ASSERT_EQ(committed, 1u, -308);

	count_after_a = surf_a->commit_count;
	ASSERT_EQ(count_after_a, count_before_a + 1, -309);

	/* surf_b was NOT dirty so its commit_count must be unchanged. */
	/* (surf_b's commit_count was incremented only during initial drain) */
	anx_iface_surface_damage_query(surf_b, NULL, NULL, NULL, NULL, &valid);
	ASSERT(!valid, -310);

	/* Run another cycle with no damage — zero commits. */
	rc = anx_iface_compositor_tick("visual-desktop", &committed);
	ASSERT(rc == ANX_OK, -311);
	ASSERT_EQ(committed, 0u, -312);

	/* surf_a commit_count unchanged. */
	ASSERT_EQ(surf_a->commit_count, count_after_a, -313);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Test suite entry point                                               */
/* ------------------------------------------------------------------ */

int test_compositor_dirty_rect(void)
{
	int rc;

	rc = test_damage_region_merge();
	if (rc != 0)
		return rc;

	rc = test_zero_render_fast_path();
	if (rc != 0)
		return rc;

	rc = test_partial_update_efficiency();
	if (rc != 0)
		return rc;

	return 0;
}
