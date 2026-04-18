/*
 * test_route_planner.c — Route planner topology affinity.
 *
 * Verifies that the planner picks the engine whose declared
 * boundary-key range overlaps the cell's topology intent, penalizes
 * engines specialized elsewhere, and stays neutral toward engines
 * that declare no affinity.
 */

#include <anx/types.h>
#include <anx/route.h>
#include <anx/engine.h>
#include <anx/cell.h>
#include <anx/uuid.h>
#include <anx/string.h>

static bool eid_eq(const anx_eid_t *a, const anx_eid_t *b)
{
	return a->hi == b->hi && a->lo == b->lo;
}

int test_route_planner(void)
{
	struct anx_engine *near, *far, *generalist;
	struct anx_cell *cell;
	struct anx_cell_intent intent;
	struct anx_route_result result;
	int32_t s_near, s_far, s_gen;

	anx_engine_registry_init();
	anx_cell_store_init();

	if (anx_engine_register("near", ANX_ENGINE_RETRIEVAL_SERVICE,
				ANX_CAP_SEMANTIC_RETRIEVAL,
				&near) != ANX_OK)
		return -1;
	if (anx_engine_register("far", ANX_ENGINE_RETRIEVAL_SERVICE,
				ANX_CAP_SEMANTIC_RETRIEVAL,
				&far) != ANX_OK)
		return -2;
	if (anx_engine_register("generalist", ANX_ENGINE_RETRIEVAL_SERVICE,
				ANX_CAP_SEMANTIC_RETRIEVAL,
				&generalist) != ANX_OK)
		return -3;

	/* All three have identical baseline so the topology signal is
	 * the only differentiator. */
	near->quality_score = 50;
	far->quality_score = 50;
	generalist->quality_score = 50;

	/* Affinity declarations. */
	if (anx_engine_set_topology(near, 1000, 2000) != ANX_OK)
		return -4;
	if (anx_engine_set_topology(far, 5000, 6000) != ANX_OK)
		return -5;
	/* generalist: no affinity. */

	/* Bounds validation. */
	if (anx_engine_set_topology(near, 100, 50) != ANX_EINVAL)
		return -6;
	if (anx_engine_set_topology(NULL, 0, 0) != ANX_EINVAL)
		return -7;

	/* --- Case A: cell's range overlaps near, not far --- */
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "query_A", sizeof(intent.name));
	if (anx_cell_create(ANX_CELL_TASK_RETRIEVAL, &intent, &cell) != ANX_OK)
		return -10;
	if (anx_cell_set_topology(cell, 1500, 1600) != ANX_OK)
		return -11;

	s_near = anx_route_score_engine(cell, near);
	s_far  = anx_route_score_engine(cell, far);
	s_gen  = anx_route_score_engine(cell, generalist);

	/* near: +25 overlap; far: -15 mismatch; generalist: neutral. */
	if (s_near != s_gen + 25)
		return -12;
	if (s_far  != s_gen - 15)
		return -13;
	if (!(s_near > s_gen && s_gen > s_far))
		return -14;

	if (anx_route_plan(cell, &result) != ANX_OK)
		return -15;
	if (result.candidate_count == 0)
		return -16;
	if (!eid_eq(&result.candidates[result.selected_index].engine_id,
		    &near->eid))
		return -17;

	anx_cell_destroy(cell);

	/* --- Case B: cell's range matches far, not near --- */
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "query_B", sizeof(intent.name));
	if (anx_cell_create(ANX_CELL_TASK_RETRIEVAL, &intent, &cell) != ANX_OK)
		return -20;
	if (anx_cell_set_topology(cell, 5500, 5800) != ANX_OK)
		return -21;

	if (anx_route_plan(cell, &result) != ANX_OK)
		return -22;
	if (!eid_eq(&result.candidates[result.selected_index].engine_id,
		    &far->eid))
		return -23;

	anx_cell_destroy(cell);

	/* --- Case C: cell's range matches neither specialist --- */
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "query_C", sizeof(intent.name));
	if (anx_cell_create(ANX_CELL_TASK_RETRIEVAL, &intent, &cell) != ANX_OK)
		return -30;
	if (anx_cell_set_topology(cell, 10000, 11000) != ANX_OK)
		return -31;

	if (anx_route_plan(cell, &result) != ANX_OK)
		return -32;
	/* Both specialists get -15, generalist stays neutral. */
	if (!eid_eq(&result.candidates[result.selected_index].engine_id,
		    &generalist->eid))
		return -33;

	anx_cell_destroy(cell);

	/* --- Case D: cell declares no topology -> no affinity effect --- */
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "query_D", sizeof(intent.name));
	if (anx_cell_create(ANX_CELL_TASK_RETRIEVAL, &intent, &cell) != ANX_OK)
		return -40;

	s_near = anx_route_score_engine(cell, near);
	s_far  = anx_route_score_engine(cell, far);
	s_gen  = anx_route_score_engine(cell, generalist);

	if (s_near != s_gen)
		return -41;
	if (s_far != s_gen)
		return -42;

	anx_cell_destroy(cell);

	/* --- Case E: clear_topology reverts affinity --- */
	anx_engine_clear_topology(near);
	if (near->has_topology_affinity)
		return -50;
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "query_E", sizeof(intent.name));
	if (anx_cell_create(ANX_CELL_TASK_RETRIEVAL, &intent, &cell) != ANX_OK)
		return -51;
	if (anx_cell_set_topology(cell, 1500, 1600) != ANX_OK)
		return -52;

	s_near = anx_route_score_engine(cell, near);
	s_gen  = anx_route_score_engine(cell, generalist);
	/* near is now a generalist too -> tied with generalist. */
	if (s_near != s_gen)
		return -53;

	anx_cell_destroy(cell);

	anx_engine_unregister(near);
	anx_engine_unregister(far);
	anx_engine_unregister(generalist);
	return 0;
}
