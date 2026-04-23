/*
 * jepa_wf.c — Workflow execution trace → JEPA training feedback loop.
 *
 * After a workflow completes, anx_jepa_ingest_wf_trace() reads the sealed
 * ANX_OBJ_EXECUTION_TRACE, determines the dominant action taken during the
 * run, and records a JEPA training sample (obs_before, action, obs_after).
 *
 * Samples accumulate in a static ring buffer.  When the buffer fills to
 * batch_size, anx_jepa_train_step() fires and the buffer resets.  This
 * gives the world model an online learning signal derived from real
 * workflow execution without requiring a separate training process.
 *
 * Action mapping (node kind → JEPA action_id):
 *
 *   CELL_CALL / AGENT_CALL / TRANSFORM / SUBFLOW → ACT_CELL_SPAWN
 *   MODEL_CALL                                   → ACT_ROUTE_LOCAL
 *   RETRIEVAL                                    → ACT_MEM_PROMOTE
 *   HUMAN_REVIEW                                 → ACT_CAP_VALIDATE
 *   TRIGGER / STATE_REF / CONDITION / FAN_* / OUTPUT → ACT_IDLE
 */

#include "jepa_internal.h"
#include <anx/jepa.h>
#include <anx/workflow.h>
#include <anx/state_object.h>
#include <anx/memplane.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>
#include <anx/types.h>

/* Maximum pending JEPA traces before a training step fires. */
#define JEPA_WF_BATCH_SIZE	32

/* Ring buffer of accumulated JEPA trace OIDs waiting for a training step. */
static anx_oid_t g_pending[JEPA_WF_BATCH_SIZE];
static uint32_t  g_pending_count;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static uint32_t node_kind_to_action(uint8_t kind)
{
	switch ((enum anx_wf_node_kind)kind) {
	case ANX_WF_NODE_CELL_CALL:
	case ANX_WF_NODE_AGENT_CALL:
	case ANX_WF_NODE_TRANSFORM:
	case ANX_WF_NODE_SUBFLOW:
		return ANX_JEPA_ACT_CELL_SPAWN;
	case ANX_WF_NODE_MODEL_CALL:
		return ANX_JEPA_ACT_ROUTE_LOCAL;
	case ANX_WF_NODE_RETRIEVAL:
		return ANX_JEPA_ACT_MEM_PROMOTE;
	case ANX_WF_NODE_HUMAN_REVIEW:
		return ANX_JEPA_ACT_CAP_VALIDATE;
	default:
		return ANX_JEPA_ACT_IDLE;
	}
}

/*
 * Scan trace entries and return the most frequent non-IDLE action_id.
 * Falls back to ACT_IDLE if all nodes are control nodes.
 */
static uint32_t dominant_action(const struct anx_wf_trace_entry *entries,
				uint32_t count)
{
	uint32_t tally[ANX_JEPA_ACT_COUNT];
	uint32_t i, best_action, best_count;

	anx_memset(tally, 0, sizeof(tally));

	for (i = 0; i < count; i++) {
		uint32_t a = node_kind_to_action(entries[i].node_kind);
		if (a < ANX_JEPA_ACT_COUNT)
			tally[a]++;
	}

	/* Prefer the most frequent non-IDLE action. */
	best_action = ANX_JEPA_ACT_IDLE;
	best_count  = 0;
	for (i = 1; i < ANX_JEPA_ACT_COUNT; i++) {
		if (tally[i] > best_count) {
			best_count  = tally[i];
			best_action = i;
		}
	}
	return best_action;
}

/* ------------------------------------------------------------------ */
/* Training batch flush                                                */
/* ------------------------------------------------------------------ */

static void maybe_train(void)
{
	int rc;

	if (g_pending_count < JEPA_WF_BATCH_SIZE)
		return;

	rc = anx_jepa_train_step(g_pending, g_pending_count);
	if (rc == ANX_OK)
		kprintf("[jepa] wf feedback: trained on %u traces\n",
			g_pending_count);
	else
		kprintf("[jepa] wf feedback: train_step failed (%d)\n", rc);

	g_pending_count = 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int anx_jepa_ingest_wf_trace(const anx_oid_t *exec_trace_oid,
			     const anx_oid_t *obs_before_oid,
			     const anx_oid_t *obs_after_oid)
{
	struct anx_wf_trace_entry *entries;
	struct anx_object_handle   oh;
	uint64_t payload_size;
	uint32_t entry_count;
	uint32_t action_id;
	anx_oid_t jepa_trace_oid;
	int rc;

	anx_memset(&jepa_trace_oid, 0, sizeof(jepa_trace_oid));

	if (!exec_trace_oid || !obs_before_oid || !obs_after_oid)
		return ANX_EINVAL;
	if (!anx_jepa_available())
		return ANX_ENOENT;

	rc = anx_so_open(exec_trace_oid, ANX_OPEN_READ, &oh);
	if (rc != ANX_OK)
		return rc;

	payload_size = oh.obj->payload_size;
	if (payload_size == 0 ||
	    (payload_size % sizeof(struct anx_wf_trace_entry)) != 0) {
		anx_so_close(&oh);
		return ANX_EINVAL;
	}

	entry_count = (uint32_t)(payload_size / sizeof(struct anx_wf_trace_entry));
	entries = (struct anx_wf_trace_entry *)anx_alloc(payload_size);
	if (!entries) {
		anx_so_close(&oh);
		return ANX_ENOMEM;
	}

	rc = anx_so_read_payload(&oh, 0, entries, payload_size);
	anx_so_close(&oh);
	if (rc != ANX_OK) {
		anx_free(entries);
		return rc;
	}

	action_id = dominant_action(entries, entry_count);
	anx_free(entries);

	/*
	 * Record the (obs_before, action, obs_after) training sample.
	 * This encodes both observations, runs the predictor, and stores
	 * the resulting ANX_OBJ_JEPA_TRACE in the memplane.
	 */
	rc = anx_jepa_record_trace(obs_before_oid, action_id, obs_after_oid,
				   &jepa_trace_oid);
	if (rc != ANX_OK) {
		kprintf("[jepa] wf feedback: record_trace failed (%d)\n", rc);
		return rc;
	}

	g_pending[g_pending_count++] = jepa_trace_oid;

	maybe_train();
	return ANX_OK;
}
