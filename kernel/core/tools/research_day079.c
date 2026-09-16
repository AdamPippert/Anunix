#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/resource_decision.h>
#include <anx/external_call.h>
#include <anx/state_object.h>
#include <anx/effect_fence.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>
struct fixture079 {
	struct anx_resource_decision_view decisions[9], decision;
	struct anx_resource_shape_view shape, output, sentinel;
	struct anx_logical_graph_view graph;
	struct anx_physical_plan_view plans[2];
	struct anx_model_use_view uses[2];
	struct anx_anxml_response response;
	struct anx_external_call call;
	bool foreign;
};
static int proposal079(struct fixture079 *f, uint32_t i, uint32_t replicas)
{ return anx_resource_decision_propose(f->graph.id,f->shape.id,replicas,&f->decisions[i]); }
static int stale079(struct fixture079 *f, uint32_t i, int expected)
{
	struct anx_resource_shape_view before, after;
	int ret=anx_resource_shape_get(f->shape.id,&before);
	if (ret != ANX_OK) return ret;
	anx_memset(&f->output,0x55,sizeof(f->output)); f->sentinel=f->output;
	ret=anx_resource_decision_commit(f->decisions[i].id,&f->output);
	if (ret != expected) {kprintf("day079 reject expected=%d actual=%d\n",expected,ret);return -7902;}
	if (anx_memcmp(&f->output,&f->sentinel,sizeof(f->output)) ||
	    anx_resource_shape_get(f->shape.id,&after) != ANX_OK || anx_memcmp(&before,&after,sizeof(before)) ||
	    anx_resource_decision_get(f->decisions[i].id,&f->decision) != ANX_OK || f->decision.state != ANX_DECISION_STALE ||
	    anx_resource_decision_commit(f->decisions[i].id,&f->output) != ANX_EBUSY) return -7903;
	return ANX_OK;
}
static int active079(struct anx_external_call *call, void *arg)
{
	(void)call;struct fixture079 *f=arg;
	if (anx_resource_decision_commit(f->decisions[7].id,&f->output) != ANX_EPERM ||
	    proposal079(f,8,1) != ANX_EPERM || anx_resource_decision_destroy(f->decisions[7].id) != ANX_EPERM) return -7904;
	if (f->foreign) return anx_resource_decision_get(f->decisions[7].id,&f->decision)==ANX_EPERM ? ANX_OK : -7905;
	int ret=anx_resource_decision_get(f->decisions[7].id,&f->decision);
	if (ret == ANX_OK && f->decision.state != ANX_DECISION_COMMITTED) ret=-7906;
	if (ret == ANX_OK) ret=anx_physical_plan_commit(f->plans[1].id,&f->response,&f->graph);
	return ret == ANX_OK && f->response.output_len==4 && !anx_memcmp(f->response.output,"AAAA",4) &&
		f->graph.completed==3 && f->graph.physical_operations==2 ? ANX_OK : -7906;
}
int anx_research_day079(void)
{
	struct fixture079 *f=anx_zalloc(sizeof(*f));
	struct anx_cell *owner=NULL,*foreign=NULL;
	struct anx_cell_intent intent={0};
	struct anx_state_object *image=NULL,*prompt=NULL;
	struct anx_adapter_image adapter={.format=1,.count=2,.deltas={{'~','A',4096},{'A','A',4096}}};
	struct anx_so_create_params p={.object_type=ANX_OBJ_STRUCTURED_DATA,.schema_uri=ANX_MODEL_USE_SCHEMA,.schema_version="1",
		.payload=&adapter,.payload_size=sizeof(adapter)};
	struct anx_phase_contract contract={.role=ANX_ROLE_RUNNER};
	struct anx_phase_request request={ANX_PHASE_INFERENCE,8192,0};
	struct anx_phase_view phase;
	struct anx_effect_fence_view fence;
	struct anx_model_use_spec use={.maximum_tokens=4};
	struct anx_logical_graph_spec graph={.count=2};
	bool attached=false;
	int ret=ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name,"research-day-079",sizeof(intent.name));
	ret=anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL,&intent,&owner);
	if (ret == ANX_OK) ret=anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL,&intent,&foreign);
	if (ret == ANX_OK) ret=anx_effect_fence_create(&fence);
	if (ret == ANX_OK) ret=anx_effect_fence_bind(owner,&fence.id);
	if (ret == ANX_OK) ret=anx_so_create(&p,&image);
	if (ret == ANX_OK) ret=anx_so_seal(&image->oid);
	contract.limits[ANX_PHASE_INFERENCE]=(struct anx_phase_limit){true,ANX_MEM_L1,16384,ANX_ACCEL_NONE,0};
	if (ret == ANX_OK) {ret=anx_phase_attach(&owner->cid,&contract);attached=ret==ANX_OK;}
	if (ret == ANX_OK) ret=anx_phase_get(&owner->cid,&phase);
	if (ret == ANX_OK) ret=anx_phase_begin(&owner->cid,phase.epoch,&request);
	if (ret == ANX_OK) ret=anx_phase_get(&owner->cid,&phase);
	if (ret == ANX_OK) ret=anx_resource_shape_create(&owner->cid,&image->oid,1,&f->shape);
	p=(struct anx_so_create_params){.object_type=ANX_OBJ_BYTE_DATA,.payload="~",.payload_size=1};
	if (ret == ANX_OK) ret=anx_so_create(&p,&prompt);
	if (ret == ANX_OK) ret=anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	use.image=image->oid;use.prompt=prompt->oid;
	for (uint32_t i=0;i<2;i++) {
		ret=anx_model_use_prepare(&owner->cid,&use,&f->uses[i]);
		if (ret != ANX_OK) goto out;
		graph.nodes[i]=(struct anx_shape_node){f->uses[i].id,i?1U:0U,ANX_SHAPE_INFER};
	}
	ret=anx_logical_graph_create(&owner->cid,&graph,&f->graph);
	if (ret != ANX_OK) goto out;
	ret=-7901;if (proposal079(f,0,2)!=ANX_OK) goto out;
	if (proposal079(f,8,0)!=ANX_EINVAL || proposal079(f,8,5)!=ANX_EINVAL || proposal079(f,8,3)!=ANX_ENOMEM) {ret=-7907;goto out;}
	/* Lazy materialization changes observed state even without a geometry epoch change. */
	ret=anx_physical_plan_compile_shaped(f->graph.id,f->graph.epoch,phase.epoch,f->shape.id,f->shape.epoch,0,&f->plans[0]);
	if (ret == ANX_OK) ret=stale079(f,0,ANX_EBUSY);
	if (ret == ANX_OK) ret=proposal079(f,1,2);
	if (ret == ANX_OK) ret=anx_physical_plan_commit(f->plans[0].id,&f->response,&f->graph);
	if (ret == ANX_OK) ret=stale079(f,1,ANX_EBUSY);
	if (ret == ANX_OK) ret=proposal079(f,2,2);
	if (ret == ANX_OK) ret=anx_phase_resize(&owner->cid,phase.epoch,12288,0,&phase);
	if (ret == ANX_OK) ret=stale079(f,2,ANX_EBUSY);
	if (ret == ANX_OK) ret=proposal079(f,3,2);
	if (ret == ANX_OK) ret=anx_resource_shape_resize(f->shape.id,f->shape.epoch,3,&f->shape);
	if (ret == ANX_OK) ret=stale079(f,3,ANX_EBUSY);
	if (ret == ANX_OK) ret=proposal079(f,4,2);
	if (ret == ANX_OK) ret=anx_effect_fence_transition(&fence.id,fence.generation,ANX_FENCE_HELD);
	if (ret == ANX_OK) ret=anx_effect_fence_get(&fence.id,&fence);
	if (ret == ANX_OK) ret=anx_effect_fence_transition(&fence.id,fence.generation,ANX_FENCE_RUNNING);
	if (ret == ANX_OK) ret=stale079(f,4,ANX_EBUSY);
	if (ret == ANX_OK) ret=proposal079(f,5,2);
	if (ret != ANX_OK) goto out;
	((uint8_t *)image->payload)[0]^=1;ret=stale079(f,5,ANX_EBUSY);((uint8_t *)image->payload)[0]^=1;
	if (ret == ANX_OK) ret=proposal079(f,6,2);
	if (ret != ANX_OK) goto out;
	image->access_policy.rule_count=1;image->access_policy.rules[0]=(struct anx_access_rule){.operations=ANX_ACCESS_READ_PAYLOAD,.effect=ANX_EFFECT_DENY};
	ret=stale079(f,6,ANX_EPERM);image->access_policy.rule_count=0;
	if (ret == ANX_OK) ret=proposal079(f,7,2);
	if (ret != ANX_OK) goto out;
	f->decisions[7].desired_replicas=4;f->decisions[7].phase.epoch=0;f->decisions[7].logical.completed=3;
	ret=anx_resource_decision_commit(f->decisions[7].id,&f->shape);
	if (ret != ANX_OK) goto out;
	if (f->shape.replicas!=2 || anx_resource_decision_commit(f->decisions[7].id,&f->output)!=ANX_EBUSY ||
	    anx_logical_graph_get(f->graph.id,&f->graph)!=ANX_OK || f->graph.completed!=1) {ret=-7908;goto out;}
	ret=anx_physical_plan_compile_shaped(f->graph.id,f->graph.epoch,phase.epoch,f->shape.id,f->shape.epoch,1,&f->plans[1]);
	if (ret == ANX_OK) ret=anx_external_register_handler("anxresearch079",active079,f);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint,"anxresearch079://inspect",sizeof(f->call.endpoint));
	foreign->execution.allow_side_effects=true;foreign->ext_call=&f->call;f->foreign=true;
	ret=anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_side_effects=true;owner->ext_call=&f->call;f->foreign=false;
	ret=anx_cell_run(owner);
	if (ret == ANX_OK) kprintf("day079 stale_decisions=7 committed_replicas=2 preserved_prefix=1 completed=3 outputs=AAAA,AAAA\n");
out:
	anx_external_unregister_handler("anxresearch079");
	for (uint32_t i=0;i<9;i++) if (f->decisions[i].id) anx_resource_decision_destroy(f->decisions[i].id);
	for (uint32_t i=0;i<2;i++) if (f->plans[i].id) anx_physical_plan_destroy(f->plans[i].id);
	if (f->graph.id) anx_logical_graph_destroy(f->graph.id);
	if (f->shape.id) anx_resource_shape_destroy(f->shape.id);
	for (uint32_t i=0;i<2;i++) if (f->uses[i].id) anx_model_use_destroy(f->uses[i].id);
	if (attached) {anx_phase_get(&owner->cid,&phase);anx_phase_finish(&owner->cid,phase.epoch);anx_phase_detach(&owner->cid);}
	if (prompt) {anx_oid_t oid=prompt->oid;anx_objstore_release(prompt);anx_so_delete(&oid,false);}
	if (image) {anx_oid_t oid=image->oid;anx_objstore_release(image);anx_so_delete(&oid,false);}
	if (foreign) anx_cell_destroy(foreign);
	if (owner) anx_cell_destroy(owner);
	if (ret != ANX_OK) kprintf("day079 failure=%d\n",ret);
	anx_free(f);return ret;
}
#endif
