/*
 * resolve.c — Turn a shell argument into a State Object OID.
 *
 * Every tool that takes <oid-or-path> used to carry its own resolver,
 * and none of them accepted an OID: `cat <oid>` failed on the OIDs that
 * `write` and `search` print. One resolver serves them all.
 */

#include <anx/types.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct prefix_match {
	const char *prefix;
	uint32_t len;
	uint32_t hits;
	anx_oid_t oid;
};

static int match_prefix(struct anx_state_object *obj, void *arg)
{
	struct prefix_match *m = arg;
	char buf[37];

	anx_uuid_to_string(&obj->oid, buf, sizeof(buf));
	if (anx_strncmp(buf, m->prefix, m->len) == 0) {
		if (m->hits == 0 || anx_uuid_compare(&m->oid, &obj->oid) != 0)
			m->hits++;
		m->oid = obj->oid;
	}
	return 0;
}

static bool is_oid_prefix(const char *s)
{
	uint32_t n = 0;

	for (; *s; s++, n++) {
		if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
		      (*s >= 'A' && *s <= 'F') || *s == '-'))
			return false;
	}
	return n >= 4 && n <= 36;
}

int anx_so_resolve(const char *arg, anx_oid_t *oid)
{
	struct prefix_match m;
	const char *colon = arg;
	char lower[37];
	uint32_t i;

	while (*colon && *colon != ':')
		colon++;
	if (*colon == ':') {
		char ns_buf[64];
		uint32_t ns_len = (uint32_t)(colon - arg);

		if (ns_len >= sizeof(ns_buf))
			return ANX_EINVAL;
		anx_memcpy(ns_buf, arg, ns_len);
		ns_buf[ns_len] = '\0';
		return anx_ns_resolve(ns_buf, colon + 1, oid);
	}

	if (anx_ns_resolve("default", arg, oid) == ANX_OK)
		return ANX_OK;
	if (anx_ns_resolve("posix", arg, oid) == ANX_OK)
		return ANX_OK;
	if (!is_oid_prefix(arg))
		return ANX_ENOENT;

	for (i = 0; arg[i]; i++)
		lower[i] = (arg[i] >= 'A' && arg[i] <= 'F') ?
			   (char)(arg[i] - 'A' + 'a') : arg[i];
	lower[i] = '\0';

	anx_memset(&m, 0, sizeof(m));
	m.prefix = lower;
	m.len = i;
	anx_objstore_iterate(match_prefix, &m);
	if (m.hits == 0)
		return ANX_ENOENT;
	if (m.hits > 1)
		return ANX_EEXIST;
	*oid = m.oid;
	return ANX_OK;
}
