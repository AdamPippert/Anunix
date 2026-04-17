/*
 * iface.c — Interface Plane subsystem (RFC-0012).
 *
 * Surface store (hashtable + z-order list), renderer registry,
 * environment registry. Event queue is in event_queue.c.
 */

#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/hashtable.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/types.h>

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

	anx_input_init();

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
	surf->state = ANX_SURF_VISIBLE;
	anx_spin_unlock_irqrestore(&surf->lock, flags);

	return ANX_OK;
}

int
anx_iface_surface_damage(struct anx_surface *surf,
                          int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	if (!surf || !surf->renderer_ops)
		return ANX_EINVAL;
	if (surf->renderer_ops->damage)
		surf->renderer_ops->damage(surf, x, y, w, h);
	return ANX_OK;
}

int
anx_iface_surface_commit(struct anx_surface *surf)
{
	if (!surf)
		return ANX_EINVAL;
	if (surf->state != ANX_SURF_VISIBLE && surf->state != ANX_SURF_MAPPED)
		return ANX_EINVAL;
	if (!surf->renderer_ops)
		return ANX_ENOENT;
	return surf->renderer_ops->commit(surf);
}

int
anx_iface_surface_destroy(struct anx_surface *surf)
{
	uint32_t i;
	bool flags;

	if (!surf)
		return ANX_EINVAL;

	/* Unmap from renderer first */
	if (surf->renderer_ops && surf->renderer_ops->unmap)
		surf->renderer_ops->unmap(surf);

	anx_spin_lock_irqsave(&iface_lock, &flags);

	anx_htable_del(&surf_ht, &surf->ht_node);
	anx_list_del(&surf->z_node);

	/* Find and free the pool slot */
	for (i = 0; i < ANX_SURF_MAX; i++) {
		if (&surf_pool[i] == surf) {
			surf_used[i] = false;
			break;
		}
	}
	surf_count--;

	anx_spin_unlock_irqrestore(&iface_lock, flags);
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
