#ifndef ANX_WORKFLOW_REUSE_H
#define ANX_WORKFLOW_REUSE_H
#include <anx/workflow.h>

#define ANX_WF_REUSE_CONDITIONS_MAX 8U
#define ANX_WF_REUSE_OBJECT_MAX (1024U * 1024U)
enum anx_wf_execution_form {
	ANX_WF_EXPLORATORY, ANX_WF_HYBRID, ANX_WF_DETERMINISTIC,
};
struct anx_wf_reuse_view {
	bool bound, demoted;
	enum anx_wf_execution_form form;
	int reason;
	uint64_t successful_runs;
};
int anx_wf_reuse_bind(const anx_oid_t *wf_oid, enum anx_wf_execution_form form,
		      const anx_oid_t *conditions, uint32_t count);
int anx_wf_reuse_status(const anx_oid_t *wf_oid, struct anx_wf_reuse_view *out);
int anx_wf_reuse_check(struct anx_wf_object *wf);
void anx_wf_reuse_observe(struct anx_wf_object *wf, int result);
#endif
