/*
 * test_rlm.c — Reasoning Language Model harness tests.
 *
 * Exercises single rollouts, tool injection, batch submission, and
 * the adapter plumbing with a deterministic stub so nothing here
 * touches the network or a real model.
 */

#include <anx/types.h>
#include <anx/rlm.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/memplane.h>
#include <anx/sched.h>
#include <anx/string.h>
#include <anx/uuid.h>

/* --- Stub inference adapter ---
 *
 * Records every call and returns a canned response. Stops after
 * the configured turn count so the harness terminates naturally.
 */

static uint32_t stub_calls;
static uint32_t stub_stop_after;
static int32_t stub_input_tokens;
static int32_t stub_output_tokens;
static char stub_last_user[ANX_RLM_MAX_CONTENT];

static void stub_reset(uint32_t stop_after)
{
	stub_calls = 0;
	stub_stop_after = stop_after;
	stub_input_tokens = 11;
	stub_output_tokens = 17;
	stub_last_user[0] = '\0';
}

/*
 * Response mirrors the input with a trailing '.' appended. The stop
 * flag fires once the input itself already contains stop_after-1
 * dots, so each rollout stops deterministically after exactly
 * stop_after calls regardless of how many rollouts are interleaved.
 */
static int stub_infer(const struct anx_rlm_infer_req *req,
		      struct anx_rlm_infer_resp *resp)
{
	char out[ANX_RLM_MAX_CONTENT];
	uint32_t dots = 0;
	uint32_t out_len;
	const char *p;

	if (!req || !resp)
		return ANX_EINVAL;
	if (req->user_message) {
		anx_strlcpy(stub_last_user, req->user_message,
			     sizeof(stub_last_user));
		for (p = req->user_message; *p; p++)
			if (*p == '.')
				dots++;
	}

	stub_calls++;

	out[0] = '\0';
	anx_strlcpy(out, req->user_message ? req->user_message : "",
		     sizeof(out));
	out_len = (uint32_t)anx_strlen(out);
	if (out_len + 1 < sizeof(out)) {
		out[out_len++] = '.';
		out[out_len] = '\0';
	}

	anx_memcpy(resp->content, out, out_len);
	resp->content_len = out_len;
	resp->input_tokens = stub_input_tokens;
	resp->output_tokens = stub_output_tokens;
	resp->stop = (dots + 1 >= stub_stop_after);
	resp->status = ANX_OK;
	return ANX_OK;
}

static int failing_infer(const struct anx_rlm_infer_req *req,
			 struct anx_rlm_infer_resp *resp)
{
	(void)req;
	if (!resp)
		return ANX_EINVAL;
	resp->status = ANX_EIO;
	resp->content_len = 0;
	return ANX_OK;
}

/* --- Helpers --- */

static int make_prompt(const char *text, anx_oid_t *out)
{
	struct anx_so_create_params params;
	struct anx_state_object *obj;
	int ret;

	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = text;
	params.payload_size = anx_strlen(text);

	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK)
		return ret;
	*out = obj->oid;
	anx_objstore_release(obj);
	return ANX_OK;
}

static void init_all(void)
{
	anx_objstore_init();
	anx_cell_store_init();
	anx_memplane_init();
	anx_sched_init();
	anx_rlm_init();
}

/* --- Tests --- */

static int test_single_rollout(void)
{
	struct anx_rlm_rollout *r;
	struct anx_rlm_config cfg;
	anx_oid_t prompt;
	int ret;

	init_all();
	anx_rlm_set_infer(stub_infer);
	stub_reset(3);

	ret = make_prompt("solve: 2+2", &prompt);
	if (ret != ANX_OK)
		return -1;

	anx_rlm_config_default(&cfg);
	cfg.max_steps = 5;
	cfg.admit_responses = true;
	cfg.persist_trace = true;

	ret = anx_rlm_rollout_create(&prompt, &cfg, &r);
	if (ret != ANX_OK)
		return -2;
	if (r->status != ANX_RLM_PENDING)
		return -3;

	ret = anx_rlm_rollout_run(r);
	if (ret != ANX_OK)
		return -4;

	if (r->status != ANX_RLM_COMPLETED)
		return -5;
	if (r->step_count != 3)
		return -6;
	if (stub_calls != 3)
		return -7;
	if (r->total_output_tokens != 3 * 17)
		return -8;
	if (r->total_input_tokens != 3 * 11)
		return -9;

	/* Step 0 input is the prompt; subsequent steps chain. */
	if (anx_uuid_compare(&r->steps[0].input_oid, &prompt) != 0)
		return -10;
	if (anx_uuid_compare(&r->steps[1].input_oid,
			     &r->steps[0].output_oid) != 0)
		return -11;

	/* The final response OID matches the last step's output. */
	if (anx_uuid_compare(&r->response_oid,
			     &r->steps[2].output_oid) != 0)
		return -12;

	/* Trace was materialized. */
	if (anx_uuid_is_nil(&r->trace_oid))
		return -13;

	/* Responses admitted to the memory plane. */
	{
		struct anx_mem_entry *e;

		e = anx_memplane_lookup(&r->steps[0].output_oid);
		if (!e)
			return -14;
	}

	/* Score API roundtrip. */
	anx_rlm_rollout_set_score(r, 42);
	if (r->score != 42)
		return -15;

	anx_rlm_rollout_destroy(r);
	return 0;
}

static int test_missing_adapter(void)
{
	struct anx_rlm_rollout *r;
	struct anx_rlm_config cfg;
	anx_oid_t prompt;
	int ret;

	init_all();
	anx_rlm_set_infer(NULL);

	ret = make_prompt("noop", &prompt);
	if (ret != ANX_OK)
		return -1;

	anx_rlm_config_default(&cfg);
	cfg.max_steps = 2;

	ret = anx_rlm_rollout_create(&prompt, &cfg, &r);
	if (ret != ANX_OK)
		return -2;

	ret = anx_rlm_rollout_step(r);
	if (ret != ANX_ENOSYS)
		return -3;

	/* Status must not be mutated by a configuration error. */
	if (r->status != ANX_RLM_RUNNING && r->status != ANX_RLM_PENDING)
		return -4;

	anx_rlm_rollout_destroy(r);
	return 0;
}

static int test_failure_path(void)
{
	struct anx_rlm_rollout *r;
	struct anx_rlm_config cfg;
	anx_oid_t prompt;
	int ret;

	init_all();
	anx_rlm_set_infer(failing_infer);

	ret = make_prompt("fail me", &prompt);
	if (ret != ANX_OK)
		return -1;

	anx_rlm_config_default(&cfg);
	cfg.max_steps = 3;

	ret = anx_rlm_rollout_create(&prompt, &cfg, &r);
	if (ret != ANX_OK)
		return -2;

	ret = anx_rlm_rollout_run(r);
	if (ret == ANX_OK)
		return -3;
	if (r->status != ANX_RLM_FAILED)
		return -4;
	if (r->error_code != ANX_EIO)
		return -5;

	anx_rlm_rollout_destroy(r);
	return 0;
}

static int test_tool_injection(void)
{
	struct anx_rlm_rollout *r;
	struct anx_rlm_config cfg;
	anx_oid_t prompt;
	anx_oid_t tool_result;
	int ret;

	init_all();
	anx_rlm_set_infer(stub_infer);
	stub_reset(2);

	ret = make_prompt("use tool", &prompt);
	if (ret != ANX_OK)
		return -1;
	ret = make_prompt("tool says: 4", &tool_result);
	if (ret != ANX_OK)
		return -2;

	anx_rlm_config_default(&cfg);
	cfg.max_steps = 4;

	ret = anx_rlm_rollout_create(&prompt, &cfg, &r);
	if (ret != ANX_OK)
		return -3;

	/* One inference, then inject a tool result, then finish. */
	ret = anx_rlm_rollout_step(r);
	if (ret != ANX_OK)
		return -4;

	ret = anx_rlm_rollout_inject(r, "calc", &tool_result);
	if (ret != ANX_OK)
		return -5;
	if (r->step_count != 2)
		return -6;
	if (r->steps[1].kind != ANX_RLM_STEP_TOOL)
		return -7;
	if (anx_uuid_compare(&r->steps[1].output_oid, &tool_result) != 0)
		return -8;

	/* The next inference should see the tool result as its input. */
	ret = anx_rlm_rollout_step(r);
	if (ret != ANX_OK)
		return -9;
	if (anx_strcmp(stub_last_user, "tool says: 4") != 0)
		return -10;

	anx_rlm_rollout_destroy(r);
	return 0;
}

static int test_batch(void)
{
	struct anx_rlm_batch *batch;
	struct anx_rlm_config cfg;
	anx_oid_t prompts[4];
	uint32_t i;
	int ret;

	init_all();
	anx_rlm_set_infer(stub_infer);
	stub_reset(2);

	for (i = 0; i < 4; i++) {
		char buf[32];
		buf[0] = 'p';
		buf[1] = '0' + (char)i;
		buf[2] = '\0';
		ret = make_prompt(buf, &prompts[i]);
		if (ret != ANX_OK)
			return -1;
	}

	anx_rlm_config_default(&cfg);
	cfg.max_steps = 3;
	cfg.queue = ANX_QUEUE_BATCH;
	cfg.priority = ANX_PRIO_NORMAL;
	/* Disable memplane admission here so duplicate admits across
	 * batches cannot collide in the shared store. */
	cfg.admit_responses = false;

	ret = anx_rlm_batch_create(prompts, 4, &cfg, &batch);
	if (ret != ANX_OK)
		return -2;
	if (batch->count != 4)
		return -3;

	ret = anx_rlm_batch_run(batch);
	if (ret != ANX_OK)
		return -4;
	if (batch->completed != 4)
		return -5;
	if (batch->failed != 0)
		return -6;

	for (i = 0; i < 4; i++) {
		struct anx_rlm_rollout *r = batch->rollouts[i];

		if (r->status != ANX_RLM_COMPLETED)
			return -7;
		if (r->step_count != 2)
			return -8;
	}

	anx_rlm_batch_destroy(batch);
	return 0;
}

int test_rlm(void)
{
	int ret;

	ret = test_single_rollout();
	if (ret != 0)
		return ret;

	ret = test_missing_adapter();
	if (ret != 0)
		return ret - 20;

	ret = test_failure_path();
	if (ret != 0)
		return ret - 40;

	ret = test_tool_injection();
	if (ret != 0)
		return ret - 60;

	ret = test_batch();
	if (ret != 0)
		return ret - 80;

	return 0;
}
