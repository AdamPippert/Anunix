/*
 * jepa_rlm.c — JEPA RLM integration: context injection and tool wiring.
 *
 * Two integration modes for LLM-based workflows:
 *
 * DATA REFERENCE MODE (anx_jepa_rlm_inject_context):
 *   Appends a human-readable JEPA world-state block to the RLM system
 *   prompt.  The LLM always sees the world model's current beliefs about
 *   system state as part of its context.  Reliable but costs tokens.
 *
 * PLUGIN MODE (anx_jepa_rlm_install_tools):
 *   Annotates the RLM config so the harness knows JEPA tools are
 *   available.  The LLM decides when to query them.  Flexible but
 *   requires the LLM to choose to use the tools.
 *
 * Both modes use the existing anx_rlm_rollout_step / anx_rlm_rollout_inject
 * APIs and do not require changes to the RLM harness itself.
 */

#include "jepa_internal.h"
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/rlm.h>
#include <anx/string.h>

/* ------------------------------------------------------------------ */
/* Data reference mode                                                 */
/* ------------------------------------------------------------------ */

/*
 * Serialises the current JEPA world state as a structured text block
 * and appends it to system_buf.  The block is designed to be readable
 * by the LLM without consuming excessive tokens.
 *
 * Example output:
 *
 *   [ANUNIX WORLD STATE — anx:world/os-default]
 *   Status: ready | Mode: online | Encode count: 142
 *   Scheduler: interactive=1 background=0 batch=3 (4 cells active)
 *   Memory: L0-decay=12 L1-decay=8 (L0 entries=34)
 *   Routing: fallbacks=0 avg_score=74.20
 *   Compute: cpu=0.12 npu=0.00
 *   Validation: avg=91.00 failures=0
 *   Security: events=0
 *   Last loss: 0.0312 | Last divergence: 0.0071 | Anomaly: normal
 */
int anx_jepa_rlm_inject_context(char *system_buf, uint32_t buf_size)
{
	struct anx_jepa_ctx              *ctx;
	const struct anx_jepa_world_profile *world;
	struct anx_jepa_obs               obs;
	uint32_t                          existing_len;
	uint32_t                          remaining;
	char                             *p;
	int                               rc;

	if (!system_buf || buf_size == 0)
		return ANX_EINVAL;

	if (!anx_jepa_available()) {
		/* Append a minimal note so the LLM knows JEPA is absent */
		existing_len = (uint32_t)anx_strlen(system_buf);
		remaining    = buf_size - existing_len - 1;
		if (remaining < 48)
			return ANX_ENOMEM;
		anx_strncat(system_buf,
			    "\n[ANUNIX WORLD STATE: unavailable]\n",
			    remaining);
		return ANX_OK;
	}

	ctx   = anx_jepa_ctx_get();
	world = anx_jepa_world_get_active();

	rc = anx_jepa_observe(&obs);
	if (rc != ANX_OK)
		return rc;

	existing_len = (uint32_t)anx_strlen(system_buf);
	p            = system_buf + existing_len;
	remaining    = buf_size - existing_len - 1;

	/* Write the block directly; anx_snprintf is not available in kernel.
	 * We use anx_jepa_tool_world_state JSON and wrap it in a text block
	 * for human readability. */

	static const char *status_names[] = {
		"uninitialized", "initializing", "ready",
		"training", "degraded", "unavailable"
	};
	static const char *anomaly_names[] = {
		"normal", "elevated", "high", "critical", "no_data"
	};

	const char *status_str =
		((uint32_t)ctx->status < 6) ?
		status_names[ctx->status] : "unknown";

	const char *anomaly_str;
	float div = ctx->last_divergence;
	if (div < 0.0f)        anomaly_str = anomaly_names[4];
	else if (div < 0.15f)  anomaly_str = anomaly_names[0];
	else if (div < 0.40f)  anomaly_str = anomaly_names[1];
	else if (div < 0.70f)  anomaly_str = anomaly_names[2];
	else                   anomaly_str = anomaly_names[3];

	/*
	 * Use anx_jepa_tool_world_state to produce JSON, then prepend the
	 * human-readable header.  This keeps the block concise and parseable.
	 */
	char json_buf[ANX_JEPA_CONTEXT_BUF_MAX];
	anx_jepa_tool_world_state(json_buf, sizeof(json_buf));

	/* Rough token estimate: 1 token ≈ 4 bytes */
	uint32_t json_len  = (uint32_t)anx_strlen(json_buf);
	uint32_t header_len = 64;

	if (remaining < json_len + header_len + 4)
		return ANX_ENOMEM;

	anx_strncat(p, "\n[ANUNIX WORLD STATE — ", remaining);
	anx_strncat(p, world->uri, remaining - (uint32_t)anx_strlen(p));
	anx_strncat(p, " | ", remaining - (uint32_t)anx_strlen(p));
	anx_strncat(p, status_str, remaining - (uint32_t)anx_strlen(p));
	anx_strncat(p, " | anomaly:", remaining - (uint32_t)anx_strlen(p));
	anx_strncat(p, anomaly_str, remaining - (uint32_t)anx_strlen(p));
	anx_strncat(p, "]\n", remaining - (uint32_t)anx_strlen(p));
	anx_strncat(p, json_buf, remaining - (uint32_t)anx_strlen(p));
	anx_strncat(p, "\n", remaining - (uint32_t)anx_strlen(p));

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Plugin mode                                                         */
/* ------------------------------------------------------------------ */

/*
 * Mark the RLM config so the harness and LLM know JEPA tools are
 * available.  The three tool names are appended to the system prompt
 * as a structured tool-availability block.
 *
 * The RLM harness's existing anx_rlm_rollout_inject() path handles
 * actual tool dispatch: the LLM emits a tool_use block naming one of
 * the three JEPA tools, and the workflow cell calls the corresponding
 * anx_jepa_tool_*() function and injects the result.
 */
int anx_jepa_rlm_install_tools(struct anx_rlm_config *config)
{
	static const char jepa_tool_block[] =
		"\n[JEPA TOOLS AVAILABLE]\n"
		"  jepa_world_state   — current world state summary (no args)\n"
		"  jepa_predict       — predict outcome: {\"action\": \"<name>\"}\n"
		"  jepa_anomaly_score — anomaly detection (no args)\n"
		"Call these as tool_use entries during your reasoning.\n";

	uint32_t sys_len, block_len, remaining;

	if (!config)
		return ANX_EINVAL;
	if (!anx_jepa_available())
		return ANX_OK;	/* no-op, not an error */

	sys_len   = (uint32_t)anx_strlen(config->system);
	block_len = (uint32_t)anx_strlen(jepa_tool_block);
	remaining = ANX_RLM_MAX_CONTENT - sys_len - 1;

	if (remaining < block_len)
		return ANX_ENOMEM;

	anx_strncat(config->system, jepa_tool_block, remaining);
	return ANX_OK;
}
