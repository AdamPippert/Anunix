/*
 * anx/access.h — Capability-based access policy.
 */

#ifndef ANX_ACCESS_H
#define ANX_ACCESS_H

#include <anx/types.h>

/* Operations that can be controlled by access policy */
enum anx_access_op {
	ANX_ACCESS_READ_PAYLOAD	= (1 << 0),
	ANX_ACCESS_READ_META	= (1 << 1),
	ANX_ACCESS_WRITE_PAYLOAD = (1 << 2),
	ANX_ACCESS_WRITE_META	= (1 << 3),
	ANX_ACCESS_SEAL		= (1 << 4),
	ANX_ACCESS_DELETE	= (1 << 5),
	ANX_ACCESS_DERIVE	= (1 << 6),
	ANX_ACCESS_QUERY_PROV	= (1 << 7),
};

/* Effect of an access rule */
enum anx_access_effect {
	ANX_EFFECT_ALLOW,
	ANX_EFFECT_DENY,
};

/* Audit level for access logging */
enum anx_audit_level {
	ANX_AUDIT_NONE,		/* no access logging */
	ANX_AUDIT_DENIED,	/* only denied attempts */
	ANX_AUDIT_WRITE,	/* denied + successful writes/deletes */
	ANX_AUDIT_ALL,		/* every access evaluation */
};

/* A single access policy rule */
struct anx_access_rule {
	anx_cid_t principal;		/* cell this rule applies to (nil = any) */
	uint32_t operations;		/* bitmask of anx_access_op */
	enum anx_access_effect effect;
};

#define ANX_MAX_ACCESS_RULES	16

/* Access policy for a state object */
struct anx_access_policy {
	struct anx_access_rule rules[ANX_MAX_ACCESS_RULES];
	uint32_t rule_count;
	enum anx_audit_level audit;
};

/* Evaluate access. Returns ANX_OK if allowed, ANX_EPERM if denied. */
int anx_access_evaluate(const struct anx_access_policy *policy,
			const anx_cid_t *cell,
			const anx_oid_t *creator_cell,
			enum anx_access_op op);

#endif /* ANX_ACCESS_H */
