#ifndef ANX_SPECULATION_H
#define ANX_SPECULATION_H
#include <anx/state_object.h>

#define ANX_SPECULATION_MAX 64U
#define ANX_SPECULATION_OWNER_MAX 2U
#define ANX_SPECULATION_ACTION_MAX 256U
#define ANX_SPECULATION_PAYLOAD_MAX (1024U * 1024U)
#define ANX_SPECULATION_BYTES_MAX (16U * 1024U * 1024U)
#define ANX_SPECULATION_LIFETIME_MAX 30000000000ULL
enum anx_speculation_state { ANX_SPECULATION_PREPARED = 1, ANX_SPECULATION_COMMITTED, ANX_SPECULATION_DISCARDED, ANX_SPECULATION_EXPIRED };
struct anx_speculation_request {
	anx_cid_t owner;
	anx_oid_t origin;
	const void *action;
	uint32_t action_size;
	const void *result;
	uint64_t result_size;
	anx_time_t expires_at;
};
struct anx_speculation_view {
	anx_oid_t id, origin;
	anx_cid_t owner;
	uint64_t origin_version, result_size;
	anx_time_t expires_at;
	enum anx_speculation_state state;
	int reason;
};
/* Private object-replacement results; these APIs never invoke external handlers. */
int anx_speculation_prepare(const struct anx_speculation_request *request, anx_oid_t *out);
int anx_speculation_get(const anx_oid_t *id, struct anx_speculation_view *out);
int anx_speculation_commit(const anx_oid_t *id, const void *actual_action, uint32_t action_size);
int anx_speculation_discard(const anx_oid_t *id);
int anx_speculation_release(const anx_oid_t *id);
#endif
