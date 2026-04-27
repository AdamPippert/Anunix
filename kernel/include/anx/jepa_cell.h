/*
 * anx/jepa_cell.h — JEPA operation dispatcher for workflow CELL_CALL nodes.
 *
 * Exposes anx_jepa_cell_dispatch(), called by workflow_exec.c when a
 * CELL_CALL node carries a "jepa-*" intent string.  This is the bridge
 * between the workflow port/edge model and the JEPA primitive API.
 */

#ifndef ANX_JEPA_CELL_H
#define ANX_JEPA_CELL_H

#include <anx/types.h>

/*
 * Dispatch a single JEPA operation identified by an intent string.
 *
 * Recognised intents:
 *   "jepa-observe"             — collect kernel observation (no input needed)
 *                                → output: ANX_OBJ_JEPA_OBS
 *   "jepa-encode"              — encode OBS → latent  (input[0]: JEPA_OBS OID)
 *                                → output: ANX_OBJ_JEPA_LATENT
 *   "jepa-observe-encode"      — observe + encode in one step (no input)
 *                                → output: ANX_OBJ_JEPA_LATENT
 *   "jepa-predict[:<action>]"  — predict next latent (input[0]: JEPA_LATENT OID)
 *                                action defaults to "idle" when omitted
 *                                → output: ANX_OBJ_JEPA_LATENT
 *   "jepa-diverge"             — cosine divergence between two latents
 *                                (input[0]: JEPA_LATENT, input[1]: JEPA_LATENT)
 *                                → output: ANX_OBJ_BYTE_DATA (float32 value)
 *
 * Returns ANX_OK and fills *out_oid_out on success.
 * Returns ANX_ENOTSUP if the intent prefix is unrecognised.
 * Returns ANX_ENODEV if JEPA is unavailable.
 */
int anx_jepa_cell_dispatch(const char *intent,
			   const anx_oid_t *in_oids, uint32_t in_count,
			   anx_oid_t *out_oid_out);

#endif /* ANX_JEPA_CELL_H */
