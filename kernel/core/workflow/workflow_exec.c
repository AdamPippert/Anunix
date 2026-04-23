/*
 * workflow_exec.c — Workflow execution engine, Phase 2 (RFC-0018).
 *
 * Parallel DAG executor: nodes with no unmet dependencies are batched
 * and dispatched concurrently up to a per-workflow cap derived from host
 * hardware (see anx_wf_cap_compute).  Within a batch, dispatch is currently
 * synchronous — anx_cell_run() blocks until the cell completes.  The batch
 * structure is parallel-ready: replacing anx_cell_run() with an async
 * scheduler dispatch in Phase 3 requires no structural changes here.
 *
 * On cell failure the executor suspends the workflow (ANX_WF_RUN_SUSPENDED),
 * saves a full continuation (in_deg, completed flags, port OID table), and
 * returns control to the caller.  An agent resumes via anx_wf_resume().
 *
 * Data flows by state-object handle (OID).  Each node's output port OIDs
 * are recorded in port_oid[slot][port] and forwarded to successor nodes via
 * the edge table when they are dispatched.
 */

#include <anx/types.h>
#include <anx/workflow.h>
#include <anx/cell.h>
#include <anx/cell_trace.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/arch.h>
#include <anx/jepa.h>
#include <anx/agent_cell.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static bool
oid_is_null(const anx_oid_t *o)
{
	return o->hi == 0 && o->lo == 0;
}

/*
 * Build slot_id[slot] → node.id and slot_by_id[node.id] → slot maps.
 * slot_by_id is indexed by node.id (1-based); slot_by_id[0] is unused.
 * Entries for unoccupied slots / unknown IDs are 0.
 */
static void
wf_build_tables(const struct anx_wf_object *wf,
		uint32_t *slot_id,
		uint32_t *slot_by_id)
{
	uint32_t i;

	anx_memset(slot_id,    0, sizeof(uint32_t) * ANX_WF_MAX_NODES);
	anx_memset(slot_by_id, 0, sizeof(uint32_t) * (ANX_WF_MAX_NODES + 1));

	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		uint16_t id = wf->nodes[i].id;

		if (id == 0)
			continue;
		slot_id[i] = id;
		if ((uint32_t)id <= ANX_WF_MAX_NODES)
			slot_by_id[id] = i;
	}
}

/*
 * Return the output OID wired to input port in_port of node_id by
 * following the edge table backwards.  Returns a null OID when no
 * matching edge exists (unconnected optional port).
 */
static anx_oid_t
wf_get_input_oid(const struct anx_wf_object *wf, uint16_t node_id,
		 uint8_t in_port, const uint32_t *slot_by_id,
		 const anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS])
{
	static const anx_oid_t null_oid = {0, 0};
	uint32_t i;

	for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
		if (wf->edges[i].to_node == node_id &&
		    wf->edges[i].to_port == in_port &&
		    wf->edges[i].from_node != 0) {
			uint16_t fn   = wf->edges[i].from_node;
			uint8_t  fp   = wf->edges[i].from_port;
			uint32_t fslot;

			if ((uint32_t)fn > ANX_WF_MAX_NODES)
				continue;
			fslot = slot_by_id[fn];
			if (fslot >= ANX_WF_MAX_NODES)
				continue;
			if (fp >= ANX_WF_MAX_PORTS)
				continue;
			return port_oid[fslot][fp];
		}
	}
	return null_oid;
}

/*
 * Bind the input ports of a cell from upstream port OIDs.
 * Iterates the node's port array and, for each IN port, looks up the
 * OID produced by the upstream output port connected to it.
 */
static void
wf_bind_cell_inputs(const struct anx_wf_object *wf, struct anx_cell *cell,
		    uint32_t slot, const uint32_t *slot_by_id,
		    const anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS])
{
	const struct anx_wf_node *node = &wf->nodes[slot];
	uint8_t p;

	for (p = 0; p < node->port_count; p++) {
		if (node->ports[p].dir != ANX_WF_PORT_IN)
			continue;
		if (cell->input_count >= ANX_MAX_CELL_INPUTS)
			break;

		cell->inputs[cell->input_count].state_object_ref =
			wf_get_input_oid(wf, node->id, p, slot_by_id, port_oid);
		anx_strlcpy(cell->inputs[cell->input_count].name,
			    node->ports[p].name, 64);
		cell->inputs[cell->input_count].required = node->ports[p].required;
		cell->input_count++;
	}
}

/*
 * Collect output OIDs from a completed cell into the port_oid table.
 * Maps cell output_refs in order to the node's OUT ports.
 */
static void
wf_collect_outputs(const struct anx_wf_object *wf, const struct anx_cell *cell,
		   uint32_t slot, anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS])
{
	const struct anx_wf_node *node = &wf->nodes[slot];
	uint32_t out_idx = 0;
	uint8_t p;

	for (p = 0; p < node->port_count; p++) {
		if (node->ports[p].dir != ANX_WF_PORT_OUT)
			continue;
		if (out_idx >= cell->output_count)
			break;
		port_oid[slot][p] = cell->output_refs[out_idx++];
	}
}

/* ------------------------------------------------------------------ */
/* Node dispatch per kind                                              */
/* ------------------------------------------------------------------ */

/*
 * Dispatch a CELL_CALL, MODEL_CALL, AGENT_CALL, or RETRIEVAL node.
 * Creates a cell with the appropriate type, binds inputs, runs it,
 * and collects output OIDs.
 */
static int
wf_dispatch_cell_node(struct anx_wf_object *wf, uint32_t slot,
		      const uint32_t *slot_by_id,
		      anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS],
		      anx_cid_t *cid_out, anx_oid_t *trace_oid_out)
{
	struct anx_wf_node *node = &wf->nodes[slot];
	struct anx_cell_intent intent;
	struct anx_cell *cell = NULL;
	struct anx_cell_trace *trace = NULL;
	enum anx_cell_type ctype;
	int ret;

	anx_memset(&intent, 0, sizeof(intent));

	switch (node->kind) {
	case ANX_WF_NODE_CELL_CALL:
		ctype = ANX_CELL_TASK_EXECUTION;
		anx_strlcpy(intent.name,      node->label, sizeof(intent.name));
		anx_strlcpy(intent.objective, node->params.cell_call.intent,
			    sizeof(intent.objective));
		break;
	case ANX_WF_NODE_MODEL_CALL:
		ctype = ANX_CELL_MODEL_SERVER;
		anx_strlcpy(intent.name, node->label, sizeof(intent.name));
		anx_strlcpy(intent.objective, node->params.model_call.prompt_template,
			    sizeof(intent.objective));
		break;
	/* ANX_WF_NODE_AGENT_CALL is dispatched via wf_dispatch_agent_node(). */
	case ANX_WF_NODE_RETRIEVAL:
		ctype = ANX_CELL_TASK_RETRIEVAL;
		anx_strlcpy(intent.name,      node->label, sizeof(intent.name));
		anx_strlcpy(intent.objective, node->params.retrieval.query_template,
			    sizeof(intent.objective));
		break;
	default:
		return ANX_EINVAL;
	}

	ret = anx_cell_create(ctype, &intent, &cell);
	if (ret != ANX_OK)
		return ret;

	wf_bind_cell_inputs(wf, cell, slot, slot_by_id, port_oid);

	ret = anx_trace_create(&cell->cid, &trace);
	if (ret == ANX_OK) {
		cell->trace_id = trace->trace_id;
		anx_trace_append(trace, ANX_TRACE_STEP_STARTED, node->label, 0);
	}

	ret = anx_cell_run(cell);

	if (trace) {
		anx_trace_append(trace,
				 ret == ANX_OK ? ANX_TRACE_STEP_COMPLETED
					       : ANX_TRACE_STEP_FAILED,
				 node->label, ret);
		anx_trace_finalize(trace, trace_oid_out);
		anx_trace_destroy(trace);
	}

	if (ret == ANX_OK)
		wf_collect_outputs(wf, cell, slot, port_oid);

	*cid_out = cell->cid;
	anx_cell_destroy(cell);
	return ret;
}

/*
 * STATE_REF: read the bound OID directly into the output port.
 * No cell is created — this is a pure data binding.
 */
static int
wf_dispatch_state_ref(struct anx_wf_object *wf, uint32_t slot,
		      anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS])
{
	struct anx_wf_node *node = &wf->nodes[slot];

	if (oid_is_null(&node->params.state_ref.obj_oid))
		return ANX_ENOENT;

	/* Output port 0 carries the referenced object's OID. */
	port_oid[slot][0] = node->params.state_ref.obj_oid;
	return ANX_OK;
}

/*
 * FAN_OUT: propagate input port 0 to all output ports.
 * Enables parallel branches reading the same upstream result.
 */
static int
wf_dispatch_fan_out(struct anx_wf_object *wf, uint32_t slot,
		    const uint32_t *slot_by_id,
		    anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS])
{
	anx_oid_t src = wf_get_input_oid(wf, wf->nodes[slot].id, 0,
					  slot_by_id, port_oid);
	uint8_t p;

	for (p = 0; p < ANX_WF_MAX_PORTS; p++)
		port_oid[slot][p] = src;
	return ANX_OK;
}

/*
 * FAN_IN: barrier — all input ports must carry a non-null OID.
 * Passes the first input through to output port 0.
 */
static int
wf_dispatch_fan_in(struct anx_wf_object *wf, uint32_t slot,
		   const uint32_t *slot_by_id,
		   anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS])
{
	struct anx_wf_node *node = &wf->nodes[slot];
	uint8_t p;

	for (p = 0; p < node->port_count; p++) {
		anx_oid_t in;

		if (node->ports[p].dir != ANX_WF_PORT_IN || !node->ports[p].required)
			continue;
		in = wf_get_input_oid(wf, node->id, p, slot_by_id, port_oid);
		if (oid_is_null(&in))
			return ANX_EBUSY;	/* prerequisite not yet ready */
	}

	/* First input OID passes to output port 0. */
	port_oid[slot][0] = wf_get_input_oid(wf, node->id, 0, slot_by_id, port_oid);
	return ANX_OK;
}

/*
 * SUBFLOW: recursive workflow execution.
 * The subflow's output_oids array is propagated to this node's output ports.
 */
static int
wf_dispatch_subflow(struct anx_wf_object *wf, uint32_t slot,
		    anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS])
{
	struct anx_wf_node *node = &wf->nodes[slot];
	struct anx_wf_object *sub;
	anx_cid_t sub_cid;
	int ret;
	uint8_t i;

	sub = anx_wf_object_get(&node->params.subflow.subflow_oid);
	if (!sub)
		return ANX_ENOENT;

	ret = anx_wf_run(&node->params.subflow.subflow_oid, &sub_cid);
	if (ret != ANX_OK)
		return ret;

	for (i = 0; i < sub->output_count && i < ANX_WF_MAX_PORTS; i++)
		port_oid[slot][i] = sub->output_oids[i];

	return ANX_OK;
}

/*
 * AGENT_CALL: match goal against the workflow template library, instantiate
 * the best-fitting template, run it, and map outputs to this node's ports.
 */
static int
wf_dispatch_agent_node(struct anx_wf_object *wf, uint32_t slot,
		       anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS])
{
	struct anx_wf_node *node = &wf->nodes[slot];
	anx_oid_t out_oids[ANX_WF_MAX_PORTS];
	uint32_t n_out = 0;
	uint8_t i;
	int ret;

	ret = anx_agent_cell_dispatch(node->params.agent_call.goal,
				      out_oids, ANX_WF_MAX_PORTS, &n_out);
	if (ret != ANX_OK)
		return ret;

	for (i = 0; i < n_out && i < ANX_WF_MAX_PORTS; i++)
		port_oid[slot][i] = out_oids[i];

	return ANX_OK;
}

/*
 * OUTPUT: record the named output OID in the workflow's output_oids table.
 * This is how results flow back to parent workflows or callers.
 */
static int
wf_dispatch_output(struct anx_wf_object *wf, uint32_t slot,
		   const uint32_t *slot_by_id,
		   anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS])
{
	anx_oid_t src = wf_get_input_oid(wf, wf->nodes[slot].id, 0,
					  slot_by_id, port_oid);

	if (oid_is_null(&src))
		return ANX_OK;	/* nothing to record */

	if (wf->output_count < ANX_WF_MAX_PORTS)
		wf->output_oids[wf->output_count++] = src;

	return ANX_OK;
}

/*
 * Dispatch a single node according to its kind.
 * Fills port_oid[slot] with output OIDs on success.
 * Returns ANX_OK, or a negative error that triggers suspension.
 * Returns a special sentinel (ANX_EBUSY) for HUMAN_REVIEW which causes
 * the executor to transition to WAITING_HUMAN instead of SUSPENDED.
 */
static int
wf_dispatch_node(struct anx_wf_object *wf, uint32_t slot,
		 const uint32_t *slot_by_id,
		 anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS],
		 struct anx_wf_trace_entry *entry)
{
	struct anx_wf_node *node = &wf->nodes[slot];
	anx_cid_t null_cid;
	int ret;

	anx_memset(&null_cid, 0, sizeof(null_cid));
	anx_memset(&entry->cell_cid, 0, sizeof(entry->cell_cid));
	anx_memset(&entry->trace_oid, 0, sizeof(entry->trace_oid));
	entry->node_id    = node->id;
	entry->node_kind  = (uint8_t)node->kind;
	entry->started_at = arch_time_now();

	switch (node->kind) {
	case ANX_WF_NODE_TRIGGER:
		/* Source node: no execution, outputs are bound externally. */
		ret = ANX_OK;
		break;

	case ANX_WF_NODE_STATE_REF:
		ret = wf_dispatch_state_ref(wf, slot, port_oid);
		break;

	case ANX_WF_NODE_CELL_CALL:
	case ANX_WF_NODE_MODEL_CALL:
	case ANX_WF_NODE_RETRIEVAL:
		ret = wf_dispatch_cell_node(wf, slot, slot_by_id, port_oid,
					    &entry->cell_cid, &entry->trace_oid);
		break;

	case ANX_WF_NODE_AGENT_CALL:
		ret = wf_dispatch_agent_node(wf, slot, port_oid);
		break;

	case ANX_WF_NODE_FAN_OUT:
		ret = wf_dispatch_fan_out(wf, slot, slot_by_id, port_oid);
		break;

	case ANX_WF_NODE_FAN_IN:
		ret = wf_dispatch_fan_in(wf, slot, slot_by_id, port_oid);
		break;

	case ANX_WF_NODE_SUBFLOW:
		ret = wf_dispatch_subflow(wf, slot, port_oid);
		break;

	case ANX_WF_NODE_OUTPUT:
		ret = wf_dispatch_output(wf, slot, slot_by_id, port_oid);
		break;

	case ANX_WF_NODE_HUMAN_REVIEW:
		/* Signal to the outer loop to transition to WAITING_HUMAN. */
		ret = ANX_EBUSY;
		break;

	case ANX_WF_NODE_CONDITION:
	case ANX_WF_NODE_TRANSFORM:
		/* Phase 3: expression evaluation engine. */
		kprintf("wf: node %u [%s] not yet implemented — skipping\n",
			(uint32_t)node->id,
			node->kind == ANX_WF_NODE_CONDITION ? "condition" : "transform");
		ret = ANX_OK;
		break;

	default:
		ret = ANX_ENOSYS;
		break;
	}

	entry->completed_at = arch_time_now();
	entry->result       = ret;
	return ret;
}

/* ------------------------------------------------------------------ */
/* Suspension                                                          */
/* ------------------------------------------------------------------ */

static void
wf_suspend(struct anx_wf_object *wf, uint16_t failed_node_id, int error_code,
	   const char *error_msg,
	   const uint32_t *in_deg, const bool *completed,
	   const anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS])
{
	struct anx_wf_continuation *cont;

	cont = anx_alloc(sizeof(*cont));
	if (!cont) {
		/* Out of memory — hard fail rather than silent corruption. */
		wf->run_state   = ANX_WF_RUN_FAILED;
		wf->last_status = ANX_ENOMEM;
		return;
	}

	cont->failed_node_id = failed_node_id;
	cont->error_code     = error_code;
	anx_strlcpy(cont->error_msg, error_msg ? error_msg : "", sizeof(cont->error_msg));

	anx_memcpy(cont->in_deg,    in_deg,    sizeof(uint32_t) * ANX_WF_MAX_NODES);
	anx_memcpy(cont->completed, completed, sizeof(bool)     * ANX_WF_MAX_NODES);
	anx_memcpy(cont->port_oid,  port_oid,
		   sizeof(anx_oid_t) * ANX_WF_MAX_NODES * ANX_WF_MAX_PORTS);

	anx_free(wf->continuation);
	wf->continuation = cont;
	wf->run_state    = ANX_WF_RUN_SUSPENDED;
	wf->last_status  = error_code;

	kprintf("wf: '%s' suspended at node %u (err %d)\n",
		wf->name, (uint32_t)failed_node_id, error_code);
}

/* ------------------------------------------------------------------ */
/* Core dispatch loop                                                  */
/* ------------------------------------------------------------------ */

/*
 * Execute the workflow from the given executor state.
 *
 * in_deg and completed are read/write: this function modifies them as
 * nodes complete.  port_oid accumulates output OIDs as nodes run.
 *
 * cap: max nodes dispatched concurrently.  In Phase 2, dispatch is
 * synchronous so "batch" nodes are run sequentially within a cap-sized
 * window.  Replace the anx_cell_run() call with async dispatch in Phase 3.
 */
static int
wf_run_inner(struct anx_wf_object *wf,
	     uint32_t *in_deg, bool *completed,
	     anx_oid_t (*port_oid)[ANX_WF_MAX_PORTS],
	     uint32_t cap)
{
	uint32_t slot_id[ANX_WF_MAX_NODES];
	uint32_t slot_by_id[ANX_WF_MAX_NODES + 1];
	uint32_t queue[ANX_WF_MAX_NODES];
	uint32_t q_head, q_tail;
	uint32_t processed;
	uint32_t total_nodes;
	uint32_t active;
	uint32_t i;

	wf_build_tables(wf, slot_id, slot_by_id);

	/* Count occupied slots. */
	total_nodes = 0;
	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		if (slot_id[i] != 0)
			total_nodes++;
	}

	if (total_nodes == 0)
		return ANX_EINVAL;

	/* Seed the ready queue with every unfinished in_deg-0 slot. */
	q_head = q_tail = 0;
	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		if (slot_id[i] != 0 && in_deg[i] == 0 && !completed[i])
			queue[q_tail++] = i;
	}

	processed = 0;
	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		if (completed[i])
			processed++;
	}

	active = 0;

	while (q_head < q_tail) {
		uint32_t batch_end;
		uint32_t s;

		/* Consume up to cap slots from the ready queue as a batch. */
		batch_end = q_head + cap;
		if (batch_end > q_tail)
			batch_end = q_tail;

		for (s = q_head; s < batch_end; s++) {
			uint32_t slot = queue[s];
			struct anx_wf_trace_entry *entry;
			int ret;

			active++;

			if (wf->trace_entries &&
			    wf->trace_entry_count < (uint32_t)ANX_WF_MAX_NODES)
				entry = &wf->trace_entries[wf->trace_entry_count];
			else
				entry = NULL;

			kprintf("wf:   dispatch node %u [%s] '%s'\n",
				(uint32_t)wf->nodes[slot].id,
				wf->nodes[slot].label,
				wf->nodes[slot].label);

			ret = wf_dispatch_node(wf, slot, slot_by_id, port_oid,
					       entry ? entry : &(struct anx_wf_trace_entry){0});

			if (entry)
				wf->trace_entry_count++;

			active--;

			if (ret == ANX_EBUSY &&
			    wf->nodes[slot].kind == ANX_WF_NODE_HUMAN_REVIEW) {
				/* Pause for human review — not a failure. */
				wf->run_state   = ANX_WF_RUN_WAITING_HUMAN;
				wf->last_status = ANX_OK;
				q_head = s + 1;
				return ANX_OK;
			}

			if (ret != ANX_OK) {
				char msg[256];

				anx_strlcpy(msg, wf->nodes[slot].label, sizeof(msg));
				wf_suspend(wf, wf->nodes[slot].id, ret, msg,
					   in_deg, completed, port_oid);
				/* Advance q_head so a subsequent resume starts correctly. */
				q_head = s + 1;
				return ret;
			}

			/* Success: mark completed, reduce successors. */
			completed[slot] = true;
			processed++;

			for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
				uint16_t from = wf->edges[i].from_node;
				uint16_t to   = wf->edges[i].to_node;
				uint32_t to_slot;

				if (from != (uint16_t)wf->nodes[slot].id || to == 0)
					continue;

				if ((uint32_t)to > ANX_WF_MAX_NODES)
					continue;
				to_slot = slot_by_id[to];
				if (to_slot >= ANX_WF_MAX_NODES)
					continue;

				if (in_deg[to_slot] > 0)
					in_deg[to_slot]--;
				if (in_deg[to_slot] == 0 && !completed[to_slot])
					queue[q_tail++] = to_slot;
			}
		}

		q_head = batch_end;
		(void)active;
	}

	if (processed != total_nodes) {
		kprintf("wf: '%s' cycle detected (%u/%u)\n",
			wf->name, processed, total_nodes);
		wf->run_state   = ANX_WF_RUN_FAILED;
		wf->last_status = ANX_EINVAL;
		return ANX_EINVAL;
	}

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int
anx_wf_run(const anx_oid_t *wf_oid, anx_cid_t *run_cid_out)
{
	struct anx_wf_object *wf;
	uint32_t in_deg[ANX_WF_MAX_NODES];
	bool     completed[ANX_WF_MAX_NODES];
	anx_oid_t port_oid[ANX_WF_MAX_NODES][ANX_WF_MAX_PORTS];
	uint32_t slot_id[ANX_WF_MAX_NODES];
	uint32_t slot_by_id[ANX_WF_MAX_NODES + 1];
	uint32_t cap;
	uint32_t i, j;
	int ret;

	struct anx_jepa_obs obs_before, obs_after;
	anx_oid_t obs_before_oid, obs_after_oid, trace_oid;
	bool jepa_obs_ok = false;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;
	if (wf->run_state == ANX_WF_RUN_RUNNING)
		return ANX_EBUSY;
	if (wf->node_count == 0)
		return ANX_EINVAL;

	/* Snapshot system state before dispatch for JEPA training. */
	if (anx_jepa_available()) {
		if (anx_jepa_observe(&obs_before) == ANX_OK &&
		    anx_jepa_observe_store(&obs_before, &obs_before_oid) == ANX_OK)
			jepa_obs_ok = true;
	}

	/* Allocate trace entry table for this run. */
	anx_free(wf->trace_entries);
	wf->trace_entries = anx_alloc(
		sizeof(struct anx_wf_trace_entry) * ANX_WF_MAX_NODES);
	if (!wf->trace_entries)
		return ANX_ENOMEM;
	wf->trace_entry_count = 0;

	/* Reset output table. */
	anx_memset(wf->output_oids, 0, sizeof(wf->output_oids));
	wf->output_count = 0;

	/* Compute concurrency cap from current host resources. */
	cap = anx_wf_cap_compute(&wf->policy);
	wf->computed_cap = cap;

	wf->run_state = ANX_WF_RUN_RUNNING;
	wf->last_run  = arch_time_now();

	/* Build tables and initialize executor state. */
	wf_build_tables(wf, slot_id, slot_by_id);

	anx_memset(in_deg,   0, sizeof(in_deg));
	anx_memset(completed, 0, sizeof(completed));
	anx_memset(port_oid,  0, sizeof(port_oid));

	for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
		uint16_t to = wf->edges[i].to_node;

		if (to == 0 || (uint32_t)to > ANX_WF_MAX_NODES)
			continue;
		j = slot_by_id[to];
		if (j < ANX_WF_MAX_NODES)
			in_deg[j]++;
	}

	kprintf("wf: running '%s' (%u nodes, %u edges, cap %u)\n",
		wf->name, (uint32_t)wf->node_count,
		(uint32_t)wf->edge_count, cap);

	ret = wf_run_inner(wf, in_deg, completed, port_oid, cap);

	if (ret == ANX_OK && wf->run_state == ANX_WF_RUN_RUNNING) {
		wf->run_state   = ANX_WF_RUN_COMPLETED;
		wf->last_status = ANX_OK;
		kprintf("wf: '%s' completed\n", wf->name);

		/* Seal trace and feed to JEPA training pipeline. */
		if (jepa_obs_ok &&
		    anx_wf_trace_seal(wf_oid, &trace_oid) == ANX_OK &&
		    anx_jepa_observe(&obs_after) == ANX_OK &&
		    anx_jepa_observe_store(&obs_after, &obs_after_oid) == ANX_OK) {
			anx_jepa_ingest_wf_trace(&trace_oid,
						 &obs_before_oid,
						 &obs_after_oid);
		}
	}

	if (run_cid_out)
		anx_memset(run_cid_out, 0, sizeof(*run_cid_out));

	return ret;
}

int
anx_wf_resume(const anx_oid_t *wf_oid,
	      enum anx_wf_resume_action action,
	      const struct anx_wf_node *replacement)
{
	struct anx_wf_object     *wf;
	struct anx_wf_continuation *cont;
	uint32_t slot_id[ANX_WF_MAX_NODES];
	uint32_t slot_by_id[ANX_WF_MAX_NODES + 1];
	uint32_t failed_slot;
	uint32_t i;
	int ret;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;
	if (wf->run_state != ANX_WF_RUN_SUSPENDED)
		return ANX_EINVAL;

	cont = wf->continuation;
	if (!cont)
		return ANX_EINVAL;

	wf_build_tables(wf, slot_id, slot_by_id);

	/* Locate the slot of the failed node. */
	failed_slot = ANX_WF_MAX_NODES;
	if ((uint32_t)cont->failed_node_id <= ANX_WF_MAX_NODES)
		failed_slot = slot_by_id[cont->failed_node_id];

	if (action == ANX_WF_RESUME_ABORT) {
		int saved_error = cont->error_code;

		anx_free(cont);
		wf->continuation = NULL;
		wf->run_state    = ANX_WF_RUN_FAILED;
		wf->last_status  = saved_error;
		return ANX_OK;
	}

	if (action == ANX_WF_RESUME_REPLACE) {
		if (!replacement || failed_slot >= ANX_WF_MAX_NODES)
			return ANX_EINVAL;
		/* Preserve the node's id and position in the graph. */
		uint16_t preserved_id = wf->nodes[failed_slot].id;

		wf->nodes[failed_slot]    = *replacement;
		wf->nodes[failed_slot].id = preserved_id;
	}

	if (action == ANX_WF_RESUME_SKIP && failed_slot < ANX_WF_MAX_NODES) {
		/* Mark the failed node as completed with null outputs. */
		cont->completed[failed_slot] = true;
		/* Reduce in_deg of its successors. */
		for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
			uint16_t from = wf->edges[i].from_node;
			uint16_t to   = wf->edges[i].to_node;
			uint32_t to_slot;

			if (from != cont->failed_node_id || to == 0)
				continue;
			if ((uint32_t)to > ANX_WF_MAX_NODES)
				continue;
			to_slot = slot_by_id[to];
			if (to_slot < ANX_WF_MAX_NODES && cont->in_deg[to_slot] > 0)
				cont->in_deg[to_slot]--;
		}
	}
	/* RETRY and REPLACE: leave failed node incomplete — it will be re-dispatched. */

	wf->run_state = ANX_WF_RUN_RUNNING;
	wf->continuation = NULL;	/* executor takes ownership back */

	ret = wf_run_inner(wf, cont->in_deg, cont->completed, cont->port_oid,
			   wf->computed_cap);

	anx_free(cont);

	if (ret == ANX_OK && wf->run_state == ANX_WF_RUN_RUNNING) {
		wf->run_state   = ANX_WF_RUN_COMPLETED;
		wf->last_status = ANX_OK;
		kprintf("wf: '%s' completed after resume\n", wf->name);
	}

	return ret;
}

/* ------------------------------------------------------------------ */
/* Trace sealing                                                       */
/* ------------------------------------------------------------------ */

int
anx_wf_trace_seal(const anx_oid_t *wf_oid, anx_oid_t *trace_oid_out)
{
	struct anx_wf_object *wf;
	struct anx_so_create_params p;
	struct anx_state_object *obj;
	int ret;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;
	if (wf->trace_entry_count == 0)
		return ANX_ENOENT;

	anx_memset(&p, 0, sizeof(p));
	p.object_type  = ANX_OBJ_EXECUTION_TRACE;
	p.payload      = wf->trace_entries;
	p.payload_size = sizeof(struct anx_wf_trace_entry) * wf->trace_entry_count;

	ret = anx_so_create(&p, &obj);
	if (ret != ANX_OK)
		return ret;

	wf->trace_oid = obj->oid;
	if (trace_oid_out)
		*trace_oid_out = obj->oid;

	anx_free(wf->trace_entries);
	wf->trace_entries     = NULL;
	wf->trace_entry_count = 0;

	kprintf("wf: '%s' trace sealed\n", wf->name);
	return ANX_OK;
}
