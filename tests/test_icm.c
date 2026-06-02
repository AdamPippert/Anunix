/*
 * test_icm.c — Tests for ICM over State Objects (RFC-0027).
 *
 * Verifies that ICM classification round-trips through anno.icm.* metadata,
 * that role is never stored on the artifact, and that the catalog view filters
 * by domain — all without introducing a new object type.
 */

#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/icm.h>
#include <anx/meta.h>
#include <anx/uuid.h>
#include <anx/string.h>

/* Create a byte_data object and return its oid by value. */
static int make_obj(anx_oid_t *out)
{
	struct anx_state_object *obj;
	struct anx_so_create_params params;
	const char *payload = "artifact";
	int ret;

	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = payload;
	params.payload_size = 8;

	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK)
		return ret;
	*out = obj->oid;
	return ANX_OK;
}

/* Visitor that records how many artifacts and remembers one domain string. */
struct collect {
	int count;
	char last_domain[256];
};

static int collect_cb(const struct anx_icm_view *view, void *arg)
{
	struct collect *c = arg;

	c->count++;
	anx_strlcpy(c->last_domain, view->domain, sizeof(c->last_domain));
	return 0;
}

int test_icm(void)
{
	anx_oid_t a, b;
	struct anx_icm_view view;
	struct collect all, web;
	int ret, n;

	anx_objstore_init();

	if (make_obj(&a) != ANX_OK)
		return -1;
	if (make_obj(&b) != ANX_OK)
		return -2;

	/* Tag artifact A as a web app; B as an infra tool. */
	ret = anx_icm_tag(&a, "web,product", "app", "own", "active",
			  "TypeScript", "npm run dev");
	if (ret != ANX_OK)
		return -3;
	ret = anx_icm_tag(&b, "infra", "tool", "own-local", "active",
			  "C", "make");
	if (ret != ANX_OK)
		return -4;

	/* Read A back: classification round-trips. */
	ret = anx_icm_read_view(&a, &view);
	if (ret != ANX_OK)
		return -5;
	if (anx_strcmp(view.kind, "app") != 0)
		return -6;
	if (anx_strcmp(view.authority, "own") != 0)
		return -7;
	if (anx_strcmp(view.stack, "TypeScript") != 0)
		return -8;

	/* Role must NOT be stored on the artifact (RFC-0027 Section 4.2). */
	if (anx_meta_get(anx_objstore_lookup(&a)->user_meta,
			 "anno.icm.role") != NULL)
		return -9;

	/* Catalog over everything sees both artifacts. */
	all.count = 0;
	all.last_domain[0] = '\0';
	ret = anx_icm_catalog(NULL, collect_cb, &all);
	if (ret != 0)
		return -10;
	if (all.count != 2)
		return -11;

	/* Domain filter: only the web artifact matches "web". */
	web.count = 0;
	web.last_domain[0] = '\0';
	ret = anx_icm_catalog("web", collect_cb, &web);
	if (ret != 0)
		return -12;
	if (web.count != 1)
		return -13;

	/* count() convenience agrees with catalog(). */
	n = anx_icm_count(NULL);
	if (n != 2)
		return -14;
	n = anx_icm_count("infra");
	if (n != 1)
		return -15;

	/* Tagging a missing object fails cleanly. */
	{
		anx_oid_t nil = ANX_UUID_NIL;

		if (anx_icm_tag(&nil, "x", NULL, NULL, NULL, NULL, NULL)
		    != ANX_ENOENT)
			return -16;
	}

	/* Publishing: mark B as a release; it then shows in the published view. */
	{
		struct collect pub;

		/* No artifact is published yet. */
		pub.count = 0;
		pub.last_domain[0] = '\0';
		if (anx_icm_published(collect_cb, &pub) != 0)
			return -17;
		if (pub.count != 0)
			return -18;

		/* Mark B as published; the marker round-trips into the view. */
		ret = anx_icm_publish(&b, "anx:pkg/tool@1.0.0");
		if (ret != ANX_OK)
			return -19;
		if (anx_icm_read_view(&b, &view) != ANX_OK)
			return -20;
		if (anx_strcmp(view.published, "anx:pkg/tool@1.0.0") != 0)
			return -21;

		/* A non-published artifact has an empty marker. */
		if (anx_icm_read_view(&a, &view) != ANX_OK)
			return -22;
		if (view.published[0] != '\0')
			return -23;

		/* The published view now contains exactly B. */
		pub.count = 0;
		if (anx_icm_published(collect_cb, &pub) != 0)
			return -24;
		if (pub.count != 1)
			return -25;

		/* Empty release URI and missing object both fail cleanly. */
		if (anx_icm_publish(&b, "") != ANX_EINVAL)
			return -26;
		{
			anx_oid_t nil = ANX_UUID_NIL;

			if (anx_icm_publish(&nil, "anx:pkg/x@1") != ANX_ENOENT)
				return -27;
		}
	}

	return 0;
}
