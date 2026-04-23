/*
 * anx/workflow_library.h — Built-in workflow template registry.
 *
 * Workflow templates are named, pre-built DAGs that agents can
 * instantiate by URI.  Each template carries keyword tags so that
 * anx_wf_lib_match() can suggest the best-fitting template given a
 * natural-language goal string.
 *
 * Built-in templates (registered at boot by anx_wf_lib_init()):
 *
 *   anx:workflow/infer             — single MODEL_CALL pipeline
 *   anx:workflow/summarize         — MODEL_CALL with summarize prompt
 *   anx:workflow/rag               — RETRIEVAL + grounded MODEL_CALL
 *   anx:workflow/chain             — two-stage MODEL_CALL chain
 *   anx:workflow/ensemble          — FAN_OUT → 3×MODEL_CALL → FAN_IN
 *   anx:workflow/spawn-collect     — CELL_CALL + collect outputs
 *   anx:workflow/decompose-goal    — MODEL_CALL that outputs a plan
 *   anx:workflow/observe-report    — JEPA obs + MODEL_CALL analysis
 */

#ifndef ANX_WORKFLOW_LIBRARY_H
#define ANX_WORKFLOW_LIBRARY_H

#include <anx/types.h>
#include <anx/workflow.h>

#define ANX_WF_LIB_URI_MAX	128
#define ANX_WF_LIB_TAG_MAX	32
#define ANX_WF_LIB_TAGS		8
#define ANX_WF_LIB_MAX		32	/* max registered templates */

/* ------------------------------------------------------------------ */
/* Template descriptor                                                 */
/* ------------------------------------------------------------------ */

struct anx_wf_template {
	char uri[ANX_WF_LIB_URI_MAX];
	char display_name[64];
	char description[256];

	char tags[ANX_WF_LIB_TAGS][ANX_WF_LIB_TAG_MAX];
	uint32_t tag_count;

	struct anx_wf_node nodes[ANX_WF_MAX_NODES];
	uint32_t node_count;

	struct anx_wf_edge edges[ANX_WF_MAX_EDGES];
	uint32_t edge_count;
};

/* ------------------------------------------------------------------ */
/* Scored match result                                                 */
/* ------------------------------------------------------------------ */

struct anx_wf_match {
	const char *uri;
	uint32_t    score;	/* number of keyword hits */
};

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Register built-in templates.  Called once by kernel init. */
int anx_wf_lib_init(void);

/* Register a custom template (must remain valid for subsystem lifetime). */
int anx_wf_lib_register(const struct anx_wf_template *tmpl);

/* Look up a template by exact URI. Returns NULL if not found. */
const struct anx_wf_template *anx_wf_lib_lookup(const char *uri);

/* Fill uris_out with up to max_count registered URIs.  *found_out = total. */
int anx_wf_lib_list(const char **uris_out, uint32_t max_count,
		    uint32_t *found_out);

/*
 * Score registered templates against space-separated keywords.
 * Fills matches_out (sorted best-first) with up to max_count results.
 * Returns the number of matches with score > 0.
 */
uint32_t anx_wf_lib_match(const char *keywords,
			  struct anx_wf_match *matches_out,
			  uint32_t max_count);

/*
 * Instantiate a template: create a fresh workflow object named `name`
 * with all template nodes and edges copied in.  The OID of the new
 * workflow is written to *wf_oid_out.
 */
int anx_wf_lib_instantiate(const char *uri, const char *name,
			   anx_oid_t *wf_oid_out);

#endif /* ANX_WORKFLOW_LIBRARY_H */
