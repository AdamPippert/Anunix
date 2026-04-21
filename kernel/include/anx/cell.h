/*
 * anx/cell.h — Execution Cell Runtime (RFC-0003).
 *
 * The Execution Cell is the canonical unit of work in Anunix,
 * replacing the POSIX process with a policy-bound, provenance-producing,
 * routable unit of work.
 */

#ifndef ANX_CELL_H
#define ANX_CELL_H

#include <anx/types.h>
#include <anx/list.h>
#include <anx/spinlock.h>

/* Forward declarations */
struct anx_cell_plan;
struct anx_cell_trace;

/* --- Cell lifecycle states (RFC-0003 Section 16) --- */

enum anx_cell_status {
	ANX_CELL_CREATED,
	ANX_CELL_ADMITTED,
	ANX_CELL_PLANNING,
	ANX_CELL_PLANNED,
	ANX_CELL_QUEUED,
	ANX_CELL_RUNNING,
	ANX_CELL_WAITING,
	ANX_CELL_VALIDATING,
	ANX_CELL_COMMITTING,
	ANX_CELL_COMPLETED,
	ANX_CELL_FAILED,
	ANX_CELL_CANCELLED,
	ANX_CELL_COMPENSATING,
	ANX_CELL_COMPENSATED,
	ANX_CELL_STATUS_COUNT,
};

/* --- Cell type families (RFC-0003 Section 7) --- */

enum anx_cell_type {
	/* Direct execution */
	ANX_CELL_TASK_EXECUTION,
	ANX_CELL_TASK_BATCH_EXECUTION,
	ANX_CELL_TASK_STREAM_EXECUTION,

	/* Planning and decomposition */
	ANX_CELL_TASK_PLAN_GENERATION,
	ANX_CELL_TASK_DECOMPOSITION,

	/* Retrieval and memory */
	ANX_CELL_TASK_RETRIEVAL,
	ANX_CELL_TASK_MEMORY_UPDATE,
	ANX_CELL_TASK_GRAPH_UPDATE,

	/* Validation */
	ANX_CELL_TASK_VALIDATION,
	ANX_CELL_TASK_CONSISTENCY_CHECK,
	ANX_CELL_TASK_POLICY_CHECK,

	/* Action */
	ANX_CELL_TASK_SIDE_EFFECT,
	ANX_CELL_TASK_EXTERNAL_CALL,

	/* Control */
	ANX_CELL_TASK_ROUTER,
	ANX_CELL_TASK_SCHEDULER_BINDING,
	ANX_CELL_TASK_COMPENSATION,

	/* Model hosting */
	ANX_CELL_MODEL_SERVER,

	ANX_CELL_TYPE_COUNT,
};

/* --- Intent (RFC-0003 Section 8) --- */

struct anx_cell_intent {
	char name[128];
	char objective[512];
	uint32_t priority;		/* 0 = default, higher = more important */
};

/* --- Constraints (RFC-0003 Section 10) --- */

enum anx_locality {
	ANX_LOCAL_ONLY,
	ANX_LOCAL_PREFERRED,
	ANX_REMOTE_ALLOWED,
	ANX_REMOTE_REQUIRED,
};

struct anx_cell_constraints {
	uint64_t max_latency_ms;	/* 0 = unlimited */
	uint32_t max_cost_usd_cents;	/* 0 = unlimited */
	enum anx_locality locality;
	uint32_t max_recursion_depth;
	uint32_t max_child_cells;

	/*
	 * Topology intent: the boundary-key range this cell expects
	 * to read or write. The planner uses this to prefer engines
	 * whose declared affinity overlaps. Both bounds inclusive.
	 * Set topology_bk_set before the range is meaningful.
	 */
	bool topology_bk_set;
	uint64_t topology_bk_lo;
	uint64_t topology_bk_hi;
};

/* --- Routing policy (RFC-0003 Section 11) --- */

enum anx_routing_strategy {
	ANX_ROUTE_DIRECT,
	ANX_ROUTE_LOCAL_FIRST,
	ANX_ROUTE_COST_FIRST,
	ANX_ROUTE_LATENCY_FIRST,
	ANX_ROUTE_CONFIDENCE_FIRST,
	ANX_ROUTE_ADAPTIVE,
	ANX_ROUTE_POLICY_LOCKED,
};

enum anx_decomp_mode {
	ANX_DECOMP_NONE,
	ANX_DECOMP_STATIC,
	ANX_DECOMP_ADAPTIVE,
	ANX_DECOMP_RECURSIVE,
};

struct anx_routing_policy {
	enum anx_routing_strategy strategy;
	enum anx_decomp_mode decomposition;
};

/* --- Validation policy (RFC-0003 Section 12) --- */

enum anx_validation_mode {
	ANX_VALIDATE_NONE,
	ANX_VALIDATE_SCHEMA_ONLY,
	ANX_VALIDATE_LIGHT,
	ANX_VALIDATE_STRICT,
	ANX_VALIDATE_DOMAIN_SPECIFIC,
};

struct anx_validation_policy {
	enum anx_validation_mode mode;
	bool block_commit_on_failure;
};

/* --- Commit policy (RFC-0003 Section 13) --- */

struct anx_commit_policy {
	bool persist_outputs;
	bool promote_to_memory;
	bool write_trace;
	bool allow_side_effects;
};

/* --- Execution policy (RFC-0003 Section 14) --- */

struct anx_execution_policy {
	bool allow_network;
	bool allow_remote_models;
	bool allow_recursive_cells;
	bool allow_side_effects;
	uint32_t max_recursion_depth;
};

/* --- Retry policy (RFC-0003 Section 22) --- */

struct anx_retry_policy {
	uint32_t max_attempts;
	uint32_t backoff_base_ms;	/* base for exponential backoff */
};

/* --- Input binding (RFC-0003 Section 9) --- */

enum anx_input_mode {
	ANX_INPUT_READ,
	ANX_INPUT_READ_WRITE,
	ANX_INPUT_APPEND,
};

struct anx_cell_input {
	char name[64];
	anx_oid_t state_object_ref;
	enum anx_input_mode mode;
	bool required;
};

#define ANX_MAX_CELL_INPUTS	16
#define ANX_MAX_CELL_OUTPUTS	16
#define ANX_MAX_CHILD_CELLS	32
#define ANX_MAX_CELL_DEPS	16

/* --- The Execution Cell --- */

struct anx_cell {
	/* Identity */
	anx_cid_t cid;
	enum anx_cell_type cell_type;
	enum anx_cell_status status;

	/* Intent and policy */
	struct anx_cell_intent intent;
	struct anx_cell_constraints constraints;
	struct anx_routing_policy routing;
	struct anx_validation_policy validation;
	struct anx_commit_policy commit;
	struct anx_execution_policy execution;
	struct anx_retry_policy retry;

	/* Inputs */
	struct anx_cell_input inputs[ANX_MAX_CELL_INPUTS];
	uint32_t input_count;

	/* Outputs (OIDs of State Objects produced) */
	anx_oid_t output_refs[ANX_MAX_CELL_OUTPUTS];
	uint32_t output_count;

	/* Lineage */
	anx_cid_t parent_cid;
	anx_cid_t child_cids[ANX_MAX_CHILD_CELLS];
	uint32_t child_count;
	uint32_t recursion_depth;

	/*
	 * DAG predecessors (RFC-0003 extension): cells that must reach
	 * ANX_CELL_COMPLETED before this cell may run. Sibling edges
	 * enable fan-in; fan-out is implicit (multiple cells can name
	 * the same predecessor).
	 */
	anx_cid_t dep_cids[ANX_MAX_CELL_DEPS];
	uint32_t dep_count;

	/* External-call task payload (RFC-0003 Section 7: EXTERNAL_CALL).
	 * Non-NULL iff cell_type == ANX_CELL_TASK_EXTERNAL_CALL and the
	 * caller has set it. Owned by the cell's creator. */
	struct anx_external_call *ext_call;

	/* Plan and trace refs */
	anx_pid_t plan_id;
	anx_tid_t trace_id;

	/* Runtime state */
	uint32_t attempt_count;
	anx_time_t created_at;
	anx_time_t started_at;
	anx_time_t completed_at;

	/* Error (set on failure) */
	int error_code;
	char error_msg[256];

	/* Kernel bookkeeping */
	struct anx_spinlock lock;
	uint32_t refcount;
	struct anx_list_head store_link;	/* cell_store hash chain */
};

/* --- Cell lifecycle API --- */

/* Check if a cell status transition is valid */
int anx_cell_transition(struct anx_cell *cell, enum anx_cell_status new_status);

/* --- Cell Store API --- */

/* Initialize the global cell store */
void anx_cell_store_init(void);

/* Create a new cell and add it to the store */
int anx_cell_create(enum anx_cell_type type,
		    const struct anx_cell_intent *intent,
		    struct anx_cell **out);

/* Look up a cell by CID (increments refcount) */
struct anx_cell *anx_cell_store_lookup(const anx_cid_t *cid);

/* Release a cell reference (decrements refcount) */
void anx_cell_store_release(struct anx_cell *cell);

/* Destroy a cell (removes from store, must have refcount == 1) */
int anx_cell_destroy(struct anx_cell *cell);

/* Iterate all cells in the store */
typedef int (*anx_cell_iter_fn)(struct anx_cell *cell, void *arg);
int anx_cell_store_iterate(anx_cell_iter_fn cb, void *arg);

/* --- Cell Runtime API --- */

/* Run a cell through the full pipeline: admit→plan→execute→validate→commit */
int anx_cell_run(struct anx_cell *cell);

/* Cancel a running or queued cell */
int anx_cell_cancel(struct anx_cell *cell);

/* Create a child cell derived from a parent */
int anx_cell_derive_child(struct anx_cell *parent,
			  enum anx_cell_type type,
			  const struct anx_cell_intent *intent,
			  struct anx_cell **child_out);

/* --- DAG composition --- */

/*
 * Declare that `cell` depends on `prereq_cid`. The cell will refuse
 * to transition out of CREATED via anx_cell_run until the predecessor
 * has reached COMPLETED. If the predecessor reaches FAILED, this cell
 * is failed on its next run attempt.
 *
 * Returns:
 *   ANX_OK       success
 *   ANX_ENOMEM   dep slot limit reached
 *   ANX_EINVAL   self-dependency, duplicate edge, or cycle detected
 *   ANX_ENOENT   prereq_cid does not resolve in the cell store
 */
int anx_cell_add_dependency(struct anx_cell *cell,
			    const anx_cid_t *prereq_cid);

/*
 * Dependency readiness. Returns:
 *   1   all predecessors COMPLETED (cell may run)
 *   0   one or more predecessors still pending
 *  <0   a predecessor has FAILED/CANCELLED (cell must fail)
 */
int anx_cell_deps_satisfied(const struct anx_cell *cell);

/*
 * Declare the cell's topology intent (boundary-key range). The route
 * planner boosts engines whose declared affinity overlaps.
 * Returns ANX_EINVAL on null cell or bk_hi < bk_lo.
 */
int anx_cell_set_topology(struct anx_cell *cell,
			  uint64_t bk_lo, uint64_t bk_hi);

/* Clear any declared topology intent. */
void anx_cell_clear_topology(struct anx_cell *cell);

#endif /* ANX_CELL_H */
