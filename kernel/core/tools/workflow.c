/*
 * workflow.c — 'workflow' shell builtin (RFC-0018).
 *
 * Exposes workflow lifecycle, node/edge editing, and status inspection
 * to the ANX shell (ansh).
 */

#include <anx/types.h>
#include <anx/workflow.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Usage                                                               */
/* ------------------------------------------------------------------ */

static void wf_usage(void)
{
	kprintf("usage: workflow <subcommand> [args]\n");
	kprintf("  create   <name> [description]\n");
	kprintf("  list\n");
	kprintf("  run      <name>\n");
	kprintf("  status   <name>\n");
	kprintf("  add-node <wf-name> <kind> <label> [parameter]\n");
	kprintf("  add-edge <wf-name> <from-node-id> <to-node-id>\n");
	kprintf("  show     <name>   — serialize to DSL text\n");
	kprintf("  graph    <name>   — ASCII DAG diagram\n");
	kprintf("  dump     <name>   — raw struct debug dump\n");
	kprintf("  destroy  <name>\n");
	kprintf("\n");
	kprintf("  kinds: trigger state_ref cell_call model_call agent_call\n");
	kprintf("         retrieval condition fan_out fan_in transform\n");
	kprintf("         human_review subflow output\n");
	kprintf("  state_ref parameter: optional <oid-or-path> (read-only)\n");
	kprintf("  cell_call parameter: optional <intent>\n");
}

/* ------------------------------------------------------------------ */
/* String helpers                                                      */
/* ------------------------------------------------------------------ */

static const char *wf_run_state_name(enum anx_wf_run_state s)
{
	switch (s) {
	case ANX_WF_RUN_IDLE:          return "idle";
	case ANX_WF_RUN_RUNNING:       return "running";
	case ANX_WF_RUN_WAITING_HUMAN: return "waiting_human";
	case ANX_WF_RUN_COMPLETED:     return "completed";
	case ANX_WF_RUN_FAILED:        return "failed";
	default:                        return "unknown";
	}
}

static const char *wf_node_kind_name(enum anx_wf_node_kind k)
{
	switch (k) {
	case ANX_WF_NODE_TRIGGER:      return "trigger";
	case ANX_WF_NODE_STATE_REF:    return "state_ref";
	case ANX_WF_NODE_CELL_CALL:    return "cell_call";
	case ANX_WF_NODE_MODEL_CALL:   return "model_call";
	case ANX_WF_NODE_AGENT_CALL:   return "agent_call";
	case ANX_WF_NODE_RETRIEVAL:    return "retrieval";
	case ANX_WF_NODE_CONDITION:    return "condition";
	case ANX_WF_NODE_FAN_OUT:      return "fan_out";
	case ANX_WF_NODE_FAN_IN:       return "fan_in";
	case ANX_WF_NODE_TRANSFORM:    return "transform";
	case ANX_WF_NODE_HUMAN_REVIEW: return "human_review";
	case ANX_WF_NODE_SUBFLOW:      return "subflow";
	case ANX_WF_NODE_OUTPUT:       return "output";
	case ANX_WF_NODE_CAP_PROMOTION: return "cap_promotion";
	default:                        return "unknown";
	}
}

/* Parse a kind string into an enum.  Returns -1 on no match. */
static int wf_parse_kind(const char *s, enum anx_wf_node_kind *out)
{
	if (anx_strcmp(s, "trigger")      == 0) { *out = ANX_WF_NODE_TRIGGER;      return 0; }
	if (anx_strcmp(s, "state_ref")    == 0) { *out = ANX_WF_NODE_STATE_REF;    return 0; }
	if (anx_strcmp(s, "cell_call")    == 0) { *out = ANX_WF_NODE_CELL_CALL;    return 0; }
	if (anx_strcmp(s, "model_call")   == 0) { *out = ANX_WF_NODE_MODEL_CALL;   return 0; }
	if (anx_strcmp(s, "agent_call")   == 0) { *out = ANX_WF_NODE_AGENT_CALL;   return 0; }
	if (anx_strcmp(s, "retrieval")    == 0) { *out = ANX_WF_NODE_RETRIEVAL;    return 0; }
	if (anx_strcmp(s, "condition")    == 0) { *out = ANX_WF_NODE_CONDITION;    return 0; }
	if (anx_strcmp(s, "fan_out")      == 0) { *out = ANX_WF_NODE_FAN_OUT;      return 0; }
	if (anx_strcmp(s, "fan_in")       == 0) { *out = ANX_WF_NODE_FAN_IN;       return 0; }
	if (anx_strcmp(s, "transform")    == 0) { *out = ANX_WF_NODE_TRANSFORM;    return 0; }
	if (anx_strcmp(s, "human_review") == 0) { *out = ANX_WF_NODE_HUMAN_REVIEW; return 0; }
	if (anx_strcmp(s, "subflow")      == 0) { *out = ANX_WF_NODE_SUBFLOW;      return 0; }
	if (anx_strcmp(s, "output")       == 0) { *out = ANX_WF_NODE_OUTPUT;       return 0; }
	if (anx_strcmp(s, "cap_promotion") == 0) { *out = ANX_WF_NODE_CAP_PROMOTION; return 0; }
	return -1;
}

static void wf_set_port(struct anx_wf_node *node, uint8_t index,
			const char *name, enum anx_wf_port_dir dir,
			uint8_t type_tag, bool required)
{
	anx_strlcpy(node->ports[index].name, name,
		    sizeof(node->ports[index].name));
	node->ports[index].dir = dir;
	node->ports[index].type_tag = type_tag;
	node->ports[index].required = required;
}

/* Give shell-created nodes the same usable port shapes as library templates. */
static void wf_set_default_ports(struct anx_wf_node *node)
{
	switch (node->kind) {
	case ANX_WF_NODE_TRIGGER:
		wf_set_port(node, 0, "out", ANX_WF_PORT_OUT, 0, false);
		node->port_count = 1;
		break;
	case ANX_WF_NODE_STATE_REF:
		wf_set_port(node, 0, "out", ANX_WF_PORT_OUT, 1, false);
		node->port_count = 1;
		break;
	case ANX_WF_NODE_CELL_CALL:
	case ANX_WF_NODE_MODEL_CALL:
	case ANX_WF_NODE_RETRIEVAL:
	case ANX_WF_NODE_TRANSFORM:
	case ANX_WF_NODE_SUBFLOW:
		wf_set_port(node, 0, "in", ANX_WF_PORT_IN, 0, true);
		wf_set_port(node, 1, "out", ANX_WF_PORT_OUT, 0, false);
		node->port_count = 2;
		break;
	case ANX_WF_NODE_AGENT_CALL:
		wf_set_port(node, 0, "out", ANX_WF_PORT_OUT, 0, false);
		node->port_count = 1;
		break;
	case ANX_WF_NODE_CONDITION:
		wf_set_port(node, 0, "in", ANX_WF_PORT_IN, 0, true);
		wf_set_port(node, 1, "true", ANX_WF_PORT_OUT, 0, false);
		wf_set_port(node, 2, "false", ANX_WF_PORT_OUT, 0, false);
		node->port_count = 3;
		break;
	case ANX_WF_NODE_FAN_OUT:
		wf_set_port(node, 0, "in", ANX_WF_PORT_IN, 0, true);
		wf_set_port(node, 1, "a", ANX_WF_PORT_OUT, 0, false);
		wf_set_port(node, 2, "b", ANX_WF_PORT_OUT, 0, false);
		node->port_count = 3;
		break;
	case ANX_WF_NODE_FAN_IN:
		wf_set_port(node, 0, "a", ANX_WF_PORT_IN, 0, true);
		wf_set_port(node, 1, "b", ANX_WF_PORT_IN, 0, true);
		wf_set_port(node, 2, "out", ANX_WF_PORT_OUT, 0, false);
		node->port_count = 3;
		break;
	case ANX_WF_NODE_HUMAN_REVIEW:
		wf_set_port(node, 0, "in", ANX_WF_PORT_IN, 0, true);
		wf_set_port(node, 1, "out", ANX_WF_PORT_OUT, 0, false);
		node->port_count = 2;
		break;
	case ANX_WF_NODE_OUTPUT:
		wf_set_port(node, 0, "in", ANX_WF_PORT_IN, 0, true);
		node->port_count = 1;
		break;
	case ANX_WF_NODE_CAP_PROMOTION:
		wf_set_port(node, 0, "evidence", ANX_WF_PORT_IN, 1, true);
		wf_set_port(node, 1, "decision", ANX_WF_PORT_OUT, 1, false);
		node->port_count = 2;
		break;
	default:
		break;
	}
}

static int wf_first_port(const struct anx_wf_object *wf, uint16_t node_id,
			 enum anx_wf_port_dir dir, uint8_t *port_out)
{
	uint32_t i;
	uint8_t p;

	if (!wf || !port_out)
		return ANX_EINVAL;
	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		const struct anx_wf_node *node = &wf->nodes[i];

		if (node->id != node_id)
			continue;
		/* Preserve pre-port shell workflows created by earlier builds. */
		if (node->port_count == 0) {
			*port_out = 0;
			return ANX_OK;
		}
		for (p = 0; p < node->port_count; p++)
			if (node->ports[p].dir == dir) {
				*port_out = p;
				return ANX_OK;
			}
		return ANX_EINVAL;
	}
	return ANX_ENOENT;
}

/* ------------------------------------------------------------------ */
/* Workflow lookup by name                                             */
/* ------------------------------------------------------------------ */

/* Scan the workflow registry for a workflow whose name matches. */
static int wf_find_by_name(const char *name, anx_oid_t *oid_out)
{
	anx_oid_t list[ANX_WF_MAX_WFS];
	uint32_t count, i;
	int ret;

	ret = anx_wf_list(list, ANX_WF_MAX_WFS, &count);
	if (ret != ANX_OK)
		return ret;

	for (i = 0; i < count; i++) {
		struct anx_wf_object *wf = anx_wf_object_get(&list[i]);

		if (wf && anx_strcmp(wf->name, name) == 0) {
			*oid_out = list[i];
			return ANX_OK;
		}
	}
	return ANX_ENOENT;
}

/* ------------------------------------------------------------------ */
/* Subcommand implementations                                          */
/* ------------------------------------------------------------------ */

static int cmd_workflow_create(int argc, char **argv)
{
	anx_oid_t oid;
	const char *desc = "";
	int ret;

	if (argc < 1) {
		kprintf("workflow create: missing name\n");
		return ANX_EINVAL;
	}
	if (argc >= 2)
		desc = argv[1];

	ret = anx_wf_create(argv[0], desc, &oid);
	if (ret != ANX_OK) {
		kprintf("workflow create: failed (%d)\n", ret);
		return ret;
	}

	kprintf("created workflow '%s'\n", argv[0]);
	return ANX_OK;
}

static int cmd_workflow_list(void)
{
	anx_oid_t list[ANX_WF_MAX_WFS];
	uint32_t count, i;

	if (anx_wf_list(list, ANX_WF_MAX_WFS, &count) != ANX_OK) {
		kprintf("workflow list: error\n");
		return ANX_EINVAL;
	}

	if (count == 0) {
		kprintf("no workflows defined\n");
		return ANX_OK;
	}

	kprintf("%-24s  %-14s  %5s  %5s\n",
		"NAME", "STATE", "NODES", "EDGES");
	kprintf("%-24s  %-14s  %5s  %5s\n",
		"------------------------",
		"--------------", "-----", "-----");

	for (i = 0; i < count; i++) {
		struct anx_wf_object *wf = anx_wf_object_get(&list[i]);

		if (!wf)
			continue;

		kprintf("%-24s  %-14s  %5u  %5u\n",
			wf->name,
			wf_run_state_name(wf->run_state),
			(uint32_t)wf->node_count,
			(uint32_t)wf->edge_count);
	}
	return ANX_OK;
}

static int cmd_workflow_run(int argc, char **argv)
{
	anx_oid_t oid;
	anx_cid_t run_cid;
	int ret;

	if (argc < 1) {
		kprintf("workflow run: missing name\n");
		return ANX_EINVAL;
	}

	ret = wf_find_by_name(argv[0], &oid);
	if (ret != ANX_OK) {
		kprintf("workflow run: '%s' not found\n", argv[0]);
		return ANX_ENOENT;
	}

	ret = anx_wf_run(&oid, &run_cid);
	if (ret != ANX_OK) {
		kprintf("workflow run '%s': error %d\n", argv[0], ret);
		return ret;
	}

	kprintf("started workflow '%s'\n", argv[0]);
	return ANX_OK;
}

static int cmd_workflow_status(int argc, char **argv)
{
	anx_oid_t oid;
	struct anx_wf_object *wf;
	int ret;

	if (argc < 1) {
		kprintf("workflow status: missing name\n");
		return ANX_EINVAL;
	}

	ret = wf_find_by_name(argv[0], &oid);
	if (ret != ANX_OK) {
		kprintf("workflow status: '%s' not found\n", argv[0]);
		return ANX_ENOENT;
	}

	wf = anx_wf_object_get(&oid);
	if (!wf)
		return ANX_ENOENT;

	kprintf("name:        %s\n",  wf->name);
	kprintf("run_state:   %s\n",  wf_run_state_name(wf->run_state));
	kprintf("last_status: %d\n",  wf->last_status);
	kprintf("node_count:  %u\n",  (uint32_t)wf->node_count);
	kprintf("edge_count:  %u\n",  (uint32_t)wf->edge_count);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* show / graph — serialization                                        */
/* ------------------------------------------------------------------ */

#define WF_SERIAL_BUF 8192

static int cmd_workflow_show(int argc, char **argv)
{
	anx_oid_t oid;
	static char buf[WF_SERIAL_BUF];
	int ret;

	if (argc < 1) {
		kprintf("workflow show: missing name\n");
		return ANX_EINVAL;
	}

	ret = wf_find_by_name(argv[0], &oid);
	if (ret != ANX_OK) {
		kprintf("workflow show: '%s' not found\n", argv[0]);
		return ANX_ENOENT;
	}

	ret = anx_wf_serialize(&oid, buf, sizeof(buf));
	if (ret != ANX_OK) {
		kprintf("workflow show: serialize failed (%d)\n", ret);
		return ret;
	}

	kprintf("%s", buf);
	return ANX_OK;
}

static int cmd_workflow_graph(int argc, char **argv)
{
	anx_oid_t oid;
	static char buf[WF_SERIAL_BUF];
	int ret;

	if (argc < 1) {
		kprintf("workflow graph: missing name\n");
		return ANX_EINVAL;
	}

	ret = wf_find_by_name(argv[0], &oid);
	if (ret != ANX_OK) {
		kprintf("workflow graph: '%s' not found\n", argv[0]);
		return ANX_ENOENT;
	}

	ret = anx_wf_render_ascii(&oid, buf, sizeof(buf));
	if (ret != ANX_OK) {
		kprintf("workflow graph: render failed (%d)\n", ret);
		return ret;
	}

	kprintf("%s", buf);
	return ANX_OK;
}

static int cmd_workflow_dump(int argc, char **argv)
{
	anx_oid_t oid;
	struct anx_wf_object *wf;
	uint32_t i;
	int ret;

	if (argc < 1) {
		kprintf("workflow dump: missing name\n");
		return ANX_EINVAL;
	}

	ret = wf_find_by_name(argv[0], &oid);
	if (ret != ANX_OK) {
		kprintf("workflow dump: '%s' not found\n", argv[0]);
		return ANX_ENOENT;
	}

	wf = anx_wf_object_get(&oid);
	if (!wf)
		return ANX_ENOENT;

	kprintf("workflow: %s\n", wf->name);
	if (wf->description[0])
		kprintf("desc:     %s\n", wf->description);
	kprintf("state:    %s\n", wf_run_state_name(wf->run_state));
	kprintf("\n");

	/* Nodes */
	kprintf("nodes (%u):\n", (uint32_t)wf->node_count);
	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		const struct anx_wf_node *n = &wf->nodes[i];

		if (n->id == 0)
			continue;

		kprintf("  [%u] kind=%-12s label='%s'  pos=(%d,%d)  size=%ux%u  ports=%u\n",
			(uint32_t)n->id,
			wf_node_kind_name(n->kind),
			n->label,
			n->canvas_x, n->canvas_y,
			n->canvas_w, n->canvas_h,
			(uint32_t)n->port_count);
	}

	kprintf("\n");

	/* Edges */
	kprintf("edges (%u):\n", (uint32_t)wf->edge_count);
	for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
		const struct anx_wf_edge *e = &wf->edges[i];

		if (e->from_node == 0 && e->to_node == 0)
			continue;

		kprintf("  node %u port %u  ->  node %u port %u\n",
			(uint32_t)e->from_node, (uint32_t)e->from_port,
			(uint32_t)e->to_node,   (uint32_t)e->to_port);
	}

	return ANX_OK;
}

static int cmd_workflow_add_node(int argc, char **argv)
{
	anx_oid_t oid;
	struct anx_wf_node spec;
	struct anx_wf_object *wf;
	enum anx_wf_node_kind kind;
	uint16_t new_id;
	int ret;

	if (argc < 3) {
		kprintf("workflow add-node: <wf-name> <kind> <label> [parameter]\n");
		return ANX_EINVAL;
	}

	ret = wf_find_by_name(argv[0], &oid);
	if (ret != ANX_OK) {
		kprintf("workflow add-node: '%s' not found\n", argv[0]);
		return ANX_ENOENT;
	}

	if (wf_parse_kind(argv[1], &kind) != 0) {
		kprintf("workflow add-node: unknown kind '%s'\n", argv[1]);
		return ANX_EINVAL;
	}

	anx_memset(&spec, 0, sizeof(spec));
	spec.kind = kind;
	anx_strlcpy(spec.label, argv[2], ANX_WF_LABEL_MAX);
	wf_set_default_ports(&spec);

	if (kind == ANX_WF_NODE_STATE_REF) {
		if (argc > 4) {
			kprintf("workflow add-node: state_ref accepts one <oid-or-path> (read-only)\n");
			return ANX_EINVAL;
		}
		if (argc == 4) {
			ret = anx_so_resolve(argv[3], &spec.params.state_ref.obj_oid);
			if (ret != ANX_OK) {
				kprintf("workflow add-node: state object '%s' not found or ambiguous (%d)\n",
					argv[3], ret);
				return ret;
			}
		}
	} else if (kind == ANX_WF_NODE_CELL_CALL) {
		if (argc > 4) {
			kprintf("workflow add-node: cell_call accepts one <intent>\n");
			return ANX_EINVAL;
		}
		if (argc == 4)
			anx_strlcpy(spec.params.cell_call.intent, argv[3],
				    sizeof(spec.params.cell_call.intent));
	}

	/*
	 * Auto-layout: place the new node to the right of all existing nodes.
	 * x = node_count * 160, y = 40.
	 */
	wf = anx_wf_object_get(&oid);
	if (wf) {
		spec.canvas_x = (int32_t)((uint32_t)wf->node_count * 160);
		spec.canvas_y = 40;
	}
	spec.canvas_w = 0;  /* let renderer use defaults */
	spec.canvas_h = 0;

	ret = anx_wf_node_add(&oid, &spec, &new_id);
	if (ret != ANX_OK) {
		kprintf("workflow add-node: failed (%d)\n", ret);
		return ret;
	}

	kprintf("added node %u\n", (uint32_t)new_id);
	return ANX_OK;
}

static int cmd_workflow_add_edge(int argc, char **argv)
{
	anx_oid_t oid;
	struct anx_wf_object *wf;
	uint16_t from_id, to_id;
	uint8_t from_port, to_port;
	int ret;

	if (argc < 3) {
		kprintf("workflow add-edge: <wf-name> <from-node-id> <to-node-id>\n");
		return ANX_EINVAL;
	}

	ret = wf_find_by_name(argv[0], &oid);
	if (ret != ANX_OK) {
		kprintf("workflow add-edge: '%s' not found\n", argv[0]);
		return ANX_ENOENT;
	}

	from_id = (uint16_t)anx_strtoul(argv[1], NULL, 10);
	to_id   = (uint16_t)anx_strtoul(argv[2], NULL, 10);
	wf = anx_wf_object_get(&oid);
	ret = wf_first_port(wf, from_id, ANX_WF_PORT_OUT, &from_port);
	if (ret == ANX_OK)
		ret = wf_first_port(wf, to_id, ANX_WF_PORT_IN, &to_port);
	if (ret != ANX_OK) {
		kprintf("workflow add-edge: incompatible nodes (%d)\n", ret);
		return ret;
	}

	ret = anx_wf_edge_add(&oid, from_id, from_port, to_id, to_port);
	if (ret != ANX_OK) {
		kprintf("workflow add-edge: failed (%d)\n", ret);
		return ret;
	}

	kprintf("added edge %u -> %u\n", (uint32_t)from_id, (uint32_t)to_id);
	return ANX_OK;
}

static int cmd_workflow_destroy(int argc, char **argv)
{
	anx_oid_t oid;
	int ret;

	if (argc < 1) {
		kprintf("workflow destroy: missing name\n");
		return ANX_EINVAL;
	}

	ret = wf_find_by_name(argv[0], &oid);
	if (ret != ANX_OK) {
		kprintf("workflow destroy: '%s' not found\n", argv[0]);
		return ANX_ENOENT;
	}

	ret = anx_wf_destroy(&oid);
	if (ret != ANX_OK) {
		kprintf("workflow destroy: error %d\n", ret);
		return ret;
	}

	kprintf("destroyed workflow '%s'\n", argv[0]);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

int cmd_workflow(int argc, char **argv)
{
	if (argc < 2) {
		wf_usage();
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "create")   == 0) return cmd_workflow_create(argc - 2, argv + 2);
	if (anx_strcmp(argv[1], "list")     == 0) return cmd_workflow_list();
	if (anx_strcmp(argv[1], "run")      == 0) return cmd_workflow_run(argc - 2, argv + 2);
	if (anx_strcmp(argv[1], "status")   == 0) return cmd_workflow_status(argc - 2, argv + 2);
	if (anx_strcmp(argv[1], "show")     == 0) return cmd_workflow_show(argc - 2, argv + 2);
	if (anx_strcmp(argv[1], "graph")    == 0) return cmd_workflow_graph(argc - 2, argv + 2);
	if (anx_strcmp(argv[1], "dump")     == 0) return cmd_workflow_dump(argc - 2, argv + 2);
	if (anx_strcmp(argv[1], "add-node") == 0) return cmd_workflow_add_node(argc - 2, argv + 2);
	if (anx_strcmp(argv[1], "add-edge") == 0) return cmd_workflow_add_edge(argc - 2, argv + 2);
	if (anx_strcmp(argv[1], "destroy")  == 0) return cmd_workflow_destroy(argc - 2, argv + 2);

	kprintf("workflow: unknown subcommand '%s'\n", argv[1]);
	wf_usage();
	return ANX_EINVAL;
}
