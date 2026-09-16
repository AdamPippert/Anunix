#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workload.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
struct fixture077 {
	struct anx_workload_contract contract;
	struct anx_workload_view view, output, saved;
	struct anx_model_use_view uses[7];
	struct anx_anxml_response response, response_saved;
	struct anx_external_call call;
	bool foreign;
};
static int unchanged077(struct fixture077 *f)
{
	struct anx_workload_view current;
	return anx_workload_get(f->view.id, &current) == ANX_OK && !anx_memcmp(&current, &f->view, sizeof(current)) ? ANX_OK : -7702;
}
static int select_denied077(struct fixture077 *f, uint32_t candidate, uint32_t limit, int expected)
{
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->saved = f->output;
	int ret = anx_workload_select(f->view.id, f->view.epoch, candidate, limit, &f->output);
	if (ret != expected || anx_memcmp(&f->output, &f->saved, sizeof(f->output))) return -7703;
	return unchanged077(f);
}
static int execute_denied077(struct fixture077 *f, uint64_t epoch, uint64_t use, int expected)
{
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->saved = f->output;
	anx_memset(&f->response, 0x55, sizeof(f->response)); f->response_saved = f->response;
	int ret = anx_workload_execute(f->view.id, epoch, use, &f->response, &f->output);
	return ret == expected && !anx_memcmp(&f->output, &f->saved, sizeof(f->output)) &&
		!anx_memcmp(&f->response, &f->response_saved, sizeof(f->response)) ? ANX_OK : -7704;
}
static int active077(struct anx_external_call *call, void *arg)
{
	(void)call; struct fixture077 *f = arg;
	if (anx_workload_create(&f->view.owner, &f->contract, &f->output) != ANX_EPERM ||
	    anx_workload_add(f->view.id, f->uses[0].id, &f->output) != ANX_EPERM ||
	    anx_workload_select(f->view.id, f->view.epoch, 0, 8, &f->output) != ANX_EPERM ||
	    anx_workload_destroy(f->view.id) != ANX_EPERM) return -7705;
	if (f->foreign) {
		if (anx_workload_get(f->view.id, &f->output) != ANX_EPERM) return -7706;
		return execute_denied077(f, f->view.epoch, f->uses[6].id, ANX_EPERM);
	}
	int ret = anx_workload_execute(f->view.id, f->view.epoch, f->uses[6].id, &f->response, &f->view);
	return ret == ANX_OK && f->response.tokens_generated == 4 && f->response.output_len == 4 &&
		!anx_memcmp(f->response.output,"AAAA",4) && f->view.executions == 2 && f->view.charged_tokens == 12 ? ANX_OK : -7707;
}
int anx_research_day077(void)
{
	struct fixture077 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *images[2] = {0}, *prompt = NULL;
	struct anx_adapter_image image = {.format=1,.count=2,.deltas={{'~','A',4096},{'A','A',4096}}};
	struct anx_so_create_params params = {.object_type=ANX_OBJ_STRUCTURED_DATA,.schema_uri=ANX_MODEL_USE_SCHEMA,
		.schema_version="1",.payload=&image,.payload_size=sizeof(image)};
	struct anx_model_use_spec use = {0};
	const uint32_t tokens[7] = {8,4,2,4,16,8,4};
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	f->contract = (struct anx_workload_contract){.minimum_output=4,.maximum_output=8,.maximum_tokens=8,.prefix_size=4,.prefix="AAAA"};
	anx_strlcpy(intent.name,"research-day-077",sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL,&intent,&owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL,&intent,&foreign);
	if (ret != ANX_OK) goto out;
	ret = -7701;
	if (anx_workload_create(&owner->cid,&f->contract,&f->view) != ANX_OK) goto out;
	for (uint32_t i = 0; i < 2; i++) {
		image.deltas[0].next = image.deltas[1].previous = image.deltas[1].next = (uint8_t)('A' + i);
		ret = anx_so_create(&params,&images[i]);
		if (ret == ANX_OK) ret = anx_so_seal(&images[i]->oid);
		if (ret != ANX_OK) goto out;
	}
	params = (struct anx_so_create_params){.object_type=ANX_OBJ_BYTE_DATA,.payload="~",.payload_size=1};
	ret = anx_so_create(&params,&prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	use.prompt = prompt->oid;
	for (uint32_t i = 0; i < 7; i++) {
		use.image = images[i == 3 ? 1 : 0]->oid; use.maximum_tokens = tokens[i];
		ret = anx_model_use_prepare(&owner->cid,&use,&f->uses[i]);
		if (ret == ANX_OK && i < 5) ret = anx_model_use_execute(f->uses[i].id,f->uses[i].epoch,&f->response,&f->uses[i]);
		if (ret != ANX_OK) goto out;
	}
	/* The original quality contract is immutable after creation. */
	anx_memcpy(f->contract.prefix,"BBBB",4); f->contract.maximum_tokens = 32;
	for (uint32_t i = 0; i < 2; i++) {
		ret = anx_workload_add(f->view.id,f->uses[i].id,&f->view);
		if (ret != ANX_OK) goto out;
	}
	for (uint32_t i = 2; i < 5; i++) {
		if (anx_workload_add(f->view.id,f->uses[i].id,&f->output) != ANX_EAUDIT || unchanged077(f) != ANX_OK) { ret = -7708; goto out; }
	}
	if (anx_workload_add(f->view.id,f->uses[0].id,&f->output) != ANX_EEXIST || unchanged077(f) != ANX_OK) { ret = -7709; goto out; }
	ret = anx_workload_select(f->view.id,f->view.epoch,0,8,&f->view);
	if (ret == ANX_OK) ret = select_denied077(f,0,4,ANX_ENOMEM);
	if (ret == ANX_OK) ret = select_denied077(f,1,16,ANX_EPERM);
	if (ret == ANX_OK) ret = select_denied077(f,7,8,ANX_EINVAL);
	if (ret == ANX_OK) ret = execute_denied077(f,f->view.epoch - 1,f->uses[5].id,ANX_EBUSY);
	if (ret == ANX_OK) ret = execute_denied077(f,f->view.epoch,f->uses[6].id,ANX_EPERM);
	if (ret != ANX_OK) goto out;
	ret = anx_workload_execute(f->view.id,f->view.epoch,f->uses[5].id,&f->response,&f->view);
	if (ret != ANX_OK || f->response.tokens_generated != 8 || f->response.output_len != 8 ||
	    anx_memcmp(f->response.output,"AAAAAAAA",8) || f->view.executions != 1 || f->view.charged_tokens != 8) { ret = -7710; goto out; }
	((uint8_t *)prompt->payload)[0] ^= 1; ret = select_denied077(f,1,4,ANX_EBUSY); ((uint8_t *)prompt->payload)[0] ^= 1;
	if (ret != ANX_OK) goto out;
	ret = anx_workload_select(f->view.id,f->view.epoch,1,4,&f->view);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch077",active077,f);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint,"anxresearch077://execute",sizeof(f->call.endpoint));
	owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	owner->ext_call = foreign->ext_call = &f->call;
	f->foreign = true; ret = anx_cell_run(foreign);
	if (ret == ANX_OK) { f->foreign = false; ret = anx_cell_run(owner); }
	if (ret == ANX_OK) kprintf("day077 runtime_tokens=8,4 quality_prefix=AAAA rejected_candidates=3 charged_tokens=12\n");
out:
	anx_external_unregister_handler("anxresearch077");
	if (f->view.id && anx_workload_destroy(f->view.id) != ANX_OK && ret == ANX_OK) ret = -7711;
	for (uint32_t i = 0; i < 7; i++) if (f->uses[i].id) anx_model_use_destroy(f->uses[i].id);
	if (prompt) { anx_so_delete(&prompt->oid,false); anx_objstore_release(prompt); }
	for (uint32_t i = 0; i < 2; i++) if (images[i]) { anx_so_delete(&images[i]->oid,false); anx_objstore_release(images[i]); }
	if (foreign && anx_cell_destroy(foreign) != ANX_OK && ret == ANX_OK) ret = -7712;
	if (owner && anx_cell_destroy(owner) != ANX_OK && ret == ANX_OK) ret = -7712;
	if (ret != ANX_OK) kprintf("day077 native failure rc=%d\n",ret);
	anx_free(f); return ret;
}
#endif
