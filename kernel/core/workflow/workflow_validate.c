/* Validate a template before it becomes a reusable workflow artifact. */
#include <anx/workflow_library.h>

static bool terminated(const char *text, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++)
		if (text[i] == '\0')
			return true;
	return false;
}

#define STRING_OK(field) terminated((field), sizeof(field))

static bool node_strings_valid(const struct anx_wf_node *node)
{
	if (!STRING_OK(node->label))
		return false;
	switch (node->kind) {
	case ANX_WF_NODE_TRIGGER:
		return STRING_OK(node->params.trigger.schedule);
	case ANX_WF_NODE_CELL_CALL:
		return STRING_OK(node->params.cell_call.intent);
	case ANX_WF_NODE_MODEL_CALL:
		return STRING_OK(node->params.model_call.model_id) &&
		       STRING_OK(node->params.model_call.prompt_template);
	case ANX_WF_NODE_AGENT_CALL:
		return STRING_OK(node->params.agent_call.goal);
	case ANX_WF_NODE_RETRIEVAL:
		return STRING_OK(node->params.retrieval.query_template);
	case ANX_WF_NODE_CONDITION:
		return STRING_OK(node->params.condition.expr);
	case ANX_WF_NODE_TRANSFORM:
		return STRING_OK(node->params.transform.op) &&
		       STRING_OK(node->params.transform.fn_expr);
	case ANX_WF_NODE_OUTPUT:
		return STRING_OK(node->params.output.dest_name);
	default:
		return true;
	}
}

int anx_wf_template_validate(const struct anx_wf_template *tmpl)
{
	uint16_t indegree[ANX_WF_MAX_NODES] = {0};
	bool visited[ANX_WF_MAX_NODES] = {false};
	uint32_t i, j, count;

	if (!tmpl || !tmpl->node_count ||
	    tmpl->node_count > ANX_WF_MAX_NODES ||
	    tmpl->edge_count > ANX_WF_MAX_EDGES ||
	    tmpl->tag_count > ANX_WF_LIB_TAGS)
		return ANX_EINVAL;
	if (!tmpl->uri[0] || !STRING_OK(tmpl->uri) ||
	    !STRING_OK(tmpl->display_name) || !STRING_OK(tmpl->description))
		return ANX_EINVAL;
	for (i = 0; i < tmpl->tag_count; i++)
		if (!STRING_OK(tmpl->tags[i]))
			return ANX_EINVAL;
	for (i = 0; i < tmpl->node_count; i++) {
		const struct anx_wf_node *node = &tmpl->nodes[i];

		/* Instantiation assigns consecutive IDs in array order. */
		if (node->id != i + 1 ||
		    (unsigned)node->kind >= ANX_WF_NODE_KIND_COUNT ||
		    node->port_count > ANX_WF_MAX_PORTS || !node_strings_valid(node))
			return ANX_EINVAL;
		for (j = 0; j < node->port_count; j++) {
			const struct anx_wf_port *port = &node->ports[j];

			if (!STRING_OK(port->name) ||
			    (port->dir != ANX_WF_PORT_IN && port->dir != ANX_WF_PORT_OUT) ||
			    port->type_tag > 3)
				return ANX_EINVAL;
		}
	}
	for (i = 0; i < tmpl->edge_count; i++) {
		const struct anx_wf_edge *edge = &tmpl->edges[i];
		const struct anx_wf_node *from, *to;
		const struct anx_wf_port *output, *input;

		if (!edge->from_node || !edge->to_node ||
		    edge->from_node > tmpl->node_count ||
		    edge->to_node > tmpl->node_count || edge->from_node == edge->to_node)
			return ANX_EINVAL;
		from = &tmpl->nodes[edge->from_node - 1];
		to = &tmpl->nodes[edge->to_node - 1];
		if (edge->from_port >= from->port_count || edge->to_port >= to->port_count)
			return ANX_EINVAL;
		output = &from->ports[edge->from_port];
		input = &to->ports[edge->to_port];
		if (output->dir != ANX_WF_PORT_OUT || input->dir != ANX_WF_PORT_IN ||
		    (output->type_tag && input->type_tag && output->type_tag != input->type_tag))
			return ANX_EINVAL;
		/* The executor resolves one producer for each input port. */
		for (j = 0; j < i; j++)
			if (tmpl->edges[j].to_node == edge->to_node &&
			    tmpl->edges[j].to_port == edge->to_port)
				return ANX_EINVAL;
		indegree[edge->to_node - 1]++;
	}
	/* Kahn's algorithm rejects cycles without allocating or running nodes. */
	for (count = 0; count < tmpl->node_count; count++) {
		for (i = 0; i < tmpl->node_count; i++)
			if (!visited[i] && indegree[i] == 0)
				break;
		if (i == tmpl->node_count)
			return ANX_EINVAL;
		visited[i] = true;
		for (j = 0; j < tmpl->edge_count; j++)
			if (tmpl->edges[j].from_node == i + 1)
				indegree[tmpl->edges[j].to_node - 1]--;
	}
	return ANX_OK;
}
