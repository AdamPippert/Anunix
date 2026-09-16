#ifndef ANX_WORKFLOW_SEMANTIC_H
#define ANX_WORKFLOW_SEMANTIC_H
#include <anx/workflow.h>

#define ANX_SEMANTIC_RESOURCES_MAX 16U
#define ANX_SEMANTIC_REQUIRES_MAX 16U
#define ANX_SEMANTIC_NAME_MAX 64U
#define ANX_SEMANTIC_OBJECT_MAX (1024U * 1024U)
enum anx_semantic_kind {
	ANX_SEMANTIC_PROMPT = 1, ANX_SEMANTIC_MODEL, ANX_SEMANTIC_TOOL,
	ANX_SEMANTIC_POLICY, ANX_SEMANTIC_SKILL, ANX_SEMANTIC_INDEX, ANX_SEMANTIC_EMBEDDING
};
struct anx_semantic_resource {
	char name[ANX_SEMANTIC_NAME_MAX];
	enum anx_semantic_kind kind;
	anx_oid_t oid;
	uint64_t version;
};
/* A controller-declared dependency contract, not a learned compatibility claim. */
struct anx_semantic_requires {
	uint32_t consumer, provider;
	anx_oid_t expected_oid;
	uint64_t expected_version;
};
struct anx_semantic_spec {
	uint32_t resource_count, requires_count;
	struct anx_semantic_resource resources[ANX_SEMANTIC_RESOURCES_MAX];
	struct anx_semantic_requires requires[ANX_SEMANTIC_REQUIRES_MAX];
};

/* Rebinding is controller-only and forbidden while a workflow is active or paused. */
int anx_wf_semantic_bind(const anx_oid_t *workflow, const struct anx_semantic_spec *spec, anx_oid_t *manifest);
int anx_wf_semantic_resolve(const anx_oid_t *workflow, const char *name, anx_oid_t *out);
/* Executor and checkpoint hooks preserve state on admission failure. */
int anx_wf_semantic_check(const struct anx_wf_object *workflow);
anx_oid_t anx_wf_semantic_id(const struct anx_wf_object *workflow);
/* Private declared resources and the manifest remain needed throughout a live run. */
bool anx_wf_semantic_needed(const struct anx_wf_object *workflow, const anx_oid_t *oid);

#endif
