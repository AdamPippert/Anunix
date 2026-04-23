/*
 * workflow_serial.c — Workflow text serialization and ASCII diagram rendering.
 *
 * Two output formats:
 *
 *   anx_wf_serialize()    — a compact DSL for human editing and offline storage:
 *
 *       workflow "rag" {
 *         description "retrieval-augmented generation"
 *         node 1 trigger   "start"    { schedule manual }
 *         node 2 retrieval "retrieve" { query "${input}" top_k 5 }
 *         node 3 model     "answer"   { model anx:model/default prompt "..." }
 *         node 4 output    "result"   { dest answer }
 *         edge 1:0 -> 2:0
 *         edge 2:0 -> 3:0
 *         edge 3:0 -> 4:0
 *       }
 *
 *   anx_wf_render_ascii()  — a terminal DAG diagram:
 *
 *       [trigger:start     ]
 *                |
 *       [retrieval:retrieve]
 *                |
 *       [model:answer      ]
 *                |
 *       [output:result     ]
 */

#include <anx/workflow.h>
#include <anx/string.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Advance pointer p and decrement rem by n bytes written.
 * anx_snprintf returns bytes that *would* be written (like POSIX snprintf),
 * so clamp to rem-1 to stay within bounds.
 */
#define WF_EMIT(p, rem, ...)						\
	do {								\
		int _n = anx_snprintf((p), (uint32_t)(rem), __VA_ARGS__);	\
		if (_n < 0) _n = 0;					\
		if (_n >= (rem)) _n = (rem) - 1;			\
		(p)   += _n;						\
		(rem) -= _n;						\
	} while (0)

/* Append a literal string (no formatting). */
static void wf_puts(char **p, int *rem, const char *s)
{
	for (; *s && *rem > 1; s++) {
		**p = *s;
		(*p)++;
		(*rem)--;
	}
	**p = '\0';
}

/* Emit a quoted string, escaping inner double-quotes. */
static void emit_quoted(char **p, int *rem, const char *s)
{
	wf_puts(p, rem, "\"");
	for (; *s && *rem > 2; s++) {
		if (*s == '"') {
			wf_puts(p, rem, "\\\"");
		} else {
			**p = *s;
			(*p)++;
			(*rem)--;
		}
	}
	**p = '\0';
	wf_puts(p, rem, "\"");
}

static const char *kind_token(enum anx_wf_node_kind k)
{
	switch (k) {
	case ANX_WF_NODE_TRIGGER:	return "trigger";
	case ANX_WF_NODE_STATE_REF:	return "state-ref";
	case ANX_WF_NODE_CELL_CALL:	return "cell";
	case ANX_WF_NODE_MODEL_CALL:	return "model";
	case ANX_WF_NODE_AGENT_CALL:	return "agent";
	case ANX_WF_NODE_RETRIEVAL:	return "retrieval";
	case ANX_WF_NODE_CONDITION:	return "condition";
	case ANX_WF_NODE_FAN_OUT:	return "fan-out";
	case ANX_WF_NODE_FAN_IN:	return "fan-in";
	case ANX_WF_NODE_TRANSFORM:	return "transform";
	case ANX_WF_NODE_HUMAN_REVIEW:	return "human-review";
	case ANX_WF_NODE_SUBFLOW:	return "subflow";
	case ANX_WF_NODE_OUTPUT:	return "output";
	default:			return "unknown";
	}
}

/* ------------------------------------------------------------------ */
/* Text serialization                                                  */
/* ------------------------------------------------------------------ */

int anx_wf_serialize(const anx_oid_t *wf_oid, char *buf, uint32_t size)
{
	struct anx_wf_object *wf;
	char *p;
	int rem;
	uint32_t i;

	if (!wf_oid || !buf || size < 4)
		return ANX_EINVAL;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;

	p   = buf;
	rem = (int)size;

	/* Header */
	wf_puts(&p, &rem, "workflow ");
	emit_quoted(&p, &rem, wf->name);
	wf_puts(&p, &rem, " {\n");

	if (wf->description[0]) {
		wf_puts(&p, &rem, "  description ");
		wf_puts(&p, &rem, wf->description);
		wf_puts(&p, &rem, "\n");
	}

	/* Nodes */
	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		const struct anx_wf_node *n = &wf->nodes[i];

		if (n->id == 0)
			continue;

		WF_EMIT(p, rem, "  node %-2u %-12s ", (uint32_t)n->id,
			kind_token(n->kind));
		emit_quoted(&p, &rem, n->label);
		wf_puts(&p, &rem, " {");

		switch (n->kind) {
		case ANX_WF_NODE_TRIGGER:
			if (n->params.trigger.schedule[0]) {
				wf_puts(&p, &rem, " schedule ");
				wf_puts(&p, &rem, n->params.trigger.schedule);
			}
			break;

		case ANX_WF_NODE_CELL_CALL:
			if (n->params.cell_call.intent[0]) {
				wf_puts(&p, &rem, " intent ");
				wf_puts(&p, &rem, n->params.cell_call.intent);
			}
			break;

		case ANX_WF_NODE_MODEL_CALL:
			if (n->params.model_call.model_id[0]) {
				wf_puts(&p, &rem, " model ");
				wf_puts(&p, &rem, n->params.model_call.model_id);
			}
			if (n->params.model_call.prompt_template[0]) {
				wf_puts(&p, &rem, " prompt ");
				emit_quoted(&p, &rem,
					    n->params.model_call.prompt_template);
			}
			break;

		case ANX_WF_NODE_AGENT_CALL:
			if (n->params.agent_call.goal[0]) {
				wf_puts(&p, &rem, " goal ");
				emit_quoted(&p, &rem, n->params.agent_call.goal);
			}
			break;

		case ANX_WF_NODE_RETRIEVAL:
			if (n->params.retrieval.query_template[0]) {
				wf_puts(&p, &rem, " query ");
				wf_puts(&p, &rem,
					n->params.retrieval.query_template);
				WF_EMIT(p, rem, " top_k %u",
					n->params.retrieval.top_k);
			}
			break;

		case ANX_WF_NODE_CONDITION:
			if (n->params.condition.expr[0]) {
				wf_puts(&p, &rem, " expr ");
				wf_puts(&p, &rem, n->params.condition.expr);
			}
			break;

		case ANX_WF_NODE_TRANSFORM:
			if (n->params.transform.op[0]) {
				wf_puts(&p, &rem, " op ");
				wf_puts(&p, &rem, n->params.transform.op);
			}
			if (n->params.transform.fn_expr[0]) {
				wf_puts(&p, &rem, " fn ");
				wf_puts(&p, &rem, n->params.transform.fn_expr);
			}
			break;

		case ANX_WF_NODE_OUTPUT:
			if (n->params.output.dest_name[0]) {
				wf_puts(&p, &rem, " dest ");
				wf_puts(&p, &rem, n->params.output.dest_name);
			}
			break;

		default:
			break;
		}

		wf_puts(&p, &rem, " }\n");
	}

	/* Edges */
	if (wf->edge_count > 0)
		wf_puts(&p, &rem, "\n");

	for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
		const struct anx_wf_edge *e = &wf->edges[i];

		if (e->from_node == 0 && e->to_node == 0)
			continue;

		WF_EMIT(p, rem, "  edge %u:%u -> %u:%u\n",
			(uint32_t)e->from_node, (uint32_t)e->from_port,
			(uint32_t)e->to_node,   (uint32_t)e->to_port);
	}

	wf_puts(&p, &rem, "}\n");
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* ASCII diagram renderer                                              */
/* ------------------------------------------------------------------ */

/*
 * Topology: compute per-node depth via BFS from sources (in-degree 0).
 * Layout: nodes bucketed by depth; each depth level is one printed row.
 * Connector row drawn between rows showing which nodes flow downward.
 * Followed by an edge legend with port numbers and node labels.
 *
 * Limits: up to ASCII_MAX_DEPTH depth levels, ASCII_MAX_PER_DEPTH nodes
 * wide. Nodes beyond these limits are omitted from the diagram but still
 * appear in the edge legend.
 */

#define ASCII_MAX_DEPTH		16
#define ASCII_MAX_PER_DEPTH	6
#define ASCII_BOX_INNER		16	/* chars between [ and ] */
#define ASCII_BOX_W		(ASCII_BOX_INNER + 2)	/* total box width */
#define ASCII_COL_W		(ASCII_BOX_W + 2)	/* box + gap */

static void ascii_node_box(char out[ASCII_BOX_W + 1],
			   const struct anx_wf_node *n)
{
	/* "kind:label" truncated to ASCII_BOX_INNER chars, padded with spaces */
	char inner[ASCII_BOX_INNER + 1];
	const char *k = kind_token(n->kind);
	uint32_t klen = (uint32_t)anx_strlen(k);
	uint32_t llen = (uint32_t)anx_strlen(n->label);
	uint32_t avail = ASCII_BOX_INNER;

	if (klen + 1 + llen <= avail) {
		anx_snprintf(inner, sizeof(inner), "%s:%s", k, n->label);
	} else if (klen + 1 < avail) {
		uint32_t room = avail - klen - 1;
		anx_snprintf(inner, sizeof(inner), "%s:%.*s",
			     k, (int)room, n->label);
	} else {
		anx_snprintf(inner, sizeof(inner), "%.*s", (int)avail, k);
	}

	anx_snprintf(out, ASCII_BOX_W + 1, "[%-*s]",
		     ASCII_BOX_INNER, inner);
}

int anx_wf_render_ascii(const anx_oid_t *wf_oid, char *buf, uint32_t size)
{
	struct anx_wf_object *wf;
	char *p;
	int rem;

	uint8_t  depth[ANX_WF_MAX_NODES];
	uint8_t  in_deg[ANX_WF_MAX_NODES];
	uint16_t node_id[ANX_WF_MAX_NODES];
	uint32_t n_nodes;

	uint16_t by_depth[ASCII_MAX_DEPTH][ASCII_MAX_PER_DEPTH];
	uint8_t  depth_count[ASCII_MAX_DEPTH];
	uint32_t max_depth;

	uint32_t i, d;

	if (!wf_oid || !buf || size < 8)
		return ANX_EINVAL;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;

	p   = buf;
	rem = (int)size;

	/* -- Collect live node IDs ----------------------------------------- */
	n_nodes = 0;
	anx_memset(in_deg, 0, sizeof(in_deg));
	anx_memset(depth,  0, sizeof(depth));

	for (i = 0; i < ANX_WF_MAX_NODES && n_nodes < ANX_WF_MAX_NODES; i++) {
		if (wf->nodes[i].id != 0)
			node_id[n_nodes++] = wf->nodes[i].id;
	}

	/* node id is 1-based; slot in nodes[] = id-1 */
#define SLOT(id) ((uint32_t)((id) - 1))

	for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
		const struct anx_wf_edge *e = &wf->edges[i];

		if (e->from_node == 0 && e->to_node == 0)
			continue;
		if (SLOT(e->to_node) < ANX_WF_MAX_NODES)
			in_deg[SLOT(e->to_node)]++;
	}

	/* -- BFS depth assignment ------------------------------------------ */
	{
		uint16_t queue[ANX_WF_MAX_NODES];
		uint32_t qhead = 0, qtail = 0;

		for (i = 0; i < n_nodes; i++) {
			if (in_deg[SLOT(node_id[i])] == 0) {
				depth[SLOT(node_id[i])] = 0;
				queue[qtail++] = node_id[i];
			}
		}

		while (qhead < qtail) {
			uint16_t cur      = queue[qhead++];
			uint8_t  next_d   = depth[SLOT(cur)] + 1;
			uint32_t ei;

			for (ei = 0; ei < ANX_WF_MAX_EDGES; ei++) {
				const struct anx_wf_edge *e = &wf->edges[ei];
				uint32_t dst;

				if (e->from_node != cur)
					continue;
				dst = SLOT(e->to_node);
				if (dst >= ANX_WF_MAX_NODES)
					continue;
				if (next_d > depth[dst])
					depth[dst] = next_d;
				if (in_deg[dst] > 0) {
					in_deg[dst]--;
					if (in_deg[dst] == 0)
						queue[qtail++] = e->to_node;
				}
			}
		}
	}

	/* -- Bucket nodes by depth ----------------------------------------- */
	anx_memset(depth_count, 0, sizeof(depth_count));
	anx_memset(by_depth,    0, sizeof(by_depth));
	max_depth = 0;

	for (i = 0; i < n_nodes; i++) {
		uint8_t dd = depth[SLOT(node_id[i])];

		if (dd >= ASCII_MAX_DEPTH) dd = ASCII_MAX_DEPTH - 1;
		if (depth_count[dd] < ASCII_MAX_PER_DEPTH)
			by_depth[dd][depth_count[dd]++] = node_id[i];
		if ((uint32_t)dd > max_depth)
			max_depth = dd;
	}

	/* -- Header --------------------------------------------------------- */
	WF_EMIT(p, rem, "workflow \"%s\"", wf->name);
	if (wf->description[0])
		WF_EMIT(p, rem, "  # %s", wf->description);
	wf_puts(&p, &rem, "\n\n");

	/* -- Render rows ---------------------------------------------------- */
	for (d = 0; d <= max_depth; d++) {
		uint32_t cnt = depth_count[d];
		uint32_t col;

		/* Node boxes */
		for (col = 0; col < cnt; col++) {
			char box[ASCII_BOX_W + 1];
			const struct anx_wf_node *n =
				&wf->nodes[SLOT(by_depth[d][col])];

			ascii_node_box(box, n);
			wf_puts(&p, &rem, box);
			if (col + 1 < cnt)
				wf_puts(&p, &rem, "  ");
		}
		wf_puts(&p, &rem, "\n");

		/* Connector row: "|" centered under each node that has a
		 * downstream edge going to depth d+1. */
		if (d < max_depth) {
			for (col = 0; col < cnt; col++) {
				uint16_t cur = by_depth[d][col];
				bool has_down = false;
				uint32_t ei, k;

				for (ei = 0; ei < ANX_WF_MAX_EDGES; ei++) {
					const struct anx_wf_edge *e =
						&wf->edges[ei];

					if (e->from_node != cur)
						continue;
					if (SLOT(e->to_node) < ANX_WF_MAX_NODES &&
					    depth[SLOT(e->to_node)] ==
					    (uint8_t)(d + 1)) {
						has_down = true;
						break;
					}
				}

				/* Print ASCII_BOX_W/2 spaces, then | or space,
				 * then remaining spaces + gap. */
				for (k = 0; k < ASCII_BOX_W / 2; k++)
					wf_puts(&p, &rem, " ");
				wf_puts(&p, &rem, has_down ? "|" : " ");
				for (k = ASCII_BOX_W / 2 + 1;
				     k < ASCII_BOX_W + (col + 1 < cnt ? 2 : 0);
				     k++)
					wf_puts(&p, &rem, " ");
			}
			wf_puts(&p, &rem, "\n");
		}
	}

	/* -- Edge legend ---------------------------------------------------- */
	if (wf->edge_count > 0) {
		wf_puts(&p, &rem, "\nedges:\n");
		for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
			const struct anx_wf_edge *e = &wf->edges[i];
			const char *fn, *tn;

			if (e->from_node == 0 && e->to_node == 0)
				continue;

			fn = wf->nodes[SLOT(e->from_node)].label;
			tn = wf->nodes[SLOT(e->to_node)].label;
			WF_EMIT(p, rem, "  %u:%u -> %u:%u  (%s -> %s)\n",
				(uint32_t)e->from_node, (uint32_t)e->from_port,
				(uint32_t)e->to_node,   (uint32_t)e->to_port,
				fn[0] ? fn : "?",
				tn[0] ? tn : "?");
		}
	}

	return ANX_OK;

#undef SLOT
}
