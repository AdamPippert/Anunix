/*
 * rollout.c — Reasoning rollout driver.
 *
 * A rollout owns a parent Execution Cell whose plan advances one
 * inference step at a time. Each step reads the current message,
 * invokes the pluggable inference adapter, and stores the response
 * as a MODEL_OUTPUT State Object linked back to its input parent.
 *
 * The harness does not itself contain a model — it coordinates
 * State Objects, memory plane admission, lineage, and trace
 * materialization. This decouples rollout control flow from any
 * particular inference backend, so the same code drives local
 * model servers, remote Claude API calls, or test stubs.
 */

#include <anx/types.h>
#include <anx/rlm.h>
#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/memplane.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/arch.h>

/* Installed inference adapter. NULL until the system wires one up. */
static anx_rlm_infer_fn g_infer_fn;

void anx_rlm_set_infer(anx_rlm_infer_fn fn)
{
	g_infer_fn = fn;
}

void anx_rlm_init(void)
{
	/*
	 * Intentionally minimal — the rollout subsystem is stateless
	 * aside from the inference adapter, which persists across
	 * init calls so the shell does not have to reinstall it.
	 */
}

void anx_rlm_config_default(struct anx_rlm_config *cfg)
{
	if (!cfg)
		return;
	anx_memset(cfg, 0, sizeof(*cfg));
	anx_strlcpy(cfg->model, "claude-sonnet-4-6", sizeof(cfg->model));
	cfg->max_steps = 8;
	cfg->max_tokens = 1024;
	cfg->min_tokens = 256;
	cfg->lambda_decay = 0.82f;
	cfg->persist_trace = true;
	cfg->admit_responses = true;
	cfg->priority = ANX_PRIO_NORMAL;
	cfg->queue = ANX_QUEUE_INTERACTIVE;
}

/* Read a State Object's payload into a bounded buffer. */
static int read_oid_text(const anx_oid_t *oid, char *buf,
			 uint32_t bufsz, uint32_t *out_len)
{
	struct anx_object_handle h;
	uint64_t sz;
	int ret;

	ret = anx_so_open(oid, ANX_OPEN_READ, &h);
	if (ret != ANX_OK)
		return ret;

	sz = h.obj->payload_size;
	if (sz >= bufsz)
		sz = bufsz - 1;

	if (sz > 0) {
		/* read_payload returns bytes read on success, negative
		 * on error. */
		ret = anx_so_read_payload(&h, 0, buf, sz);
		if (ret < 0) {
			anx_so_close(&h);
			return ret;
		}
		sz = (uint64_t)ret;
	}
	buf[sz] = '\0';
	anx_so_close(&h);
	if (out_len)
		*out_len = (uint32_t)sz;
	return ANX_OK;
}

static uint32_t lambda_rlm_tokens_for_step(const struct anx_rlm_rollout *r)
{
	uint32_t step, tokens;
	float decay;

	if (!r)
		return 0;
	tokens = r->config.max_tokens;
	if (tokens == 0)
		return 0;

	decay = r->config.lambda_decay;
	if (decay <= 0.0f || decay >= 1.0f)
		return tokens;

	for (step = 0; step < r->step_count; step++)
		tokens = (uint32_t)((float)tokens * decay);

	if (r->config.min_tokens && tokens < r->config.min_tokens)
		tokens = r->config.min_tokens;
	if (tokens == 0)
		tokens = 1;
	return tokens;
}

static int create_response_so(const struct anx_rlm_rollout *r,
			      const char *content, uint32_t len,
			      const anx_oid_t *parent,
			      anx_oid_t *out_oid)
{
	struct anx_state_object *obj;
	struct anx_so_create_params params;
	int ret;

	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_MODEL_OUTPUT;
	params.payload = content;
	params.payload_size = len;
	params.parent_oids = parent;
	params.parent_count = parent ? 1 : 0;
	params.creator_cell = r->root_cid;

	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK)
		return ret;

	*out_oid = obj->oid;
	anx_objstore_release(obj);
	return ANX_OK;
}

int anx_rlm_rollout_create(const anx_oid_t *prompt_oid,
			   const struct anx_rlm_config *config,
			   struct anx_rlm_rollout **out)
{
	struct anx_rlm_rollout *r;
	struct anx_state_object *prompt;
	struct anx_cell *cell;
	struct anx_cell_intent intent;
	int ret;

	if (!prompt_oid || !config || !out)
		return ANX_EINVAL;

	/* The prompt must already exist and be readable. */
	prompt = anx_objstore_lookup(prompt_oid);
	if (!prompt)
		return ANX_ENOENT;
	anx_objstore_release(prompt);

	r = anx_zalloc(sizeof(*r));
	if (!r)
		return ANX_ENOMEM;

	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "rlm.rollout", sizeof(intent.name));
	anx_strlcpy(intent.objective, "multi-step reasoning rollout",
		     sizeof(intent.objective));
	intent.priority = config->priority;

	ret = anx_cell_create(ANX_CELL_TASK_STREAM_EXECUTION, &intent, &cell);
	if (ret != ANX_OK) {
		anx_free(r);
		return ret;
	}

	/* Rollouts spawn tool child cells; allow it. */
	cell->execution.allow_recursive_cells = true;
	cell->execution.max_recursion_depth = 4;

	if (cell->input_count < ANX_MAX_CELL_INPUTS) {
		struct anx_cell_input *in = &cell->inputs[cell->input_count++];
		anx_strlcpy(in->name, "prompt", sizeof(in->name));
		in->state_object_ref = *prompt_oid;
		in->mode = ANX_INPUT_READ;
		in->required = true;
	}

	r->root_cid = cell->cid;
	r->prompt_oid = *prompt_oid;
	r->config = *config;
	r->status = ANX_RLM_PENDING;
	r->started_at = arch_time_now();

	*out = r;
	return ANX_OK;
}

int anx_rlm_rollout_step(struct anx_rlm_rollout *r)
{
	char input_buf[ANX_RLM_MAX_CONTENT];
	struct anx_rlm_infer_req req;
	struct anx_rlm_infer_resp resp;
	struct anx_rlm_step *s;
	const anx_oid_t *input_oid;
	anx_oid_t resp_oid;
	uint32_t input_len = 0;
	int ret;

	if (!r)
		return ANX_EINVAL;

	switch (r->status) {
	case ANX_RLM_COMPLETED:
	case ANX_RLM_FAILED:
	case ANX_RLM_CANCELLED:
		return ANX_ENOENT;
	default:
		break;
	}

	if (!g_infer_fn) {
		/*
		 * No adapter installed is a configuration error, not a
		 * per-rollout failure — leave status intact so the
		 * caller can install an adapter and retry.
		 */
		return ANX_ENOSYS;
	}

	if (r->step_count >= ANX_RLM_MAX_STEPS ||
	    (r->config.max_steps && r->step_count >= r->config.max_steps)) {
		r->status = ANX_RLM_COMPLETED;
		r->completed_at = arch_time_now();
		return ANX_OK;
	}

	r->status = ANX_RLM_RUNNING;

	input_oid = (r->step_count == 0)
		? &r->prompt_oid
		: &r->steps[r->step_count - 1].output_oid;

	ret = read_oid_text(input_oid, input_buf, sizeof(input_buf),
			    &input_len);
	if (ret != ANX_OK) {
		r->status = ANX_RLM_FAILED;
		r->error_code = ret;
		return ret;
	}

	anx_memset(&req, 0, sizeof(req));
	anx_memset(&resp, 0, sizeof(resp));
	req.model = r->config.model[0] ? r->config.model : NULL;
	req.system = r->config.system[0] ? r->config.system : NULL;
	req.user_message = input_buf;
	req.max_tokens = lambda_rlm_tokens_for_step(r);

	ret = g_infer_fn(&req, &resp);
	if (ret != ANX_OK) {
		r->status = ANX_RLM_FAILED;
		r->error_code = ret;
		return ret;
	}
	if (resp.status != ANX_OK) {
		r->status = ANX_RLM_FAILED;
		r->error_code = resp.status;
		return resp.status;
	}

	/* Clamp reported length to the buffer. */
	if (resp.content_len > ANX_RLM_MAX_CONTENT)
		resp.content_len = ANX_RLM_MAX_CONTENT;

	ret = create_response_so(r, resp.content, resp.content_len,
				 input_oid, &resp_oid);
	if (ret != ANX_OK) {
		r->status = ANX_RLM_FAILED;
		r->error_code = ret;
		return ret;
	}

	if (r->config.admit_responses) {
		struct anx_mem_entry *entry = NULL;
		/*
		 * Best-effort admission into the transient cache tier.
		 * Responses are volatile by default; callers who want
		 * durability can re-admit with a stronger profile.
		 */
		anx_memplane_admit(&resp_oid, ANX_ADMIT_CACHEABLE, &entry);
	}

	s = &r->steps[r->step_count++];
	s->kind = ANX_RLM_STEP_INFER;
	s->cell_id = r->root_cid;
	s->input_oid = *input_oid;
	s->output_oid = resp_oid;
	s->input_tokens = resp.input_tokens;
	s->output_tokens = resp.output_tokens;
	s->stop = resp.stop;
	s->status = ANX_OK;
	anx_strlcpy(s->label, "infer", sizeof(s->label));

	r->total_input_tokens += resp.input_tokens;
	r->total_output_tokens += resp.output_tokens;
	r->response_oid = resp_oid;

	if (resp.stop) {
		r->status = ANX_RLM_COMPLETED;
		r->completed_at = arch_time_now();
	}

	return ANX_OK;
}

int anx_rlm_rollout_inject(struct anx_rlm_rollout *r,
			   const char *label,
			   const anx_oid_t *tool_result_oid)
{
	struct anx_rlm_step *s;
	struct anx_state_object *obj;

	if (!r || !tool_result_oid)
		return ANX_EINVAL;
	if (r->status == ANX_RLM_COMPLETED || r->status == ANX_RLM_FAILED ||
	    r->status == ANX_RLM_CANCELLED)
		return ANX_EPERM;
	if (r->step_count >= ANX_RLM_MAX_STEPS)
		return ANX_EFULL;

	/* Validate the tool result exists. */
	obj = anx_objstore_lookup(tool_result_oid);
	if (!obj)
		return ANX_ENOENT;
	anx_objstore_release(obj);

	s = &r->steps[r->step_count++];
	s->kind = ANX_RLM_STEP_TOOL;
	s->cell_id = r->root_cid;
	s->input_oid = r->step_count > 1
		? r->steps[r->step_count - 2].output_oid
		: r->prompt_oid;
	s->output_oid = *tool_result_oid;
	s->stop = false;
	s->status = ANX_OK;
	anx_strlcpy(s->label, label ? label : "tool", sizeof(s->label));

	return ANX_OK;
}

static void finalize_trace(struct anx_rlm_rollout *r)
{
	struct anx_state_object *obj;
	struct anx_so_create_params params;

	if (!r->config.persist_trace)
		return;
	if (r->step_count == 0)
		return;

	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_EXECUTION_TRACE;
	params.payload = r->steps;
	params.payload_size = sizeof(r->steps[0]) * r->step_count;
	params.creator_cell = r->root_cid;

	if (anx_so_create(&params, &obj) == ANX_OK) {
		r->trace_oid = obj->oid;
		anx_objstore_release(obj);
	}
}

int anx_rlm_rollout_run(struct anx_rlm_rollout *r)
{
	int ret;

	if (!r)
		return ANX_EINVAL;

	while (r->status == ANX_RLM_PENDING || r->status == ANX_RLM_RUNNING) {
		ret = anx_rlm_rollout_step(r);
		if (ret != ANX_OK)
			return ret;
		if (r->config.max_steps &&
		    r->step_count >= r->config.max_steps)
			break;
	}

	if (r->status == ANX_RLM_RUNNING || r->status == ANX_RLM_PENDING) {
		r->status = ANX_RLM_COMPLETED;
		r->completed_at = arch_time_now();
	}

	if (r->status == ANX_RLM_COMPLETED)
		finalize_trace(r);

	return (r->status == ANX_RLM_COMPLETED) ? ANX_OK : r->error_code;
}

void anx_rlm_rollout_set_score(struct anx_rlm_rollout *r, int32_t score)
{
	if (r)
		r->score = score;
}

void anx_rlm_rollout_destroy(struct anx_rlm_rollout *r)
{
	struct anx_cell *cell;

	if (!r)
		return;

	cell = anx_cell_store_lookup(&r->root_cid);
	if (cell) {
		/*
		 * lookup incremented refcount to 2; drop back to 1 so
		 * destroy will succeed. If the rollout was never
		 * completed the cell stays in whatever state it was.
		 */
		anx_cell_store_release(cell);
		anx_cell_destroy(cell);
	}

	anx_free(r);
}
