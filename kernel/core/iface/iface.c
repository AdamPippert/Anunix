#include <anx/interface_plane.h>
#include <anx/errno.h>
#include <anx/alloc.h>

/*
 * Interface Plane kernel subsystem (RFC-0012).
 *
 * Medium-agnostic interaction layer. Surfaces are State Objects.
 * Events flow through the Routing Plane. Renderers are Engine classes.
 * The compositor is an Execution Cell.
 *
 * See RFC-0012 for full specification. This file is the implementation
 * stub — all functions return ANX_ENOTIMPL until filled in.
 */

int
anx_iface_surface_create(struct anx_cell *owner, int renderer_class,
                          struct anx_content_node *root,
                          struct anx_surface **out)
{
        (void)owner; (void)renderer_class; (void)root; (void)out;
        return ANX_ENOTIMPL;
}

int
anx_iface_surface_map(struct anx_surface *surf, anx_eid_t renderer_eid)
{
        (void)surf; (void)renderer_eid;
        return ANX_ENOTIMPL;
}

int
anx_iface_surface_damage(struct anx_surface *surf,
                          int32_t x, int32_t y, uint32_t w, uint32_t h)
{
        (void)surf; (void)x; (void)y; (void)w; (void)h;
        return ANX_ENOTIMPL;
}

int
anx_iface_surface_commit(struct anx_surface *surf)
{
        (void)surf;
        return ANX_ENOTIMPL;
}

int
anx_iface_surface_destroy(struct anx_surface *surf)
{
        (void)surf;
        return ANX_ENOTIMPL;
}

int
anx_iface_event_post(struct anx_event *ev)
{
        (void)ev;
        return ANX_ENOTIMPL;
}

int
anx_iface_event_subscribe(anx_oid_t surf_oid, anx_cid_t cell_cid)
{
        (void)surf_oid; (void)cell_cid;
        return ANX_ENOTIMPL;
}

int
anx_iface_event_unsubscribe(anx_oid_t surf_oid, anx_cid_t cell_cid)
{
        (void)surf_oid; (void)cell_cid;
        return ANX_ENOTIMPL;
}

int
anx_iface_renderer_register(int renderer_class, anx_eid_t eid,
                              const char *name)
{
        (void)renderer_class; (void)eid; (void)name;
        return ANX_ENOTIMPL;
}

int
anx_iface_renderer_unregister(anx_eid_t eid)
{
        (void)eid;
        return ANX_ENOTIMPL;
}

anx_eid_t
anx_iface_renderer_find(int renderer_class)
{
        (void)renderer_class;
        return (anx_eid_t){0};
}

int
anx_iface_env_activate(const char *env_name)
{
        (void)env_name;
        return ANX_ENOTIMPL;
}

int
anx_iface_env_deactivate(const char *env_name)
{
        (void)env_name;
        return ANX_ENOTIMPL;
}

int
anx_iface_env_query(const char *env_name, struct anx_environment *out)
{
        (void)env_name; (void)out;
        return ANX_ENOTIMPL;
}

int
anx_iface_surface_list(anx_oid_t *oids_out, uint32_t max, uint32_t *count_out)
{
        (void)oids_out; (void)max; (void)count_out;
        return ANX_ENOTIMPL;
}

int
anx_iface_surface_lookup(anx_oid_t oid, struct anx_surface **out)
{
        (void)oid; (void)out;
        return ANX_ENOTIMPL;
}

int
anx_wayland_surface_wrap(void *wl_buffer, uint32_t width, uint32_t height,
                          anx_cid_t owner_cid, struct anx_surface **out)
{
        (void)wl_buffer; (void)width; (void)height;
        (void)owner_cid; (void)out;
        return ANX_ENOTIMPL;
}
