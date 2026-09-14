/* The specialized workflow inference path must honor the active Cell's token cap. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/anxml.h>
#include <anx/workflow.h>
#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct context058 { anx_oid_t workflow, output; };
static int workflow058(const anx_oid_t *prompt, anx_oid_t *out)
{
	struct anx_wf_node node = { .kind = ANX_WF_NODE_STATE_REF, .port_count = 1 };
	uint16_t source, model, sink;
	int ret = anx_wf_create("research-day-058", NULL, out);
	node.params.state_ref.obj_oid = *prompt; node.ports[0].dir = ANX_WF_PORT_OUT;
	if (ret == ANX_OK) ret = anx_wf_node_add(out, &node, &source);
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_CELL_CALL; node.port_count = 2;
	anx_strlcpy(node.params.cell_call.intent, "anxml-generate", sizeof(node.params.cell_call.intent));
	node.ports[0].dir = ANX_WF_PORT_IN; node.ports[1].dir = ANX_WF_PORT_OUT;
	if (ret == ANX_OK) ret = anx_wf_node_add(out, &node, &model);
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_OUTPUT; node.port_count = 1; node.ports[0].dir = ANX_WF_PORT_IN;
	if (ret == ANX_OK) ret = anx_wf_node_add(out, &node, &sink);
	if (ret == ANX_OK) ret = anx_wf_edge_add(out, source, 0, model, 0);
	if (ret == ANX_OK) ret = anx_wf_edge_add(out, model, 1, sink, 0);
	return ret;
}
static int dispatch058(struct anx_external_call *call, void *context)
{
	struct context058 *c = context;
	(void)call;
	int ret = anx_wf_run(&c->workflow, NULL);
	if (ret != ANX_OK) return ret;
	struct anx_wf_object *wf = anx_wf_object_get(&c->workflow);
	if (wf->run_state != ANX_WF_RUN_COMPLETED || wf->output_count != 1) return -5802;
	c->output = wf->output_oids[0];
	struct anx_state_object *out = anx_objstore_lookup(&c->output);
	ret = out && out->payload_size == 4 ? ANX_OK : -5801;
	anx_objstore_release(out);
	return ret;
}
int anx_research_day058(void)
{
	struct context058 c = {0};
	struct anx_state_object *prompt = NULL;
	struct anx_cell *caller = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_so_create_params p = { .object_type = ANX_OBJ_BYTE_DATA,
		.payload = "AAAAAAAAAAAAAAAA", .payload_size = 16 };
	int ret = anx_so_create(&p, &prompt);
	if (ret == ANX_OK) ret = workflow058(&prompt->oid, &c.workflow);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch058", dispatch058, &c);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call)); ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch058://generate", sizeof(call->endpoint));
	anx_strlcpy(intent.name, "research-day-058", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (ret == ANX_OK) ret = anx_cell_set_cognitive_envelope(caller, 4, 0);
	if (ret != ANX_OK) goto out;
	caller->execution.allow_side_effects = true; caller->ext_call = call;
	ret = anx_cell_run(caller);
out:
	if (caller) anx_cell_destroy(caller);
	anx_free(call); anx_external_unregister_handler("anxresearch058");
	if (!anx_uuid_is_nil(&c.workflow)) anx_wf_destroy(&c.workflow);
	if (!anx_uuid_is_nil(&c.output)) anx_so_delete(&c.output, false);
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	return ret;
}
#endif
