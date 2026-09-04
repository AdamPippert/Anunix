/*
 * configurator.c — RLM-driven configuration (RFC-0027 Section 5).
 *
 * The bridge from a natural-language goal to a governed configuration change:
 * run an RLM rollout (the existing harness, RFC/anx_rlm), take its final
 * response, and apply it as a proposal that must clear the same commit gate a
 * hand-typed `config set` would. The reasoning model proposes; the gate decides.
 */

#include <anx/config.h>
#include <anx/rlm.h>
#include <anx/state_object.h>
#include <anx/string.h>

/* Instruction to the model: emit only machine-applicable directives. */
static const char *CONFIG_SYSTEM_PROMPT =
	"You are a system configurator. Respond ONLY with lines of the form:\n"
	"SET <area> <key> <value>\n"
	"One directive per line, no prose. Example: SET system log_level debug";

int anx_configurator_run(const char *goal,
			 struct anx_world_commit_report *report)
{
	struct anx_so_create_params params;
	struct anx_state_object *prompt_obj;
	struct anx_state_object *resp;
	struct anx_rlm_rollout *r;
	struct anx_rlm_config cfg;
	char text[ANX_RLM_MAX_CONTENT];
	anx_oid_t prompt_oid;
	int rc;

	if (!goal || !goal[0])
		return ANX_EINVAL;

	anx_config_init();
	anx_rlm_init();		/* idempotent */

	/* The goal becomes a prompt State Object. */
	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = goal;
	params.payload_size = anx_strlen(goal);
	rc = anx_so_create(&params, &prompt_obj);
	if (rc != ANX_OK)
		return rc;
	prompt_oid = prompt_obj->oid;
	anx_objstore_release(prompt_obj);

	anx_rlm_config_default(&cfg);
	anx_strlcpy(cfg.system, CONFIG_SYSTEM_PROMPT, sizeof(cfg.system));
	cfg.max_steps = 2;
	cfg.admit_responses = false;
	cfg.persist_trace = false;

	rc = anx_rlm_rollout_create(&prompt_oid, &cfg, &r);
	if (rc != ANX_OK)
		return rc;
	rc = anx_rlm_rollout_run(r);
	if (rc != ANX_OK) {
		anx_rlm_rollout_destroy(r);
		return rc;
	}

	/* Read the model's final response text. */
	resp = anx_objstore_lookup(&r->response_oid);
	if (!resp) {
		anx_rlm_rollout_destroy(r);
		return ANX_ENOENT;
	}
	{
		uint32_t n = (uint32_t)resp->payload_size;

		if (n >= sizeof(text))
			n = sizeof(text) - 1;
		if (resp->payload && n)
			anx_memcpy(text, resp->payload, n);
		text[n] = '\0';
	}
	anx_objstore_release(resp);
	anx_rlm_rollout_destroy(r);

	/* The proposal is advisory; the commit gate has the final say. */
	return anx_configurator_apply(text, report);
}
