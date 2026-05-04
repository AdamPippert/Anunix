/*
 * cell.c — Editor cell dispatch (RFC-0023).
 *
 * Workflow CELL_CALL nodes route here on intents prefixed "editor-".
 *
 * Intent: editor-eval
 *   in[0] = source-text BYTE_DATA OID (loaded into a buffer-handle h0)
 *   in[1] = elisp-form  BYTE_DATA OID (executed against the session)
 *   out   = BYTE_DATA OID containing either the printed final value or,
 *           if the form's value is a buffer handle, the buffer's text.
 */

#include <anx/anunixmacs.h>
#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>

#define EDITOR_RESULT_MAX	8192

static int read_payload_string(const anx_oid_t *oid, char **out, uint32_t *len_out)
{
	struct anx_state_object *obj = anx_objstore_lookup(oid);
	char                    *buf;
	uint32_t                 n;

	if (!obj) return ANX_ENOENT;
	if (!obj->payload) { anx_objstore_release(obj); return ANX_EINVAL; }
	n = (uint32_t)obj->payload_size;
	buf = (char *)anx_alloc(n + 1);
	if (!buf) { anx_objstore_release(obj); return ANX_ENOMEM; }
	anx_memcpy(buf, obj->payload, n);
	buf[n] = '\0';
	anx_objstore_release(obj);
	*out = buf;
	*len_out = n;
	return ANX_OK;
}

static int do_eval(const anx_oid_t *src, const anx_oid_t *form_oid,
		   anx_oid_t *out_oid)
{
	struct anx_ed_session *sess = NULL;
	char                  *src_text = NULL;
	uint32_t               src_len = 0;
	char                  *form_text = NULL;
	uint32_t               form_len = 0;
	char                  *result_buf = NULL;
	int                    rc;

	rc = anx_ed_session_create(&sess);
	if (rc != ANX_OK) goto out;

	if (src) {
		rc = read_payload_string(src, &src_text, &src_len);
		if (rc != ANX_OK) goto out;
		/* Preload an h0 buffer with source via an inline form */
		struct anx_ed_buffer *buf;
		rc = anx_ed_buf_create_from_bytes(src_text, src_len, &buf);
		if (rc != ANX_OK) goto out;
		sess->buffers[0] = buf;
	}

	rc = read_payload_string(form_oid, &form_text, &form_len);
	if (rc != ANX_OK) goto out;

	result_buf = (char *)anx_alloc(EDITOR_RESULT_MAX);
	if (!result_buf) { rc = ANX_ENOMEM; goto out; }
	rc = anx_ed_eval(sess, form_text, true, result_buf, EDITOR_RESULT_MAX);
	if (rc != ANX_OK) goto out;

	{
		struct anx_so_create_params cp;
		struct anx_state_object    *out_obj;
		uint32_t                    rlen = (uint32_t)anx_strlen(result_buf);

		anx_memset(&cp, 0, sizeof(cp));
		cp.object_type    = ANX_OBJ_BYTE_DATA;
		cp.schema_uri     = "anx:schema/anunixmacs/result/v1";
		cp.schema_version = "1";
		cp.payload        = result_buf;
		cp.payload_size   = rlen;
		rc = anx_so_create(&cp, &out_obj);
		if (rc == ANX_OK) {
			*out_oid = out_obj->oid;
			anx_objstore_release(out_obj);
		}
	}

out:
	if (sess)        anx_ed_session_free(sess);
	if (src_text)    anx_free(src_text);
	if (form_text)   anx_free(form_text);
	if (result_buf)  anx_free(result_buf);
	return rc;
}

int anx_ed_cell_dispatch(const char *intent,
			 const anx_oid_t *in_oids, uint32_t in_count,
			 anx_oid_t *out_oid_out)
{
	if (!intent || !out_oid_out) return ANX_EINVAL;
	anx_memset(out_oid_out, 0, sizeof(*out_oid_out));

	if (anx_strcmp(intent, "editor-eval") == 0) {
		if (in_count < 2)
			return ANX_EINVAL;
		return do_eval(&in_oids[0], &in_oids[1], out_oid_out);
	}
	return ANX_ENOSYS;
}
