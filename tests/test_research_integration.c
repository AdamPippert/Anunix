/*
 * test_research_integration.c — Composition across research days 077 to 082.
 *
 * Days 077 through 082 each landed on their own branch and each shipped
 * their own probe in kernel/core/tools/research_dayNNN.c. Every probe
 * exercises one day's subsystem alone. Nothing exercised two of them at
 * once, and nothing in the host suite covered these days at all.
 *
 * These cases sit on the seams, at the places where a later day reuses or
 * sits beside an earlier day's state:
 *
 *   A. Days 081 and 082 both add fields to struct anx_cell, and both hook
 *      the same cell at dispatch. 081 adds routing.catalog_id, .catalog_epoch
 *      and .catalog_index; 082 adds group enrollment checked by
 *      anx_continuation_group_check(). A group's whole lifecycle must leave
 *      the routing selection untouched, and admission must follow the group,
 *      not the routing selection.
 *
 *   B. Days 078 and 082 both account physical pages. 078 reclaims a
 *      continuation's pages through anx_continuation_idle_reclaim(); 082
 *      reserves shared scratch pages through anx_continuation_group_create().
 *      A reclaim must not disturb scratch the group already holds.
 *
 *   C. Day 077 creates owner-bound, epoch-versioned workloads. Day 082
 *      moves execution authority between cells with a group handoff. A
 *      handoff must not shift a workload's identity, epoch or owner.
 *
 *   D. Days 081 and 082 both gate the same cell at dispatch, through
 *      different paths. The two gates must stay independent: revoking a
 *      group must not change what the planner reports, and a rejected
 *      catalog index must not change whether the group admits the cell.
 *
 * Each case returns a distinct negative code so a failure names its seam.
 */

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/continuation.h>
#include <anx/continuation_group.h>
#include <anx/route.h>
#include <anx/route_catalog.h>
#include <anx/workload.h>
#include <anx/engine.h>
#include <anx/string.h>
#include <anx/uuid.h>

#define SCRATCH_OFF	64U
#define SCRATCH_LEN	16U
static const char scratch_pattern[SCRATCH_LEN] = "seam-078-082-ab";

struct seam_cells {
	struct anx_cell *owner;
	struct anx_cell *member[2];
};

/* Build one owner with two direct children, the shape a group needs. */
static int seam_cells_make(struct seam_cells *c, const char *name)
{
	struct anx_cell_intent intent;
	uint32_t i;
	int ret;

	anx_memset(c, 0, sizeof(*c));
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, name, sizeof(intent.name));

	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &c->owner);
	if (ret != ANX_OK)
		return ret;
	c->owner->execution.allow_recursive_cells = true;
	c->owner->execution.allow_side_effects = true;

	for (i = 0; i < 2; i++) {
		ret = anx_cell_derive_child(c->owner, ANX_CELL_TASK_EXTERNAL_CALL,
					    &intent, &c->member[i]);
		if (ret != ANX_OK)
			return ret;
	}
	return ANX_OK;
}

/*
 * Seam A — a group's lifecycle must not touch a cell's routing selection.
 *
 * Day 081 stores its catalog selection in struct anx_cell. Day 082 enrolls
 * the same struct in a group and mutates group state on it. If either day
 * reused a field, or cleared more of the cell than it owns, the selection
 * would not survive create, handoff and revoke.
 */
static int seam_cell_fields_are_disjoint(void)
{
	struct seam_cells c;
	struct anx_continuation_group_view group, after;
	anx_cid_t members[2];
	const uint64_t marker_id = 0x5150818181818151ULL;
	const uint64_t marker_epoch = 0x0081008100810081ULL;
	const uint32_t marker_index = 0x81u;
	uint32_t i;

	if (seam_cells_make(&c, "seam-a") != ANX_OK)
		return -9001;

	/* Day 081 state, written before day 082 ever sees the cells. */
	for (i = 0; i < 2; i++) {
		c.member[i]->routing.catalog_id = marker_id;
		c.member[i]->routing.catalog_epoch = marker_epoch;
		c.member[i]->routing.catalog_index = marker_index;
		members[i] = c.member[i]->cid;
	}

	if (anx_continuation_group_create(&c.owner->cid, members, 2, 1, &group) != ANX_OK)
		return -9002;

	for (i = 0; i < 2; i++)
		if (c.member[i]->routing.catalog_id != marker_id ||
		    c.member[i]->routing.catalog_epoch != marker_epoch ||
		    c.member[i]->routing.catalog_index != marker_index)
			return -9003;	/* group create clobbered day 081 state */

	if (anx_continuation_group_handoff(group.id, group.epoch,
					   &c.member[1]->cid, &after) != ANX_OK)
		return -9004;

	for (i = 0; i < 2; i++)
		if (c.member[i]->routing.catalog_id != marker_id ||
		    c.member[i]->routing.catalog_epoch != marker_epoch ||
		    c.member[i]->routing.catalog_index != marker_index)
			return -9005;	/* handoff clobbered day 081 state */

	if (anx_continuation_group_revoke(group.id, after.epoch, &after) != ANX_OK)
		return -9006;

	for (i = 0; i < 2; i++)
		if (c.member[i]->routing.catalog_id != marker_id ||
		    c.member[i]->routing.catalog_epoch != marker_epoch ||
		    c.member[i]->routing.catalog_index != marker_index)
			return -9007;	/* revoke clobbered day 081 state */

	if (!after.revoked)
		return -9008;

	anx_continuation_group_destroy(group.id);
	return ANX_OK;
}

/*
 * Seam B — reclaiming a continuation's pages must leave group scratch alone.
 *
 * Both days count pages in a field they both call physical_pages. Day 078
 * hands anx_continuation_idle_reclaim() a max_pages budget; day 082 reserves
 * scratch at group creation. The reclaim runs against a continuation the
 * group does not contain, so the scratch bytes and the group's page count
 * must both come through unchanged.
 */
static int seam_reclaim_spares_group_scratch(void)
{
	struct seam_cells c;
	struct anx_continuation_group_view group, after;
	struct anx_continuation_view cont;
	struct anx_continuation_idle_request idle;
	anx_cid_t members[2];
	char readback[SCRATCH_LEN];
	uint32_t i;
	int reclaimed;

	if (seam_cells_make(&c, "seam-b") != ANX_OK)
		return -9101;
	for (i = 0; i < 2; i++)
		members[i] = c.member[i]->cid;

	if (anx_continuation_group_create(&c.owner->cid, members, 2, 2, &group) != ANX_OK)
		return -9102;
	if (group.physical_pages == 0)
		return -9103;

	if (anx_continuation_group_write(group.id, group.epoch, SCRATCH_OFF,
					 scratch_pattern, SCRATCH_LEN) != ANX_OK)
		return -9104;

	/* A continuation owned by the same cell, outside the group. */
	if (anx_continuation_create(&c.owner->cid, &cont) != ANX_OK)
		return -9105;

	anx_memset(&idle, 0, sizeof(idle));
	idle.lineage_parent = c.owner->cid;
	idle.phase_epoch = cont.phase_epoch;
	idle.max_pages = group.physical_pages + 4u;

	/*
	 * The reclaim itself may legitimately refuse this continuation: it has
	 * no configured model source and has never waited. Either answer is
	 * acceptable. What is not acceptable is collateral damage to the group.
	 */
	reclaimed = anx_continuation_idle_reclaim(cont.id, cont.epoch, &idle, &cont);
	(void)reclaimed;

	anx_memset(readback, 0, sizeof(readback));
	if (anx_continuation_group_read(group.id, group.epoch, SCRATCH_OFF,
					readback, SCRATCH_LEN) != ANX_OK)
		return -9106;
	if (anx_memcmp(readback, scratch_pattern, SCRATCH_LEN) != 0)
		return -9107;	/* reclaim corrupted group scratch */

	if (anx_continuation_group_get(group.id, &after) != ANX_OK)
		return -9108;
	if (after.physical_pages != group.physical_pages)
		return -9109;	/* reclaim stole pages the group had reserved */
	if (after.revoked)
		return -9110;

	anx_continuation_destroy(cont.id);
	anx_continuation_group_destroy(group.id);
	return ANX_OK;
}

/*
 * Seam C — a group handoff must not move a workload's identity.
 *
 * Day 077 binds a workload to an owner cid at creation. Day 082 moves the
 * execution holder inside a group owned by that same cell. Execution
 * authority moving is not the same thing as ownership moving, and the
 * workload's id, epoch and owner must all read back unchanged.
 */
static int seam_handoff_preserves_workload(void)
{
	struct seam_cells c;
	struct anx_workload_contract contract;
	struct anx_workload_view before, after;
	struct anx_continuation_group_view group, moved;
	anx_cid_t members[2];
	uint32_t i;

	if (seam_cells_make(&c, "seam-c") != ANX_OK)
		return -9201;
	for (i = 0; i < 2; i++)
		members[i] = c.member[i]->cid;

	anx_memset(&contract, 0, sizeof(contract));
	contract.minimum_output = 1;
	contract.maximum_output = 4;
	contract.maximum_tokens = 64;
	contract.prefix_size = 1;
	contract.prefix[0] = '~';

	if (anx_workload_create(&c.owner->cid, &contract, &before) != ANX_OK)
		return -9202;

	if (anx_continuation_group_create(&c.owner->cid, members, 2, 1, &group) != ANX_OK)
		return -9203;
	if (anx_continuation_group_handoff(group.id, group.epoch,
					   &c.member[1]->cid, &moved) != ANX_OK)
		return -9204;
	if (moved.holder == group.holder)
		return -9205;	/* handoff did not actually move the holder */

	if (anx_workload_get(before.id, &after) != ANX_OK)
		return -9206;
	if (after.id != before.id || after.epoch != before.epoch)
		return -9207;	/* handoff shifted the workload's identity */
	if (anx_uuid_compare(&after.owner, &before.owner) != 0)
		return -9208;	/* handoff moved workload ownership */
	if (after.token_limit != before.token_limit ||
	    after.candidates != before.candidates)
		return -9209;	/* handoff perturbed the frozen contract */

	anx_workload_destroy(before.id);
	anx_continuation_group_destroy(group.id);
	return ANX_OK;
}

/*
 * Seam D — the two dispatch-time gates stay independent.
 *
 * Day 081 rejects an out-of-range catalog index and keeps the incumbent
 * weights; day 082 refuses admission once a group is revoked. A cell can
 * be subject to both. Neither outcome may leak into the other: a revoked
 * group must not change the planner's verdict, and a bad catalog index must
 * not change admission.
 */
static int seam_dispatch_gates_are_independent(void)
{
	struct seam_cells c;
	struct anx_continuation_group_view group, after;
	struct anx_route_result route_live, route_revoked;
	anx_cid_t members[2];
	uint32_t i;
	int admit_live, admit_revoked, plan_live, plan_revoked;

	if (seam_cells_make(&c, "seam-d") != ANX_OK)
		return -9301;
	for (i = 0; i < 2; i++)
		members[i] = c.member[i]->cid;

	/* A catalog id that names nothing, with an index day 081 must reject. */
	c.member[0]->routing.catalog_id = 0;
	c.member[0]->routing.catalog_epoch = 0;
	c.member[0]->routing.catalog_index = ~(uint32_t)0;

	if (anx_continuation_group_create(&c.owner->cid, members, 2, 1, &group) != ANX_OK)
		return -9302;

	/* member[0] is the holder at creation, so the group gate admits it. */
	admit_live = anx_continuation_group_check(c.member[0]);
	if (admit_live != ANX_OK)
		return -9303;

	anx_memset(&route_live, 0, sizeof(route_live));
	plan_live = anx_route_plan(c.member[0], &route_live);

	if (anx_continuation_group_revoke(group.id, group.epoch, &after) != ANX_OK)
		return -9304;
	if (!after.revoked)
		return -9305;

	admit_revoked = anx_continuation_group_check(c.member[0]);

	anx_memset(&route_revoked, 0, sizeof(route_revoked));
	plan_revoked = anx_route_plan(c.member[0], &route_revoked);

	/*
	 * The planner's verdict about the catalog selection is the day 081
	 * gate. Revoking the group is the day 082 gate. Revocation must not
	 * move the first.
	 */
	if (plan_live != plan_revoked)
		return -9307;	/* group revocation leaked into the planner's verdict */
	if (route_live.profile_status != route_revoked.profile_status)
		return -9310;	/* group revocation leaked into routing */
	if (route_live.profile_applied != route_revoked.profile_applied)
		return -9308;	/* group revocation leaked into routing */

	/*
	 * And the reverse: the group gate must actually respond to revocation
	 * even though the cell carries a selection day 081 rejects. If both
	 * answers are identical the group gate is not reading group state.
	 */
	if (admit_revoked == ANX_OK)
		return -9309;	/* revoked group still admits the cell */

	anx_continuation_group_destroy(group.id);
	return ANX_OK;
}

int test_research_integration(void)
{
	int ret;

	struct anx_engine *engine;

	anx_cell_store_init();
	anx_engine_registry_init();
	anx_route_planner_init();

	/* The planner needs at least one candidate engine to report on. */
	if (anx_engine_register("seam-engine", ANX_ENGINE_RETRIEVAL_SERVICE,
				ANX_CAP_SEMANTIC_RETRIEVAL, &engine) != ANX_OK)
		return -9000;

	ret = seam_cell_fields_are_disjoint();
	if (ret != ANX_OK)
		return ret;

	ret = seam_reclaim_spares_group_scratch();
	if (ret != ANX_OK)
		return ret;

	ret = seam_handoff_preserves_workload();
	if (ret != ANX_OK)
		return ret;

	ret = seam_dispatch_gates_are_independent();
	if (ret != ANX_OK)
		return ret;

	return ANX_OK;
}
