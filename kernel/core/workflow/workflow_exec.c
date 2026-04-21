/*
 * workflow_exec.c — Workflow execution engine (RFC-0018).
 *
 * Phase 1: topological sort of the workflow node graph, logging each node
 * in execution order.  Full Cell dispatch is deferred to Phase 2.
 *
 * The in-degree table and traversal queue are stack-allocated arrays of
 * ANX_WF_MAX_NODES entries — no heap allocation needed for the sort itself.
 */

#include <anx/types.h>
#include <anx/workflow.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/cell.h>

/* Map a node kind to a short printable name. */
static const char *
wf_node_kind_name(enum anx_wf_node_kind k)
{
	switch (k) {
	case ANX_WF_NODE_TRIGGER:	return "trigger";
	case ANX_WF_NODE_STATE_REF:	return "state_ref";
	case ANX_WF_NODE_CELL_CALL:	return "cell_call";
	case ANX_WF_NODE_MODEL_CALL:	return "model_call";
	case ANX_WF_NODE_AGENT_CALL:	return "agent_call";
	case ANX_WF_NODE_RETRIEVAL:	return "retrieval";
	case ANX_WF_NODE_CONDITION:	return "condition";
	case ANX_WF_NODE_FAN_OUT:	return "fan_out";
	case ANX_WF_NODE_FAN_IN:	return "fan_in";
	case ANX_WF_NODE_TRANSFORM:	return "transform";
	case ANX_WF_NODE_HUMAN_REVIEW:	return "human_review";
	case ANX_WF_NODE_SUBFLOW:	return "subflow";
	case ANX_WF_NODE_OUTPUT:	return "output";
	default:			return "unknown";
	}
}

/*
 * Run a workflow: topological sort of its node graph, dispatching each
 * node in dependency order.  Phase 1 logs the execution sequence only;
 * real Cell dispatch is wired in Phase 2.
 *
 * Returns ANX_OK on success, ANX_EBUSY if already running, ANX_EINVAL if
 * the workflow has no nodes or contains a cycle (sort stalls before
 * processing all nodes).
 */
int
anx_wf_run(const anx_oid_t *wf_oid, anx_cid_t *run_cid_out)
{
	struct anx_wf_object *wf;

	/*
	 * Parallel arrays indexed 0..ANX_WF_MAX_NODES-1, one entry per
	 * possible node slot.
	 *
	 * in_deg[i]   — number of unresolved incoming edges for slot i
	 * slot_id[i]  — the node id stored in slot i (0 if slot unused)
	 * queue[]     — slot indices waiting to be processed (Kahn's algorithm)
	 */
	uint32_t in_deg[ANX_WF_MAX_NODES];
	uint32_t slot_id[ANX_WF_MAX_NODES];
	uint32_t queue[ANX_WF_MAX_NODES];
	uint32_t q_head, q_tail;	/* queue read/write cursors */
	uint32_t processed;		/* nodes emitted so far */
	uint32_t i, j;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;
	if (wf->run_state == ANX_WF_RUN_RUNNING)
		return ANX_EBUSY;
	if (wf->node_count == 0)
		return ANX_EINVAL;

	wf->run_state = ANX_WF_RUN_RUNNING;
	wf->last_run  = 0;	/* timestamp placeholder */

	kprintf("wf: running '%s' (%u nodes, %u edges)\n",
		wf->name, (uint32_t)wf->node_count, (uint32_t)wf->edge_count);

	/* --- Phase 1: build slot→id map and zero in-degree table. --- */
	anx_memset(in_deg,  0, sizeof(in_deg));
	anx_memset(slot_id, 0, sizeof(slot_id));

	for (i = 0; i < ANX_WF_MAX_NODES; i++)
		slot_id[i] = wf->nodes[i].id;	/* 0 if slot unused */

	/*
	 * For each edge, find the slot of to_node and increment its
	 * in-degree.  Edges with to_node == 0 are unused (zeroed slots).
	 */
	for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
		uint16_t to = wf->edges[i].to_node;

		if (to == 0)
			continue;	/* unused edge slot */

		/* Find slot of to_node. */
		for (j = 0; j < ANX_WF_MAX_NODES; j++) {
			if (slot_id[j] == (uint32_t)to) {
				in_deg[j]++;
				break;
			}
		}
	}

	/* --- Phase 2: Kahn's algorithm. --- */
	q_head = q_tail = 0;
	processed = 0;

	/* Seed the queue with every occupied slot that has in-degree 0. */
	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		if (slot_id[i] != 0 && in_deg[i] == 0)
			queue[q_tail++] = i;
	}

	while (q_head < q_tail) {
		uint32_t slot = queue[q_head++];
		struct anx_wf_node *node = &wf->nodes[slot];

		kprintf("wf:   node %u [%s] %s\n",
			(uint32_t)node->id,
			wf_node_kind_name(node->kind),
			node->label);

		processed++;

		/*
		 * Reduce in-degree of every successor.  A successor is any
		 * node that appears as to_node on an edge whose from_node
		 * matches this node's id.
		 */
		for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
			uint16_t from = wf->edges[i].from_node;
			uint16_t to   = wf->edges[i].to_node;

			if (from != node->id || to == 0)
				continue;

			/* Find the successor's slot. */
			for (j = 0; j < ANX_WF_MAX_NODES; j++) {
				if (slot_id[j] == (uint32_t)to) {
					if (in_deg[j] > 0)
						in_deg[j]--;
					if (in_deg[j] == 0)
						queue[q_tail++] = j;
					break;
				}
			}
		}
	}

	/* If we didn't process every node the graph has a cycle. */
	if (processed != wf->node_count) {
		kprintf("wf: '%s' aborted — cycle detected (%u/%u nodes)\n",
			wf->name, processed, (uint32_t)wf->node_count);
		wf->run_state  = ANX_WF_RUN_FAILED;
		wf->last_status = ANX_EINVAL;
		return ANX_EINVAL;
	}

	wf->run_state   = ANX_WF_RUN_COMPLETED;
	wf->last_status = ANX_OK;

	kprintf("wf: '%s' completed\n", wf->name);

	/* Phase 1: placeholder CID — real Cell binding in Phase 2. */
	if (run_cid_out)
		anx_memset(run_cid_out, 0, sizeof(*run_cid_out));

	return ANX_OK;
}
