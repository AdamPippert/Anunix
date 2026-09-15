#ifndef ANX_WORKLOAD_H
#define ANX_WORKLOAD_H
#include <anx/model_use.h>
#define ANX_WORKLOAD_MAX 16U
#define ANX_WORKLOAD_CANDIDATES 8U
struct anx_workload_contract {
	uint32_t minimum_output, maximum_output, maximum_tokens, prefix_size;
	char prefix[129];
};
struct anx_workload_view {
	uint64_t id, epoch;
	anx_cid_t owner;
	uint32_t candidates, active, token_limit;
	uint64_t executions, charged_tokens;
	int last_result;
};
int anx_workload_create(const anx_cid_t *owner, const struct anx_workload_contract *contract, struct anx_workload_view *out);
int anx_workload_add(uint64_t id, uint64_t sample_use, struct anx_workload_view *out);
int anx_workload_select(uint64_t id, uint64_t epoch, uint32_t candidate, uint32_t token_limit, struct anx_workload_view *out);
int anx_workload_execute(uint64_t id, uint64_t epoch, uint64_t use, struct anx_anxml_response *response, struct anx_workload_view *out);
int anx_workload_get(uint64_t id, struct anx_workload_view *out);
int anx_workload_destroy(uint64_t id);
#endif
