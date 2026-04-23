/*
 * agent_cell.c — Agent cell: goal → template match → workflow → run.
 *
 * Dispatched when the workflow executor encounters ANX_WF_NODE_AGENT_CALL.
 * The agent:
 *   1. Extracts the goal string from the node spec.
 *   2. Scores all registered workflow templates against the goal keywords.
 *   3. If a template scores >= ANX_AGENT_MATCH_THRESHOLD, instantiates it
 *      and runs it; the agent cell's output ports receive the sub-workflow
 *      output OIDs.
 *   4. If no template matches well enough, falls back to a MODEL_CALL with
 *      a planning prompt (requires a model server to be running).
 *
 * The fallback model path produces a free-text plan stored as a state
 * object — useful as a trace for JEPA training even when no template
 * matches.  Full dynamic workflow construction from model output is a
 * future Phase 4 capability.
 */

#include <anx/types.h>
#include <anx/workflow.h>
#include <anx/workflow_library.h>
#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>

/* Minimum keyword-match score to accept a template as a good fit. */
#define ANX_AGENT_MATCH_THRESHOLD	2
#define ANX_AGENT_MAX_CANDIDATES	4

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Run a workflow by OID and copy its output OIDs into out_oids[].
 * Returns the number of outputs collected (0 if run failed or no outputs).
 */
static int agent_run_workflow(const anx_oid_t *wf_oid,
			      anx_oid_t *out_oids, uint32_t max_out,
			      uint32_t *out_count)
{
	struct anx_wf_object *wf;
	int rc;

	rc = anx_wf_run(wf_oid, NULL);
	if (rc != ANX_OK)
		return rc;

	wf = anx_wf_object_get(wf_oid);
	if (!wf) {
		*out_count = 0;
		return ANX_OK;
	}

	*out_count = wf->output_count < max_out ? wf->output_count : max_out;
	anx_memcpy(out_oids, wf->output_oids,
		   *out_count * sizeof(anx_oid_t));
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Public: dispatch an AGENT_CALL node                                */
/* ------------------------------------------------------------------ */

/*
 * anx_agent_cell_dispatch — entry point called by wf_dispatch_node()
 * for ANX_WF_NODE_AGENT_CALL nodes.
 *
 * goal:         the goal string from node->params.agent_call.goal
 * out_oids:     output OID table (filled with sub-workflow outputs)
 * max_out_ports: size of out_oids array
 * out_count:    number of OIDs actually written
 */
int anx_agent_cell_dispatch(const char *goal,
			    anx_oid_t *out_oids, uint32_t max_out_ports,
			    uint32_t *out_count)
{
	struct anx_wf_match candidates[ANX_AGENT_MAX_CANDIDATES];
	uint32_t n_candidates;
	anx_oid_t wf_oid;
	char inst_name[ANX_WF_NAME_MAX];
	uint32_t n_out = 0;
	int rc;

	if (!goal || !out_oids || !out_count)
		return ANX_EINVAL;

	*out_count = 0;

	kprintf("[agent] dispatching goal: '%.60s'\n", goal);

	/* ---- Step 1: template matching -------------------------------- */
	n_candidates = anx_wf_lib_match(goal, candidates, ANX_AGENT_MAX_CANDIDATES);

	if (n_candidates > 0 &&
	    candidates[0].score >= ANX_AGENT_MATCH_THRESHOLD) {

		kprintf("[agent] template match: %s (score %u)\n",
			candidates[0].uri, candidates[0].score);

		/* Build an instance name from the best-match URI's suffix. */
		{
			const char *tail = candidates[0].uri;
			const char *slash = tail;

			while (*slash) {
				if (*slash == '/')
					tail = slash + 1;
				slash++;
			}
			anx_snprintf(inst_name, sizeof(inst_name),
				     "agent-%s", tail);
		}

		rc = anx_wf_lib_instantiate(candidates[0].uri,
					    inst_name, &wf_oid);
		if (rc != ANX_OK) {
			kprintf("[agent] instantiate failed (%d)\n", rc);
			return rc;
		}

		rc = agent_run_workflow(&wf_oid, out_oids, max_out_ports, &n_out);
		anx_wf_destroy(&wf_oid);

		if (rc != ANX_OK) {
			kprintf("[agent] sub-workflow failed (%d)\n", rc);
			return rc;
		}

		*out_count = n_out;
		kprintf("[agent] completed via template, %u outputs\n", n_out);
		return ANX_OK;
	}

	/* ---- Step 2: fallback — decompose-goal template --------------- */
	kprintf("[agent] no strong template match, using decompose-goal\n");

	rc = anx_wf_lib_instantiate("anx:workflow/decompose-goal",
				    "agent-decompose", &wf_oid);
	if (rc == ANX_OK) {
		rc = agent_run_workflow(&wf_oid, out_oids, max_out_ports, &n_out);
		anx_wf_destroy(&wf_oid);

		if (rc == ANX_OK) {
			*out_count = n_out;
			kprintf("[agent] decompose-goal completed, %u outputs\n",
				n_out);
			return ANX_OK;
		}
	}

	/*
	 * Both paths failed (no model server running, or library not
	 * initialised).  Return a null output — the workflow executor
	 * will either continue with null inputs or suspend depending on
	 * downstream port requirements.
	 */
	kprintf("[agent] no dispatch path available for goal\n");
	return ANX_ENOENT;
}
