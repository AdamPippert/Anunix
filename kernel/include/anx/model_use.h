#ifndef ANX_MODEL_USE_H
#define ANX_MODEL_USE_H
#include <anx/cell.h>
#include <anx/adapter.h>
#define ANX_MODEL_USE_MAX 16U
#define ANX_MODEL_USE_SCHEMA "anx:model/toy-adapter/v1"
enum anx_model_use_state { ANX_MODEL_USE_READY, ANX_MODEL_USE_BUSY, ANX_MODEL_USE_COMPLETED };
struct anx_model_use_spec { anx_oid_t image, prompt; uint32_t maximum_tokens, seed; };
struct anx_model_use_source { anx_oid_t oid; uint64_t version; uint32_t size, sensitivity; uint8_t digest[32]; };
struct anx_model_use_view {
	uint64_t id, epoch;
	anx_cid_t owner;
	anx_oid_t identity_record;
	struct anx_model_use_source image, prompt;
	enum anx_model_use_state state;
	uint32_t maximum_tokens, seed, tokens_generated, output_size;
	uint8_t request_digest[32], consumed_image_digest[32], output_digest[32];
};
/* Controller prepares/removes records; execution and inspection also allow the exact owner. */
int anx_model_use_prepare(const anx_cid_t *owner, const struct anx_model_use_spec *spec, struct anx_model_use_view *out);
int anx_model_use_get(uint64_t id, struct anx_model_use_view *out);
int anx_model_use_execute(uint64_t id, uint64_t epoch, struct anx_anxml_response *response, struct anx_model_use_view *out);
int anx_model_use_destroy(uint64_t id);
#endif
