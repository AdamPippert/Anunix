#include <anx/workflow.h>
#include <anx/cell.h>
#include <anx/string.h>

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

int anx_wf_topology_enable(const anx_oid_t *oid, uint32_t revision_limit)
{
	struct anx_wf_object *wf;
	int ret = controllable(oid, &wf);
	if (ret != ANX_OK) return ret;
	if (!revision_limit || revision_limit > ANX_WF_TOPOLOGY_REVISIONS_MAX) return ANX_EINVAL;
	if (wf->topology.enabled) return ANX_EBUSY;
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
	anx_memcpy(wf->topology.previous, wf->edges, sizeof(candidate));
	wf->topology.previous_count = wf->edge_count;
	anx_memcpy(wf->edges, candidate, sizeof(candidate));
	wf->edge_count = (uint16_t)count;
	wf->topology.evidence_oid = wf->trace_oid;
	wf->topology.can_rollback = true;
	wf->topology.accepted++;
	wf->topology.epoch++;
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
	anx_memcpy(wf->edges, wf->topology.previous, sizeof(wf->topology.previous));
	wf->edge_count = wf->topology.previous_count;
	wf->topology.can_rollback = false;
	wf->topology.epoch++;
	return ANX_OK;
}
