/*
 * test_compositor_cell.c — P0-003 compositor-as-execution-cell tests.
 */

#include <anx/types.h>
#include <anx/interface_plane.h>
#include <anx/cell.h>

#define ASSERT(cond, code) do { if (!(cond)) return (code); } while (0)

#define COMMIT_LOG_MAX 32

static anx_oid_t commit_log[COMMIT_LOG_MAX];
static uint32_t commit_log_count;

static int log_map(struct anx_surface *surf)
{
	(void)surf;
	return ANX_OK;
}

static int log_commit(struct anx_surface *surf)
{
	if (commit_log_count < COMMIT_LOG_MAX)
		commit_log[commit_log_count++] = surf->oid;
	return ANX_OK;
}

static void log_damage(struct anx_surface *surf,
		       int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	(void)surf;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}

static void log_unmap(struct anx_surface *surf)
{
	(void)surf;
}

static const struct anx_renderer_ops log_renderer_ops = {
	.map = log_map,
	.commit = log_commit,
	.damage = log_damage,
	.unmap = log_unmap,
};

static int setup_compositor_fixture(void)
{
	int rc;

	anx_cell_store_init();
	rc = anx_iface_init();
	if (rc != ANX_OK)
		return rc;
	rc = anx_iface_renderer_register(ANX_ENGINE_RENDERER_HEADLESS,
					 &log_renderer_ops, "test-log");
	if (rc != ANX_OK)
		return rc;
	rc = anx_iface_env_define("visual-desktop", "anx:test", ANX_ENGINE_RENDERER_HEADLESS);
	if (rc != ANX_OK)
		return rc;
	commit_log_count = 0;
	return ANX_OK;
}

static int create_visible_surface(struct anx_surface **out)
{
	int rc;
	struct anx_surface *surf;

	rc = anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS,
				      NULL, 0, 0, 64, 64, &surf);
	if (rc != ANX_OK)
		return rc;
	rc = anx_iface_surface_map(surf);
	if (rc != ANX_OK)
		return rc;
	*out = surf;
	return ANX_OK;
}

/* P0-003-U01 */
static int test_one_compositor_per_domain(void)
{
	int rc;

	rc = setup_compositor_fixture();
	ASSERT(rc == ANX_OK, -100);

	rc = anx_iface_compositor_start("visual-desktop");
	ASSERT(rc == ANX_OK, -101);

	rc = anx_iface_compositor_start("visual-desktop");
	ASSERT(rc == ANX_EEXIST, -102);

	return 0;
}

/* P0-003-U02 */
static int test_registry_survives_restart(void)
{
	int rc;
	struct anx_surface *a;
	struct anx_surface *b;
	anx_oid_t before[8];
	anx_oid_t after[8];
	uint32_t before_n;
	uint32_t after_n;
	uint32_t i;

	rc = setup_compositor_fixture();
	ASSERT(rc == ANX_OK, -110);
	rc = create_visible_surface(&a);
	ASSERT(rc == ANX_OK, -111);
	rc = create_visible_surface(&b);
	ASSERT(rc == ANX_OK, -112);
	(void)a;
	(void)b;

	rc = anx_iface_surface_list(before, 8, &before_n);
	ASSERT(rc == ANX_OK, -113);
	ASSERT(before_n == 2, -114);

	rc = anx_iface_compositor_start("visual-desktop");
	ASSERT(rc == ANX_OK, -115);
	rc = anx_iface_compositor_crash("visual-desktop");
	ASSERT(rc == ANX_OK, -116);
	rc = anx_iface_compositor_restart("visual-desktop");
	ASSERT(rc == ANX_OK, -117);

	rc = anx_iface_surface_list(after, 8, &after_n);
	ASSERT(rc == ANX_OK, -118);
	ASSERT(after_n == before_n, -119);
	for (i = 0; i < before_n; i++) {
		ASSERT(before[i].hi == after[i].hi && before[i].lo == after[i].lo, -120);
	}

	return 0;
}

/* P0-003-I01 */
static int test_z_order_commit_sequence(void)
{
	int rc;
	struct anx_surface *s1;
	struct anx_surface *s2;
	struct anx_surface *s3;
	uint32_t committed;

	rc = setup_compositor_fixture();
	ASSERT(rc == ANX_OK, -130);
	rc = create_visible_surface(&s1);
	ASSERT(rc == ANX_OK, -131);
	rc = create_visible_surface(&s2);
	ASSERT(rc == ANX_OK, -132);
	rc = create_visible_surface(&s3);
	ASSERT(rc == ANX_OK, -133);

	rc = anx_iface_compositor_start("visual-desktop");
	ASSERT(rc == ANX_OK, -134);

	commit_log_count = 0;
	rc = anx_iface_compositor_tick("visual-desktop", &committed);
	ASSERT(rc == ANX_OK, -135);
	ASSERT(committed == 3, -136);
	ASSERT(commit_log_count == 3, -137);

	/* Highest z-order first: newest surface first. */
	ASSERT(commit_log[0].hi == s3->oid.hi && commit_log[0].lo == s3->oid.lo, -138);
	ASSERT(commit_log[1].hi == s2->oid.hi && commit_log[1].lo == s2->oid.lo, -139);
	ASSERT(commit_log[2].hi == s1->oid.hi && commit_log[2].lo == s1->oid.lo, -140);

	return 0;
}

int test_compositor_cell(void)
{
	int rc;

	rc = test_one_compositor_per_domain();
	if (rc != 0)
		return rc;
	rc = test_registry_survives_restart();
	if (rc != 0)
		return rc;
	rc = test_z_order_commit_sequence();
	if (rc != 0)
		return rc;
	return 0;
}
