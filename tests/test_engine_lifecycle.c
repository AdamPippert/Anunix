/*
 * test_engine_lifecycle.c — Tests for engine lifecycle state machine
 * and model registration.
 */

#include <anx/types.h>
#include <anx/engine.h>
#include <anx/string.h>

int test_engine_lifecycle(void)
{
	struct anx_engine *eng;
	struct anx_model_desc desc;
	int ret;

	anx_engine_registry_init();

	/* --- Model registration --- */

	anx_memset(&desc, 0, sizeof(desc));
	desc.param_count = 8000000000ULL;	/* 8B params */
	desc.quant = ANX_QUANT_Q4;
	desc.context_window = 32768;
	desc.bench_tok_per_sec = 45;
	desc.mem_footprint_bytes = 4500000000ULL;	/* ~4.5 GB */
	desc.offline_capable = true;

	ret = anx_engine_register_model("qwen3-8b-q4",
					ANX_ENGINE_LOCAL_MODEL,
					ANX_CAP_SUMMARIZATION |
					ANX_CAP_QUESTION_ANSWERING,
					&desc, &eng);
	if (ret != ANX_OK)
		return -1;

	/* Model engines start in REGISTERED state */
	if (eng->status != ANX_ENGINE_REGISTERED)
		return -2;

	/* Model descriptor copied correctly */
	if (eng->model.param_count != 8000000000ULL)
		return -3;
	if (eng->model.quant != ANX_QUANT_Q4)
		return -4;
	if (eng->model.context_window != 32768)
		return -5;
	if (eng->model.bench_tok_per_sec != 45)
		return -6;
	if (!eng->model.offline_capable)
		return -7;

	/* Context window propagated to engine */
	if (eng->max_context_tokens != 32768)
		return -8;

	/* Locality set from class */
	if (!eng->is_local)
		return -9;
	if (eng->requires_network)
		return -10;

	/* --- Valid transitions --- */

	/* REGISTERED -> LOADING */
	ret = anx_engine_transition(eng, ANX_ENGINE_LOADING);
	if (ret != ANX_OK)
		return -11;
	if (eng->status != ANX_ENGINE_LOADING)
		return -12;

	/* LOADING -> READY */
	ret = anx_engine_transition(eng, ANX_ENGINE_READY);
	if (ret != ANX_OK)
		return -13;

	/* READY -> AVAILABLE */
	ret = anx_engine_transition(eng, ANX_ENGINE_AVAILABLE);
	if (ret != ANX_OK)
		return -14;

	/* AVAILABLE -> DEGRADED */
	ret = anx_engine_transition(eng, ANX_ENGINE_DEGRADED);
	if (ret != ANX_OK)
		return -15;

	/* DEGRADED -> AVAILABLE (recovery) */
	ret = anx_engine_transition(eng, ANX_ENGINE_AVAILABLE);
	if (ret != ANX_OK)
		return -16;

	/* AVAILABLE -> DRAINING */
	ret = anx_engine_transition(eng, ANX_ENGINE_DRAINING);
	if (ret != ANX_OK)
		return -17;

	/* DRAINING -> UNLOADING */
	ret = anx_engine_transition(eng, ANX_ENGINE_UNLOADING);
	if (ret != ANX_OK)
		return -18;

	/* UNLOADING -> OFFLINE */
	ret = anx_engine_transition(eng, ANX_ENGINE_OFFLINE);
	if (ret != ANX_OK)
		return -19;

	/* OFFLINE -> LOADING (reload) */
	ret = anx_engine_transition(eng, ANX_ENGINE_LOADING);
	if (ret != ANX_OK)
		return -20;

	/* --- Invalid transitions --- */

	/* LOADING -> AVAILABLE (must go through READY) */
	ret = anx_engine_transition(eng, ANX_ENGINE_AVAILABLE);
	if (ret != ANX_EPERM)
		return -21;

	/* LOADING -> DRAINING (invalid) */
	ret = anx_engine_transition(eng, ANX_ENGINE_DRAINING);
	if (ret != ANX_EPERM)
		return -22;

	/* --- Maintenance override from any state --- */

	ret = anx_engine_transition(eng, ANX_ENGINE_MAINTENANCE);
	if (ret != ANX_OK)
		return -23;

	/* MAINTENANCE -> LOADING (admin re-enable) */
	ret = anx_engine_transition(eng, ANX_ENGINE_LOADING);
	if (ret != ANX_OK)
		return -24;

	/* --- Invalid class for model registration --- */

	ret = anx_engine_register_model("bad_tool",
					ANX_ENGINE_DETERMINISTIC_TOOL,
					ANX_CAP_TOOL_EXECUTION,
					&desc, NULL);
	if (ret != ANX_EINVAL)
		return -25;

	/* --- Remote model registration --- */

	{
		struct anx_engine *remote;
		struct anx_model_desc rdesc;

		anx_memset(&rdesc, 0, sizeof(rdesc));
		rdesc.param_count = 70000000000ULL;
		rdesc.quant = ANX_QUANT_NONE;
		rdesc.context_window = 128000;
		rdesc.offline_capable = false;

		ret = anx_engine_register_model("claude-sonnet",
						ANX_ENGINE_REMOTE_MODEL,
						ANX_CAP_SUMMARIZATION |
						ANX_CAP_LONG_CONTEXT,
						&rdesc, &remote);
		if (ret != ANX_OK)
			return -26;

		if (remote->is_local)
			return -27;
		if (!remote->requires_network)
			return -28;
		if (remote->status != ANX_ENGINE_REGISTERED)
			return -29;

		anx_engine_unregister(remote);
	}

	anx_engine_unregister(eng);
	return 0;
}
