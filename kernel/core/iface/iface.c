/*
 * iface.c — Interface Plane subsystem (RFC-0012).
 *
 * Surface store (hashtable + z-order list), renderer registry,
 * environment registry. Event queue is in event_queue.c.
 */

#include <anx/interface_plane.h>
#include <anx/clipboard.h>
#include <anx/input.h>
#include <anx/cell.h>
#include <anx/hashtable.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/types.h>
#include <anx/arch.h>

/* ------------------------------------------------------------------ */
/* Surface store                                                        */
/* ------------------------------------------------------------------ */

#define SURF_HT_BITS  8   /* 256 buckets */

static struct anx_surface   surf_pool[ANX_SURF_MAX];
static bool                 surf_used[ANX_SURF_MAX];
static struct anx_htable    surf_ht;
static struct anx_list_head surf_zlist;  /* z-ordered front-to-back */
static uint32_t             surf_count;
static struct anx_spinlock  iface_lock;

/* ------------------------------------------------------------------ */
/* Renderer registry                                                    */
/* ------------------------------------------------------------------ */

#define RENDERER_MAX  16

struct anx_renderer_reg {
	int                          renderer_class;
	const struct anx_renderer_ops *ops;
	char                         name[64];
	bool                         active;
};

static struct anx_renderer_reg renderers[RENDERER_MAX];
static uint32_t                renderer_count;

/* ------------------------------------------------------------------ */
/* Environment registry                                                 */
/* ------------------------------------------------------------------ */

static struct anx_environment envs[ANX_ENV_MAX];
static uint32_t               env_count;

/* ------------------------------------------------------------------ */
/* Compositor cell registry                                             */
/* ------------------------------------------------------------------ */

struct anx_compositor_slot {
	char domain[ANX_ENV_NAME_MAX];
	struct anx_cell *cell;
	bool running;
	bool crashed;
	uint64_t repaint_cycles;
	uint64_t committed_surfaces;
	uint32_t last_cycle_commits;
	uint64_t last_cycle_ns;
};

static struct anx_compositor_slot compositors[ANX_IFACE_COMPOSITOR_MAX];

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

int
anx_iface_init(void)
{
	int rc;

	anx_spin_init(&iface_lock);
	anx_list_init(&surf_zlist);
	surf_count    = 0;
	renderer_count = 0;
	env_count      = 0;

	rc = anx_htable_init(&surf_ht, SURF_HT_BITS);
	if (rc != ANX_OK)
		return rc;

	anx_memset(surf_pool,  0, sizeof(surf_pool));
	anx_memset(surf_used,  0, sizeof(surf_used));
	anx_memset(renderers,  0, sizeof(renderers));
	anx_memset(envs,       0, sizeof(envs));
	anx_memset(compositors, 0, sizeof(compositors));

	anx_input_init();
	anx_iface_event_reset();
	anx_clipboard_init();

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Surface management                                                   */
/* ------------------------------------------------------------------ */

int
anx_iface_surface_create(int renderer_class,
                          struct anx_content_node *root,
                          int32_t x, int32_t y,
                          uint32_t width, uint32_t height,
                          struct anx_surface **out)
{
	struct anx_surface *surf;
	uint32_t i;
	bool flags;

	if (!out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);

	for (i = 0; i < ANX_SURF_MAX; i++) {
		if (!surf_used[i])
			break;
	}
	if (i == ANX_SURF_MAX) {
		anx_spin_unlock_irqrestore(&iface_lock, flags);
		return ANX_EFULL;
	}

	surf = &surf_pool[i];
	anx_memset(surf, 0, sizeof(*surf));

	anx_uuid_generate(&surf->oid);
	surf->state          = ANX_SURF_CREATED;
	surf->renderer_class = renderer_class;
	surf->content_root   = root;
	surf->x              = x;
	surf->y              = y;
	surf->width          = width;
	surf->height         = height;
	surf->z_order        = surf_count;  /* new surfaces on top */
	surf->renderer_ops   = NULL;        /* assigned at map time */

	anx_spin_init(&surf->lock);
	anx_list_init(&surf->children);
	anx_list_init(&surf->ht_node);
	anx_list_init(&surf->z_node);

	anx_htable_add(&surf_ht, &surf->ht_node, anx_uuid_hash(&surf->oid));
	anx_list_add(&surf->z_node, &surf_zlist); /* front of list = highest z */

	surf_used[i] = true;
	surf_count++;

	anx_spin_unlock_irqrestore(&iface_lock, flags);
	*out = surf;
	return ANX_OK;
}

int
anx_iface_surface_map(struct anx_surface *surf)
{
	const struct anx_renderer_ops *ops;
	bool flags;
	int rc;

	if (!surf)
		return ANX_EINVAL;

	ops = anx_iface_renderer_ops(surf->renderer_class);
	if (!ops)
		return ANX_ENOENT;

	anx_spin_lock_irqsave(&surf->lock, &flags);
	surf->renderer_ops = ops;
	surf->state        = ANX_SURF_MAPPED;
	anx_spin_unlock_irqrestore(&surf->lock, flags);

	rc = ops->map(surf);
	if (rc != ANX_OK) {
		anx_spin_lock_irqsave(&surf->lock, &flags);
		surf->renderer_ops = NULL;
		surf->state        = ANX_SURF_CREATED;
		anx_spin_unlock_irqrestore(&surf->lock, flags);
		return rc;
	}

	anx_spin_lock_irqsave(&surf->lock, &flags);
	surf->state    = ANX_SURF_VISIBLE;
	/* Initial damage covers the full surface so the first compositor tick renders it. */
	surf->damage_x = 0;
	surf->damage_y = 0;
	surf->damage_w = surf->width;
	surf->damage_h = surf->height;
	surf->damage_valid = true;
	anx_spin_unlock_irqrestore(&surf->lock, flags);

	return ANX_OK;
}

int
anx_iface_surface_damage(struct anx_surface *surf,
                          int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	bool flags;
	int32_t x2, y2, nx2, ny2, ux, uy;

	if (!surf || w == 0 || h == 0)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&surf->lock, &flags);
	if (!surf->damage_valid) {
		surf->damage_x = x;
		surf->damage_y = y;
		surf->damage_w = w;
		surf->damage_h = h;
		surf->damage_valid = true;
	} else {
		/* Expand accumulated rect to union with new rect. */
		x2  = surf->damage_x + (int32_t)surf->damage_w;
		y2  = surf->damage_y + (int32_t)surf->damage_h;
		nx2 = x + (int32_t)w;
		ny2 = y + (int32_t)h;
		ux  = (x  < surf->damage_x) ? x  : surf->damage_x;
		uy  = (y  < surf->damage_y) ? y  : surf->damage_y;
		surf->damage_x = ux;
		surf->damage_y = uy;
		surf->damage_w = (uint32_t)((nx2 > x2 ? nx2 : x2) - ux);
		surf->damage_h = (uint32_t)((ny2 > y2 ? ny2 : y2) - uy);
	}
	anx_spin_unlock_irqrestore(&surf->lock, flags);

	/* Forward hint to renderer (advisory; renderer may ignore). */
	if (surf->renderer_ops && surf->renderer_ops->damage)
		surf->renderer_ops->damage(surf, x, y, w, h);

	return ANX_OK;
}

void
anx_iface_surface_damage_query(struct anx_surface *surf,
                                 int32_t *x_out, int32_t *y_out,
                                 uint32_t *w_out, uint32_t *h_out,
                                 bool *valid_out)
{
	bool flags;

	if (!surf)
		return;
	anx_spin_lock_irqsave(&surf->lock, &flags);
	if (x_out)     *x_out     = surf->damage_x;
	if (y_out)     *y_out     = surf->damage_y;
	if (w_out)     *w_out     = surf->damage_w;
	if (h_out)     *h_out     = surf->damage_h;
	if (valid_out) *valid_out = surf->damage_valid;
	anx_spin_unlock_irqrestore(&surf->lock, flags);
}

int
anx_iface_surface_commit(struct anx_surface *surf)
{
	bool flags;
	int rc;

	if (!surf)
		return ANX_EINVAL;
	if (surf->state != ANX_SURF_VISIBLE && surf->state != ANX_SURF_MAPPED)
		return ANX_EINVAL;
	if (!surf->renderer_ops)
		return ANX_ENOENT;

	rc = surf->renderer_ops->commit(surf);
	if (rc == ANX_OK) {
		anx_spin_lock_irqsave(&surf->lock, &flags);
		surf->damage_valid = false;
		surf->commit_count++;
		anx_spin_unlock_irqrestore(&surf->lock, flags);
	}
	return rc;
}

int
anx_iface_surface_destroy(struct anx_surface *surf)
{
	struct anx_list_head *pos;
	anx_oid_t focused, new_focus;
	bool need_focus_transfer;
	uint32_t i;
	bool flags;

	if (!surf)
		return ANX_EINVAL;

	/* Cascade-destroy all children before removing the parent. */
	for (i = 0; i < ANX_SURF_MAX; i++) {
		if (surf_used[i] && &surf_pool[i] != surf &&
		    anx_uuid_compare(&surf_pool[i].parent_oid, &surf->oid) == 0)
			anx_iface_surface_destroy(&surf_pool[i]);
	}

	focused = anx_input_focus_get();
	need_focus_transfer = (anx_uuid_compare(&focused, &surf->oid) == 0);

	if (surf->renderer_ops && surf->renderer_ops->unmap)
		surf->renderer_ops->unmap(surf);

	anx_spin_lock_irqsave(&iface_lock, &flags);
	anx_htable_del(&surf_ht, &surf->ht_node);
	anx_list_del(&surf->z_node);
	for (i = 0; i < ANX_SURF_MAX; i++) {
		if (&surf_pool[i] == surf) {
			surf_used[i] = false;
			break;
		}
	}
	surf_count--;
	anx_spin_unlock_irqrestore(&iface_lock, flags);

	if (need_focus_transfer) {
		new_focus = ANX_UUID_NIL;
		anx_spin_lock_irqsave(&iface_lock, &flags);
		ANX_LIST_FOR_EACH(pos, &surf_zlist) {
			struct anx_surface *s =
				ANX_LIST_ENTRY(pos, struct anx_surface, z_node);
			if (s->state == ANX_SURF_VISIBLE &&
			    anx_uuid_is_nil(&s->parent_oid)) {
				new_focus = s->oid;
				break;
			}
		}
		anx_spin_unlock_irqrestore(&iface_lock, flags);
		anx_input_focus_set(new_focus);
	}

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Surface queries                                                      */
/* ------------------------------------------------------------------ */

int
anx_iface_surface_lookup(anx_oid_t oid, struct anx_surface **out)
{
	struct anx_list_head *pos;
	uint64_t h;
	bool flags;

	if (!out)
		return ANX_EINVAL;

	h = anx_uuid_hash(&oid);

	anx_spin_lock_irqsave(&iface_lock, &flags);
	ANX_HTABLE_FOR_BUCKET(pos, &surf_ht, h) {
		struct anx_surface *s =
			ANX_LIST_ENTRY(pos, struct anx_surface, ht_node);
		if (anx_uuid_compare(&s->oid, &oid) == 0) {
			*out = s;
			anx_spin_unlock_irqrestore(&iface_lock, flags);
			return ANX_OK;
		}
	}
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_ENOENT;
}

int
anx_iface_surface_list(anx_oid_t *oids_out, uint32_t max, uint32_t *count_out)
{
	struct anx_list_head *pos;
	uint32_t n = 0;
	bool flags;

	if (!oids_out || !count_out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	ANX_LIST_FOR_EACH(pos, &surf_zlist) {
		if (n >= max)
			break;
		struct anx_surface *s =
			ANX_LIST_ENTRY(pos, struct anx_surface, z_node);
		oids_out[n++] = s->oid;
	}
	*count_out = n;
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Renderer registry                                                    */
/* ------------------------------------------------------------------ */

int
anx_iface_renderer_register(int renderer_class,
                              const struct anx_renderer_ops *ops,
                              const char *name)
{
	uint32_t i;
	bool flags;

	if (!ops || !name)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);

	/* Replace existing registration for the same class */
	for (i = 0; i < renderer_count; i++) {
		if (renderers[i].renderer_class == renderer_class) {
			renderers[i].ops    = ops;
			renderers[i].active = true;
			anx_strlcpy(renderers[i].name, name,
			            sizeof(renderers[i].name) - 1);
			anx_spin_unlock_irqrestore(&iface_lock, flags);
			return ANX_OK;
		}
	}

	if (renderer_count >= RENDERER_MAX) {
		anx_spin_unlock_irqrestore(&iface_lock, flags);
		return ANX_EFULL;
	}

	renderers[renderer_count].renderer_class = renderer_class;
	renderers[renderer_count].ops            = ops;
	renderers[renderer_count].active         = true;
	anx_strlcpy(renderers[renderer_count].name, name,
	            sizeof(renderers[renderer_count].name) - 1);
	renderer_count++;

	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_OK;
}

int
anx_iface_renderer_unregister(int renderer_class)
{
	uint32_t i;
	bool flags;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	for (i = 0; i < renderer_count; i++) {
		if (renderers[i].renderer_class == renderer_class) {
			renderers[i].active = false;
			anx_spin_unlock_irqrestore(&iface_lock, flags);
			return ANX_OK;
		}
	}
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_ENOENT;
}

const struct anx_renderer_ops *
anx_iface_renderer_ops(int renderer_class)
{
	uint32_t i;

	for (i = 0; i < renderer_count; i++) {
		if (renderers[i].renderer_class == renderer_class &&
		    renderers[i].active)
			return renderers[i].ops;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Environment registry                                                 */
/* ------------------------------------------------------------------ */

int
anx_iface_env_define(const char *name, const char *schema,
                      int renderer_class)
{
	uint32_t i;
	bool flags;

	if (!name)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);

	for (i = 0; i < env_count; i++) {
		if (anx_strcmp(envs[i].name, name) == 0) {
			anx_spin_unlock_irqrestore(&iface_lock, flags);
			return ANX_EEXIST;
		}
	}
	if (env_count >= ANX_ENV_MAX) {
		anx_spin_unlock_irqrestore(&iface_lock, flags);
		return ANX_EFULL;
	}

	anx_strlcpy(envs[env_count].name, name, ANX_ENV_NAME_MAX - 1);
	if (schema)
		anx_strlcpy(envs[env_count].schema, schema,
		            sizeof(envs[env_count].schema) - 1);
	envs[env_count].renderer_class = renderer_class;
	envs[env_count].active         = false;
	env_count++;

	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_OK;
}

int
anx_iface_env_activate(const char *env_name)
{
	uint32_t i;
	bool flags;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	for (i = 0; i < env_count; i++) {
		if (anx_strcmp(envs[i].name, env_name) == 0) {
			envs[i].active = true;
			anx_spin_unlock_irqrestore(&iface_lock, flags);
			return ANX_OK;
		}
	}
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_ENOENT;
}

int
anx_iface_env_deactivate(const char *env_name)
{
	uint32_t i;
	bool flags;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	for (i = 0; i < env_count; i++) {
		if (anx_strcmp(envs[i].name, env_name) == 0) {
			envs[i].active = false;
			anx_spin_unlock_irqrestore(&iface_lock, flags);
			return ANX_OK;
		}
	}
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_ENOENT;
}

int
anx_iface_env_query(const char *env_name, struct anx_environment *out)
{
	uint32_t i;
	bool flags;

	if (!out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	for (i = 0; i < env_count; i++) {
		if (anx_strcmp(envs[i].name, env_name) == 0) {
			*out = envs[i];
			anx_spin_unlock_irqrestore(&iface_lock, flags);
			return ANX_OK;
		}
	}
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_ENOENT;
}

/* ------------------------------------------------------------------ */
/* Wayland compatibility bridge                                         */
/* ------------------------------------------------------------------ */

int
anx_wayland_surface_wrap(void *pixels, uint32_t width, uint32_t height,
                          int32_t x, int32_t y, struct anx_surface **out)
{
	struct anx_content_node *node;
	struct anx_surface      *surf;
	int                      rc;

	if (!pixels || !out)
		return ANX_EINVAL;

	node = anx_zalloc(sizeof(*node));
	if (!node)
		return ANX_ENOMEM;

	node->type     = ANX_CONTENT_CANVAS;
	node->data     = pixels;
	node->data_len = width * height * 4;

	rc = anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, node,
	                               x, y, width, height, &surf);
	if (rc != ANX_OK) {
		anx_free(node);
		return rc;
	}

	rc = anx_iface_surface_map(surf);
	if (rc != ANX_OK) {
		anx_iface_surface_destroy(surf);
		anx_free(node);
		return rc;
	}

	*out = surf;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Compositor support                                                   */
/* ------------------------------------------------------------------ */

static struct anx_compositor_slot *
compositor_find_slot(const char *domain)
{
	uint32_t i;

	for (i = 0; i < ANX_IFACE_COMPOSITOR_MAX; i++) {
		if (compositors[i].domain[0] == '\0')
			continue;
		if (anx_strcmp(compositors[i].domain, domain) == 0)
			return &compositors[i];
	}
	return NULL;
}

static struct anx_compositor_slot *
compositor_alloc_slot(const char *domain)
{
	uint32_t i;

	for (i = 0; i < ANX_IFACE_COMPOSITOR_MAX; i++) {
		if (compositors[i].domain[0] != '\0')
			continue;
		anx_memset(&compositors[i], 0, sizeof(compositors[i]));
		anx_strlcpy(compositors[i].domain, domain,
			    sizeof(compositors[i].domain));
		return &compositors[i];
	}
	return NULL;
}

static int
compositor_run_cycle(struct anx_compositor_slot *slot, uint32_t *committed_out)
{
	struct anx_list_head *pos;
	uint32_t committed;
	bool flags;
	uint64_t t0, t1;

	committed = 0;
	t0 = arch_time_now();

	anx_spin_lock_irqsave(&iface_lock, &flags);
	ANX_LIST_FOR_EACH(pos, &surf_zlist) {
		struct anx_surface *s;

		s = ANX_LIST_ENTRY(pos, struct anx_surface, z_node);
		if (s->state != ANX_SURF_VISIBLE)
			continue;

		/* P1-001: skip surfaces with no pending damage — zero-render fast path. */
		if (!s->damage_valid)
			continue;

		if (s->renderer_ops && s->renderer_ops->commit) {
			anx_spin_unlock_irqrestore(&iface_lock, flags);
			s->renderer_ops->commit(s);
			anx_spin_lock_irqsave(&iface_lock, &flags);
			s->damage_valid = false;
			s->commit_count++;
			committed++;
		}
	}
	anx_spin_unlock_irqrestore(&iface_lock, flags);

	t1 = arch_time_now();
	slot->repaint_cycles++;
	slot->committed_surfaces += committed;
	slot->last_cycle_commits = committed;
	slot->last_cycle_ns      = t1 - t0;
	if (committed_out)
		*committed_out = committed;
	return ANX_OK;
}

int
anx_iface_compositor_start(const char *domain)
{
	struct anx_compositor_slot *slot;
	struct anx_environment env;
	struct anx_cell_intent intent;
	struct anx_cell *cell;
	bool flags;
	int rc;

	if (!domain)
		return ANX_EINVAL;
	if (anx_iface_env_query(domain, &env) != ANX_OK)
		return ANX_ENOENT;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	slot = compositor_find_slot(domain);
	if (slot && slot->running) {
		anx_spin_unlock_irqrestore(&iface_lock, flags);
		return ANX_EEXIST;
	}
	if (!slot)
		slot = compositor_alloc_slot(domain);
	if (!slot) {
		anx_spin_unlock_irqrestore(&iface_lock, flags);
		return ANX_EFULL;
	}
	anx_spin_unlock_irqrestore(&iface_lock, flags);

	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "iface-compositor", sizeof(intent.name));
	anx_strlcpy(intent.objective, domain, sizeof(intent.objective));
	intent.priority = 10;

	rc = anx_cell_create(ANX_CELL_TASK_SCHEDULER_BINDING, &intent, &cell);
	if (rc != ANX_OK)
		return rc;

	rc = anx_cell_transition(cell, ANX_CELL_ADMITTED);
	if (rc == ANX_OK)
		rc = anx_cell_transition(cell, ANX_CELL_PLANNING);
	if (rc == ANX_OK)
		rc = anx_cell_transition(cell, ANX_CELL_PLANNED);
	if (rc == ANX_OK)
		rc = anx_cell_transition(cell, ANX_CELL_QUEUED);
	if (rc == ANX_OK)
		rc = anx_cell_transition(cell, ANX_CELL_RUNNING);
	if (rc != ANX_OK) {
		anx_cell_destroy(cell);
		return rc;
	}

	anx_spin_lock_irqsave(&iface_lock, &flags);
	slot->cell = cell;
	slot->running = true;
	slot->crashed = false;
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_OK;
}

int
anx_iface_compositor_stop(const char *domain)
{
	struct anx_compositor_slot *slot;
	bool flags;

	if (!domain)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	slot = compositor_find_slot(domain);
	if (!slot || !slot->running || !slot->cell) {
		anx_spin_unlock_irqrestore(&iface_lock, flags);
		return ANX_ENOENT;
	}
	anx_cell_transition(slot->cell, ANX_CELL_CANCELLED);
	anx_cell_destroy(slot->cell);
	slot->cell = NULL;
	slot->running = false;
	slot->crashed = false;
	anx_memset(slot->domain, 0, sizeof(slot->domain));
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_OK;
}

int
anx_iface_compositor_crash(const char *domain)
{
	struct anx_compositor_slot *slot;
	bool flags;

	if (!domain)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	slot = compositor_find_slot(domain);
	if (!slot || !slot->running || !slot->cell) {
		anx_spin_unlock_irqrestore(&iface_lock, flags);
		return ANX_ENOENT;
	}
	anx_cell_transition(slot->cell, ANX_CELL_FAILED);
	anx_cell_destroy(slot->cell);
	slot->cell = NULL;
	slot->running = false;
	slot->crashed = true;
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_OK;
}

int
anx_iface_compositor_restart(const char *domain)
{
	struct anx_compositor_slot *slot;
	bool flags;

	if (!domain)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	slot = compositor_find_slot(domain);
	if (!slot || !slot->crashed) {
		anx_spin_unlock_irqrestore(&iface_lock, flags);
		return ANX_ENOENT;
	}
	slot->running = false;
	anx_spin_unlock_irqrestore(&iface_lock, flags);

	return anx_iface_compositor_start(domain);
}

int
anx_iface_compositor_tick(const char *domain, uint32_t *committed_out)
{
	struct anx_compositor_slot *slot;
	bool flags;

	if (!domain)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	slot = compositor_find_slot(domain);
	if (!slot || !slot->running || !slot->cell) {
		anx_spin_unlock_irqrestore(&iface_lock, flags);
		return ANX_ENOENT;
	}
	anx_spin_unlock_irqrestore(&iface_lock, flags);

	return compositor_run_cycle(slot, committed_out);
}

int
anx_iface_compositor_stats(const char *domain, struct anx_iface_compositor_stats *out)
{
	struct anx_compositor_slot *slot;
	bool flags;

	if (!domain || !out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	slot = compositor_find_slot(domain);
	if (!slot) {
		anx_spin_unlock_irqrestore(&iface_lock, flags);
		return ANX_ENOENT;
	}

	anx_memset(out, 0, sizeof(*out));
	anx_strlcpy(out->domain, slot->domain, sizeof(out->domain));
	out->running = slot->running;
	out->crashed = slot->crashed;
	out->repaint_cycles     = slot->repaint_cycles;
	out->committed_surfaces = slot->committed_surfaces;
	out->last_cycle_commits = slot->last_cycle_commits;
	out->last_cycle_ns      = slot->last_cycle_ns;
	if (slot->cell)
		out->cell_cid = slot->cell->cid;
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_OK;
}

int
anx_iface_compositor_repaint(void)
{
	uint32_t committed;
	int rc;

	rc = anx_iface_compositor_tick("visual-desktop", &committed);
	if (rc == ANX_OK)
		return (int)committed;
	return rc;
}

/* ------------------------------------------------------------------ */
/* PIT-driven frame scheduler                                           */
/* ------------------------------------------------------------------ */

static volatile uint32_t frame_tick_counter;
static uint32_t          frame_repaint_interval; /* PIT ticks per frame */

static void iface_frame_tick(void)
{
	frame_tick_counter++;
	if (frame_tick_counter >= frame_repaint_interval) {
		frame_tick_counter = 0;
		anx_iface_compositor_repaint();
	}
}

void anx_iface_frame_scheduler_init(uint32_t target_fps)
{
	if (target_fps == 0)
		target_fps = 30;
	/* PIT runs at 100 Hz; round up to at least 1 tick */
	frame_repaint_interval = 100u / target_fps;
	if (frame_repaint_interval == 0)
		frame_repaint_interval = 1;
	frame_tick_counter = 0;
	arch_set_timer_callback(iface_frame_tick);
}

/* ------------------------------------------------------------------ */
/* Window management helpers                                            */
/* ------------------------------------------------------------------ */

int anx_iface_surface_move(struct anx_surface *surf, int32_t x, int32_t y)
{
	bool flags;

	if (!surf)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	surf->x = x;
	surf->y = y;
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_OK;
}

/* Reassign z_order fields front-to-back; must be called under iface_lock. */
static void
z_renumber(void)
{
	struct anx_list_head *pos;
	uint32_t z;

	z = surf_count;
	ANX_LIST_FOR_EACH(pos, &surf_zlist) {
		struct anx_surface *s =
			ANX_LIST_ENTRY(pos, struct anx_surface, z_node);
		s->z_order = --z;
	}
}

int anx_iface_surface_raise(struct anx_surface *surf)
{
	bool flags;
	bool is_toplevel;

	if (!surf)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	anx_list_del(&surf->z_node);
	anx_list_add(&surf->z_node, &surf_zlist);	/* front = highest z */
	z_renumber();
	is_toplevel = anx_uuid_is_nil(&surf->parent_oid);
	anx_spin_unlock_irqrestore(&iface_lock, flags);

	/* Focus follows raise for top-level surfaces. */
	if (is_toplevel)
		anx_input_focus_set(surf->oid);
	return ANX_OK;
}

int anx_iface_surface_lower(struct anx_surface *surf)
{
	bool flags;

	if (!surf)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	anx_list_del(&surf->z_node);
	anx_list_add_tail(&surf->z_node, &surf_zlist);	/* tail = lowest z */
	z_renumber();
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_OK;
}

int anx_iface_surface_set_parent(struct anx_surface *child,
                                   anx_oid_t parent_oid)
{
	struct anx_surface *parent;
	bool flags;
	int rc;

	if (!child)
		return ANX_EINVAL;

	rc = anx_iface_surface_lookup(parent_oid, &parent);
	if (rc != ANX_OK)
		return rc;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	child->parent_oid = parent_oid;
	/* Place child just above parent in z-order. */
	anx_list_del(&child->z_node);
	anx_list_add(&child->z_node, parent->z_node.prev);
	z_renumber();
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return ANX_OK;
}

struct anx_surface *anx_iface_surface_at(int32_t x, int32_t y)
{
	struct anx_list_head *pos;
	struct anx_surface   *found = NULL;
	bool flags;

	anx_spin_lock_irqsave(&iface_lock, &flags);
	/* Walk front-to-back; first hit is topmost */
	ANX_LIST_FOR_EACH(pos, &surf_zlist) {
		struct anx_surface *s =
			ANX_LIST_ENTRY(pos, struct anx_surface, z_node);

		if (s->state != ANX_SURF_VISIBLE)
			continue;
		if (x >= s->x && x < s->x + (int32_t)s->width &&
		    y >= s->y && y < s->y + (int32_t)s->height) {
			found = s;
			break;
		}
	}
	anx_spin_unlock_irqrestore(&iface_lock, flags);
	return found;
}

void anx_iface_surface_set_title(struct anx_surface *surf, const char *title)
{
	if (!surf || !title)
		return;
	anx_strlcpy(surf->title, title, sizeof(surf->title));
}
