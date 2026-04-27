/*
 * object_group.c — Object Group registry for sandbox access control.
 *
 * Static table of up to ANX_OBJECT_GROUP_MAX groups.  OIDs are minted
 * from a monotonic counter tagged "ANXGRP\0\0" so a group OID is never
 * confused with a State Object OID.  Once sealed a group's grants are
 * immutable; revoking access means dropping the reference from a
 * sandbox lens, not editing the group.
 */

#include <anx/types.h>
#include <anx/object_group.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>

#define ANX_OBJECT_GROUP_MAX	16

static struct anx_object_group group_table[ANX_OBJECT_GROUP_MAX];
static uint64_t group_oid_seq;
static bool     group_initialized;

void anx_object_group_init(void)
{
	anx_memset(group_table, 0, sizeof(group_table));
	group_oid_seq = 0;
	group_initialized = true;
}

static bool oid_eq(const anx_oid_t *a, const anx_oid_t *b)
{
	return a->hi == b->hi && a->lo == b->lo;
}

static bool oid_zero(const anx_oid_t *o)
{
	return o->hi == 0 && o->lo == 0;
}

static anx_oid_t group_oid_mint(void)
{
	anx_oid_t oid;

	oid.hi = 0x414e58475250ULL << 16;	/* "ANXGRP\0\0" */
	oid.lo = ++group_oid_seq;
	return oid;
}

struct anx_object_group *anx_object_group_get(const anx_oid_t *oid)
{
	uint32_t i;

	if (!group_initialized || !oid)
		return NULL;
	for (i = 0; i < ANX_OBJECT_GROUP_MAX; i++) {
		if (group_table[i].in_use &&
		    oid_eq(&group_table[i].oid, oid))
			return &group_table[i];
	}
	return NULL;
}

int anx_object_group_create(const char *name, anx_oid_t *oid_out)
{
	uint32_t i;
	struct anx_object_group *g = NULL;

	if (!group_initialized || !name || !oid_out || name[0] == '\0')
		return ANX_EINVAL;

	for (i = 0; i < ANX_OBJECT_GROUP_MAX; i++) {
		if (!group_table[i].in_use) {
			g = &group_table[i];
			break;
		}
	}
	if (!g)
		return ANX_ENOMEM;

	anx_memset(g, 0, sizeof(*g));
	g->in_use = true;
	g->oid = group_oid_mint();
	anx_strlcpy(g->name, name, ANX_GROUP_NAME_MAX);
	g->sealed = false;
	g->grant_count = 0;

	*oid_out = g->oid;
	kprintf("group: created '%s'\n", g->name);
	return ANX_OK;
}

int anx_object_group_grant(const anx_oid_t *group_oid,
			   const struct anx_sbx_grant *grant)
{
	struct anx_object_group *g;

	if (!grant)
		return ANX_EINVAL;
	g = anx_object_group_get(group_oid);
	if (!g)
		return ANX_ENOENT;
	if (g->sealed)
		return ANX_EPERM;
	if (g->grant_count >= ANX_GROUP_MAX_GRANTS)
		return ANX_ENOMEM;

	/*
	 * Reject empty grants — a grant must either name a specific OID
	 * or carry a namespace prefix.  An entry with neither would
	 * silently match nothing and is almost certainly a bug.
	 */
	if (oid_zero(&grant->oid) && grant->ns_prefix[0] == '\0')
		return ANX_EINVAL;
	if (grant->op_mask == 0)
		return ANX_EINVAL;

	g->grants[g->grant_count] = *grant;
	g->grant_count++;
	return ANX_OK;
}

int anx_object_group_seal(const anx_oid_t *group_oid)
{
	struct anx_object_group *g = anx_object_group_get(group_oid);

	if (!g)
		return ANX_ENOENT;
	g->sealed = true;
	return ANX_OK;
}

int anx_object_group_destroy(const anx_oid_t *group_oid)
{
	struct anx_object_group *g = anx_object_group_get(group_oid);

	if (!g)
		return ANX_ENOENT;
	anx_memset(g, 0, sizeof(*g));
	return ANX_OK;
}

/*
 * A grant matches if either the OID is the same, or the grant has a
 * namespace prefix.  In Phase 1 we don't yet know an object's namespace
 * path inside the lens-check fast path, so prefix grants are treated as
 * "applies to any OID" — i.e. namespace prefixes are an honour-system
 * scoping mechanism until namespace-aware lookups land.
 */
static bool grant_matches(const struct anx_sbx_grant *gr,
			  const anx_oid_t *oid)
{
	if (!oid_zero(&gr->oid))
		return oid_eq(&gr->oid, oid);
	/* prefix-only grant: matches any OID for now */
	return gr->ns_prefix[0] != '\0';
}

bool anx_object_group_check(const struct anx_object_group *g,
			    const anx_oid_t *oid, uint32_t op)
{
	uint32_t i;

	if (!g || !oid)
		return false;
	for (i = 0; i < g->grant_count; i++) {
		const struct anx_sbx_grant *gr = &g->grants[i];

		if ((gr->op_mask & op) != op)
			continue;
		if (grant_matches(gr, oid))
			return true;
	}
	return false;
}
