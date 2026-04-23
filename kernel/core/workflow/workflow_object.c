/*
 * workflow_object.c — Workflow Object lifecycle management (RFC-0018).
 *
 * Manages a static registry of up to ANX_WF_MAX_WFS workflow objects.
 * OIDs are generated from a monotonic counter (Phase 1 — full UUID v7 later).
 * Node and edge arrays are heap-allocated at create time and freed on destroy.
 */

#include <anx/types.h>
#include <anx/workflow.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/hwprobe.h>
#include <anx/engine_lease.h>

static struct anx_wf_object	wf_table[ANX_WF_MAX_WFS];
static uint32_t			wf_count;
static bool			wf_initialized;
static uint64_t			wf_oid_seq;	/* monotonic OID counter */

static anx_oid_t
wf_oid_generate(void)
{
	anx_oid_t oid;

	oid.hi = 0x414e5857464c4f57ULL;	/* "ANXWFLOW" */
	oid.lo = ++wf_oid_seq;
	return oid;
}

static bool
wf_oid_eq(const anx_oid_t *a, const anx_oid_t *b)
{
	return a->hi == b->hi && a->lo == b->lo;
}

/* Initialize the workflow subsystem (called at kernel boot). */
int
anx_wf_init(void)
{
	anx_memset(wf_table, 0, sizeof(wf_table));
	wf_count = 0;
	wf_oid_seq = 0;
	wf_initialized = true;
	kprintf("wf: subsystem initialized (max %u workflows)\n",
		(uint32_t)ANX_WF_MAX_WFS);
	return ANX_OK;
}

/* Look up workflow object by OID — returns internal pointer (valid until destroy). */
struct anx_wf_object *
anx_wf_object_get(const anx_oid_t *oid)
{
	uint32_t i;

	if (!wf_initialized || !oid)
		return NULL;

	for (i = 0; i < ANX_WF_MAX_WFS; i++) {
		if (wf_table[i].in_use &&
		    wf_oid_eq(&wf_table[i].oid, oid))
			return &wf_table[i];
	}
	return NULL;
}

/* Create a new empty workflow. */
int
anx_wf_create(const char *name, const char *description, anx_oid_t *oid_out)
{
	struct anx_wf_object *wf;
	uint32_t i;

	if (!name || name[0] == '\0' || !oid_out)
		return ANX_EINVAL;
	if (wf_count >= ANX_WF_MAX_WFS)
		return ANX_ENOMEM;

	/* Reject duplicate names. */
	for (i = 0; i < ANX_WF_MAX_WFS; i++) {
		if (wf_table[i].in_use &&
		    anx_strncmp(wf_table[i].name, name, ANX_WF_NAME_MAX) == 0)
			return ANX_EEXIST;
	}

	/* Find a free slot. */
	wf = NULL;
	for (i = 0; i < ANX_WF_MAX_WFS; i++) {
		if (!wf_table[i].in_use) {
			wf = &wf_table[i];
			break;
		}
	}
	if (!wf)
		return ANX_ENOMEM;

	/* Allocate node and edge arrays. */
	wf->nodes = anx_alloc(sizeof(struct anx_wf_node) * ANX_WF_MAX_NODES);
	if (!wf->nodes)
		return ANX_ENOMEM;

	wf->edges = anx_alloc(sizeof(struct anx_wf_edge) * ANX_WF_MAX_EDGES);
	if (!wf->edges) {
		anx_free(wf->nodes);
		wf->nodes = NULL;
		return ANX_ENOMEM;
	}

	anx_memset(wf->nodes, 0, sizeof(struct anx_wf_node) * ANX_WF_MAX_NODES);
	anx_memset(wf->edges, 0, sizeof(struct anx_wf_edge) * ANX_WF_MAX_EDGES);

	wf->in_use	= true;
	wf->oid		= wf_oid_generate();
	anx_strlcpy(wf->name, name, ANX_WF_NAME_MAX);
	if (description)
		anx_strlcpy(wf->description, description, sizeof(wf->description));
	else
		wf->description[0] = '\0';

	wf->node_count	= 0;
	wf->edge_count	= 0;
	wf->run_state	= ANX_WF_RUN_IDLE;

	wf->policy.max_parallel	= 0;	/* 0 = derive from host resources */
	wf->policy.timeout_ms	= 0;
	wf->policy.auto_retry	= false;
	wf->policy.max_retries	= 0;

	wf->computed_cap	= anx_wf_cap_compute(&wf->policy);
	wf->continuation	= NULL;
	wf->trace_entries	= NULL;
	wf->trace_entry_count	= 0;
	wf->output_count	= 0;

	*oid_out = wf->oid;
	wf_count++;

	kprintf("wf: created '%s' (cap %u)\n", wf->name, wf->computed_cap);
	return ANX_OK;
}

/* Destroy a workflow and free its node/edge storage. */
int
anx_wf_destroy(const anx_oid_t *oid)
{
	struct anx_wf_object *wf;

	wf = anx_wf_object_get(oid);
	if (!wf)
		return ANX_ENOENT;
	if (wf->run_state == ANX_WF_RUN_RUNNING)
		return ANX_EBUSY;

	anx_free(wf->nodes);
	anx_free(wf->edges);
	anx_free(wf->continuation);
	anx_free(wf->trace_entries);

	anx_memset(wf, 0, sizeof(*wf));
	wf_count--;
	return ANX_OK;
}

/* Add a node; returns assigned node id in *id_out. */
int
anx_wf_node_add(const anx_oid_t *wf_oid, const struct anx_wf_node *spec,
		uint16_t *id_out)
{
	struct anx_wf_object *wf;
	uint32_t i;
	uint16_t id;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;
	if (!spec || !id_out)
		return ANX_EINVAL;
	if (wf->node_count >= ANX_WF_MAX_NODES)
		return ANX_ENOMEM;

	/* Find first unused slot (id == 0 means unused). */
	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		if (wf->nodes[i].id == 0)
			break;
	}
	if (i >= ANX_WF_MAX_NODES)
		return ANX_ENOMEM;

	/* IDs are 1-based; slot index 0 → id 1. */
	id = (uint16_t)(i + 1);

	wf->nodes[i]	 = *spec;
	wf->nodes[i].id	 = id;

	wf->node_count++;
	*id_out = id;
	return ANX_OK;
}

/* Remove a node and all edges connected to it. */
int
anx_wf_node_remove(const anx_oid_t *wf_oid, uint16_t node_id)
{
	struct anx_wf_object *wf;
	uint32_t i;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;

	/* Find the node. */
	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		if (wf->nodes[i].id == node_id)
			break;
	}
	if (i >= ANX_WF_MAX_NODES)
		return ANX_ENOENT;

	anx_memset(&wf->nodes[i], 0, sizeof(wf->nodes[i]));
	wf->node_count--;

	/* Remove all edges that reference this node. */
	for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
		if (wf->edges[i].from_node == node_id ||
		    wf->edges[i].to_node   == node_id) {
			anx_memset(&wf->edges[i], 0, sizeof(wf->edges[i]));
			wf->edge_count--;
		}
	}

	return ANX_OK;
}

/* Connect two node ports. */
int
anx_wf_edge_add(const anx_oid_t *wf_oid, uint16_t from_node, uint8_t from_port,
		uint16_t to_node, uint8_t to_port)
{
	struct anx_wf_object *wf;
	uint32_t i;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;
	if (wf->edge_count >= ANX_WF_MAX_EDGES)
		return ANX_ENOMEM;
	if (from_node == to_node)
		return ANX_EINVAL;

	/* Find first unused edge slot (both nodes == 0 means unused). */
	for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
		if (wf->edges[i].from_node == 0 &&
		    wf->edges[i].to_node   == 0)
			break;
	}
	if (i >= ANX_WF_MAX_EDGES)
		return ANX_ENOMEM;

	wf->edges[i].from_node	= from_node;
	wf->edges[i].from_port	= from_port;
	wf->edges[i].to_node	= to_node;
	wf->edges[i].to_port	= to_port;

	wf->edge_count++;
	return ANX_OK;
}

/* Remove an edge. */
int
anx_wf_edge_remove(const anx_oid_t *wf_oid, uint16_t from_node, uint8_t from_port,
		   uint16_t to_node, uint8_t to_port)
{
	struct anx_wf_object *wf;
	uint32_t i;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;

	for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
		if (wf->edges[i].from_node == from_node &&
		    wf->edges[i].from_port == from_port &&
		    wf->edges[i].to_node   == to_node   &&
		    wf->edges[i].to_port   == to_port) {
			anx_memset(&wf->edges[i], 0, sizeof(wf->edges[i]));
			wf->edge_count--;
			return ANX_OK;
		}
	}
	return ANX_ENOENT;
}

/* List all workflows. */
int
anx_wf_list(anx_oid_t *results, uint32_t max, uint32_t *count_out)
{
	uint32_t i, n = 0;

	if (!results || !count_out)
		return ANX_EINVAL;

	for (i = 0; i < ANX_WF_MAX_WFS && n < max; i++) {
		if (wf_table[i].in_use)
			results[n++] = wf_table[i].oid;
	}
	*count_out = n;
	return ANX_OK;
}

/* Get current run state. */
int
anx_wf_run_state_get(const anx_oid_t *wf_oid, enum anx_wf_run_state *state_out)
{
	struct anx_wf_object *wf;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;
	if (!state_out)
		return ANX_EINVAL;
	*state_out = wf->run_state;
	return ANX_OK;
}

/* Return the continuation for a SUSPENDED workflow, or NULL. */
const struct anx_wf_continuation *
anx_wf_continuation_get(const anx_oid_t *oid)
{
	struct anx_wf_object *wf = anx_wf_object_get(oid);

	if (!wf || wf->run_state != ANX_WF_RUN_SUSPENDED)
		return NULL;
	return wf->continuation;
}

/*
 * Compute the concurrency cap from host hardware.
 * If policy->max_parallel > 0 it acts as a ceiling (user can restrict,
 * never expand beyond what the hardware supports).
 */
uint32_t
anx_wf_cap_compute(const struct anx_wf_policy *policy)
{
	const struct anx_hw_inventory *hw = anx_hwprobe_get();
	uint32_t hw_cap;
	uint32_t i;

	if (!hw || hw->cpu_count == 0) {
		hw_cap = 2;
	} else {
		hw_cap = hw->cpu_count / 2;
		if (hw_cap < 1)
			hw_cap = 1;
		/* GPU/NPU accelerators can service additional concurrent cells */
		for (i = 0; i < hw->accel_count; i++) {
			if (hw->accels[i].type == ANX_ACCEL_GPU ||
			    hw->accels[i].type == ANX_ACCEL_NPU)
				hw_cap += 2;
		}
	}

	if (hw_cap > (uint32_t)ANX_WF_MAX_NODES)
		hw_cap = ANX_WF_MAX_NODES;

	if (policy && policy->max_parallel > 0 &&
	    policy->max_parallel < hw_cap)
		return policy->max_parallel;

	return hw_cap;
}
