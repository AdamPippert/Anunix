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

/* --- Engine status (hosting-aware lifecycle) --- */

enum anx_engine_status {
	ANX_ENGINE_REGISTERED,	/* known but not loaded */
	ANX_ENGINE_LOADING,	/* weights being loaded into memory */
	ANX_ENGINE_READY,	/* loaded, not yet serving */
	ANX_ENGINE_AVAILABLE,	/* actively serving requests */
	ANX_ENGINE_DEGRADED,	/* serving but impaired */
	ANX_ENGINE_DRAINING,	/* finishing active, rejecting new */
	ANX_ENGINE_UNLOADING,	/* releasing memory/accelerator */
	ANX_ENGINE_OFFLINE,	/* not loaded, not available */
	ANX_ENGINE_MAINTENANCE,	/* administratively disabled */
	ANX_ENGINE_STATUS_COUNT,
};

/* --- Model descriptor (LOCAL_MODEL / REMOTE_MODEL only) --- */

enum anx_quant_format {
	ANX_QUANT_NONE,		/* full precision (fp32/fp16) */
	ANX_QUANT_Q8,
	ANX_QUANT_Q6,
	ANX_QUANT_Q4,
	ANX_QUANT_Q3,
	ANX_QUANT_Q2,
	ANX_QUANT_GGUF,		/* GGUF mixed quantization */
};

struct anx_model_desc {
	uint64_t param_count;		/* total parameters */
	enum anx_quant_format quant;
	uint32_t context_window;	/* max tokens */
	uint32_t bench_tok_per_sec;	/* measured throughput (0 = unknown) */
	uint64_t mem_footprint_bytes;	/* weight memory requirement */
	bool offline_capable;		/* can serve without network */
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

/* Tensor compute capabilities (RFC-0013) */
#define ANX_CAP_TENSOR_INT8		(1U << 16)
#define ANX_CAP_TENSOR_INT32		(1U << 17)
#define ANX_CAP_TENSOR_FP32		(1U << 18)
#define ANX_CAP_TENSOR_BF16		(1U << 19)
#define ANX_CAP_TENSOR_GPU		(1U << 20)	/* discrete GPU (NVIDIA/AMD) */
#define ANX_CAP_TENSOR_NPU		(1U << 21)	/* dedicated NPU (AMD XDNA, ANE) */
#define ANX_CAP_TENSOR_SIMD		(1U << 22)	/* CPU SIMD (AVX-512/NEON) */

/* Forward declaration for lease pointer */
struct anx_engine_lease;

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

	/*
	 * Topology affinity (RFC-0005 extension).
	 *
	 * Engines that specialize in a boundary-key range — for example,
	 * a retrieval service holding a specific vindex shard, or a model
	 * server co-located with a layer-band slice — declare the range
	 * they serve. The planner boosts scores when a cell's topology
	 * intent overlaps, and penalizes when the engine is specialized
	 * elsewhere. Engines without affinity declared are treated as
	 * generalists and are scored neutrally on this axis.
	 */
	bool has_topology_affinity;
	uint64_t topology_bk_lo;
	uint64_t topology_bk_hi;		/* inclusive */

	/* Model descriptor (valid for LOCAL_MODEL / REMOTE_MODEL) */
	struct anx_model_desc model;

	/* Resource lease (non-NULL when loaded) */
	struct anx_engine_lease *lease;

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

/* Update engine status (raw setter, no transition validation) */
int anx_engine_set_status(struct anx_engine *engine,
			  enum anx_engine_status status);

/* Validate and perform an engine status transition */
int anx_engine_transition(struct anx_engine *engine,
			  enum anx_engine_status new_status);

/* Register a model engine with full descriptor */
int anx_engine_register_model(const char *name,
			      enum anx_engine_class engine_class,
			      uint32_t capabilities,
			      const struct anx_model_desc *model,
			      struct anx_engine **out);

/* Unregister an engine */
int anx_engine_unregister(struct anx_engine *engine);

/*
 * Declare a topology affinity range on an engine. Both endpoints
 * are inclusive. Returns ANX_EINVAL on null engine or bk_hi < bk_lo.
 */
int anx_engine_set_topology(struct anx_engine *engine,
			    uint64_t bk_lo, uint64_t bk_hi);

/* Clear any declared topology affinity. */
void anx_engine_clear_topology(struct anx_engine *engine);

#endif /* ANX_ENGINE_H */
