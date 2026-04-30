/*
 * access.c — Capability-based access policy evaluation (RFC-0002 Phase 2).
 *
 * Rules are evaluated in declaration order; the first rule whose principal
 * and operation mask both match determines the outcome.  A nil principal
 * (all-zero UUID) matches any cell identity.  If no rule matches the
 * request, access is permitted (permissive default, consistent with the
 * Phase 1 always-allow behavior for objects that carry no rules).
 *
 * Cell identity (RFC-0003) is not yet available at the kernel level, so
 * callers pass a nil cell; rules with specific principals therefore only
 * take effect once cell runtime is wired in.
 */

#include <anx/types.h>
#include <anx/access.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>


int anx_access_evaluate(const struct anx_access_policy *policy,
			const anx_cid_t *cell,
			const anx_oid_t *creator_cell,
			enum anx_access_op op)
{
	uint32_t i;

	if (!policy || policy->rule_count == 0)
		return ANX_OK;

	for (i = 0; i < policy->rule_count && i < ANX_MAX_ACCESS_RULES; i++) {
		const struct anx_access_rule *rule = &policy->rules[i];
		bool principal_match;

		/* Nil principal matches any requesting cell */
		if (anx_uuid_is_nil(&rule->principal)) {
			principal_match = true;
		} else if (cell && anx_uuid_compare(&rule->principal, cell) == 0) {
			principal_match = true;
		} else if (creator_cell &&
			   anx_uuid_compare(&rule->principal,
					    (const struct anx_uuid *)creator_cell) == 0) {
			principal_match = true;
		} else {
			principal_match = false;
		}

		if (!principal_match)
			continue;

		if (!(rule->operations & (uint32_t)op))
			continue;

		/* First matching rule determines outcome */
		if (rule->effect == ANX_EFFECT_DENY) {
			if (policy->audit >= ANX_AUDIT_DENIED)
				kprintf("[access] denied op=0x%x rule=%u\n",
					(unsigned)op, i);
			return ANX_EPERM;
		}
		return ANX_OK;
	}

	/* No rule matched: permissive default */
	return ANX_OK;
}
