/*
 * jepa_workflow.c — JEPA agent workflow definitions.
 *
 * Registers two standard workflows using the existing anx_wf_* API:
 *
 *   anx:workflow/jepa-agent-loop
 *     Data reference mode: JEPA state is injected into the LLM's context
 *     before every inference step.  The LLM always has world state but
 *     cannot query JEPA interactively.
 *
 *   anx:workflow/jepa-plugin-loop
 *     Plugin mode: JEPA tools (world_state, predict, anomaly_score) are
 *     registered in the RLM config.  The LLM decides when to call them.
 *
 * Both workflows follow the same five-node structure:
 *
 *   [TRIGGER] → [CELL_CALL: observe+encode]
 *             → [MODEL_CALL: LLM plan]
 *             → [CELL_CALL: act]
 *             → [CELL_CALL: record trace]
 *             → [CONDITION: loop back to trigger]
 *
 * The workflows are registered at JEPA init time and are available to
 * any agent that calls anx_wf_run() with the appropriate OID.
 */

#include "jepa_internal.h"
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/workflow.h>
#include <anx/string.h>
#include <anx/alloc.h>

/* Stored OIDs for external callers to look up and run the workflows */
static anx_oid_t g_wf_context_loop_oid;
static anx_oid_t g_wf_plugin_loop_oid;

/* ------------------------------------------------------------------ */
/* Helper: add a node and wire it to the previous node's output       */
/* ------------------------------------------------------------------ */

static int add_and_wire(const anx_oid_t *wf_oid,
			const struct anx_wf_node *spec,
			uint16_t prev_id,
			uint16_t *id_out)
{
	int rc;

	rc = anx_wf_node_add(wf_oid, spec, id_out);
	if (rc != ANX_OK)
		return rc;

	if (prev_id != 0) {
		rc = anx_wf_edge_add(wf_oid, prev_id, 0, *id_out, 0);
		if (rc != ANX_OK)
			return rc;
	}
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Build one JEPA agent workflow                                       */
/* ------------------------------------------------------------------ */

static int build_jepa_workflow(const char *name,
				const char *description,
				const char *mode,	/* "context" or "plugin" */
				anx_oid_t  *wf_oid_out)
{
	anx_oid_t wf_oid;
	struct anx_wf_node node;
	uint16_t trigger_id, observe_id, plan_id, act_id, record_id, loop_id;
	int rc;

	rc = anx_wf_create(name, description, &wf_oid);
	if (rc != ANX_OK)
		return rc;

	/* ---- Node 1: TRIGGER ---- */
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_TRIGGER;
	anx_strlcpy(node.label, "Start / Loop", ANX_WF_LABEL_MAX);
	anx_strlcpy(node.params.trigger.schedule, "manual",
		    sizeof(node.params.trigger.schedule));
	node.canvas_x = 100; node.canvas_y = 100;
	node.canvas_w = 140; node.canvas_h = 60;
	rc = add_and_wire(&wf_oid, &node, 0, &trigger_id);
	if (rc != ANX_OK) goto fail;

	/* ---- Node 2: CELL_CALL — observe + encode ---- */
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_CELL_CALL;
	anx_strlcpy(node.label, "JEPA Observe + Encode", ANX_WF_LABEL_MAX);
	anx_strlcpy(node.params.cell_call.intent,
		    "collect system observation and encode latent state",
		    ANX_WF_EXPR_MAX);
	node.canvas_x = 100; node.canvas_y = 200;
	node.canvas_w = 200; node.canvas_h = 60;
	rc = add_and_wire(&wf_oid, &node, trigger_id, &observe_id);
	if (rc != ANX_OK) goto fail;

	/* ---- Node 3: MODEL_CALL — LLM plan ---- */
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_MODEL_CALL;
	anx_strlcpy(node.label, "LLM Agent Decision", ANX_WF_LABEL_MAX);
	anx_strlcpy(node.params.model_call.model_id,
		    "gpt-4o-mini",
		    sizeof(node.params.model_call.model_id));

	if (anx_strcmp(mode, "context") == 0) {
		anx_strlcpy(node.params.model_call.prompt_template,
			    "You are an Anunix OS agent. Review the world state "
			    "below and choose the best next action from the "
			    "available action vocabulary. "
			    "Output: {\"action\": \"<action_name>\", "
			    "\"rationale\": \"<brief reason>\"}",
			    ANX_WF_EXPR_MAX);
	} else {
		anx_strlcpy(node.params.model_call.prompt_template,
			    "You are an Anunix OS agent. Use the JEPA tools "
			    "to query world state and predict action outcomes. "
			    "Choose the best next action. "
			    "Output: {\"action\": \"<action_name>\", "
			    "\"rationale\": \"<brief reason>\"}",
			    ANX_WF_EXPR_MAX);
	}
	node.canvas_x = 100; node.canvas_y = 300;
	node.canvas_w = 200; node.canvas_h = 80;
	rc = add_and_wire(&wf_oid, &node, observe_id, &plan_id);
	if (rc != ANX_OK) goto fail;

	/* ---- Node 4: CELL_CALL — execute action ---- */
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_CELL_CALL;
	anx_strlcpy(node.label, "Execute Action", ANX_WF_LABEL_MAX);
	anx_strlcpy(node.params.cell_call.intent,
		    "execute the action recommended by the LLM agent",
		    ANX_WF_EXPR_MAX);
	node.canvas_x = 100; node.canvas_y = 420;
	node.canvas_w = 180; node.canvas_h = 60;
	rc = add_and_wire(&wf_oid, &node, plan_id, &act_id);
	if (rc != ANX_OK) goto fail;

	/* ---- Node 5: CELL_CALL — record JEPA trace ---- */
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_CELL_CALL;
	anx_strlcpy(node.label, "Record JEPA Trace", ANX_WF_LABEL_MAX);
	anx_strlcpy(node.params.cell_call.intent,
		    "record JEPA training trace: obs_t, action, obs_t+1",
		    ANX_WF_EXPR_MAX);
	node.canvas_x = 100; node.canvas_y = 520;
	node.canvas_w = 180; node.canvas_h = 60;
	rc = add_and_wire(&wf_oid, &node, act_id, &record_id);
	if (rc != ANX_OK) goto fail;

	/* ---- Node 6: CONDITION — loop or stop ---- */
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_CONDITION;
	anx_strlcpy(node.label, "Continue Loop?", ANX_WF_LABEL_MAX);
	anx_strlcpy(node.params.condition.expr,
		    "agent.should_continue",
		    ANX_WF_EXPR_MAX);
	node.canvas_x = 100; node.canvas_y = 620;
	node.canvas_w = 160; node.canvas_h = 60;
	rc = add_and_wire(&wf_oid, &node, record_id, &loop_id);
	if (rc != ANX_OK) goto fail;

	/* Loop back edge: condition true → trigger */
	rc = anx_wf_edge_add(&wf_oid, loop_id, 0, trigger_id, 0);
	if (rc != ANX_OK) goto fail;

	/* Output node: condition false → output (workflow ends) */
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_OUTPUT;
	anx_strlcpy(node.label, "Output", ANX_WF_LABEL_MAX);
	anx_strlcpy(node.params.output.dest_name, "jepa_agent_result",
		    ANX_WF_NAME_MAX);
	node.canvas_x = 320; node.canvas_y = 620;
	node.canvas_w = 140; node.canvas_h = 60;
	{
		uint16_t out_id;
		rc = anx_wf_node_add(&wf_oid, &node, &out_id);
		if (rc != ANX_OK) goto fail;
		rc = anx_wf_edge_add(&wf_oid, loop_id, 1, out_id, 0);
		if (rc != ANX_OK) goto fail;
	}

	*wf_oid_out = wf_oid;
	return ANX_OK;

fail:
	anx_wf_destroy(&wf_oid);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Public registration                                                 */
/* ------------------------------------------------------------------ */

int anx_jepa_workflow_register(void)
{
	int rc;

	rc = build_jepa_workflow(
		"anx:workflow/jepa-agent-loop",
		"JEPA agent loop (data reference mode): world state injected "
		"into LLM context before each inference step.",
		"context",
		&g_wf_context_loop_oid);
	if (rc != ANX_OK) {
		kprintf("[jepa] failed to register jepa-agent-loop (%d)\n", rc);
		return rc;
	}

	rc = build_jepa_workflow(
		"anx:workflow/jepa-plugin-loop",
		"JEPA agent loop (plugin mode): LLM calls JEPA as tool_use "
		"entries (jepa_world_state, jepa_predict, jepa_anomaly_score).",
		"plugin",
		&g_wf_plugin_loop_oid);
	if (rc != ANX_OK) {
		kprintf("[jepa] failed to register jepa-plugin-loop (%d)\n", rc);
		return rc;
	}

	kprintf("[jepa] workflows registered: jepa-agent-loop, jepa-plugin-loop\n");
	return ANX_OK;
}

/* Return OID of a named JEPA workflow for external callers */
int anx_jepa_workflow_oid(const char *name, anx_oid_t *oid_out)
{
	if (!name || !oid_out)
		return ANX_EINVAL;

	if (anx_strcmp(name, "anx:workflow/jepa-agent-loop") == 0) {
		*oid_out = g_wf_context_loop_oid;
		return ANX_OK;
	}
	if (anx_strcmp(name, "anx:workflow/jepa-plugin-loop") == 0) {
		*oid_out = g_wf_plugin_loop_oid;
		return ANX_OK;
	}
	return ANX_ENOENT;
}
