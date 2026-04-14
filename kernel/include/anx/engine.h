/*
 * anx/engine.h — Engine Registry (RFC-0005 Section 7-8).
 *
 * An Engine is any execution surface capable of completing part of
 * a task. The registry tracks available engines, their capabilities,
 * status, and constraints.
 */

#ifndef ANX_ENGINE_H
#define ANX_ENGINE_H

#include <anx/types.h>
#include <anx/list.h>
#include <anx/spinlock.h>

/* --- Engine classes (RFC-0005 Section 7.1) --- */

enum anx_engine_class {
	ANX_ENGINE_DETERMINISTIC_TOOL,
	ANX_ENGINE_LOCAL_MODEL,
	ANX_ENGINE_REMOTE_MODEL,
	ANX_ENGINE_RETRIEVAL_SERVICE,
	ANX_ENGINE_GRAPH_SERVICE,
	ANX_ENGINE_VALIDATION_SERVICE,
	ANX_ENGINE_EXECUTION_SERVICE,
	ANX_ENGINE_DEVICE_SERVICE,
	ANX_ENGINE_INSTALLED_CAPABILITY,	/* RFC-0007 */
	ANX_ENGINE_CLASS_COUNT,
};

/* --- Engine status --- */

enum anx_engine_status {
	ANX_ENGINE_AVAILABLE,
	ANX_ENGINE_DEGRADED,
	ANX_ENGINE_OFFLINE,
	ANX_ENGINE_MAINTENANCE,
};

/* --- Capability tags (bitmask, RFC-0005 Section 8.4) --- */

#define ANX_CAP_SUMMARIZATION		(1U << 0)
#define ANX_CAP_QUESTION_ANSWERING	(1U << 1)
#define ANX_CAP_STRUCTURED_EXTRACTION	(1U << 2)
#define ANX_CAP_LONG_CONTEXT		(1U << 3)
#define ANX_CAP_SEMANTIC_RETRIEVAL	(1U << 4)
#define ANX_CAP_LEXICAL_RETRIEVAL	(1U << 5)
#define ANX_CAP_GRAPH_TRAVERSAL		(1U << 6)
#define ANX_CAP_SCHEMA_VALIDATION	(1U << 7)
#define ANX_CAP_CONTRADICTION_DETECT	(1U << 8)
#define ANX_CAP_TOOL_EXECUTION		(1U << 9)
#define ANX_CAP_MULTIMODAL_INPUT	(1U << 10)

/* --- Engine struct --- */

struct anx_engine {
	anx_eid_t eid;
	char name[64];
	enum anx_engine_class engine_class;
	enum anx_engine_status status;

	/* Capability bitmask */
	uint32_t capabilities;

	/* Constraints */
	bool supports_private_data;
	bool requires_network;
	uint32_t max_context_tokens;

	/* Cost model (relative weights, 0-100) */
	uint32_t cpu_weight;
	uint32_t gpu_weight;

	/* Quality (0-100) */
	uint32_t quality_score;

	/* Locality */
	bool is_local;

	/* Bookkeeping */
	struct anx_spinlock lock;
	struct anx_list_head registry_link;
};

/* --- Engine Registry API --- */

/* Initialize the engine registry */
void anx_engine_registry_init(void);

/* Register a new engine */
int anx_engine_register(const char *name,
			enum anx_engine_class engine_class,
			uint32_t capabilities,
			struct anx_engine **out);

/* Look up an engine by EID */
struct anx_engine *anx_engine_lookup(const anx_eid_t *eid);

/* Find engines matching a capability mask and class */
int anx_engine_find(enum anx_engine_class engine_class,
		    uint32_t required_caps,
		    struct anx_engine **results,
		    uint32_t max_results,
		    uint32_t *found_count);

/* Update engine status */
int anx_engine_set_status(struct anx_engine *engine,
			  enum anx_engine_status status);

/* Unregister an engine */
int anx_engine_unregister(struct anx_engine *engine);

#endif /* ANX_ENGINE_H */
