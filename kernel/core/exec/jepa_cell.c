/*
 * jepa_cell.c — JEPA operation dispatcher for workflow CELL_CALL nodes.
 *
 * When a workflow CELL_CALL node carries a "jepa-*" intent string,
 * workflow_exec.c routes control here instead of creating a generic
 * execution cell.  Each intent maps directly to one JEPA primitive,
 * keeping the workflow executor decoupled from the JEPA internals.
 *
 * All operations produce a single output State Object OID.  They are
 * non-fatal: if JEPA is unavailable the operation returns ANX_ENODEV
 * and the workflow executor falls back to suspending the node.
 */

#include <anx/jepa_cell.h>
#include <anx/jepa.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Action name → action_id table                                       */
/* ------------------------------------------------------------------ */

static uint32_t action_from_suffix(const char *suffix)
{
	static const char *names[] = {
		"idle",
		"route_local",
		"route_remote",
		"route_fallback",
		"mem_promote",
		"mem_demote",
		"mem_forget",
		"cell_spawn",
		"cell_cancel",
		"cap_validate",
		"cap_suspend",
		"security_alert",
	};
	uint32_t i;

	for (i = 0; i < (uint32_t)(sizeof(names) / sizeof(names[0])); i++) {
		if (anx_strcmp(suffix, names[i]) == 0)
			return i;
	}
	return 0; /* default: IDLE */
}

/* ------------------------------------------------------------------ */
/* Store a float as ANX_OBJ_BYTE_DATA for diverge output              */
/* ------------------------------------------------------------------ */

static int store_float_obj(float val, anx_oid_t *oid_out)
{
	struct anx_so_create_params cp;
	struct anx_state_object    *obj;
	int rc;

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_BYTE_DATA;
	cp.schema_uri     = "anx:schema/jepa/divergence/v1";
	cp.schema_version = "1";
	cp.payload        = &val;
	cp.payload_size   = sizeof(val);

	rc = anx_so_create(&cp, &obj);
	if (rc != ANX_OK)
		return rc;

	*oid_out = obj->oid;
	anx_objstore_release(obj);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Public dispatch entry point                                         */
/* ------------------------------------------------------------------ */

int anx_jepa_cell_dispatch(const char *intent,
			   const anx_oid_t *in_oids, uint32_t in_count,
			   anx_oid_t *out_oid_out)
{
	anx_memset(out_oid_out, 0, sizeof(*out_oid_out));

	if (!anx_jepa_available())
		return ANX_ENODEV;

	/* jepa-observe: collect kernel observation, store as OBS SO.
	 * If no world profile is active, fall back to a zero observation
	 * (same non-fatal pattern as loop_belief.c) so downstream nodes
	 * always receive a valid OBS OID. */
	if (anx_strcmp(intent, "jepa-observe") == 0) {
		struct anx_jepa_obs obs;
		int rc;

		anx_memset(&obs, 0, sizeof(obs));
		(void)anx_jepa_observe(&obs); /* best-effort; ignore ENOENT */
		rc = anx_jepa_observe_store(&obs, out_oid_out);
		kprintf("[jepa-cell] observe: rc=%d\n", rc);
		return rc;
	}

	/* jepa-encode: encode OBS → LATENT (input[0] = OBS OID) */
	if (anx_strcmp(intent, "jepa-encode") == 0) {
		int rc;
		if (in_count < 1)
			return ANX_EINVAL;
		rc = anx_jepa_encode(&in_oids[0], out_oid_out);
		kprintf("[jepa-cell] encode: rc=%d\n", rc);
		return rc;
	}

	/* jepa-observe-encode: observe + encode in one shot.
	 * Same zero-obs fallback as jepa-observe. */
	if (anx_strcmp(intent, "jepa-observe-encode") == 0) {
		struct anx_jepa_obs obs;
		int rc;

		anx_memset(&obs, 0, sizeof(obs));
		(void)anx_jepa_observe(&obs);
		rc = anx_jepa_encode_obs(&obs, out_oid_out);
		kprintf("[jepa-cell] observe-encode: rc=%d\n", rc);
		return rc;
	}

	/* jepa-predict[:<action>]: predict next latent from LATENT + action */
	if (intent[0]=='j' && intent[1]=='e' && intent[2]=='p' &&
	    intent[3]=='a' && intent[4]=='-' && intent[5]=='p' &&
	    intent[6]=='r' && intent[7]=='e' && intent[8]=='d' &&
	    intent[9]=='i' && intent[10]=='c' && intent[11]=='t' &&
	    (intent[12]=='\0' || intent[12]==':')) {
		const char *sep = &intent[12];
		uint32_t action_id = 0;
		int rc;

		if (in_count < 1)
			return ANX_EINVAL;

		/* Optional :<action> suffix */
		if (*sep == ':')
			action_id = action_from_suffix(sep + 1);

		rc = anx_jepa_predict(&in_oids[0], action_id, out_oid_out);
		kprintf("[jepa-cell] predict: action=%u rc=%d\n", action_id, rc);
		return rc;
	}

	/* jepa-diverge: cosine divergence between two latents */
	if (anx_strcmp(intent, "jepa-diverge") == 0) {
		float d;
		int rc;

		if (in_count < 2)
			return ANX_EINVAL;

		d = anx_jepa_divergence(&in_oids[0], &in_oids[1]);
		if (d < 0.0f)
			return ANX_EIO;

		rc = store_float_obj(d, out_oid_out);
		kprintf("[jepa-cell] diverge: d=%u/1000 rc=%d\n",
			(unsigned int)(d * 1000.0f + 0.5f), rc);
		return rc;
	}

	kprintf("[jepa-cell] unknown intent: %s\n", intent);
	return ANX_ENOTSUP;
}
