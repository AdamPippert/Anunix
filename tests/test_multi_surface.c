/*
 * test_multi_surface.c — Deterministic tests for P1-002: multi-window/
 * surface model hardening.
 *
 * Tests:
 * - P1-002-U01: z-order operations produce expected surface ordering
 * - P1-002-I01: focus follows active top-level surface after raise
 * - P1-002-I02: closing active surface transfers focus deterministically
 *               with parent-child cascade destroy
 */

#include <anx/types.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/uuid.h>
#include <anx/string.h>

#define ASSERT(cond, code)    do { if (!(cond)) return (code); } while (0)
#define ASSERT_EQ(a, b, code) do { if ((a) != (b)) return (code); } while (0)

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

	rc = anx_iface_env_define("visual-desktop", "anx:schema/env/v1", 0);
	if (rc != ANX_OK && rc != ANX_EEXIST)
		return rc;

	anx_iface_event_reset();
	anx_input_stats_reset();
	return ANX_OK;
}

static int make_surface(struct anx_surface **out)
{
	int rc;

	rc = anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS,
	                               NULL, 0, 0, 100, 100, out);
	if (rc != ANX_OK)
		return rc;
	return anx_iface_surface_map(*out);
}

/* ------------------------------------------------------------------ */
/* P1-002-U01: z-order operations produce expected ordering            */
/* ------------------------------------------------------------------ */

static int test_z_order_operations(void)
{
	struct anx_surface *sa, *sb, *sc;
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -100);

	rc = make_surface(&sa);
	ASSERT(rc == ANX_OK, -101);
	rc = make_surface(&sb);
	ASSERT(rc == ANX_OK, -102);
	rc = make_surface(&sc);
	ASSERT(rc == ANX_OK, -103);

	/*
	 * After creation (newest at front): [sc, sb, sa]
	 * z_order assigned at create time: sa=0, sb=1, sc=2.
	 * No z_renumber yet — values stale after create but raise/lower fix them.
	 */

	/* Raise sa to front: [sa, sc, sb] → after renumber sa=2, sc=1, sb=0 */
	rc = anx_iface_surface_raise(sa);
	ASSERT(rc == ANX_OK, -104);
	ASSERT_EQ(sa->z_order, 2u, -105);
	ASSERT_EQ(sc->z_order, 1u, -106);
	ASSERT_EQ(sb->z_order, 0u, -107);

	/* Lower sc to back: [sa, sb, sc] → after renumber sa=2, sb=1, sc=0 */
	rc = anx_iface_surface_lower(sc);
	ASSERT(rc == ANX_OK, -108);
	ASSERT_EQ(sa->z_order, 2u, -109);
	ASSERT_EQ(sb->z_order, 1u, -110);
	ASSERT_EQ(sc->z_order, 0u, -111);

	/* Raise sb to front: [sb, sa, sc] → sb=2, sa=1, sc=0 */
	rc = anx_iface_surface_raise(sb);
	ASSERT(rc == ANX_OK, -112);
	ASSERT_EQ(sb->z_order, 2u, -113);
	ASSERT_EQ(sa->z_order, 1u, -114);
	ASSERT_EQ(sc->z_order, 0u, -115);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-002-I01: focus follows active top-level surface                  */
/* ------------------------------------------------------------------ */

static int test_focus_follows_raise(void)
{
	struct anx_surface *sa, *sb, *sc;
	anx_oid_t focus;
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -200);

	rc = make_surface(&sa);
	ASSERT(rc == ANX_OK, -201);
	rc = make_surface(&sb);
	ASSERT(rc == ANX_OK, -202);
	rc = make_surface(&sc);
	ASSERT(rc == ANX_OK, -203);

	/* Raise sb → focus should be sb (top-level, no parent). */
	rc = anx_iface_surface_raise(sb);
	ASSERT(rc == ANX_OK, -204);
	focus = anx_input_focus_get();
	ASSERT(anx_uuid_compare(&focus, &sb->oid) == 0, -205);

	/* Raise sc → focus should follow to sc. */
	rc = anx_iface_surface_raise(sc);
	ASSERT(rc == ANX_OK, -206);
	focus = anx_input_focus_get();
	ASSERT(anx_uuid_compare(&focus, &sc->oid) == 0, -207);

	/* Raise sa → focus follows to sa. */
	rc = anx_iface_surface_raise(sa);
	ASSERT(rc == ANX_OK, -208);
	focus = anx_input_focus_get();
	ASSERT(anx_uuid_compare(&focus, &sa->oid) == 0, -209);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-002-I02: closing active surface transfers focus; cascade destroy */
/* ------------------------------------------------------------------ */

static int test_destroy_transfers_focus(void)
{
	struct anx_surface *sa, *sb, *sch;
	struct anx_surface *found;
	anx_oid_t focus, ch_oid;
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -300);

	rc = make_surface(&sa);
	ASSERT(rc == ANX_OK, -301);
	rc = make_surface(&sb);
	ASSERT(rc == ANX_OK, -302);

	/* Create child surface sch owned by sb. */
	rc = make_surface(&sch);
	ASSERT(rc == ANX_OK, -303);
	rc = anx_iface_surface_set_parent(sch, sb->oid);
	ASSERT(rc == ANX_OK, -304);

	/* Verify sch's parent is set correctly. */
	ASSERT(anx_uuid_compare(&sch->parent_oid, &sb->oid) == 0, -305);

	/* Raise sb (top-level) → focus = sb. */
	rc = anx_iface_surface_raise(sb);
	ASSERT(rc == ANX_OK, -306);
	focus = anx_input_focus_get();
	ASSERT(anx_uuid_compare(&focus, &sb->oid) == 0, -307);

	/* Stash sch oid before destroy (pointer becomes invalid after). */
	ch_oid = sch->oid;

	/* Destroy sb: cascade should destroy sch first, then sb.
	 * Focus must transfer to sa (next topmost visible top-level). */
	rc = anx_iface_surface_destroy(sb);
	ASSERT(rc == ANX_OK, -308);

	/* sch must no longer be findable. */
	rc = anx_iface_surface_lookup(ch_oid, &found);
	ASSERT(rc == ANX_ENOENT, -309);

	/* Focus should have transferred to sa. */
	focus = anx_input_focus_get();
	ASSERT(anx_uuid_compare(&focus, &sa->oid) == 0, -310);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Test suite entry point                                               */
/* ------------------------------------------------------------------ */

int test_multi_surface(void)
{
	int rc;

	rc = test_z_order_operations();
	if (rc != 0)
		return rc;

	rc = test_focus_follows_raise();
	if (rc != 0)
		return rc;

	rc = test_destroy_transfers_focus();
	if (rc != 0)
		return rc;

	return 0;
}
