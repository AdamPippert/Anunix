/*
 * test_engine_registry.c — Tests for Engine Registry (RFC-0005).
 */

#include <anx/types.h>
#include <anx/engine.h>
#include <anx/uuid.h>

int test_engine_registry(void)
{
	struct anx_engine *eng1, *eng2, *eng3;
	struct anx_engine *results[8];
	uint32_t found;
	int ret;

	anx_engine_registry_init();

	/* Register engines */
	ret = anx_engine_register("local_tool",
				  ANX_ENGINE_DETERMINISTIC_TOOL,
				  ANX_CAP_TOOL_EXECUTION,
				  &eng1);
	if (ret != ANX_OK)
		return -1;

	ret = anx_engine_register("local_model",
				  ANX_ENGINE_LOCAL_MODEL,
				  ANX_CAP_SUMMARIZATION | ANX_CAP_QUESTION_ANSWERING,
				  &eng2);
	if (ret != ANX_OK)
		return -2;

	ret = anx_engine_register("remote_model",
				  ANX_ENGINE_REMOTE_MODEL,
				  ANX_CAP_LONG_CONTEXT | ANX_CAP_SUMMARIZATION,
				  &eng3);
	if (ret != ANX_OK)
		return -3;

	eng3->is_local = false;
	eng3->requires_network = true;

	/* Lookup by EID */
	{
		struct anx_engine *found_eng = anx_engine_lookup(&eng1->eid);
		if (!found_eng)
			return -4;
		if (found_eng != eng1)
			return -5;
	}

	/* Find by class */
	ret = anx_engine_find(ANX_ENGINE_LOCAL_MODEL, 0, results, 8, &found);
	if (ret != ANX_OK)
		return -6;
	if (found != 1)
		return -7;

	/* Find by class + capability */
	ret = anx_engine_find(ANX_ENGINE_REMOTE_MODEL,
			      ANX_CAP_SUMMARIZATION,
			      results, 8, &found);
	if (ret != ANX_OK)
		return -8;
	if (found != 1)
		return -9;

	/* Set offline, should be excluded from find */
	anx_engine_set_status(eng3, ANX_ENGINE_OFFLINE);
	ret = anx_engine_find(ANX_ENGINE_REMOTE_MODEL, 0, results, 8, &found);
	if (ret != ANX_OK)
		return -10;
	if (found != 0)
		return -11;

	/* Unregister */
	anx_engine_unregister(eng1);
	anx_engine_unregister(eng2);
	anx_engine_unregister(eng3);

	return 0;
}
