/*
 * workflow_library.c — Built-in workflow template registry.
 *
 * Eight templates cover the most common agent patterns.  Each template
 * is a static anx_wf_template descriptor; instantiation copies its
 * nodes and edges into a fresh workflow object via the standard builder
 * API so each run starts from a clean, independent workflow.
 *
 * Keyword matching is plain set intersection: split the goal string on
 * whitespace, lowercase, count how many words hit a template's tag set.
 * First-call overhead is O(templates × tags × goal_words); typical
 * goals are short and the template set is small.
 */

#include <anx/workflow_library.h>
#include <anx/workflow.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

static const struct anx_wf_template *g_lib[ANX_WF_LIB_MAX];
static uint32_t g_lib_count;

/* ------------------------------------------------------------------ */
/* Built-in template definitions                                       */
/* ------------------------------------------------------------------ */

/*
 * 1. anx:workflow/infer
 *    TRIGGER → MODEL_CALL → OUTPUT
 *    The simplest useful pattern: run one model call on an input.
 */
static const struct anx_wf_template tmpl_infer = {
	.uri          = "anx:workflow/infer",
	.display_name = "Infer",
	.description  = "Single model inference: pass input through one model call and collect output.",
	.tags         = { "infer", "inference", "model", "llm", "generate" },
	.tag_count    = 5,
	.node_count   = 3,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_MODEL_CALL,
			.label = "infer",
			.params.model_call = {
				.model_id        = "anx:model/default",
				.prompt_template = "${input}",
			},
			.port_count = 2,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "out", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_OUTPUT,
			.label = "result",
			.params.output = { .dest_name = "output" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 2,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
	},
};

/*
 * 2. anx:workflow/summarize
 *    TRIGGER → MODEL_CALL(summarize) → OUTPUT
 *    Like /infer but with a fixed summarization prompt prefix.
 */
static const struct anx_wf_template tmpl_summarize = {
	.uri          = "anx:workflow/summarize",
	.display_name = "Summarize",
	.description  = "Summarize a state object using a model call.",
	.tags         = { "summarize", "summary", "compress", "distill", "shorten" },
	.tag_count    = 5,
	.node_count   = 3,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_MODEL_CALL,
			.label = "summarize",
			.params.model_call = {
				.model_id        = "anx:model/default",
				.prompt_template =
					"Summarize the following concisely:\n${input}",
			},
			.port_count = 2,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "out", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_OUTPUT,
			.label = "summary",
			.params.output = { .dest_name = "summary" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 2,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
	},
};

/*
 * 3. anx:workflow/rag
 *    TRIGGER → RETRIEVAL ──┐
 *                           ├→ MODEL_CALL(grounded) → OUTPUT
 *    TRIGGER ───────────────┘
 *    Retrieval-augmented generation: fetch relevant context, then answer.
 */
static const struct anx_wf_template tmpl_rag = {
	.uri          = "anx:workflow/rag",
	.display_name = "RAG",
	.description  = "Retrieval-augmented generation: retrieve relevant context then run grounded inference.",
	.tags         = { "rag", "retrieval", "search", "grounded", "context", "augment" },
	.tag_count    = 6,
	.node_count   = 4,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 2,
			.ports = {
				{ .name = "query",  .dir = ANX_WF_PORT_OUT },
				{ .name = "query2", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_RETRIEVAL,
			.label = "retrieve",
			.params.retrieval = {
				.query_template = "${query}",
				.top_k          = 5,
			},
			.port_count = 2,
			.ports = {
				{ .name = "query",   .dir = ANX_WF_PORT_IN  },
				{ .name = "context", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_MODEL_CALL,
			.label = "answer",
			.params.model_call = {
				.model_id        = "anx:model/default",
				.prompt_template =
					"Context:\n${context}\n\nQuestion: ${query}\nAnswer:",
			},
			.port_count = 3,
			.ports = {
				{ .name = "context", .dir = ANX_WF_PORT_IN  },
				{ .name = "query",   .dir = ANX_WF_PORT_IN  },
				{ .name = "answer",  .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 4, .kind = ANX_WF_NODE_OUTPUT,
			.label = "result",
			.params.output = { .dest_name = "answer" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 4,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
		{ .from_node = 1, .from_port = 1, .to_node = 3, .to_port = 1 },
		{ .from_node = 3, .from_port = 2, .to_node = 4, .to_port = 0 },
	},
};

/*
 * 4. anx:workflow/chain
 *    TRIGGER → MODEL_CALL(step1) → MODEL_CALL(step2) → OUTPUT
 *    Two-stage chain: first call produces intermediate reasoning,
 *    second call refines it.  Useful for chain-of-thought patterns.
 */
static const struct anx_wf_template tmpl_chain = {
	.uri          = "anx:workflow/chain",
	.display_name = "Chain",
	.description  = "Two-stage model chain: first call reasons, second call refines.",
	.tags         = { "chain", "cot", "reason", "refine", "sequential", "think" },
	.tag_count    = 6,
	.node_count   = 4,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_MODEL_CALL,
			.label = "reason",
			.params.model_call = {
				.model_id        = "anx:model/default",
				.prompt_template =
					"Think step by step about: ${input}\nReasoning:",
			},
			.port_count = 2,
			.ports = {
				{ .name = "in",        .dir = ANX_WF_PORT_IN  },
				{ .name = "reasoning", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_MODEL_CALL,
			.label = "conclude",
			.params.model_call = {
				.model_id        = "anx:model/default",
				.prompt_template =
					"Given this reasoning:\n${reasoning}\n\nFinal answer:",
			},
			.port_count = 2,
			.ports = {
				{ .name = "reasoning", .dir = ANX_WF_PORT_IN  },
				{ .name = "out",       .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 4, .kind = ANX_WF_NODE_OUTPUT,
			.label = "result",
			.params.output = { .dest_name = "output" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 3,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
		{ .from_node = 3, .from_port = 1, .to_node = 4, .to_port = 0 },
	},
};

/*
 * 5. anx:workflow/ensemble
 *    TRIGGER → FAN_OUT → MODEL_CALL(a) ─┐
 *                       → MODEL_CALL(b) ─┤→ FAN_IN → OUTPUT
 *                       → MODEL_CALL(c) ─┘
 *    Three independent model calls on the same input; FAN_IN
 *    collects all three for downstream selection or voting.
 */
static const struct anx_wf_template tmpl_ensemble = {
	.uri          = "anx:workflow/ensemble",
	.display_name = "Ensemble",
	.description  = "Fan-out to three model calls then collect all outputs for voting.",
	.tags         = { "ensemble", "vote", "multiple", "parallel", "consensus", "diverse" },
	.tag_count    = 6,
	.node_count   = 7,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_FAN_OUT,
			.label = "split",
			.port_count = 4,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "a",   .dir = ANX_WF_PORT_OUT },
				{ .name = "b",   .dir = ANX_WF_PORT_OUT },
				{ .name = "c",   .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_MODEL_CALL,
			.label = "model-a",
			.params.model_call = {
				.model_id        = "anx:model/default",
				.prompt_template = "${input}",
			},
			.port_count = 2,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "out", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 4, .kind = ANX_WF_NODE_MODEL_CALL,
			.label = "model-b",
			.params.model_call = {
				.model_id        = "anx:model/default",
				.prompt_template = "${input}",
			},
			.port_count = 2,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "out", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 5, .kind = ANX_WF_NODE_MODEL_CALL,
			.label = "model-c",
			.params.model_call = {
				.model_id        = "anx:model/default",
				.prompt_template = "${input}",
			},
			.port_count = 2,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "out", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 6, .kind = ANX_WF_NODE_FAN_IN,
			.label = "collect",
			.port_count = 4,
			.ports = {
				{ .name = "a",   .dir = ANX_WF_PORT_IN  },
				{ .name = "b",   .dir = ANX_WF_PORT_IN  },
				{ .name = "c",   .dir = ANX_WF_PORT_IN  },
				{ .name = "out", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 7, .kind = ANX_WF_NODE_OUTPUT,
			.label = "result",
			.params.output = { .dest_name = "outputs" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 7,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
		{ .from_node = 2, .from_port = 2, .to_node = 4, .to_port = 0 },
		{ .from_node = 2, .from_port = 3, .to_node = 5, .to_port = 0 },
		{ .from_node = 3, .from_port = 1, .to_node = 6, .to_port = 0 },
		{ .from_node = 4, .from_port = 1, .to_node = 6, .to_port = 1 },
		{ .from_node = 5, .from_port = 1, .to_node = 6, .to_port = 2 },
	},
};

/*
 * 6. anx:workflow/spawn-collect
 *    TRIGGER → CELL_CALL → OUTPUT
 *    Spawn an execution cell for an arbitrary task and collect its output.
 */
static const struct anx_wf_template tmpl_spawn_collect = {
	.uri          = "anx:workflow/spawn-collect",
	.display_name = "Spawn and Collect",
	.description  = "Spawn a cell for an arbitrary task and collect its output.",
	.tags         = { "spawn", "cell", "execute", "task", "run", "compute" },
	.tag_count    = 6,
	.node_count   = 3,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_CELL_CALL,
			.label = "execute",
			.params.cell_call = { .intent = "${task}" },
			.port_count = 2,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "out", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_OUTPUT,
			.label = "result",
			.params.output = { .dest_name = "output" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 2,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
	},
};

/*
 * 7. anx:workflow/decompose-goal
 *    TRIGGER → MODEL_CALL(plan) → OUTPUT
 *    Calls the task-planner model with a structured prompt that asks it
 *    to produce a step-by-step plan or workflow specification.
 */
static const struct anx_wf_template tmpl_decompose = {
	.uri          = "anx:workflow/decompose-goal",
	.display_name = "Decompose Goal",
	.description  = "Use a model to decompose a goal into a step-by-step plan or workflow spec.",
	.tags         = {
		"decompose", "plan", "breakdown", "workflow",
		"goal", "steps", "strategy",
	},
	.tag_count    = 7,
	.node_count   = 3,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "goal",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_MODEL_CALL,
			.label = "plan",
			.params.model_call = {
				.model_id        = "anx:model/task-planner",
				.prompt_template =
					"Break down this goal into concrete steps.\n"
					"Output a numbered list.\n"
					"Goal: ${input}",
			},
			.port_count = 2,
			.ports = {
				{ .name = "goal", .dir = ANX_WF_PORT_IN  },
				{ .name = "plan", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_OUTPUT,
			.label = "plan",
			.params.output = { .dest_name = "plan" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 2,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
	},
};

/*
 * 8. anx:workflow/observe-report
 *    TRIGGER → CELL_CALL(jepa-observe) → MODEL_CALL(interpret) → OUTPUT
 *    Collect a JEPA system observation and use a model to interpret it
 *    as a human-readable health report.
 */
static const struct anx_wf_template tmpl_observe = {
	.uri          = "anx:workflow/observe-report",
	.display_name = "Observe and Report",
	.description  = "Collect system state via JEPA observation and produce a health report.",
	.tags         = {
		"observe", "monitor", "health", "system",
		"jepa", "report", "status", "anomaly",
	},
	.tag_count    = 8,
	.node_count   = 4,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_CELL_CALL,
			.label = "observe",
			.params.cell_call = { .intent = "jepa-observe" },
			.port_count = 2,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "obs", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_MODEL_CALL,
			.label = "interpret",
			.params.model_call = {
				.model_id        = "anx:model/default",
				.prompt_template =
					"System health analyst. "
					"Observation: ${input}\n"
					"Report anomalies concisely.",
			},
			.port_count = 2,
			.ports = {
				{ .name = "obs",    .dir = ANX_WF_PORT_IN  },
				{ .name = "report", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 4, .kind = ANX_WF_NODE_OUTPUT,
			.label = "report",
			.params.output = { .dest_name = "health_report" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 3,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
		{ .from_node = 3, .from_port = 1, .to_node = 4, .to_port = 0 },
	},
};

/*
 * 9. anx:workflow/ibal/default/v1
 *    TRIGGER → CELL_CALL(ibal-run) → OUTPUT
 *    Full IBAL loop: create session, advance to halt, output committed plan.
 */
static const struct anx_wf_template tmpl_ibal_default = {
	.uri          = "anx:workflow/ibal/default/v1",
	.display_name = "IBAL Default",
	.description  = "Iterative Belief-Action Loop: full session with EBM scoring and memory consolidation.",
	.tags         = { "ibal", "loop", "iterative", "belief", "plan", "agent" },
	.tag_count    = 6,
	.node_count   = 3,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_CELL_CALL,
			.label = "ibal-run",
			.params.cell_call = {
				.intent = "anx:cell/loop/ibal-runner goal=${goal}",
			},
			.port_count = 2,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "out", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_OUTPUT,
			.label = "plan",
			.params.output = { .dest_name = "committed_plan" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 2,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
	},
};

/*
 * 10. anx:workflow/ibal/lite/v1
 *     TRIGGER → CELL_CALL(ibal-lite) → OUTPUT
 *     Lightweight IBAL: fixed 4-iteration budget, no memory consolidation.
 */
static const struct anx_wf_template tmpl_ibal_lite = {
	.uri          = "anx:workflow/ibal/lite/v1",
	.display_name = "IBAL Lite",
	.description  = "Lightweight IBAL loop: 4-iteration budget, fast path without memory consolidation.",
	.tags         = { "ibal", "lite", "fast", "loop", "quick", "plan" },
	.tag_count    = 6,
	.node_count   = 3,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_CELL_CALL,
			.label = "ibal-lite",
			.params.cell_call = {
				.intent = "anx:cell/loop/ibal-runner-lite goal=${goal}",
			},
			.port_count = 2,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "out", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_OUTPUT,
			.label = "plan",
			.params.output = { .dest_name = "lite_plan" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 2,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
	},
};

/*
 * 11. anx:workflow/ibal/symbolic/v1
 *     TRIGGER → CELL_CALL(ibal-symbolic) → OUTPUT
 *     Symbolic IBAL: goal decomposition + rule-based proposal generation.
 */
static const struct anx_wf_template tmpl_ibal_symbolic = {
	.uri          = "anx:workflow/ibal/symbolic/v1",
	.display_name = "IBAL Symbolic",
	.description  = "Symbolic IBAL loop: rule-based proposal generation without learned JEPA weights.",
	.tags         = { "ibal", "symbolic", "rules", "deterministic", "loop", "plan" },
	.tag_count    = 6,
	.node_count   = 3,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_CELL_CALL,
			.label = "ibal-symbolic",
			.params.cell_call = {
				.intent = "anx:cell/loop/ibal-runner-symbolic goal=${goal}",
			},
			.port_count = 2,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "out", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_OUTPUT,
			.label = "plan",
			.params.output = { .dest_name = "symbolic_plan" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 2,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
	},
};

/*
 * 12. anx:workflow/jepa/observe-encode/v1
 *     TRIGGER → CELL_CALL(jepa-observe) → CELL_CALL(jepa-encode) → OUTPUT
 *     Capture a JEPA observation and encode it to a latent vector.
 *     Useful as the first stage of a prediction or anomaly-detection pipeline.
 */
static const struct anx_wf_template tmpl_jepa_observe_encode = {
	.uri          = "anx:workflow/jepa/observe-encode/v1",
	.display_name = "JEPA Observe + Encode",
	.description  = "Capture a world observation via JEPA and encode it to "
			"a latent vector.",
	.tags         = {
		"jepa", "observe", "encode", "latent", "world", "state",
	},
	.tag_count    = 6,
	.node_count   = 4,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_CELL_CALL,
			.label = "observe",
			.params.cell_call = { .intent = "jepa-observe" },
			.port_count = 2,
			.ports = {
				{ .name = "in",  .dir = ANX_WF_PORT_IN  },
				{ .name = "obs", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_CELL_CALL,
			.label = "encode",
			.params.cell_call = { .intent = "jepa-encode" },
			.port_count = 2,
			.ports = {
				{ .name = "obs",    .dir = ANX_WF_PORT_IN  },
				{ .name = "latent", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 4, .kind = ANX_WF_NODE_OUTPUT,
			.label = "latent",
			.params.output = { .dest_name = "jepa_latent" },
			.port_count = 1,
			.ports = {{ .name = "in", .dir = ANX_WF_PORT_IN }},
		},
	},
	.edge_count = 3,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
		{ .from_node = 3, .from_port = 1, .to_node = 4, .to_port = 0 },
	},
};

/*
 * 13. anx:workflow/jepa/predict-route/v1
 *     TRIGGER → jepa-observe-encode → jepa-predict:route_local → OUTPUT
 *     Full single-step prediction pipeline: observe current world state,
 *     encode to latent, then predict the next latent for the route_local
 *     action.  Used by the IBAL loop to generate JEPA proposals.
 */
static const struct anx_wf_template tmpl_jepa_predict_route = {
	.uri          = "anx:workflow/jepa/predict-route/v1",
	.display_name = "JEPA Predict (route_local)",
	.description  = "Observe + encode world state, then predict the next "
			"latent under the route_local action.",
	.tags         = {
		"jepa", "predict", "route", "latent", "planning", "ibal",
	},
	.tag_count    = 6,
	.node_count   = 3,
	.nodes = {
		{
			.id = 1, .kind = ANX_WF_NODE_TRIGGER,
			.label = "start",
			.port_count = 1,
			.ports = {{ .name = "out", .dir = ANX_WF_PORT_OUT }},
		},
		{
			.id = 2, .kind = ANX_WF_NODE_CELL_CALL,
			.label = "observe-encode",
			.params.cell_call = { .intent = "jepa-observe-encode" },
			.port_count = 2,
			.ports = {
				{ .name = "in",     .dir = ANX_WF_PORT_IN  },
				{ .name = "latent", .dir = ANX_WF_PORT_OUT },
			},
		},
		{
			.id = 3, .kind = ANX_WF_NODE_CELL_CALL,
			.label = "predict",
			.params.cell_call = { .intent = "jepa-predict:route_local" },
			.port_count = 2,
			.ports = {
				{ .name = "latent",    .dir = ANX_WF_PORT_IN  },
				{ .name = "predicted", .dir = ANX_WF_PORT_OUT },
			},
		},
	},
	.edge_count = 2,
	.edges = {
		{ .from_node = 1, .from_port = 0, .to_node = 2, .to_port = 0 },
		{ .from_node = 2, .from_port = 1, .to_node = 3, .to_port = 0 },
	},
};

/* All built-in templates in order. */
static const struct anx_wf_template *g_builtins[] = {
	&tmpl_infer,
	&tmpl_summarize,
	&tmpl_rag,
	&tmpl_chain,
	&tmpl_ensemble,
	&tmpl_spawn_collect,
	&tmpl_decompose,
	&tmpl_observe,
	&tmpl_ibal_default,
	&tmpl_ibal_lite,
	&tmpl_ibal_symbolic,
	&tmpl_jepa_observe_encode,
	&tmpl_jepa_predict_route,
};
#define BUILTIN_COUNT ((uint32_t)(sizeof(g_builtins) / sizeof(g_builtins[0])))

/* ------------------------------------------------------------------ */
/* Keyword matching helpers                                            */
/* ------------------------------------------------------------------ */

static void lower_word(const char *src, char *dst, uint32_t max)
{
	uint32_t i = 0;

	for (; src[i] && i < max - 1; i++) {
		char c = src[i];
		dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	dst[i] = '\0';
}

static uint32_t score_template(const struct anx_wf_template *t,
				const char *keywords)
{
	uint32_t score = 0;
	char word[ANX_WF_LIB_TAG_MAX];
	const char *p = keywords;
	uint32_t wi, ti;

	while (*p) {
		/* skip whitespace */
		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		if (!*p)
			break;

		/* extract one word */
		wi = 0;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
		       wi < sizeof(word) - 1)
			word[wi++] = *p++;
		word[wi] = '\0';
		if (wi == 0)
			continue;

		/* lowercase */
		lower_word(word, word, sizeof(word));

		/* compare against all tags */
		for (ti = 0; ti < t->tag_count; ti++) {
			if (anx_strcmp(word, t->tags[ti]) == 0) {
				score++;
				break;
			}
		}
	}
	return score;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int anx_wf_lib_init(void)
{
	uint32_t i;
	int rc;

	for (i = 0; i < BUILTIN_COUNT; i++) {
		rc = anx_wf_lib_register(g_builtins[i]);
		if (rc != ANX_OK && rc != ANX_EEXIST)
			return rc;
	}

	kprintf("[wf-lib] registered %u built-in workflow templates\n",
		BUILTIN_COUNT);
	return ANX_OK;
}

int anx_wf_lib_register(const struct anx_wf_template *tmpl)
{
	if (!tmpl || tmpl->uri[0] == '\0')
		return ANX_EINVAL;
	if (g_lib_count >= ANX_WF_LIB_MAX)
		return ANX_ENOMEM;

	/* Reject duplicates */
	{
		uint32_t i;

		for (i = 0; i < g_lib_count; i++) {
			if (anx_strcmp(g_lib[i]->uri, tmpl->uri) == 0)
				return ANX_EEXIST;
		}
	}

	g_lib[g_lib_count++] = tmpl;
	return ANX_OK;
}

const struct anx_wf_template *anx_wf_lib_lookup(const char *uri)
{
	uint32_t i;

	if (!uri)
		return NULL;
	for (i = 0; i < g_lib_count; i++) {
		if (anx_strcmp(g_lib[i]->uri, uri) == 0)
			return g_lib[i];
	}
	return NULL;
}

int anx_wf_lib_list(const char **uris_out, uint32_t max_count,
		    uint32_t *found_out)
{
	uint32_t i, n;

	if (!uris_out || !found_out)
		return ANX_EINVAL;

	n = (g_lib_count < max_count) ? g_lib_count : max_count;
	for (i = 0; i < n; i++)
		uris_out[i] = g_lib[i]->uri;
	*found_out = g_lib_count;
	return ANX_OK;
}

uint32_t anx_wf_lib_match(const char *keywords,
			  struct anx_wf_match *matches_out,
			  uint32_t max_count)
{
	uint32_t i, found = 0;
	struct anx_wf_match tmp;

	if (!keywords || !matches_out || max_count == 0)
		return 0;

	/* Score each template, collect into matches_out (insertion sort). */
	for (i = 0; i < g_lib_count; i++) {
		uint32_t s = score_template(g_lib[i], keywords);
		uint32_t j;

		if (s == 0)
			continue;

		if (found < max_count) {
			matches_out[found].uri   = g_lib[i]->uri;
			matches_out[found].score = s;
			found++;
		} else if (s > matches_out[max_count - 1].score) {
			matches_out[max_count - 1].uri   = g_lib[i]->uri;
			matches_out[max_count - 1].score = s;
		} else {
			continue;
		}

		/* Bubble the new entry into sorted order (best-first). */
		j = found - 1;
		while (j > 0 && matches_out[j].score > matches_out[j - 1].score) {
			tmp = matches_out[j - 1];
			matches_out[j - 1] = matches_out[j];
			matches_out[j]     = tmp;
			j--;
		}
	}

	return found;
}

int anx_wf_lib_instantiate(const char *uri, const char *name,
			   anx_oid_t *wf_oid_out)
{
	const struct anx_wf_template *tmpl;
	anx_oid_t wf_oid;
	uint32_t i;
	int rc;

	if (!uri || !name || !wf_oid_out)
		return ANX_EINVAL;

	tmpl = anx_wf_lib_lookup(uri);
	if (!tmpl)
		return ANX_ENOENT;

	rc = anx_wf_create(name, tmpl->description, &wf_oid);
	if (rc != ANX_OK)
		return rc;

	for (i = 0; i < tmpl->node_count; i++) {
		uint16_t assigned_id;

		rc = anx_wf_node_add(&wf_oid, &tmpl->nodes[i], &assigned_id);
		if (rc != ANX_OK)
			goto fail;
	}

	for (i = 0; i < tmpl->edge_count; i++) {
		rc = anx_wf_edge_add(&wf_oid,
				     tmpl->edges[i].from_node,
				     tmpl->edges[i].from_port,
				     tmpl->edges[i].to_node,
				     tmpl->edges[i].to_port);
		if (rc != ANX_OK)
			goto fail;
	}

	*wf_oid_out = wf_oid;
	return ANX_OK;

fail:
	anx_wf_destroy(&wf_oid);
	return rc;
}
