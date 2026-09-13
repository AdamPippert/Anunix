/* A rejected topology revision preserves the runnable original graph. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workflow.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>

int anx_research_day037(void)
{
	anx_oid_t oid;
	struct anx_wf_object *wf;
	struct anx_wf_node node = {0};
	struct anx_wf_edge edges[3] = {0}, original[ANX_WF_MAX_EDGES];
	uint16_t ids[3], unused;
	uint64_t epoch;
	int ret = anx_wf_create("research-day-037", NULL, &oid);
	if (ret != ANX_OK) return ret;
	wf = anx_wf_object_get(&oid);
	node.kind = ANX_WF_NODE_TRIGGER;
	node.port_count = 2;
	node.ports[0].dir = ANX_WF_PORT_IN;
	node.ports[1].dir = ANX_WF_PORT_OUT;
	for (uint32_t i = 0; i < 3; i++) {
		ret = anx_wf_node_add(&oid, &node, &ids[i]);
		if (ret != ANX_OK) goto out;
	}
	ret = anx_wf_edge_add(&oid, ids[0], 1, ids[1], 0);
	if (ret == ANX_OK) ret = anx_wf_edge_add(&oid, ids[1], 1, ids[2], 0);
	if (ret == ANX_OK) ret = anx_wf_topology_enable(&oid, 1);
	if (ret == ANX_OK) ret = anx_wf_run(&oid, NULL);
	if (ret != ANX_OK) goto out;
	anx_memcpy(original, wf->edges, sizeof(original));
	epoch = wf->topology.epoch;
	edges[0] = original[0]; edges[1] = original[1];
	edges[2] = (struct anx_wf_edge){.from_node=ids[2], .to_node=ids[0], .from_port=1, .to_port=0};
	ret = -3701;
	if (anx_wf_topology_revise(&oid, epoch, edges, 3) != ANX_EINVAL ||
	    wf->topology.epoch != epoch || wf->topology.accepted || wf->edge_count != 2 ||
	    anx_memcmp(original, wf->edges, sizeof(original))) goto out;
	ret = -3702;
	edges[1].to_node = ANX_WF_MAX_NODES;
	if (anx_wf_topology_revise(&oid, epoch, edges, 2) != ANX_EINVAL) goto out;
	edges[1] = original[1]; edges[1].from_port = ANX_WF_MAX_PORTS;
	if (anx_wf_topology_revise(&oid, epoch, edges, 2) != ANX_EINVAL) goto out;
	edges[1] = original[1]; edges[1].from_port = 0;
	if (anx_wf_topology_revise(&oid, epoch, edges, 2) != ANX_EINVAL) goto out;
	edges[1] = original[0];
	if (anx_wf_topology_revise(&oid, epoch, edges, 2) != ANX_EINVAL) goto out;
	if (anx_wf_node_add(&oid, &node, &unused) != ANX_EPERM ||
	    anx_wf_node_remove(&oid, ids[2]) != ANX_EPERM ||
	    anx_wf_edge_add(&oid, ids[0], 1, ids[2], 0) != ANX_EPERM ||
	    anx_wf_edge_remove(&oid, ids[0], 1, ids[1], 0) != ANX_EPERM) goto out;
	if (anx_so_delete(&wf->trace_oid, false) != ANX_OK ||
	    anx_wf_topology_revise(&oid, epoch, original, 2) != ANX_ENOENT || wf->topology.accepted) goto out;
	ret = anx_wf_run(&oid, NULL);
	if (ret != ANX_OK) goto out;
	ret = -3703;
	if (wf->trace_entry_count != 3 || wf->trace_entries[0].node_id != ids[0] ||
	    wf->trace_entries[1].node_id != ids[1] || wf->trace_entries[2].node_id != ids[2]) goto out;
	edges[0] = (struct anx_wf_edge){.from_node=ids[0], .to_node=ids[2], .from_port=1, .to_port=0};
	edges[1] = (struct anx_wf_edge){.from_node=ids[2], .to_node=ids[1], .from_port=1, .to_port=0};
	anx_oid_t evidence = wf->trace_oid;
	ret = anx_wf_topology_revise(&oid, epoch, edges, 2);
	if (ret != ANX_OK) goto out;
	ret = -3704;
	if (wf->topology.epoch != epoch + 1 || wf->topology.accepted != 1 ||
	    anx_uuid_compare(&wf->topology.evidence_oid, &evidence) ||
	    anx_wf_topology_revise(&oid, epoch, edges, 2) != ANX_EBUSY ||
	    anx_wf_topology_revise(&oid, epoch + 1, edges, 2) != ANX_EFULL) goto out;
	ret = anx_wf_run(&oid, NULL);
	if (ret != ANX_OK) goto out;
	ret = -3705;
	if (wf->trace_entry_count != 3 || wf->trace_entries[1].node_id != ids[2] ||
	    wf->trace_entries[2].node_id != ids[1] || anx_wf_topology_rollback(&oid, epoch) != ANX_EBUSY) goto out;
	ret = anx_wf_topology_rollback(&oid, epoch + 1);
	if (ret == ANX_OK) ret = anx_wf_run(&oid, NULL);
	if (ret != ANX_OK) goto out;
	ret = -3706;
	if (anx_memcmp(original, wf->edges, sizeof(original)) || wf->topology.epoch != epoch + 2 ||
	    wf->topology.accepted != 1 || wf->trace_entries[1].node_id != ids[1] ||
	    anx_wf_topology_revise(&oid, epoch + 2, edges, 2) != ANX_EFULL ||
	    anx_wf_topology_rollback(&oid, epoch + 2) != ANX_EPERM) goto out;
	ret = ANX_OK;
out:
	anx_wf_destroy(&oid);
	return ret;
}
#endif
