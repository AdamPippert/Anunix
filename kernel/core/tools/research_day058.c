/* The specialized workflow inference path must honor the active Cell's token cap. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/anxml.h>
#include <anx/adapter.h>
#include <anx/workflow.h>
#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct context058 {
	anx_oid_t workflow, output;
	struct anx_cell *parent, *child, *target;
	struct anx_state_object *prompt;
	struct anx_adapter_view adapter;
	struct anx_adapter_cache_key key;
	struct anx_anxml_request request;
	struct anx_anxml_response response, sentinel;
	uint32_t mode;
	bool in_child;
};
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
static int direct058(struct context058 *c, uint32_t requested, uint32_t expected)
{
	c->request.max_tokens = requested;
	int ret = anx_anxml_generate(&c->request, &c->response);
	if (ret != ANX_OK) return ret;
	if (c->response.tokens_generated != expected || c->response.output_len != expected) return -5803;
	for (uint32_t i = 0; i < expected; i++) if (c->response.output[i] != 'A') return -5803;
	return ANX_OK;
}
static int dispatch058(struct anx_external_call *call, void *context)
{
	struct context058 *c = context;
	(void)call;
	if (c->mode == 1) {
		int ret = direct058(c, 0, ANX_ANXML_DEFAULT_MAX);
		return ret == ANX_OK ? direct058(c, ~(uint32_t)0, ANX_ANXML_OUTPUT_MAX - 1) : ret;
	}
	if (c->mode == 2) return direct058(c, 100, 1);
	if (c->mode == 3) { c->parent->cognitive.max_tokens = 0; return ANX_OK; }
	if (c->mode == 4) {
		if (anx_cell_cancel(c->parent) != ANX_OK) return -5812;
		c->response = c->sentinel;
		if (anx_anxml_generate(&c->request, &c->response) != ANX_ECANCELED ||
		    anx_memcmp(&c->response, &c->sentinel, sizeof(c->response))) return -5812;
		return ANX_OK;
	}
	if (c->in_child) {
		uint32_t original = c->parent->cognitive.max_tokens;
		c->parent->cognitive.max_tokens = 0;
		c->response = c->sentinel;
		int denied = anx_anxml_generate(&c->request, &c->response);
		c->parent->cognitive.max_tokens = original;
		if (denied != ANX_EPERM || anx_memcmp(&c->response, &c->sentinel, sizeof(c->response))) return -5809;
		return direct058(c, 16, 2);
	}
	int ret = anx_wf_run(&c->workflow, NULL);
	if (ret != ANX_OK) return ret;
	struct anx_wf_object *wf = anx_wf_object_get(&c->workflow);
	if (wf->run_state != ANX_WF_RUN_COMPLETED || wf->output_count != 1) return -5802;
	c->output = wf->output_oids[0];
	struct anx_state_object *out = anx_objstore_lookup(&c->output);
	ret = out && out->payload_size == 4 ? ANX_OK : -5801;
	if (ret == ANX_OK && (out->parent_count != 1 ||
	    anx_uuid_compare(&out->parent_oids[0], &c->prompt->oid) ||
	    anx_uuid_compare(&out->creator_cell, &c->parent->cid) || out->sensitivity != c->prompt->sensitivity)) ret = -5804;
	anx_objstore_release(out);
	if (ret != ANX_OK) return ret;
	ret = direct058(c, 0, 4);
	if (ret == ANX_OK) ret = direct058(c, 2, 2);
	if (ret == ANX_OK) ret = direct058(c, ~(uint32_t)0, 4);
	if (ret != ANX_OK) return ret;
	c->request.max_tokens = 16;
	ret = anx_adapter_generate(&c->adapter.id, c->adapter.generation, &c->request, &c->response, &c->key);
	if (ret != ANX_OK || c->response.tokens_generated != 4 ||
	    anx_adapter_cache_check(&c->adapter.id, &c->request, &c->key) != ANX_OK) return -5805;
	/* Public field drift cannot raise an active ceiling. */
	if (anx_cell_set_cognitive_envelope(c->parent, 0, 0) != ANX_EPERM ||
	    anx_cell_set_cognitive_envelope(c->target, 100, 0) != ANX_EPERM || c->target->cognitive.max_tokens) return -5806;
	uint32_t original = c->parent->cognitive.max_tokens;
	c->parent->cognitive.max_tokens = 0;
	c->response = c->sentinel;
	int denied = anx_anxml_generate(&c->request, &c->response);
	c->parent->cognitive.max_tokens = original;
	if (denied != ANX_EPERM || anx_memcmp(&c->response, &c->sentinel, sizeof(c->response))) return -5806;
	/* Prompt reads retain the active caller's access policy. */
	c->prompt->access_policy.rule_count = 1;
	c->prompt->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	c->prompt->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	anx_oid_t denied_output = ANX_UUID_NIL;
	denied = anx_anxml_cell_dispatch("anxml-generate", &c->prompt->oid, 1, &denied_output);
	c->prompt->access_policy.rule_count = 0;
	if (denied != ANX_EPERM || !anx_uuid_is_nil(&denied_output)) return -5807;
	if (anx_anxml_cell_dispatch("anxml-generate", NULL, 1, &denied_output) != ANX_EINVAL ||
	    anx_anxml_cell_dispatch("anxml-generate", &c->prompt->oid, 2, &denied_output) != ANX_EINVAL) return -5808;
	c->in_child = true;
	ret = anx_cell_run(c->child);
	c->in_child = false;
	if (ret != ANX_OK) return ret;
	return direct058(c, 16, 4);
}
int anx_research_day058(void)
{
	struct context058 *c = anx_zalloc(sizeof(*c));
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_so_create_params p = { .object_type = ANX_OBJ_BYTE_DATA,
		.payload = "AAAAAAAAAAAAAAAA", .payload_size = 16, .sensitivity = ANX_SENSITIVITY_INTERNAL };
	int ret = ANX_ENOMEM;
	if (!c) return ret;
	anx_memset(&c->sentinel, 0x55, sizeof(c->sentinel));
	anx_memcpy(c->request.prompt, p.payload, 16); c->request.prompt_len = 16; c->request.max_tokens = 16;
	ret = anx_so_create(&p, &c->prompt);
	if (ret == ANX_OK) ret = workflow058(&c->prompt->oid, &c->workflow);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch058", dispatch058, c);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call)); ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch058://generate", sizeof(call->endpoint));
	anx_strlcpy(intent.name, "research-day-058", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &c->target);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &c->parent);
	if (ret == ANX_OK) ret = anx_cell_set_cognitive_envelope(c->parent, 4, 0);
	if (ret != ANX_OK) goto out;
	c->parent->execution.allow_side_effects = true; c->parent->execution.allow_recursive_cells = true;
	c->parent->execution.max_recursion_depth = c->parent->constraints.max_recursion_depth = 2;
	c->parent->ext_call = call;
	ret = anx_cell_derive_child(c->parent, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &c->child);
	if (ret == ANX_OK) ret = anx_cell_set_cognitive_envelope(c->child, 2, 0);
	if (ret != ANX_OK) goto out;
	c->child->ext_call = call;
	struct anx_adapter_image image = { .format = 1, .count = 1, .deltas = {{'A', 'A', 4096}} };
	ret = anx_adapter_create(&c->parent->cid, &image, &c->adapter);
	if (ret == ANX_OK) ret = anx_cell_run(c->parent);
	if (ret != ANX_OK) goto out;
	ret = -5810;
	/* Cache identity includes the admitted cap, not just the caller's larger request. */
	c->request.max_tokens = 16;
	if (anx_adapter_cache_check(&c->adapter.id, &c->request, &c->key) != ANX_EBUSY) goto out;
	c->request.max_tokens = 4;
	if (anx_adapter_cache_check(&c->adapter.id, &c->request, &c->key) != ANX_OK) goto out;
	anx_adapter_destroy(&c->adapter.id); c->adapter.id = ANX_UUID_NIL;
	anx_cell_destroy(c->child); c->child = NULL;
	anx_cell_destroy(c->parent); c->parent = NULL;
	for (c->mode = 1; c->mode <= 4; c->mode++) {
		ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &c->parent);
		if (ret == ANX_OK) ret = anx_cell_set_cognitive_envelope(c->parent, c->mode == 1 ? 0 : c->mode == 2 ? 1 : 4, 0);
		if (ret != ANX_OK) goto out;
		c->parent->execution.allow_side_effects = true; c->parent->ext_call = call;
		int expected = c->mode == 3 ? ANX_EPERM : c->mode == 4 ? ANX_ECANCELED : ANX_OK;
		ret = -5811;
		if (anx_cell_run(c->parent) != expected ||
		    (c->mode == 3 && c->parent->status != ANX_CELL_FAILED)) goto out;
		anx_cell_destroy(c->parent); c->parent = NULL;
	}
	ret = direct058(c, 16, 16);
	uint32_t limit = 123;
	if (ret == ANX_OK && (anx_cell_cognitive_limit(0, &limit) != ANX_EINVAL || limit != 123 ||
	    anx_cell_cognitive_limit(1, NULL) != ANX_EINVAL)) ret = -5813;
out:
	if (!anx_uuid_is_nil(&c->adapter.id)) anx_adapter_destroy(&c->adapter.id);
	if (c->child) anx_cell_destroy(c->child);
	if (c->parent) anx_cell_destroy(c->parent);
	if (c->target) anx_cell_destroy(c->target);
	anx_free(call); anx_external_unregister_handler("anxresearch058");
	if (!anx_uuid_is_nil(&c->workflow)) anx_wf_destroy(&c->workflow);
	if (!anx_uuid_is_nil(&c->output)) anx_so_delete(&c->output, false);
	if (c->prompt) { anx_so_delete(&c->prompt->oid, false); anx_objstore_release(c->prompt); }
	anx_free(c);
	return ret;
}
#endif
