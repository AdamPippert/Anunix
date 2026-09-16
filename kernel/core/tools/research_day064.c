/* Bind validated source objects to the exact bytes consumed by inference. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/model_use.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/crypto.h>
#include <anx/identity.h>
#include <anx/external_call.h>
#include <anx/kprintf.h>
struct fixture064 {
	struct anx_anxml_response response, sentinel_response;
	struct anx_model_use_view use, output, sentinel;
	struct anx_model_use_spec spec;
	struct anx_external_call call;
	bool foreign;
	uint32_t calls;
};
static int denied064(struct fixture064 *f, const struct anx_model_use_view *use, int expected)
{
	struct anx_model_use_view current;
	anx_memset(&f->response, 0x55, sizeof(f->response)); f->sentinel_response = f->response;
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->sentinel = f->output;
	int ret = anx_model_use_execute(use->id, use->epoch, &f->response, &f->output);
	if (ret != expected) { kprintf("day064 denial expected=%d actual=%d\n", expected, ret); return -6402; }
	return !anx_memcmp(&f->response, &f->sentinel_response, sizeof(f->response)) &&
		!anx_memcmp(&f->output, &f->sentinel, sizeof(f->output)) &&
		anx_model_use_get(use->id, &current) == ANX_OK && !anx_memcmp(use, &current, sizeof(current)) ? ANX_OK : -6403;
}
static int active064(struct anx_external_call *call, void *arg)
{
	struct fixture064 *f = arg;
	(void)call; f->calls++;
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->sentinel = f->output;
	if (anx_model_use_prepare(&f->use.owner, &f->spec, &f->output) != ANX_EPERM ||
	    anx_model_use_destroy(f->use.id) != ANX_EPERM || anx_anxml_test_image_fault(true) != ANX_EPERM) return -6410;
	if (f->foreign) {
		anx_memset(&f->response, 0x55, sizeof(f->response)); f->sentinel_response = f->response;
		return anx_model_use_get(f->use.id, &f->output) == ANX_EPERM &&
			anx_model_use_execute(f->use.id, f->use.epoch, &f->response, &f->output) == ANX_EPERM &&
			!anx_memcmp(&f->output, &f->sentinel, sizeof(f->output)) &&
			!anx_memcmp(&f->response, &f->sentinel_response, sizeof(f->response)) ? ANX_OK : -6411;
	}
	int ret = anx_model_use_execute(f->use.id, f->use.epoch, &f->response, &f->use);
	return ret == ANX_OK && f->use.state == ANX_MODEL_USE_COMPLETED && f->response.output_len == 4 &&
		!anx_memcmp(f->response.output, "BBBB", 4) ? ANX_OK : -6412;
}
static int sign064(const struct anx_identity_commitment *c, const uint8_t key[64], uint8_t digest[32], uint8_t signature[64])
{
	int ret = anx_identity_digest(c, digest);
	if (ret == ANX_OK) anx_ed25519_sign(signature, digest, 32, key);
	return ret;
}
int anx_research_day064(void)
{
	struct fixture064 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *image = NULL, *prompt = NULL, *new_image = NULL;
	struct anx_adapter_image adapter = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_model_use_view use = {0}, stale = {0};
	struct anx_model_use_spec spec = { .maximum_tokens = 4 };
	struct anx_identity_commitment identity = { .schema = 1, .generation = 1, .authority = ANX_CAP_AUTH_SIDE_EFFECT };
	struct anx_identity_view identity_view;
	uint8_t seed[32] = {0x64}, key[64], signature[64], digest[32];
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA,
		.schema_uri = ANX_MODEL_USE_SCHEMA, .schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-064", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_so_create(&params, &image);
	if (ret == ANX_OK) ret = anx_so_seal(&image->oid);
	params.object_type = ANX_OBJ_BYTE_DATA; params.schema_uri = NULL; params.schema_version = NULL;
	params.payload = "~"; params.payload_size = 1;
	if (ret == ANX_OK) ret = anx_so_create(&params, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_side_effects = true;
	anx_uuid_generate(&identity.identity_id);
	anx_ed25519_keypair(identity.operator_key, key, seed);
	ret = sign064(&identity, key, digest, signature);
	if (ret == ANX_OK) ret = anx_identity_create(&identity, signature);
	if (ret == ANX_OK) ret = anx_identity_bind(owner, &identity.identity_id);
	if (ret == ANX_OK) ret = anx_identity_get(&identity.identity_id, &identity_view);
	if (ret != ANX_OK) goto out;
	spec.image = image->oid; spec.prompt = prompt->oid;
	ret = -6401;
	if (anx_model_use_prepare(&owner->cid, &spec, &use) != ANX_OK) goto out;
	ret = -6404;
	anx_sha256(&adapter, sizeof(adapter), digest);
	if (use.state != ANX_MODEL_USE_READY || use.epoch != 1 || use.maximum_tokens != 4 ||
	    use.image.version != image->version || use.prompt.version != prompt->version ||
	    anx_memcmp(use.image.digest, digest, 32) || anx_uuid_compare(&use.identity_record, &identity_view.record_oid)) goto out;
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->sentinel = f->output;
	struct anx_model_use_spec invalid = spec;
	invalid.maximum_tokens = 129;
	if (anx_model_use_prepare(&owner->cid, &invalid, &f->output) != ANX_EINVAL) goto out;
	invalid.maximum_tokens = 0;
	if (anx_model_use_prepare(&owner->cid, &invalid, &f->output) != ANX_EINVAL) goto out;
	invalid = spec; invalid.image = prompt->oid;
	if (anx_model_use_prepare(&owner->cid, &invalid, &f->output) != ANX_EINVAL) goto out;
	owner->cognitive.max_tokens = 3;
	int cap_ret = anx_model_use_prepare(&owner->cid, &spec, &f->output);
	ret = denied064(f, &use, ANX_EPERM); owner->cognitive.max_tokens = 0;
	if (ret != ANX_OK) goto out;
	ret = -6404;
	if (cap_ret != ANX_EPERM || anx_memcmp(&f->output, &f->sentinel, sizeof(f->output))) goto out;
	/* Fault injection changes live sources after preparation; every rejection preserves the record and output. */
	for (uint32_t i = 0; i < 2; i++) {
		struct anx_state_object *source = i ? prompt : image;
		source->version++;
		ret = denied064(f, &use, ANX_EBUSY); source->version--;
		if (ret != ANX_OK) goto out;
		((uint8_t *)source->payload)[source->payload_size - 1] ^= 1;
		ret = denied064(f, &use, ANX_EBUSY); ((uint8_t *)source->payload)[source->payload_size - 1] ^= 1;
		if (ret != ANX_OK) goto out;
		source->access_policy.rule_count = 1; source->access_policy.rules[0].effect = ANX_EFFECT_DENY;
		source->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
		ret = denied064(f, &use, ANX_EPERM); source->access_policy.rule_count = 0;
		if (ret != ANX_OK) goto out;
	}
	ret = -6405;
	if (anx_model_use_execute(use.id, use.epoch + 1, &f->response, &f->output) != ANX_EBUSY) goto out;
	/* The attested source stays unchanged while the private CPU execution copy is damaged. */
	ret = anx_anxml_test_image_fault(true);
	if (ret == ANX_OK) ret = denied064(f, &use, ANX_EIO);
	if (ret != ANX_OK) goto out;
	spec.maximum_tokens = 128; spec.seed = 42; /* Caller copies do not alter the prepared request. */
	ret = anx_model_use_execute(use.id, use.epoch, &f->response, &use);
	if (ret != ANX_OK) goto out;
	anx_sha256("AAAA", 4, digest);
	ret = -6406;
	if (use.state != ANX_MODEL_USE_COMPLETED || use.epoch != 2 || use.maximum_tokens != 4 || use.seed ||
	    use.output_size != 4 || use.tokens_generated != 4 || f->response.output_len != 4 ||
	    anx_memcmp(f->response.output, "AAAA", 4) || anx_memcmp(use.output_digest, digest, 32) ||
	    anx_memcmp(use.image.digest, use.consumed_image_digest, 32) || denied064(f, &use, ANX_EBUSY) != ANX_OK) goto out;
	/* An identity commitment transition invalidates an unconsumed record even with unchanged permissions. */
	spec.maximum_tokens = 4; spec.seed = 0;
	ret = anx_model_use_prepare(&owner->cid, &spec, &stale);
	if (ret != ANX_OK) goto out;
	identity.generation++;
	anx_memcpy(identity.previous_digest, identity_view.digest, 32);
	ret = sign064(&identity, key, digest, signature);
	if (ret == ANX_OK) ret = anx_identity_transition(&identity, signature);
	if (ret == ANX_OK) ret = denied064(f, &stale, ANX_EBUSY);
	if (ret != ANX_OK) goto out;
	ret = anx_model_use_destroy(stale.id); stale.id = 0;
	if (ret != ANX_OK) goto out;
	adapter.deltas[0].next = adapter.deltas[1].previous = adapter.deltas[1].next = 'B';
	params = (struct anx_so_create_params){ .object_type = ANX_OBJ_STRUCTURED_DATA,
		.schema_uri = ANX_MODEL_USE_SCHEMA, .schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	ret = anx_so_create(&params, &new_image);
	if (ret == ANX_OK) ret = anx_so_seal(&new_image->oid);
	if (ret != ANX_OK) goto out;
	spec.image = new_image->oid;
	ret = anx_model_use_prepare(&owner->cid, &spec, &stale);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	f->use = stale; f->spec = spec;
	anx_strlcpy(f->call.endpoint, "anxresearch064://model-use", sizeof(f->call.endpoint));
	ret = anx_external_register_handler("anxresearch064", active064, f);
	if (ret != ANX_OK) goto out;
	foreign->execution.allow_side_effects = true; foreign->ext_call = &f->call;
	f->foreign = true;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	f->foreign = false; owner->ext_call = &f->call;
	ret = anx_cell_run(owner); stale = f->use;
	if (ret != ANX_OK) goto out;
	ret = -6407;
	anx_sha256("BBBB", 4, digest);
	if (f->calls != 2 || stale.state != ANX_MODEL_USE_COMPLETED || anx_memcmp(stale.output_digest, digest, 32) ||
	    anx_uuid_compare(&stale.image.oid, &new_image->oid) || !anx_uuid_compare(&stale.identity_record, &use.identity_record)) goto out;
	ret = ANX_OK;
out:
	anx_anxml_test_image_fault(false);
	anx_external_unregister_handler("anxresearch064");
	if (stale.id && anx_model_use_destroy(stale.id) != ANX_OK && ret == ANX_OK) ret = -6413;
	if (use.id && anx_model_use_destroy(use.id) != ANX_OK && ret == ANX_OK) ret = -6414;
	if (foreign && anx_cell_destroy(foreign) != ANX_OK && ret == ANX_OK) ret = -6415;
	if (owner && anx_cell_destroy(owner) != ANX_OK && ret == ANX_OK) ret = -6416;
	if (image) { anx_so_delete(&image->oid, false); anx_objstore_release(image); }
	if (new_image) { anx_so_delete(&new_image->oid, false); anx_objstore_release(new_image); }
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	anx_memset(key, 0, sizeof(key)); anx_free(f);
	if (ret != ANX_OK) kprintf("day064 failure rc=%d\n", ret);
	return ret;
}
#endif
