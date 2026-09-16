#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/continuation.h>
#include <anx/model_use.h>
#include <anx/phase.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>
struct fixture078 {
	struct anx_continuation_view view[2], output, sentinel;
	struct anx_continuation_idle_request idle[2];
	struct anx_adapter_image image;
	struct anx_anxml_request request;
	struct anx_anxml_response response;
	struct anx_external_call call;
	bool foreign;
};
static int generate078(struct fixture078 *f, uint32_t i)
{
	struct anx_adapter_image restored;
	uint8_t digest[32]; anx_sha256(&f->image, sizeof(f->image), digest);
	int ret = anx_continuation_acceleration_read(f->view[i].id, f->view[i].epoch, f->view[i].cache_generation, &restored);
	if (ret == ANX_OK) ret = anx_anxml_generate_verified(&f->request, &restored, digest, &f->response);
	return ret == ANX_OK && f->response.output_len == 4 && !anx_memcmp(f->response.output, "AAAA", 4) ? ANX_OK : -7802;
}
static int denied078(struct fixture078 *f, uint32_t i, uint64_t epoch, int expected)
{
	struct anx_continuation_view current;
	struct anx_resource_pool_stats before, after;
	int ret = anx_continuation_cache_stats(f->view[i].id, &before);
	if (ret != ANX_OK) return ret;
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->sentinel = f->output;
	ret = anx_continuation_idle_reclaim(f->view[i].id, epoch, &f->idle[i], &f->output);
	if (ret != expected) { kprintf("day078 rejection expected=%d actual=%d\n", expected, ret); return -7803; }
	return !anx_memcmp(&f->output, &f->sentinel, sizeof(f->output)) &&
		anx_continuation_get(f->view[i].id, &current) == ANX_OK && !anx_memcmp(&current, &f->view[i], sizeof(current)) &&
		anx_continuation_cache_stats(f->view[i].id, &after) == ANX_OK && !anx_memcmp(&before, &after, sizeof(before)) ? ANX_OK : -7804;
}
static int active078(struct anx_external_call *call, void *arg)
{
	(void)call; struct fixture078 *f = arg;
	if (anx_continuation_idle_reclaim(f->view[0].id, f->view[0].epoch, &f->idle[0], &f->output) != ANX_EPERM ||
	    anx_continuation_pause(f->view[0].id, f->view[0].epoch, ANX_SUSPEND_MODEL_WAIT, &f->output) != ANX_EPERM) return -7805;
	if (f->foreign) return anx_continuation_get(f->view[0].id, &f->output) == ANX_EPERM ? ANX_OK : -7806;
	return generate078(f, 0);
}
int anx_research_day078(void)
{
	struct fixture078 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *parent = NULL, *owner[2] = {0}, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *model = NULL;
	struct anx_phase_view phases[2], current;
	struct anx_phase_contract contract = {.role=ANX_ROLE_RUNNER};
	struct anx_phase_request request = {ANX_PHASE_INFERENCE,4096,0};
	bool attached[2] = {0};
	bool journals_saved = false;
	uint64_t available, held;
	struct anx_resource_pool_stats stats;
	anx_oid_t events[ANX_CONTINUATION_EVENTS * 2]; uint32_t event_count = 0;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	f->image = (struct anx_adapter_image){.format=1,.count=2,.deltas={{'~','A',4096},{'A','A',4096}}};
	f->request.prompt[0]='~'; f->request.prompt_len=1; f->request.max_tokens=4;
	struct anx_so_create_params params = {.object_type=ANX_OBJ_STRUCTURED_DATA,.schema_uri=ANX_MODEL_USE_SCHEMA,
		.schema_version="1",.payload=&f->image,.payload_size=sizeof(f->image)};
	contract.limits[ANX_PHASE_INFERENCE]=(struct anx_phase_limit){true,ANX_MEM_L1,8192,ANX_ACCEL_NONE,0};
	anx_strlcpy(intent.name,"research-day-078",sizeof(intent.name));
	ret=anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL,&intent,&parent);
	if (ret == ANX_OK) ret=anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL,&intent,&foreign);
	if (ret != ANX_OK) goto out;
	parent->execution.allow_recursive_cells=parent->execution.allow_side_effects=true;
	ret=anx_so_create(&params,&model);
	if (ret == ANX_OK) ret=anx_so_seal(&model->oid);
	if (ret != ANX_OK) goto out;
	anx_lease_avail_mem(ANX_MEM_L1,&available);
	for (uint32_t i=0;i<2;i++) {
		ret=anx_cell_derive_child(parent,ANX_CELL_TASK_EXTERNAL_CALL,&intent,&owner[i]);
		if (ret == ANX_OK) ret=anx_continuation_create(&owner[i]->cid,&f->view[i]);
		if (ret == ANX_OK) ret=anx_phase_attach(&owner[i]->cid,&contract);
		if (ret != ANX_OK) goto out;
		attached[i]=true;
		ret=anx_phase_get(&owner[i]->cid,&phases[i]);
		if (ret == ANX_OK) ret=anx_phase_begin(&owner[i]->cid,phases[i].epoch,&request);
		if (ret == ANX_OK) ret=anx_phase_get(&owner[i]->cid,&phases[i]);
		if (ret == ANX_OK) ret=anx_continuation_suspend_configure(f->view[i].id,f->view[i].epoch,phases[i].epoch,&model->oid,&f->view[i]);
		if (ret != ANX_OK) goto out;
		f->idle[i]=(struct anx_continuation_idle_request){parent->cid,model->oid,phases[i].epoch,1};
	}
	ret=-7801;
	if (anx_continuation_pause(f->view[0].id,f->view[0].epoch,ANX_SUSPEND_MODEL_WAIT,&f->view[0]) != ANX_OK) goto out;
	ret=denied078(f,1,f->view[1].epoch,ANX_EBUSY); /* Runnable siblings stay resident. */
	if (ret != ANX_OK) goto out;
	f->idle[0].max_pages=0; ret=denied078(f,0,f->view[0].epoch,ANX_ENOMEM); f->idle[0].max_pages=1;
	if (ret != ANX_OK) goto out;
	f->idle[0].lineage_parent=foreign->cid; ret=denied078(f,0,f->view[0].epoch,ANX_EPERM); f->idle[0].lineage_parent=parent->cid;
	if (ret != ANX_OK) goto out;
	f->idle[0].model_source=ANX_UUID_NIL; ret=denied078(f,0,f->view[0].epoch,ANX_EPERM); f->idle[0].model_source=model->oid;
	if (ret != ANX_OK) goto out;
	f->idle[0].phase_epoch++; ret=denied078(f,0,f->view[0].epoch,ANX_EBUSY); f->idle[0].phase_epoch--;
	if (ret == ANX_OK) ret=denied078(f,0,f->view[0].epoch-1,ANX_EBUSY);
	if (ret != ANX_OK) goto out;
	((uint8_t *)model->payload)[0]^=1; ret=denied078(f,0,f->view[0].epoch,ANX_EBUSY); ((uint8_t *)model->payload)[0]^=1;
	if (ret != ANX_OK) goto out;
	model->access_policy.rule_count=1;
	model->access_policy.rules[0]=(struct anx_access_rule){.operations=ANX_ACCESS_READ_PAYLOAD,.effect=ANX_EFFECT_DENY};
	ret=denied078(f,0,f->view[0].epoch,ANX_EPERM); model->access_policy.rule_count=0;
	if (ret != ANX_OK) goto out;
	ret=anx_continuation_idle_reclaim(f->view[0].id,f->view[0].epoch,&f->idle[0],&f->view[0]);
	if (ret != ANX_OK) goto out;
	if (f->view[0].resource_state != ANX_CONT_IDLE_RECLAIMED || f->view[0].physical_pages ||
	    anx_continuation_cache_stats(f->view[0].id,&stats) != ANX_OK || stats.physical_pages || stats.live_bytes ||
	    anx_continuation_acceleration_read(f->view[0].id,f->view[0].epoch,1,&f->image) != ANX_EBUSY) {ret=-7807;goto out;}
	ret=denied078(f,0,f->view[0].epoch,ANX_EBUSY);
	if (ret == ANX_OK) ret=generate078(f,1);
	if (ret == ANX_OK) ret=anx_continuation_pause(f->view[1].id,f->view[1].epoch,ANX_SUSPEND_TOOL_WAIT,&f->view[1]);
	if (ret == ANX_OK) ret=denied078(f,1,f->view[1].epoch,ANX_EBUSY);
	if (ret == ANX_OK) ret=anx_continuation_resume(f->view[1].id,f->view[1].epoch,&f->view[1]);
	if (ret == ANX_OK) ret=anx_continuation_pause(f->view[1].id,f->view[1].epoch,ANX_SUSPEND_MODEL_WAIT,&f->view[1]);
	if (ret == ANX_OK) ret=anx_continuation_idle_reclaim(f->view[1].id,f->view[1].epoch,&f->idle[1],&f->view[1]);
	if (ret != ANX_OK) goto out;
	anx_lease_avail_mem(ANX_MEM_L1,&held);
	if (held != available-8192 || f->view[1].physical_pages ||
	    anx_phase_get(&owner[0]->cid,&current) != ANX_OK || anx_memcmp(&current,&phases[0],sizeof(current))) {ret=-7808;goto out;}
	/* A failed restore keeps the wait state and its existing reservation. */
	anx_memset(&f->output,0x55,sizeof(f->output));f->sentinel=f->output;
	model->version++;
	ret=anx_continuation_resume(f->view[0].id,f->view[0].epoch,&f->output);
	model->version--;
	if (ret != ANX_EBUSY || anx_memcmp(&f->output,&f->sentinel,sizeof(f->output)) ||
	    anx_continuation_cache_stats(f->view[0].id,&stats) != ANX_OK || stats.physical_pages) {ret=-7809;goto out;}
	for (uint32_t i=0;i<2;i++) {
		ret=anx_continuation_resume(f->view[i].id,f->view[i].epoch,&f->view[i]);
		if (ret == ANX_OK) ret=generate078(f,i);
		if (ret != ANX_OK) goto out;
		if (f->view[i].physical_pages != 1 || f->view[i].cache_generation != 2 ||
		    anx_phase_get(&owner[i]->cid,&current) != ANX_OK || anx_memcmp(&current,&phases[i],sizeof(current))) {ret=-7810;goto out;}
	}
	/* A changed actual phase invalidates a previously prepared idle request. */
	ret=anx_phase_resize(&owner[1]->cid,phases[1].epoch,4096,0,&current);
	if (ret == ANX_OK) ret=denied078(f,1,f->view[1].epoch,ANX_EBUSY);
	if (ret != ANX_OK) goto out;
	for (uint32_t i=0;i<2;i++) {
		events[event_count++]=f->view[i].head;
		for (uint32_t j=1;j<f->view[i].events;j++) {
			struct anx_continuation_event event;
			ret=anx_continuation_event_get(f->view[i].id,j,&event);
			if (ret != ANX_OK) goto out;
			events[event_count++]=event.previous;
		}
	}
	journals_saved=true;
	ret=anx_external_register_handler("anxresearch078",active078,f);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint,"anxresearch078://inspect",sizeof(f->call.endpoint));
	foreign->execution.allow_side_effects=true;foreign->ext_call=&f->call;f->foreign=true;
	ret=anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	owner[0]->ext_call=&f->call;f->foreign=false;
	ret=anx_cell_run(owner[0]);
	if (ret == ANX_OK) kprintf("day078 sibling_cache_pages=2,1,0,2 held_reservation=8192 outputs=AAAA,AAAA phase_and_lineage=checked\n");
out:
	anx_external_unregister_handler("anxresearch078");
	for (uint32_t i=0;i<2;i++) {
		/* Event objects are separate from the reclaimed acceleration pages. */
		if (f->view[i].id) {
			if (!journals_saved && !anx_uuid_is_nil(&f->view[i].head)) events[event_count++]=f->view[i].head;
			for (uint32_t j=1;!journals_saved && j<f->view[i].events;j++) {
				struct anx_continuation_event event;
				if (anx_continuation_event_get(f->view[i].id,j,&event)==ANX_OK) events[event_count++]=event.previous;
			}
			anx_continuation_destroy(f->view[i].id);
		}
		if (attached[i]) {anx_phase_get(&owner[i]->cid,&current);anx_phase_finish(&owner[i]->cid,current.epoch);anx_phase_detach(&owner[i]->cid);}
		if (owner[i]) anx_cell_destroy(owner[i]);
	}
	for (uint32_t i=0;i<event_count;i++) anx_so_delete(&events[i],false);
	if (model) {anx_oid_t oid=model->oid;anx_objstore_release(model);anx_so_delete(&oid,false);}
	if (foreign) anx_cell_destroy(foreign);
	if (parent) anx_cell_destroy(parent);
	if (ret != ANX_OK) kprintf("day078 failure=%d\n",ret);
	anx_free(f);return ret;
}
#endif
