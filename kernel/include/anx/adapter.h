#ifndef ANX_ADAPTER_H
#define ANX_ADAPTER_H
#include <anx/anxml.h>
#define ANX_ADAPTER_MAX 16U
#define ANX_ADAPTER_DELTA_MAX 16U
#define ANX_ADAPTER_TRIAL_MAX 4U
struct anx_adapter_delta { uint8_t previous, next; uint16_t boost; };
struct anx_adapter_image { uint32_t format, count; struct anx_adapter_delta deltas[ANX_ADAPTER_DELTA_MAX]; };
struct anx_adapter_view { anx_oid_t id; uint64_t generation; struct anx_hash image_hash; };
struct anx_adapter_cache_key { struct anx_adapter_view version; struct anx_hash request_hash; };
struct anx_adapter_trial { struct anx_anxml_request request; char expected[129]; uint32_t expected_len; };
/* Mutable operations are controller-only. Reads require the owner or controller. */
int anx_adapter_create(const anx_cid_t *owner, const struct anx_adapter_image *image, struct anx_adapter_view *out);
int anx_adapter_get(const anx_oid_t *id, struct anx_adapter_view *out);
int anx_adapter_stage(const anx_oid_t *id, uint64_t generation, const struct anx_adapter_image *image, uint64_t *candidate);
int anx_adapter_verify(const anx_oid_t *id, uint64_t candidate, const struct anx_adapter_trial *trials, uint32_t count);
int anx_adapter_publish(const anx_oid_t *id, uint64_t candidate, uint64_t generation, struct anx_adapter_view *out);
int anx_adapter_abort(const anx_oid_t *id, uint64_t candidate);
int anx_adapter_rollback(const anx_oid_t *id, uint64_t generation, struct anx_adapter_view *out);
int anx_adapter_destroy(const anx_oid_t *id);
int anx_adapter_generate(const anx_oid_t *id, uint64_t generation, const struct anx_anxml_request *request,
		struct anx_anxml_response *response, struct anx_adapter_cache_key *key);
int anx_adapter_cache_check(const anx_oid_t *id, const struct anx_anxml_request *request, const struct anx_adapter_cache_key *key);
/* Internal CPU backend helper; validates a bounded immutable delta image. */
int anx_adapter_image_check(const struct anx_adapter_image *image);
int anx_anxml_generate_image(const struct anx_anxml_request *request, const struct anx_adapter_image *image,
		struct anx_anxml_response *response);
/* Verify the private execution copy immediately before populating the CPU model table. */
int anx_anxml_generate_verified(const struct anx_anxml_request *request, const struct anx_adapter_image *image,
		const uint8_t digest[32], struct anx_anxml_response *response);
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
int anx_anxml_test_image_fault(bool enabled);
#endif
#endif
