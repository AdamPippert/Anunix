/* Publish a tested CPU adapter and its cache generation as one transition. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/adapter.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct adapter_fixture {
	struct anx_anxml_request request;
	struct anx_anxml_response response;
	struct anx_adapter_trial trial;
	struct anx_adapter_view version;
	bool owner;
};
static int expect_output(struct adapter_fixture *f, const struct anx_adapter_view *v, const char *expected,
		struct anx_adapter_cache_key *key)
{
	int ret = anx_adapter_generate(&v->id, v->generation, &f->request, &f->response, key);
	if (ret == ANX_OK && (f->response.output_len != 4 || anx_memcmp(f->response.output, expected, 4))) ret = -5402;
	return ret;
}
static int adapter_authority(struct anx_external_call *call, void *arg)
{
	struct adapter_fixture *f = arg;
	struct anx_adapter_view out;
	struct anx_adapter_cache_key key;
	(void)call;
	int ret = anx_adapter_get(&f->version.id, &out);
	if (ret != (f->owner ? ANX_OK : ANX_EPERM)) return -5410;
	ret = anx_adapter_generate(&f->version.id, f->version.generation, &f->request, &f->response, &key);
	if (ret != (f->owner ? ANX_OK : ANX_EPERM)) return -5410;
	if (anx_adapter_publish(&f->version.id, 1, f->version.generation, &out) != ANX_EPERM ||
	    anx_adapter_rollback(&f->version.id, f->version.generation, &out) != ANX_EPERM ||
	    anx_adapter_destroy(&f->version.id) != ANX_EPERM) return -5410;
	return ANX_OK;
}
int anx_research_day054(void)
{
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_adapter_image initial = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_adapter_image candidate = { .format = 1, .count = 2, .deltas = {{'~','B',4096},{'B','B',4096}} };
	struct anx_adapter_view current, next, other, unchanged;
	struct anx_adapter_cache_key old_key, new_key, other_key;
	struct adapter_fixture *f = anx_zalloc(sizeof(*f));
	bool created = false, other_created = false;
	uint64_t token = 0, pending = 0;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-054", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret != ANX_OK) goto out;
	ret = -5401;
	if (anx_adapter_create(&owner->cid, &initial, &current) != ANX_OK) goto out;
	created = true;
	f->request.prompt[0] = '~'; f->request.prompt_len = 1; f->request.max_tokens = 4;
	ret = expect_output(f, &current, "AAAA", &old_key);
	if (ret != ANX_OK) goto out;
	ret = anx_adapter_create(&owner->cid, &initial, &other);
	if (ret != ANX_OK) goto out;
	other_created = true;
	ret = expect_output(f, &other, "AAAA", &other_key);
	if (ret != ANX_OK) goto out;
	unchanged = current;
	ret = anx_adapter_stage(&current.id, current.generation, &candidate, &token);
	if (ret != ANX_OK) goto out;
	pending = token;
	ret = -5403;
	if (anx_adapter_publish(&current.id, token, current.generation, &next) != ANX_EPERM ||
	    anx_adapter_destroy(&current.id) != ANX_EBUSY || anx_adapter_get(&current.id, &next) != ANX_OK ||
	    anx_memcmp(&next, &current, sizeof(next)) || anx_adapter_cache_check(&current.id, &f->request, &old_key) != ANX_OK) goto out;
	ret = expect_output(f, &current, "AAAA", &old_key);
	if (ret != ANX_OK) goto out;
	/* A failed exact-output check must leave the old serving image authoritative. */
	f->trial.request = f->request; f->trial.expected_len = 4;
	anx_memcpy(f->trial.expected, "ZZZZ", 4);
	ret = -5404;
	if (anx_adapter_verify(&current.id, token, &f->trial, 1) != ANX_EIO ||
	    anx_adapter_publish(&current.id, token, current.generation, &next) != ANX_EPERM) goto out;
	anx_memcpy(f->trial.expected, "BBBB", 4);
	/* Stage owns its copy; changing the caller's image cannot change the candidate. */
	candidate.deltas[0].next = 'C'; candidate.deltas[1].previous = candidate.deltas[1].next = 'C';
	ret = anx_adapter_verify(&current.id, token, &f->trial, 1);
	if (ret != ANX_OK) goto out;
	ret = expect_output(f, &current, "AAAA", &old_key);
	if (ret != ANX_OK) goto out;
	ret = -5405;
	if (anx_adapter_publish(&current.id, token + 1, current.generation, &next) != ANX_EBUSY ||
	    anx_adapter_publish(&current.id, token, current.generation + 1, &next) != ANX_EBUSY) goto out;
	ret = anx_adapter_publish(&current.id, token, current.generation, &next);
	if (ret != ANX_OK) goto out;
	pending = 0;
	ret = -5406;
	if (next.generation <= current.generation || anx_uuid_compare(&next.id, &current.id) ||
	    !anx_memcmp(next.image_hash.bytes, current.image_hash.bytes, 32) ||
	    anx_adapter_cache_check(&current.id, &f->request, &old_key) != ANX_EBUSY ||
	    anx_adapter_cache_check(&other.id, &f->request, &other_key) != ANX_OK ||
	    anx_adapter_generate(&current.id, current.generation, &f->request, &f->response, &new_key) != ANX_EBUSY) goto out;
	ret = expect_output(f, &next, "BBBB", &new_key);
	if (ret != ANX_OK) goto out;
	ret = -5407;
	if (anx_adapter_cache_check(&next.id, &f->request, &new_key) != ANX_OK ||
	    anx_adapter_cache_check(&other.id, &f->request, &new_key) != ANX_EBUSY) goto out;
	f->request.seed = 1;
	if (anx_adapter_cache_check(&next.id, &f->request, &new_key) != ANX_EBUSY) goto out;
	f->request.seed = 0; f->request.prompt[0] = '!';
	if (anx_adapter_cache_check(&next.id, &f->request, &new_key) != ANX_EBUSY) goto out;
	f->request.prompt[0] = '~';
	ret = anx_adapter_stage(&next.id, next.generation, &candidate, &token);
	if (ret != ANX_OK) goto out;
	pending = token;
	ret = -5408;
	if (anx_adapter_rollback(&next.id, next.generation, &current) != ANX_EBUSY ||
	    anx_adapter_abort(&next.id, token + 1) != ANX_EBUSY) goto out;
	ret = anx_adapter_abort(&next.id, token);
	if (ret != ANX_OK) goto out;
	pending = 0;
	ret = anx_adapter_rollback(&next.id, next.generation, &current);
	if (ret != ANX_OK) goto out;
	ret = -5409;
	if (current.generation <= next.generation || anx_memcmp(current.image_hash.bytes, unchanged.image_hash.bytes, 32) ||
	    anx_adapter_cache_check(&current.id, &f->request, &old_key) != ANX_EBUSY ||
	    anx_adapter_cache_check(&current.id, &f->request, &new_key) != ANX_EBUSY ||
	    anx_adapter_publish(&current.id, token, next.generation, &next) != ANX_EBUSY) goto out;
	ret = expect_output(f, &current, "AAAA", &old_key);
	if (ret != ANX_OK) goto out;
	/* Malformed images and requests cannot publish or overwrite outputs. */
	candidate.count = ANX_ADAPTER_DELTA_MAX + 1;
	ret = -5411;
	if (anx_adapter_stage(&current.id, current.generation, &candidate, &token) != ANX_EINVAL) goto out;
	f->request.prompt_len = ANX_ANXML_PROMPT_MAX + 1;
	if (anx_adapter_generate(&current.id, current.generation, &f->request, &f->response, &new_key) != ANX_EINVAL ||
	    f->response.output_len != 4 || anx_memcmp(f->response.output, "AAAA", 4)) goto out;
	f->request.prompt_len = 1;
	f->version = current;
	call = anx_zalloc(sizeof(*call));
	if (!call) { ret = ANX_ENOMEM; goto out; }
	anx_strlcpy(call->endpoint, "anxresearch054://adapter", sizeof(call->endpoint));
	ret = anx_external_register_handler("anxresearch054", adapter_authority, f);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	foreign->ext_call = owner->ext_call = call;
	foreign->execution.allow_side_effects = owner->execution.allow_side_effects = true;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	f->owner = true;
	ret = anx_cell_run(owner);
	if (ret != ANX_OK) goto out;
	candidate.count = 2;
	ret = -5412;
	if (anx_adapter_stage(&current.id, current.generation, &candidate, &token) != ANX_EPERM) goto out;
	ret = ANX_OK;
out:
	anx_external_unregister_handler("anxresearch054");
	if (pending) anx_adapter_abort(&current.id, pending);
	if (other_created) anx_adapter_destroy(&other.id);
	if (created) anx_adapter_destroy(&current.id);
	if (owner) anx_cell_destroy(owner);
	if (foreign) anx_cell_destroy(foreign);
	anx_free(call); anx_free(f);
	return ret;
}
#endif
