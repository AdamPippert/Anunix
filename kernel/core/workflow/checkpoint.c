/* Baseline adapter preserves graph text but has no executor-state restore. */
#include <anx/workflow.h>
#include <anx/alloc.h>
#include <anx/string.h>
int anx_wf_checkpoint_save(const anx_oid_t *oid, anx_oid_t *out)
{
	char *text = anx_zalloc(8192);
	struct anx_so_create_params p = {0};
	struct anx_state_object *obj;
	if (!text) return ANX_ENOMEM;
	int ret = anx_wf_serialize(oid, text, 8192);
	p.object_type = ANX_OBJ_BYTE_DATA;
	p.payload = text; p.payload_size = anx_strlen(text);
	if (ret >= 0) ret = anx_so_create(&p, &obj);
	if (ret == ANX_OK) { *out = obj->oid; anx_objstore_release(obj); }
	anx_free(text);
	return ret;
}
int anx_wf_checkpoint_restore(const anx_oid_t *oid, const anx_oid_t *checkpoint)
{
	(void)oid; (void)checkpoint;
	return ANX_ENOSYS;
}
