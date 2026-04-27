/*
 * loop_llm_propose.c — LLM world proposal cell (RFC-0020 Phase 3).
 *
 * anx_loop_llm_propose() runs one RLM rollout for the session's goal,
 * maps the response to a JEPA action via keyword matching, and creates
 * an ANX_OBJ_WORLD_PROPOSAL with source=ANX_LOOP_PROPOSAL_LLM.
 *
 * Non-fatal: if the RLM harness or inference adapter is unavailable,
 * the function still creates a proposal using the PAL-preferred action
 * as a fallback, so the EBM always has at least one LLM-labelled
 * candidate to score alongside the JEPA-generated proposals.
 */

#include <anx/loop.h>
#include <anx/rlm.h>
#include <anx/jepa.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include "loop_internal.h"

/* ------------------------------------------------------------------ */
/* Prompt State Object                                                 */
/* ------------------------------------------------------------------ */

static int make_prompt_obj(const char *text, anx_oid_t *oid_out)
{
	struct anx_so_create_params cp;
	struct anx_state_object    *obj;
	int rc;

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_BYTE_DATA;
	cp.schema_uri     = "anx:schema/rlm/prompt/v1";
	cp.schema_version = "1";
	cp.payload        = text;
	cp.payload_size   = (uint32_t)anx_strlen(text) + 1;

	rc = anx_so_create(&cp, &obj);
	if (rc != ANX_OK)
		return rc;

	*oid_out = obj->oid;
	anx_objstore_release(obj);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Response → action_id mapping                                        */
/* ------------------------------------------------------------------ */

/*
 * Find the action that is most goal-aligned with the LLM's response text.
 * Reuses anx_loop_goal_alignment_energy: the action with the lowest energy
 * (most aligned) is selected.  Falls back to 0 (IDLE) on no match.
 */
static uint32_t response_to_action(const char *resp)
{
	uint32_t best_id = 0;
	float    best_e  = 1.0f;
	uint32_t i;

	if (!resp || resp[0] == '\0')
		return 0;

	for (i = 0; i < (uint32_t)ANX_JEPA_ACT_COUNT; i++) {
		float e = anx_loop_goal_alignment_energy(resp, i);

		if (e < best_e) {
			best_e  = e;
			best_id = i;
		}
	}
	return best_id;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

int anx_loop_llm_propose(anx_oid_t session_oid, uint32_t iteration,
			  anx_oid_t *proposal_oid_out)
{
	struct anx_loop_session  *s;
	struct anx_rlm_config     cfg;
	struct anx_rlm_rollout   *rollout = NULL;
	anx_oid_t                 prompt_oid;
	anx_oid_t                 pred_latent;
	struct anx_loop_proposal_payload payload;
	struct anx_so_create_params      cp;
	struct anx_state_object         *obj;
	char     prompt[512];
	const char *resp = "";
	uint32_t action_id = 0;
	int      rc;

	if (!proposal_oid_out)
		return ANX_EINVAL;

	s = anx_loop_session_get(session_oid);
	if (!s)
		return ANX_ENOENT;

	/* Build a structured prompt for the inference model */
	anx_snprintf(prompt, sizeof(prompt),
		     "World: %s\nGoal: %s\nIteration: %u\n"
		     "Actions: idle,route_local,route_remote,route_fallback,"
		     "mem_promote,mem_demote,mem_forget,"
		     "cell_spawn,cell_cancel,cap_validate,cap_suspend,security_alert\n"
		     "Which single action best serves the goal? Reply with the action name.",
		     s->world_uri, s->goal_text, iteration);

	/* Create prompt State Object */
	rc = make_prompt_obj(prompt, &prompt_oid);
	if (rc != ANX_OK)
		goto fallback;

	/* Run single-step RLM rollout (non-blocking, no tool loops) */
	anx_rlm_config_default(&cfg);
	cfg.max_steps       = 1;
	cfg.persist_trace   = false;
	cfg.admit_responses = false;

	rc = anx_rlm_rollout_create(&prompt_oid, &cfg, &rollout);
	if (rc != ANX_OK)
		goto fallback;

	rc = anx_rlm_rollout_run(rollout);
	if (rc == ANX_OK && rollout->status == ANX_RLM_COMPLETED &&
	    rollout->step_count > 0) {
		struct anx_rlm_step *last_step =
			&rollout->steps[rollout->step_count - 1];
		static char resp_buf[256];
		struct anx_object_handle h;

		if (anx_so_open(&last_step->output_oid, ANX_OPEN_READ, &h) == ANX_OK) {
			uint64_t rlen = h.obj->payload_size < sizeof(resp_buf) - 1
					? h.obj->payload_size : sizeof(resp_buf) - 1;
			if (anx_so_read_payload(&h, 0, resp_buf, rlen) == ANX_OK) {
				resp_buf[rlen] = '\0';
				resp = resp_buf;
			}
			anx_so_close(&h);
		}
	}

	action_id = response_to_action(resp);
	goto build_proposal;

fallback:
	/* RLM unavailable: use PAL-preferred action as implicit LLM suggestion */
	action_id = anx_loop_select_action_by_prior(s->world_uri,
						     (uint32_t)ANX_JEPA_ACT_COUNT);

build_proposal:
	/* Obtain a predicted latent for the suggested action */
	anx_memset(&pred_latent, 0, sizeof(pred_latent));
	(void)anx_jepa_predict(&s->active_belief, action_id, &pred_latent);

	/* Create the LLM proposal */
	anx_memset(&payload, 0, sizeof(payload));
	payload.session_oid = session_oid;
	payload.iteration   = iteration;
	payload.source      = ANX_LOOP_PROPOSAL_LLM;
	payload.latent_oid  = pred_latent;
	payload.action_id   = action_id;
	payload.status      = ANX_LOOP_PROPOSAL_CANDIDATE;

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_WORLD_PROPOSAL;
	cp.schema_uri     = "anx:schema/loop/world-proposal/v1";
	cp.schema_version = "1";
	cp.payload        = &payload;
	cp.payload_size   = sizeof(payload);

	rc = anx_so_create(&cp, &obj);

	if (rollout)
		anx_rlm_rollout_destroy(rollout);

	if (rc != ANX_OK)
		return rc;

	*proposal_oid_out = obj->oid;
	anx_loop_session_add_candidate(session_oid, *proposal_oid_out);

	kprintf("[loop-llm] iter=%u: action=%u (%s)\n",
		iteration, action_id,
		(resp[0] != '\0') ? "llm" : "pal-fallback");

	return ANX_OK;
}
