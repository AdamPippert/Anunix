/*
 * access.c — Capability-based access policy evaluation (RFC-0002 §9.2).
 *
 * Rules are evaluated in order; the first matching rule wins.
 * A rule matches when:
 *   - its operations bitmask covers the requested op, AND
 *   - its principal is the nil CID (matches any caller), OR equals cell.
 *
 * If the creator_cell is provided and matches cell, the creator is
 * treated as the implicit owner and allowed read/write regardless of
 * explicit rules (owner override).
 *
 * If no rule matches the default is ANX_OK (allow).
 */

#include <anx/types.h>
#include <anx/access.h>

static bool cid_eq(const anx_cid_t *a, const anx_cid_t *b)
{
	return a && b && a->hi == b->hi && a->lo == b->lo;
}

static bool cid_nil(const anx_cid_t *c)
{
	return !c || (c->hi == 0 && c->lo == 0);
}

int anx_access_evaluate(const struct anx_access_policy *policy,
			const anx_cid_t *cell,
			const anx_oid_t *creator_cell,
			enum anx_access_op op)
{
	uint32_t i;

	if (!policy || policy->rule_count == 0)
		return ANX_OK;

	/*
	 * Owner override: if cell identity matches the object's creator cell,
	 * allow read and write unconditionally.  This covers the common case
	 * where a cell accesses its own objects without needing explicit rules.
	 */
	if (creator_cell && cell &&
	    creator_cell->hi == cell->hi && creator_cell->lo == cell->lo) {
		uint32_t owner_ops = ANX_ACCESS_READ_PAYLOAD |
				     ANX_ACCESS_READ_META   |
				     ANX_ACCESS_WRITE_PAYLOAD |
				     ANX_ACCESS_WRITE_META;
		if ((uint32_t)op & owner_ops)
			return ANX_OK;
	}

	/* First-match rule evaluation */
	for (i = 0; i < policy->rule_count; i++) {
		const struct anx_access_rule *r = &policy->rules[i];

		/* Skip rules that don't cover this operation */
		if (!(r->operations & (uint32_t)op))
			continue;

		/* Check principal: nil = any, otherwise must match caller */
		if (!cid_nil(&r->principal) && !cid_eq(&r->principal, cell))
			continue;

		/* Rule matches */
		return (r->effect == ANX_EFFECT_DENY) ? ANX_EPERM : ANX_OK;
	}

	/* No matching rule → default allow */
	return ANX_OK;
}
