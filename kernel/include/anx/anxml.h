/*
 * anx/anxml.h — Inference Runtime (RFC-0021).
 *
 * anxml turns a model namespace plus an input prompt into a stream of
 * output tokens.  The kernel-side runtime exposes:
 *
 *   - operator extensions on top of tensor_ops (RMSNorm, embed, sampler)
 *   - a generation loop that consumes a model manifest and emits tokens
 *   - an inference cell dispatch hook used by workflow_exec
 *
 * v1 ships a CPU-only path against a built-in toy model so that the
 * MODEL_CALL workflow path is end-to-end functional.  Loading external
 * GGUF / safetensors weights is wired through anx_model_* (RFC-0013).
 */

#ifndef ANX_ANXML_H
#define ANX_ANXML_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Generation request                                                  */
/* ------------------------------------------------------------------ */

#define ANX_ANXML_PROMPT_MAX	1024
#define ANX_ANXML_OUTPUT_MAX	2048
#define ANX_ANXML_MODEL_NAME_MAX 64

struct anx_anxml_request {
	char     model_name[ANX_ANXML_MODEL_NAME_MAX];
	char     prompt[ANX_ANXML_PROMPT_MAX];
	uint32_t prompt_len;
	uint32_t max_tokens;	/* 0 → ANX_ANXML_DEFAULT_MAX */
	uint32_t seed;		/* 0 → deterministic */
};

struct anx_anxml_response {
	char     output[ANX_ANXML_OUTPUT_MAX];
	uint32_t output_len;
	uint32_t tokens_generated;
};

#define ANX_ANXML_DEFAULT_MAX	128

/* ------------------------------------------------------------------ */
/* Subsystem lifecycle                                                 */
/* ------------------------------------------------------------------ */

void anx_anxml_init(void);

/*
 * Run a single generation request.  Synchronous; returns ANX_OK on
 * success and fills *resp.
 *
 * Model name semantics (v1):
 *   "anx:model/default"   → built-in toy model (always available).
 *   "anx:model/<name>"    → look up a model manifest registered via
 *                            anx_model_create; fall back to default if
 *                            not found.
 */
int anx_anxml_generate(const struct anx_anxml_request *req,
		       struct anx_anxml_response *resp);

/* ------------------------------------------------------------------ */
/* Cell dispatch                                                       */
/* ------------------------------------------------------------------ */

/*
 * Dispatch entry point used by workflow_exec when a CELL_CALL intent
 * starts with "anxml-".  Single supported intent in v1:
 *
 *   anxml-generate    in[0] = prompt object (BYTE_DATA, UTF-8)
 *                     out   = MODEL_OUTPUT object with the response text
 */
int anx_anxml_cell_dispatch(const char *intent,
			    const anx_oid_t *in_oids, uint32_t in_count,
			    anx_oid_t *out_oid_out);

/* ------------------------------------------------------------------ */
/* Toy model parameters (v1; exposed for tests)                        */
/* ------------------------------------------------------------------ */

/*
 * The built-in toy model is a fixed-vocabulary character LM that maps
 * 7-bit ASCII bytes to themselves with a small bigram smoothing.  It is
 * deterministic given (prompt, seed) and exists so that the MODEL_CALL
 * path can be exercised end-to-end without external weights.
 */
#define ANX_ANXML_TOY_VOCAB	128

#endif /* ANX_ANXML_H */
