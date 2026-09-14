#ifndef ANX_EXTERNAL_OPERATION_H
#define ANX_EXTERNAL_OPERATION_H
#include <anx/external_call.h>
#include <anx/effect.h>
#define ANX_EXT_OPERATION_MAX 32U
#define ANX_EXT_OPERATION_BODY_MAX 4096U
struct anx_external_operation_view {
	anx_oid_t id;
	anx_cid_t owner;
	enum anx_effect_phase phase;
	int transport_result;
	struct anx_hash request_hash;
	anx_oid_t source;
	uint64_t source_version;
	struct anx_hash source_hash;
	char sink_name[64];
};
/* Controller preparation copies the complete request and binds its destination. */
int anx_external_operation_prepare(const anx_cid_t *owner, const struct anx_external_call *call,
		const char *sink_name, const anx_oid_t *source, anx_oid_t *id);
/* The executing owner dispatches once; ambiguous provider errors become UNKNOWN. */
int anx_external_operation_dispatch(const anx_oid_t *id, struct anx_external_call *response);
int anx_external_operation_get(const anx_oid_t *id, struct anx_external_operation_view *out);
/* Controller cleanup allows PREPARED and COMMITTED; UNKNOWN requires reconciliation. */
int anx_external_operation_discard(const anx_oid_t *id);
#endif
