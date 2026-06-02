/*
 * icm.c — Information Context Management over State Objects (RFC-0027).
 *
 * ICM views are reads of facts the kernel already holds: classification lives
 * in anno.icm.* user metadata, identity is the oid, dependencies are the
 * provenance graph. Nothing here introduces a new object type; this file only
 * tags and renders. See RFC-0027 and anx/icm.h.
 */

#include <anx/icm.h>
#include <anx/meta.h>
#include <anx/state_object.h>
#include <anx/string.h>

/* Copy a string meta value into a fixed buffer, or empty it if unset. */
static void read_str(struct anx_meta_store *store, const char *key,
		     char *buf, size_t len)
{
	const struct anx_meta_value *v = anx_meta_get(store, key);

	if (v && v->type == ANX_META_STRING && v->v.str.data)
		anx_strlcpy(buf, v->v.str.data, len);
	else
		buf[0] = '\0';
}

/* True if the comma-separated tag list in `domains` contains `want`. */
static bool domain_contains(const char *domains, const char *want)
{
	size_t wlen = anx_strlen(want);
	const char *p = domains;

	if (wlen == 0)
		return true;

	while (*p) {
		const char *start = p;
		size_t seg;

		while (*p && *p != ',')
			p++;
		seg = (size_t)(p - start);
		/* trim a single leading space, common in "a, b" lists */
		if (seg > 0 && *start == ' ') {
			start++;
			seg--;
		}
		if (seg == wlen && anx_strncmp(start, want, wlen) == 0)
			return true;
		if (*p == ',')
			p++;
	}
	return false;
}

int anx_icm_tag(const anx_oid_t *oid, const char *domain, const char *kind,
		const char *authority, const char *status,
		const char *stack, const char *entry)
{
	struct anx_state_object *obj;

	if (!oid)
		return ANX_EINVAL;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_ENOENT;
	if (!obj->user_meta)
		return ANX_EINVAL;

	if (domain)
		anx_meta_set_str(obj->user_meta, ANX_ICM_KEY_DOMAIN, domain);
	if (kind)
		anx_meta_set_str(obj->user_meta, ANX_ICM_KEY_KIND, kind);
	if (authority)
		anx_meta_set_str(obj->user_meta, ANX_ICM_KEY_AUTHORITY, authority);
	if (status)
		anx_meta_set_str(obj->user_meta, ANX_ICM_KEY_STATUS, status);
	if (stack)
		anx_meta_set_str(obj->user_meta, ANX_ICM_KEY_STACK, stack);
	if (entry)
		anx_meta_set_str(obj->user_meta, ANX_ICM_KEY_ENTRY, entry);

	return ANX_OK;
}

int anx_icm_publish(const anx_oid_t *oid, const char *release_uri)
{
	struct anx_state_object *obj;

	if (!oid || !release_uri || release_uri[0] == '\0')
		return ANX_EINVAL;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_ENOENT;
	if (!obj->user_meta)
		return ANX_EINVAL;

	anx_meta_set_str(obj->user_meta, ANX_ICM_KEY_PUBLISHED, release_uri);
	return ANX_OK;
}

static void fill_view(struct anx_state_object *obj, struct anx_icm_view *out)
{
	out->oid = obj->oid;
	out->object_type = obj->object_type;
	if (obj->user_meta) {
		read_str(obj->user_meta, ANX_ICM_KEY_DOMAIN,
			 out->domain, sizeof(out->domain));
		read_str(obj->user_meta, ANX_ICM_KEY_KIND,
			 out->kind, sizeof(out->kind));
		read_str(obj->user_meta, ANX_ICM_KEY_AUTHORITY,
			 out->authority, sizeof(out->authority));
		read_str(obj->user_meta, ANX_ICM_KEY_STATUS,
			 out->status, sizeof(out->status));
		read_str(obj->user_meta, ANX_ICM_KEY_STACK,
			 out->stack, sizeof(out->stack));
		read_str(obj->user_meta, ANX_ICM_KEY_PUBLISHED,
			 out->published, sizeof(out->published));
	} else {
		out->domain[0] = '\0';
		out->kind[0] = '\0';
		out->authority[0] = '\0';
		out->status[0] = '\0';
		out->stack[0] = '\0';
		out->published[0] = '\0';
	}
}

int anx_icm_read_view(const anx_oid_t *oid, struct anx_icm_view *out)
{
	struct anx_state_object *obj;

	if (!oid || !out)
		return ANX_EINVAL;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_ENOENT;

	fill_view(obj, out);
	return ANX_OK;
}

/* Context threaded through the objstore iterator. */
struct catalog_ctx {
	const char *domain;	/* NULL = no filter */
	anx_icm_visit_fn cb;
	void *arg;
	int stop;		/* first non-zero cb return */
};

static int catalog_iter(struct anx_state_object *obj, void *arg)
{
	struct catalog_ctx *ctx = arg;
	struct anx_icm_view view;

	fill_view(obj, &view);

	if (ctx->domain && !domain_contains(view.domain, ctx->domain))
		return 0;	/* skip, keep iterating */

	ctx->stop = ctx->cb(&view, ctx->arg);
	return ctx->stop;	/* non-zero stops anx_objstore_iterate */
}

int anx_icm_catalog(const char *domain, anx_icm_visit_fn cb, void *arg)
{
	struct catalog_ctx ctx;
	int ret;

	if (!cb)
		return ANX_EINVAL;

	ctx.domain = (domain && domain[0]) ? domain : NULL;
	ctx.cb = cb;
	ctx.arg = arg;
	ctx.stop = 0;

	ret = anx_objstore_iterate(catalog_iter, &ctx);
	if (ret != ANX_OK && ctx.stop == 0)
		return ret;	/* iteration machinery error */
	return ctx.stop;	/* 0, or the cb's stop value */
}

static int count_cb(const struct anx_icm_view *view, void *arg)
{
	(void)view;
	(*(int *)arg)++;
	return 0;
}

int anx_icm_count(const char *domain)
{
	int n = 0;
	int ret = anx_icm_catalog(domain, count_cb, &n);

	if (ret != 0)
		return ret;
	return n;
}

/* Context for the published-only view: wraps the caller's visitor and skips
 * artifacts with no anno.icm.published marker. */
struct published_ctx {
	anx_icm_visit_fn cb;
	void *arg;
	int stop;
};

static int published_cb(const struct anx_icm_view *view, void *arg)
{
	struct published_ctx *ctx = arg;

	if (view->published[0] == '\0')
		return 0;	/* not a published release; keep iterating */

	ctx->stop = ctx->cb(view, ctx->arg);
	return ctx->stop;
}

int anx_icm_published(anx_icm_visit_fn cb, void *arg)
{
	struct published_ctx ctx;
	int ret;

	if (!cb)
		return ANX_EINVAL;

	ctx.cb = cb;
	ctx.arg = arg;
	ctx.stop = 0;

	ret = anx_icm_catalog(NULL, published_cb, &ctx);
	if (ret != 0 && ctx.stop == 0)
		return ret;
	return ctx.stop;
}
