/*
 * access.c — Access policy evaluation.
 *
 * Currently stubbed: always allows access. Full capability-token
 * based evaluation will be implemented when the Execution Cell
 * runtime (RFC-0003) provides cell identity.
 */

#include <anx/types.h>
#include <anx/access.h>

int anx_access_evaluate(const struct anx_access_policy *policy,
			const anx_cid_t *cell,
			const anx_oid_t *creator_cell,
			enum anx_access_op op)
{
	(void)policy;
	(void)cell;
	(void)creator_cell;
	(void)op;

	/* TODO: implement full evaluation per RFC-0002 Section 9.2 */
	return ANX_OK;
}
