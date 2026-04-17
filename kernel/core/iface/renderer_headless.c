/*
 * renderer_headless.c — Headless (no-op) renderer for the Interface Plane.
 *
 * Used for CI testing, agent-only surfaces, and any environment that has
 * no physical output medium. All operations succeed silently.
 *
 * Registered at boot as ANX_ENGINE_RENDERER_HEADLESS.
 */

#include <anx/interface_plane.h>
#include <anx/types.h>

static int  headless_map(struct anx_surface *s)                            { (void)s; return ANX_OK; }
static int  headless_commit(struct anx_surface *s)                         { (void)s; return ANX_OK; }
static void headless_damage(struct anx_surface *s, int32_t x, int32_t y,
                             uint32_t w, uint32_t h)
{
	(void)s; (void)x; (void)y; (void)w; (void)h;
}
static void headless_unmap(struct anx_surface *s)                          { (void)s; }

static const struct anx_renderer_ops headless_ops = {
	.map    = headless_map,
	.commit = headless_commit,
	.damage = headless_damage,
	.unmap  = headless_unmap,
};

int
anx_renderer_headless_register(void)
{
	return anx_iface_renderer_register(ANX_ENGINE_RENDERER_HEADLESS,
	                                    &headless_ops, "headless");
}
