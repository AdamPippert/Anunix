#ifndef ANX_CONTINUATION_H
#define ANX_CONTINUATION_H
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/adapter.h>
#include <anx/resource_view.h>
#define ANX_CONTINUATION_MAX 16U
#define ANX_CONTINUATION_CAPABILITIES 4U
#define ANX_CONTINUATION_OPERATIONS 8U
#define ANX_CONTINUATION_EVENTS 32U
#define ANX_CONTINUATION_SCHEMA "anx:continuation/event/v1"
#define ANX_CONTINUATION_RESULT_SCHEMA "anx:continuation/result/v1"
enum anx_continuation_event_kind { ANX_CONT_BIND, ANX_CONT_INTENT, ANX_CONT_COMMITTED, ANX_CONT_UNCERTAIN, ANX_CONT_REJECTED,
	ANX_CONT_RESOURCE_CONFIG, ANX_CONT_WAIT_EVENT, ANX_CONT_SUSPEND_EVENT, ANX_CONT_HIBERNATE_EVENT, ANX_CONT_RESUME_EVENT };
enum anx_continuation_resource_state { ANX_CONT_RUNNABLE, ANX_CONT_SHORT_WAIT, ANX_CONT_SUSPENDED, ANX_CONT_HIBERNATED };
enum anx_suspension_reason { ANX_SUSPEND_NONE, ANX_SUSPEND_TOOL_WAIT, ANX_SUSPEND_HUMAN_APPROVAL };
struct anx_continuation_view {
	uint64_t id, epoch;
	anx_cid_t owner;
	anx_cid_t workers[ANX_CONTINUATION_CAPABILITIES];
	uint64_t generations[ANX_CONTINUATION_CAPABILITIES];
	uint32_t events, operations, completed, uncertain;
	anx_oid_t head;
	enum anx_continuation_resource_state resource_state;
	enum anx_suspension_reason suspension_reason;
	anx_oid_t semantic_checkpoint, model_source;
	uint64_t phase_epoch, cache_generation;
	uint32_t physical_pages;
};
struct anx_continuation_event {
	uint64_t continuation, epoch, key, generation;
	uint32_t kind, slot;
	anx_cid_t worker;
	anx_oid_t previous, source, result_object;
	uint8_t previous_digest[32], request_digest[32], result_digest[32];
	int result;
};
struct anx_continuation_result {
	uint64_t key;
	int status_code;
	uint32_t size;
	uint8_t bytes[ANX_EXT_RESPONSE_MAX];
};
/* Controller mutations preserve the logical owner across child-worker replacement. */
int anx_continuation_create(const anx_cid_t *owner, struct anx_continuation_view *out);
int anx_continuation_bind(uint64_t id, uint64_t epoch, uint32_t slot, const anx_cid_t *worker, struct anx_continuation_view *out);
int anx_continuation_dispatch(uint64_t id, uint64_t epoch, uint32_t slot, uint64_t key,
		const struct anx_external_call *call, const char *sink, const anx_oid_t *source, struct anx_continuation_view *out);
/* Owner/controller reads revalidate the sealed transcript and current authority. */
int anx_continuation_get(uint64_t id, struct anx_continuation_view *out);
int anx_continuation_event_get(uint64_t id, uint32_t index, struct anx_continuation_event *out);
int anx_continuation_read(uint64_t id, uint64_t key, struct anx_continuation_result *out);
int anx_continuation_destroy(uint64_t id);
/* Bind one reconstructible toy-model materialization and an existing active phase. */
int anx_continuation_suspend_configure(uint64_t id, uint64_t epoch, uint64_t phase_epoch,
		const anx_oid_t *model, struct anx_continuation_view *out);
int anx_continuation_pause(uint64_t id, uint64_t epoch, enum anx_suspension_reason reason, struct anx_continuation_view *out);
int anx_continuation_hibernate(uint64_t id, uint64_t epoch, struct anx_continuation_view *out);
int anx_continuation_resume(uint64_t id, uint64_t epoch, struct anx_continuation_view *out);
int anx_continuation_cache_stats(uint64_t id, struct anx_resource_pool_stats *out);
int anx_continuation_acceleration_read(uint64_t id, uint64_t epoch, uint64_t generation, struct anx_adapter_image *out);
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
/* Drop one completion reply after sealing its result; production builds omit this hook. */
int anx_continuation_test_drop_reply(bool enabled);
int anx_continuation_test_cache_corrupt(uint64_t id);
int anx_continuation_test_restore_fault(bool enabled);
#endif
#endif
