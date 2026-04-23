/*
 * anx/workflow.h — Workflow Objects (RFC-0018).
 *
 * A Workflow is a directed node graph that is a first-class State Object
 * (ANX_OBJ_WORKFLOW).  Nodes represent triggers, state access, cell
 * invocations, model inference, agent spawns, fan-out/fan-in barriers,
 * transforms, human review gates, nested subflows, and outputs.  Edges
 * connect named ports between nodes.  The runtime reduces a workflow to
 * an ordered sequence of Cell invocations via topological sort.
 */

#ifndef ANX_WORKFLOW_H
#define ANX_WORKFLOW_H

#include <anx/types.h>
#include <anx/state_object.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define ANX_WF_MAX_NODES	64	/* nodes per workflow */
#define ANX_WF_MAX_EDGES	128	/* edges per workflow */
#define ANX_WF_MAX_PORTS	8	/* ports per node */
#define ANX_WF_NAME_MAX		64	/* workflow / output name */
#define ANX_WF_LABEL_MAX	64	/* node display label */
#define ANX_WF_EXPR_MAX		128	/* expression / template strings */
#define ANX_WF_MAX_WFS		16	/* max concurrent workflows in registry */

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */

/* Kind of work a node performs. */
enum anx_wf_node_kind {
	ANX_WF_NODE_TRIGGER,		/* manual/cron/webhook/event trigger */
	ANX_WF_NODE_STATE_REF,		/* read/write a State Object */
	ANX_WF_NODE_CELL_CALL,		/* invoke a Cell by type+intent */
	ANX_WF_NODE_MODEL_CALL,		/* model server inference */
	ANX_WF_NODE_AGENT_CALL,		/* spawn AI agent Cell with a goal */
	ANX_WF_NODE_RETRIEVAL,		/* search/retrieve from object store */
	ANX_WF_NODE_CONDITION,		/* if/switch branch on upstream output */
	ANX_WF_NODE_FAN_OUT,		/* parallel split: copy to N downstream */
	ANX_WF_NODE_FAN_IN,		/* barrier join: wait for N upstream */
	ANX_WF_NODE_TRANSFORM,		/* inline map/filter/reduce */
	ANX_WF_NODE_HUMAN_REVIEW,	/* pause for human approval */
	ANX_WF_NODE_SUBFLOW,		/* nested workflow reference */
	ANX_WF_NODE_OUTPUT,		/* write result to named object or event */
	ANX_WF_NODE_KIND_COUNT,
};

/* Direction of a node port. */
enum anx_wf_port_dir {
	ANX_WF_PORT_IN  = 0,
	ANX_WF_PORT_OUT = 1,
};

/* Execution state of a workflow run. */
enum anx_wf_run_state {
	ANX_WF_RUN_IDLE,
	ANX_WF_RUN_RUNNING,
	ANX_WF_RUN_WAITING_HUMAN,	/* paused at a HUMAN_REVIEW node */
	ANX_WF_RUN_SUSPENDED,		/* cell failed; agent intervention required */
	ANX_WF_RUN_COMPLETED,
	ANX_WF_RUN_FAILED,
};

/* ------------------------------------------------------------------ */
/* Port                                                                */
/* ------------------------------------------------------------------ */

/*
 * A typed connection point on a node.
 * type_tag: 0=any, 1=state_obj, 2=cell_result, 3=model_output.
 */
struct anx_wf_port {
	char			name[32];
	enum anx_wf_port_dir	dir;
	uint8_t			type_tag;
	bool			required;
};

/* ------------------------------------------------------------------ */
/* Node                                                                */
/* ------------------------------------------------------------------ */

/*
 * A single node in the workflow graph.
 * id == 0 means this slot is unused.
 * canvas_x/y/w/h are UI layout hints in pixels.
 */
struct anx_wf_node {
	uint16_t		id;
	enum anx_wf_node_kind	kind;
	char			label[ANX_WF_LABEL_MAX];

	/* Kind-specific parameters. */
	union {
		/* ANX_WF_NODE_TRIGGER */
		struct {
			/* cron expression, "manual", or "event:NAME" */
			char	schedule[64];
		} trigger;

		/* ANX_WF_NODE_STATE_REF */
		struct {
			anx_oid_t	obj_oid;
			bool		write_mode;
		} state_ref;

		/* ANX_WF_NODE_CELL_CALL */
		struct {
			char	intent[ANX_WF_EXPR_MAX];
		} cell_call;

		/* ANX_WF_NODE_MODEL_CALL */
		struct {
			char	model_id[64];
			char	prompt_template[ANX_WF_EXPR_MAX];
		} model_call;

		/* ANX_WF_NODE_AGENT_CALL */
		struct {
			char	goal[ANX_WF_EXPR_MAX];
		} agent_call;

		/* ANX_WF_NODE_RETRIEVAL */
		struct {
			char		query_template[ANX_WF_EXPR_MAX];
			uint32_t	top_k;
		} retrieval;

		/* ANX_WF_NODE_CONDITION */
		struct {
			char	expr[ANX_WF_EXPR_MAX];
		} condition;

		/* ANX_WF_NODE_TRANSFORM */
		struct {
			char	op[32];		/* "map", "filter", "reduce" */
			char	fn_expr[ANX_WF_EXPR_MAX];
		} transform;

		/* ANX_WF_NODE_SUBFLOW */
		struct {
			anx_oid_t	subflow_oid;
		} subflow;

		/* ANX_WF_NODE_OUTPUT */
		struct {
			char	dest_name[ANX_WF_NAME_MAX];
		} output;
	} params;

	/* Visual canvas position and size (pixels). */
	int32_t		canvas_x;
	int32_t		canvas_y;
	uint32_t	canvas_w;
	uint32_t	canvas_h;

	uint8_t			port_count;
	struct anx_wf_port	ports[ANX_WF_MAX_PORTS];
};

/* ------------------------------------------------------------------ */
/* Edge                                                                */
/* ------------------------------------------------------------------ */

/* A directed data edge from one node port to another. */
struct anx_wf_edge {
	uint16_t	from_node;
	uint16_t	to_node;
	uint8_t		from_port;
	uint8_t		to_port;
};

/* ------------------------------------------------------------------ */
/* Trace entry                                                         */
/* ------------------------------------------------------------------ */

/* One record per executed node — collected into a trace state object. */
struct anx_wf_trace_entry {
	uint16_t	node_id;
	uint8_t		node_kind;	/* enum anx_wf_node_kind — for JEPA action mapping */
	uint8_t		_pad;
	anx_cid_t	cell_cid;	/* CID of the cell dispatched (null for control nodes) */
	anx_oid_t	trace_oid;	/* finalized cell trace state object */
	int		result;
	anx_time_t	started_at;
	anx_time_t	completed_at;
};

/* ------------------------------------------------------------------ */
/* Continuation                                                        */
/* ------------------------------------------------------------------ */

/*
 * Saved executor state when a workflow suspends.
 *
 * The first three fields are agent-readable: they describe what failed
 * and why.  The remaining arrays are the full executor snapshot needed
 * to resume exactly where execution left off.
 *
 * Heap-allocated on suspension; freed on resume or destroy.
 */
struct anx_wf_continuation {
	/* Agent-readable failure context */
	uint16_t	failed_node_id;
	int		error_code;
	char		error_msg[256];

	/* Executor state snapshot */
	uint32_t	in_deg[ANX_WF_MAX_NODES];
	bool		completed[ANX_WF_MAX_NODES];
	anx_oid_t	port_oid[ANX_WF_MAX_NODES][ANX_WF_MAX_PORTS];
};

/* ------------------------------------------------------------------ */
/* Resume action                                                       */
/* ------------------------------------------------------------------ */

enum anx_wf_resume_action {
	ANX_WF_RESUME_RETRY,	  /* retry the failed node with the same spec */
	ANX_WF_RESUME_SKIP,	  /* skip the failed node, continue with null outputs */
	ANX_WF_RESUME_ABORT,	  /* abort and mark the workflow failed */
	ANX_WF_RESUME_REPLACE,	  /* replace the failed node spec, then retry */
};

/* ------------------------------------------------------------------ */
/* Policy                                                              */
/* ------------------------------------------------------------------ */

/* Execution policy for a workflow run. */
struct anx_wf_policy {
	uint32_t	max_parallel;	/* 0 = derive from host resources */
	uint32_t	timeout_ms;	/* 0 = no timeout */
	bool		auto_retry;
	uint8_t		max_retries;
};

/* ------------------------------------------------------------------ */
/* Workflow Object                                                     */
/* ------------------------------------------------------------------ */

/*
 * The top-level workflow object stored in the registry.
 * nodes and edges are dynamically allocated (ANX_WF_MAX_NODES /
 * ANX_WF_MAX_EDGES entries respectively) and freed on destroy.
 */
struct anx_wf_object {
	bool		in_use;
	anx_oid_t	oid;
	char		name[ANX_WF_NAME_MAX];
	char		description[256];

	uint16_t		node_count;
	uint16_t		edge_count;
	struct anx_wf_node	*nodes;	/* ANX_WF_MAX_NODES entries */
	struct anx_wf_edge	*edges;	/* ANX_WF_MAX_EDGES entries */

	enum anx_wf_run_state	run_state;
	anx_cid_t		running_cid;
	anx_time_t		last_run;
	int			last_status;

	struct anx_wf_policy	policy;

	/* Phase 2: parallel executor state */
	uint32_t		computed_cap;		/* hw-derived; set at run start */
	struct anx_wf_continuation *continuation;	/* non-NULL when SUSPENDED */

	/* Trace accumulation (heap-alloc'd at run start, sealed at completion) */
	struct anx_wf_trace_entry *trace_entries;	/* ANX_WF_MAX_NODES entries */
	uint32_t		trace_entry_count;
	anx_oid_t		trace_oid;		/* sealed trace state object */

	/* Outputs produced by ANX_WF_NODE_OUTPUT nodes */
	anx_oid_t		output_oids[ANX_WF_MAX_PORTS];
	uint8_t			output_count;
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Initialize the workflow subsystem (called at kernel boot). */
int anx_wf_init(void);

/* Create a new empty workflow. */
int anx_wf_create(const char *name, const char *description, anx_oid_t *oid_out);

/* Destroy a workflow and free its node/edge storage. */
int anx_wf_destroy(const anx_oid_t *oid);

/* Add a node; returns assigned node id in *id_out. */
int anx_wf_node_add(const anx_oid_t *wf_oid, const struct anx_wf_node *spec,
		    uint16_t *id_out);

/* Remove a node and all edges connected to it. */
int anx_wf_node_remove(const anx_oid_t *wf_oid, uint16_t node_id);

/* Connect two node ports. */
int anx_wf_edge_add(const anx_oid_t *wf_oid, uint16_t from_node, uint8_t from_port,
		    uint16_t to_node, uint8_t to_port);

/* Remove an edge. */
int anx_wf_edge_remove(const anx_oid_t *wf_oid, uint16_t from_node, uint8_t from_port,
		       uint16_t to_node, uint8_t to_port);

/* Run a workflow (topological sort -> Cell sequence). */
int anx_wf_run(const anx_oid_t *wf_oid, anx_cid_t *run_cid_out);

/* Get current run state. */
int anx_wf_run_state_get(const anx_oid_t *wf_oid, enum anx_wf_run_state *state_out);

/* List all workflows. */
int anx_wf_list(anx_oid_t *results, uint32_t max, uint32_t *count_out);

/* Look up workflow object by OID — returns internal pointer (valid until destroy). */
struct anx_wf_object *anx_wf_object_get(const anx_oid_t *oid);

/* Render the workflow node graph onto a pixel buffer (width x height, 32bpp). */
int anx_wf_render_canvas(const anx_oid_t *wf_oid, uint32_t *pixels,
			  uint32_t width, uint32_t height);

/*
 * Compute the hardware-derived concurrency cap for a workflow.
 * If policy->max_parallel is non-zero it acts as a ceiling; the hw cap
 * is the floor.  Never returns less than 1 or more than ANX_WF_MAX_NODES.
 */
uint32_t anx_wf_cap_compute(const struct anx_wf_policy *policy);

/*
 * Resume a suspended workflow.  REPLACE requires a non-NULL replacement
 * node spec; all other actions ignore it.  Returns ANX_EINVAL if the
 * workflow is not in ANX_WF_RUN_SUSPENDED state.
 */
int anx_wf_resume(const anx_oid_t *wf_oid,
		  enum anx_wf_resume_action action,
		  const struct anx_wf_node *replacement);

/* Return the continuation for an SUSPENDED workflow, or NULL. */
const struct anx_wf_continuation *anx_wf_continuation_get(const anx_oid_t *oid);

/*
 * Seal the accumulated per-node trace entries into an ANX_OBJ_EXECUTION_TRACE
 * state object.  Sets wf->trace_oid; frees trace_entries.
 * Returns ANX_ENOENT if no entries were recorded.
 */
int anx_wf_trace_seal(const anx_oid_t *wf_oid, anx_oid_t *trace_oid_out);

/*
 * Serialize a workflow to a human-readable/editable DSL string.
 * buf must be at least size bytes; result is NUL-terminated.
 */
int anx_wf_serialize(const anx_oid_t *wf_oid, char *buf, uint32_t size);

/*
 * Render a workflow as a terminal ASCII DAG diagram.
 * buf must be at least size bytes; result is NUL-terminated.
 */
int anx_wf_render_ascii(const anx_oid_t *wf_oid, char *buf, uint32_t size);

#endif /* ANX_WORKFLOW_H */
