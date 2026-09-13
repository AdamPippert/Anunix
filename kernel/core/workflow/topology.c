#include <anx/workflow.h>
#include <anx/cell.h>
#include <anx/string.h>
#include <anx/state_object.h>

static int validate_graph(const struct anx_wf_object *wf, const struct anx_wf_edge *edges, uint32_t count)
{
	uint32_t node_count = 0, edge_count = 0, visited = 0;
	uint32_t indegree[ANX_WF_MAX_NODES] = {0};
	bool done[ANX_WF_MAX_NODES] = {0};
	if (!wf->nodes || !edges || !wf->node_count || wf->node_count > ANX_WF_MAX_NODES || count > ANX_WF_MAX_EDGES)
		return ANX_EINVAL;
	for (uint32_t i = 0; i < ANX_WF_MAX_NODES; i++) {
		const struct anx_wf_node *node = &wf->nodes[i];
		if (!node->id) continue;
		if (node->id != i + 1 || (int)node->kind < 0 || node->kind >= ANX_WF_NODE_KIND_COUNT ||
		    node->port_count > ANX_WF_MAX_PORTS) return ANX_EINVAL;
		for (uint32_t p = 0; p < node->port_count; p++)
			if ((node->ports[p].dir != ANX_WF_PORT_IN && node->ports[p].dir != ANX_WF_PORT_OUT) ||
			    node->ports[p].type_tag > 3) return ANX_EINVAL;
		node_count++;
	}
	if (node_count != wf->node_count) return ANX_EINVAL;
	for (uint32_t i = 0; i < ANX_WF_MAX_EDGES; i++) {
		const struct anx_wf_edge *edge = &edges[i];
		const struct anx_wf_node *from, *to;
		if (!edge->from_node && !edge->to_node) continue;
		if (!edge->from_node || !edge->to_node || edge->from_node > ANX_WF_MAX_NODES ||
		    edge->to_node > ANX_WF_MAX_NODES || edge->from_node == edge->to_node) return ANX_EINVAL;
		from = &wf->nodes[edge->from_node - 1]; to = &wf->nodes[edge->to_node - 1];
		if (!from->id || !to->id || edge->from_port >= from->port_count || edge->to_port >= to->port_count ||
		    from->ports[edge->from_port].dir != ANX_WF_PORT_OUT || to->ports[edge->to_port].dir != ANX_WF_PORT_IN)
			return ANX_EINVAL;
		uint8_t ft = from->ports[edge->from_port].type_tag, tt = to->ports[edge->to_port].type_tag;
		if (ft && tt && ft != tt) return ANX_EINVAL;
		for (uint32_t j = 0; j < i; j++)
			if (edges[j].from_node == edge->from_node && edges[j].to_node == edge->to_node &&
			    edges[j].from_port == edge->from_port && edges[j].to_port == edge->to_port) return ANX_EINVAL;
		indegree[edge->to_node - 1]++;
		edge_count++;
	}
	if (edge_count != count) return ANX_EINVAL;
	for (uint32_t step = 0; step < node_count; step++) {
		uint32_t chosen = ANX_WF_MAX_NODES;
		for (uint32_t i = 0; i < ANX_WF_MAX_NODES; i++)
			if (wf->nodes[i].id && !done[i] && !indegree[i]) { chosen = i; break; }
		if (chosen == ANX_WF_MAX_NODES) return ANX_EINVAL;
		done[chosen] = true;
		visited++;
		for (uint32_t i = 0; i < ANX_WF_MAX_EDGES; i++)
			if (edges[i].from_node == chosen + 1) indegree[edges[i].to_node - 1]--;
	}
	return visited == node_count ? ANX_OK : ANX_EINVAL;
}

static int check_run_evidence(const struct anx_wf_object *wf)
{
	struct anx_object_handle handle = {0};
	int ret;
	if (wf->run_state != ANX_WF_RUN_COMPLETED || !wf->trace_entries || wf->trace_entry_count != wf->node_count)
		return ANX_EPERM;
	ret = anx_so_open(&wf->trace_oid, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK) return ret;
	anx_spin_lock(&handle.obj->lock);
	uint64_t size = wf->trace_entry_count * sizeof(*wf->trace_entries);
	if (handle.obj->state != ANX_OBJ_SEALED || handle.obj->object_type != ANX_OBJ_EXECUTION_TRACE ||
	    !handle.obj->payload || handle.obj->payload_size != size || anx_memcmp(handle.obj->payload, wf->trace_entries, size))
		ret = ANX_EPERM;
	anx_spin_unlock(&handle.obj->lock);
	anx_so_close(&handle);
	return ret;
}

static int controllable(const anx_oid_t *oid, struct anx_wf_object **out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!oid || !out) return ANX_EINVAL;
	*out = anx_wf_object_get(oid);
	if (!*out) return ANX_ENOENT;
	if ((*out)->run_state == ANX_WF_RUN_RUNNING || (*out)->run_state == ANX_WF_RUN_SUSPENDED ||
	    (*out)->run_state == ANX_WF_RUN_WAITING_HUMAN) return ANX_EBUSY;
	return ANX_OK;
}

static void require_new_run(struct anx_wf_object *wf)
{
	wf->run_state = ANX_WF_RUN_IDLE;
	wf->output_count = 0;
	anx_memset(wf->output_oids, 0, sizeof(wf->output_oids));
}

int anx_wf_topology_enable(const anx_oid_t *oid, uint32_t revision_limit)
{
	struct anx_wf_object *wf;
	int ret = controllable(oid, &wf);
	if (ret != ANX_OK) return ret;
	if (!revision_limit || revision_limit > ANX_WF_TOPOLOGY_REVISIONS_MAX) return ANX_EINVAL;
	if (wf->topology.enabled) return ANX_EBUSY;
	ret = validate_graph(wf, wf->edges, wf->edge_count);
	if (ret != ANX_OK) return ret;
	wf->topology.enabled = true;
	wf->topology.limit = revision_limit;
	wf->topology.epoch = 1;
	return ANX_OK;
}

int anx_wf_topology_revise(const anx_oid_t *oid, uint64_t expected_epoch,
			   const struct anx_wf_edge *edges, uint32_t count)
{
	struct anx_wf_object *wf;
	struct anx_wf_edge candidate[ANX_WF_MAX_EDGES] = {0};
	int ret = controllable(oid, &wf);
	if (ret != ANX_OK) return ret;
	if (!wf->topology.enabled) return ANX_EPERM;
	if (!expected_epoch || expected_epoch != wf->topology.epoch) return ANX_EBUSY;
	if (count > ANX_WF_MAX_EDGES || (!edges && count)) return ANX_EINVAL;
	if (wf->topology.accepted >= wf->topology.limit || wf->topology.epoch == ~(uint64_t)0) return ANX_EFULL;
	if (count) anx_memcpy(candidate, edges, count * sizeof(*edges));
	ret = validate_graph(wf, candidate, count);
	if (ret == ANX_OK) ret = check_run_evidence(wf);
	if (ret != ANX_OK) return ret;
	anx_memcpy(wf->topology.previous, wf->edges, sizeof(candidate));
	wf->topology.previous_count = wf->edge_count;
	anx_memcpy(wf->edges, candidate, sizeof(candidate));
	wf->edge_count = (uint16_t)count;
	wf->topology.evidence_oid = wf->trace_oid;
	wf->topology.can_rollback = true;
	wf->topology.accepted++;
	wf->topology.epoch++;
	require_new_run(wf);
	return ANX_OK;
}

int anx_wf_topology_rollback(const anx_oid_t *oid, uint64_t expected_epoch)
{
	struct anx_wf_object *wf;
	int ret = controllable(oid, &wf);
	if (ret != ANX_OK) return ret;
	if (!wf->topology.enabled || !wf->topology.can_rollback) return ANX_EPERM;
	if (!expected_epoch || expected_epoch != wf->topology.epoch) return ANX_EBUSY;
	if (wf->topology.epoch == ~(uint64_t)0) return ANX_EFULL;
	ret = validate_graph(wf, wf->topology.previous, wf->topology.previous_count);
	if (ret != ANX_OK) return ret;
	anx_memcpy(wf->edges, wf->topology.previous, sizeof(wf->topology.previous));
	wf->edge_count = wf->topology.previous_count;
	wf->topology.can_rollback = false;
	wf->topology.epoch++;
	require_new_run(wf);
	return ANX_OK;
}
