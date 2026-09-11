/* Session affinity is subordinate to current backend and cell eligibility. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/route.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

int anx_research_day015(void)
{
	struct anx_cell *cell = anx_zalloc(sizeof(*cell));
	struct anx_engine *a = NULL, *b = NULL;
	struct anx_route_session session = {0}, saved;
	struct anx_route_result result = {0}, previous;
	int rc = ANX_ENOMEM;
	uint32_t i;
	const enum anx_engine_status unavailable[] = {ANX_ENGINE_REGISTERED,
		ANX_ENGINE_LOADING, ANX_ENGINE_READY, ANX_ENGINE_DRAINING,
		ANX_ENGINE_UNLOADING, ANX_ENGINE_OFFLINE, ANX_ENGINE_MAINTENANCE};

	if (!cell)
		goto out;
	rc = anx_engine_register("research-day-015-a", ANX_ENGINE_LOCAL_MODEL,
		ANX_CAP_SUMMARIZATION, &a);
	if (rc != ANX_OK)
		goto out;
	rc = anx_engine_register("research-day-015-b", ANX_ENGINE_LOCAL_MODEL,
		ANX_CAP_SUMMARIZATION, &b);
	if (rc != ANX_OK)
		goto out;
	a->is_local = b->is_local = true;
	a->quality_score = 90;
	b->quality_score = 10;
	cell->constraints.locality = ANX_LOCAL_ONLY;
	session.eligible_engines[0] = a->eid;
	session.eligible_engines[1] = b->eid;
	session.engine_count = 2;
	session.required_caps = ANX_CAP_SUMMARIZATION;
	rc = -1501;
	if (anx_route_plan_session(cell, &session, &result) != ANX_OK ||
	    anx_uuid_compare(&session.selected_engine, &a->eid) != 0 || session.placement_count != 1)
		goto out;
	a->quality_score = 1;
	b->quality_score = 100;
	rc = -1502;
	if (anx_route_plan_session(cell, &session, &result) != ANX_OK ||
	    anx_uuid_compare(&session.selected_engine, &a->eid) != 0 || session.placement_count != 2)
		goto out;
	for (i = 0; i < sizeof(unavailable) / sizeof(unavailable[0]); i++) {
		a->status = unavailable[i];
		b->status = ANX_ENGINE_AVAILABLE;
		session.selected_engine = a->eid;
		rc = -1503;
		if (anx_route_plan_session(cell, &session, &result) != ANX_OK ||
		    anx_uuid_compare(&session.selected_engine, &b->eid) != 0 || result.candidates[0].feasible)
			goto out;
	}
	a->status = ANX_ENGINE_AVAILABLE;
	b->requires_network = true;
	rc = -1504;
	if (anx_route_plan_session(cell, &session, &result) != ANX_OK ||
	    anx_uuid_compare(&session.selected_engine, &a->eid) != 0 || result.candidates[1].feasible)
		goto out;
	a->capabilities = 0;
	saved = session;
	previous = result;
	rc = -1505;
	if (anx_route_plan_session(cell, &session, &result) != ANX_EPERM ||
	    anx_memcmp(&saved, &session, sizeof(saved)) || anx_memcmp(&previous, &result, sizeof(result)))
		goto out;
	for (i = 0; i < 4; i++) {
		session = saved;
		switch (i) {
		case 0: session.engine_count = ANX_MAX_ROUTE_CANDIDATES + 1; break;
		case 1: session.eligible_engines[1] = session.eligible_engines[0]; break;
		case 2: session.selected_engine = ANX_UUID_NIL; break;
		case 3: session.placement_count = ~(uint64_t)0; break;
		}
		saved = session;
		rc = -1506;
		if (anx_route_plan_session(cell, &session, &result) != (i == 3 ? ANX_EFULL : ANX_EINVAL) ||
		    anx_memcmp(&saved, &session, sizeof(saved)) || anx_memcmp(&previous, &result, sizeof(result)))
			goto out;
		/* Restore the valid shape for the next malformed case. */
		saved.engine_count = 2;
		saved.eligible_engines[1] = b->eid;
		saved.selected_engine = a->eid;
		saved.placement_count = 10;
	}
	rc = ANX_OK;
out:
	if (a)
		anx_engine_unregister(a);
	if (b)
		anx_engine_unregister(b);
	anx_free(cell);
	return rc;
}
#endif
