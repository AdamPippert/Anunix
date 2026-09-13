/* Baseline adapter records the execution form without enforcing reuse conditions. */
#include <anx/workflow_reuse.h>
#include <anx/alloc.h>
struct anx_wf_reuse_guard { struct anx_wf_reuse_view view; };
int anx_wf_reuse_bind(const anx_oid_t *oid, enum anx_wf_execution_form form,
		      const anx_oid_t *conditions, uint32_t count)
{
	struct anx_wf_object *wf = anx_wf_object_get(oid);
	(void)conditions; (void)count;
	if (!wf) return ANX_ENOENT;
	anx_free(wf->reuse);
	wf->reuse = anx_zalloc(sizeof(*wf->reuse));
	if (!wf->reuse) return ANX_ENOMEM;
	wf->reuse->view.bound = true;
	wf->reuse->view.form = form;
	return ANX_OK;
}
int anx_wf_reuse_status(const anx_oid_t *oid, struct anx_wf_reuse_view *out)
{
	struct anx_wf_object *wf = anx_wf_object_get(oid);
	if (!wf || !out || !wf->reuse) return ANX_EINVAL;
	*out = wf->reuse->view;
	return ANX_OK;
}
int anx_wf_reuse_check(struct anx_wf_object *wf) { (void)wf; return ANX_OK; }
void anx_wf_reuse_observe(struct anx_wf_object *wf, int result) { (void)wf; (void)result; }
