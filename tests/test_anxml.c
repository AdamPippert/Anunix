/*
 * test_anxml.c — Tests for the inference runtime (RFC-0021).
 */

#include <anx/types.h>
#include <anx/anxml.h>
#include <anx/state_object.h>
#include <anx/string.h>

int test_anxml(void)
{
	struct anx_anxml_request  req;
	struct anx_anxml_response resp;
	int                       rc;

	anx_objstore_init();
	anx_anxml_init();

	/* --- Test 1: deterministic greedy generate --- */
	anx_memset(&req, 0, sizeof(req));
	anx_strlcpy(req.prompt, "anunix is ", sizeof(req.prompt));
	req.prompt_len  = (uint32_t)anx_strlen(req.prompt);
	req.max_tokens  = 32;
	req.seed        = 0;
	rc = anx_anxml_generate(&req, &resp);
	if (rc != ANX_OK)            return -1;
	if (resp.tokens_generated == 0) return -2;
	if (resp.output_len == 0)       return -3;

	/* Two greedy runs with same prompt+seed must match. */
	struct anx_anxml_response resp2;
	rc = anx_anxml_generate(&req, &resp2);
	if (rc != ANX_OK)             return -4;
	if (resp.output_len != resp2.output_len)            return -5;
	if (anx_memcmp(resp.output, resp2.output, resp.output_len) != 0)
		return -6;

	/* --- Test 2: seeded sampling produces different stream from greedy --- */
	struct anx_anxml_response resp3;
	req.seed = 0x12345678;
	rc = anx_anxml_generate(&req, &resp3);
	if (rc != ANX_OK)             return -7;
	if (resp3.tokens_generated == 0) return -8;
	/* (Seeded vs greedy may coincidentally match on a short prefix; we
	 * just require *some* output.) */

	/* --- Test 3: cell dispatch through OID --- */
	{
		struct anx_so_create_params cp;
		struct anx_state_object    *prompt_obj;
		anx_oid_t                   prompt_oid, out_oid;
		const char                 *p = "the answer is ";

		anx_memset(&cp, 0, sizeof(cp));
		cp.object_type    = ANX_OBJ_BYTE_DATA;
		cp.payload        = p;
		cp.payload_size   = (uint64_t)anx_strlen(p);
		rc = anx_so_create(&cp, &prompt_obj);
		if (rc != ANX_OK) return -9;
		prompt_oid = prompt_obj->oid;
		anx_objstore_release(prompt_obj);

		anx_memset(&out_oid, 0, sizeof(out_oid));
		rc = anx_anxml_cell_dispatch("anxml-generate",
					     &prompt_oid, 1, &out_oid);
		if (rc != ANX_OK)              return -10;
		struct anx_state_object *out_obj = anx_objstore_lookup(&out_oid);
		if (!out_obj)                  return -11;
		if (out_obj->object_type != ANX_OBJ_MODEL_OUTPUT) {
			anx_objstore_release(out_obj);
			return -12;
		}
		if (out_obj->payload_size == 0) {
			anx_objstore_release(out_obj);
			return -13;
		}
		anx_objstore_release(out_obj);
	}

	/* --- Test 4: bad intent rejected --- */
	anx_oid_t out;
	rc = anx_anxml_cell_dispatch("anxml-bogus", NULL, 0, &out);
	if (rc != ANX_ENOSYS) return -14;

	return 0;
}
