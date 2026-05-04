/*
 * test_anunixmacs.c — Tests for the editor (RFC-0023).
 */

#include <anx/types.h>
#include <anx/anunixmacs.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/alloc.h>

static int strstarts(const char *s, const char *pre)
{
	while (*pre) {
		if (*s++ != *pre++) return 0;
	}
	return 1;
}

extern void anx_diag_dump(void);

int test_anunixmacs(void)
{
	int rc;

	anx_objstore_init();
	anx_diag_dump();

	/* --- Test 1: gap buffer insert/delete --- */
	{
		struct anx_ed_buffer *b;
		char tmp[64];
		uint32_t n;
		rc = anx_ed_buf_create(&b);
		if (rc != ANX_OK) return -1;
		anx_ed_buf_insert(b, "hello world", 11);
		if (anx_ed_buf_length(b) != 11) {
			anx_ed_buf_free(b); return -2;
		}
		anx_ed_buf_text(b, tmp, sizeof(tmp), &n);
		if (n != 11 || anx_strcmp(tmp, "hello world") != 0) {
			anx_ed_buf_free(b); return -3;
		}
		anx_ed_buf_goto(b, 5);
		anx_ed_buf_delete(b, 1);
		anx_ed_buf_insert(b, "_", 1);
		anx_ed_buf_text(b, tmp, sizeof(tmp), &n);
		if (anx_strcmp(tmp, "hello_world") != 0) {
			anx_ed_buf_free(b); return -4;
		}
		uint32_t pos;
		rc = anx_ed_buf_search(b, "world", &pos);
		if (rc != ANX_OK || pos != 6) {
			anx_ed_buf_free(b); return -5;
		}
		uint32_t cnt;
		anx_ed_buf_replace_all(b, "world", "anunix", &cnt);
		if (cnt != 1) { anx_ed_buf_free(b); return -6; }
		anx_ed_buf_text(b, tmp, sizeof(tmp), &n);
		if (anx_strcmp(tmp, "hello_anunix") != 0) {
			anx_ed_buf_free(b); return -7;
		}
		anx_ed_buf_free(b);
	}

	/* --- Test 2: eLISP arithmetic --- */
	{
		struct anx_ed_session *s;
		char out[64];
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -8;
		rc = anx_ed_eval(s, "(+ 1 2 3)", false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "6") != 0) {
			anx_ed_session_free(s); return -9;
		}
		rc = anx_ed_eval(s, "(* 7 6)", false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "42") != 0) {
			anx_ed_session_free(s); return -10;
		}
		anx_ed_session_free(s);
	}

	/* --- Test 3: cons / car / cdr / list --- */
	{
		struct anx_ed_session *s;
		char out[64];
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -11;
		rc = anx_ed_eval(s, "(car (cons 1 2))", false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "1") != 0) {
			anx_ed_session_free(s); return -12;
		}
		rc = anx_ed_eval(s, "(length (list 1 2 3 4))", false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "4") != 0) {
			anx_ed_session_free(s); return -13;
		}
		anx_ed_session_free(s);
	}

	/* --- Test 4: setq + lambda --- */
	{
		struct anx_ed_session *s;
		char out[64];
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -14;
		rc = anx_ed_eval(s,
				 "(progn (setq sq (lambda (x) (* x x))) (sq 9))",
				 true, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "81") != 0) {
			anx_ed_session_free(s); return -15;
		}
		anx_ed_session_free(s);
	}

	/* --- Test 5: buffer primitives --- */
	{
		struct anx_ed_session *s;
		char out[128];
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -16;
		rc = anx_ed_eval(s,
				 "(progn"
				 " (setq b (buffer-create))"
				 " (buffer-insert b \"alpha beta\")"
				 " (buffer-goto b 0)"
				 " (buffer-replace b \"alpha\" \"gamma\")"
				 " (buffer-text b))",
				 true, out, sizeof(out));
		if (rc != ANX_OK) { anx_ed_session_free(s); return -17; }
		/* Printer wraps strings in quotes */
		if (anx_strcmp(out, "\"gamma beta\"") != 0) {
			anx_ed_session_free(s); return -18;
		}
		anx_ed_session_free(s);
	}

	/* --- Test 6a: defun + recursion --- */
	{
		struct anx_ed_session *s;
		char out[64];
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -30;
		rc = anx_ed_eval(s,
				 "(progn"
				 " (defun fact (n)"
				 "   (if (= n 0) 1 (* n (fact (- n 1)))))"
				 " (fact 6))",
				 true, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "720") != 0) {
			anx_ed_session_free(s); return -31;
		}
		anx_ed_session_free(s);
	}

	/* --- Test 6b: cond / when / unless / not --- */
	{
		struct anx_ed_session *s;
		char out[64];
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -32;
		rc = anx_ed_eval(s,
				 "(cond ((= 1 2) 99) ((= 1 1) 7) (t 0))",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "7") != 0) {
			anx_ed_session_free(s); return -33;
		}
		rc = anx_ed_eval(s, "(when (= 1 1) 42)",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "42") != 0) {
			anx_ed_session_free(s); return -34;
		}
		rc = anx_ed_eval(s, "(unless nil 99)",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "99") != 0) {
			anx_ed_session_free(s); return -35;
		}
		rc = anx_ed_eval(s, "(not (= 1 2))",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "t") != 0) {
			anx_ed_session_free(s); return -36;
		}
		anx_ed_session_free(s);
	}

	/* --- Test 6c: dolist accumulates via setq --- */
	{
		struct anx_ed_session *s;
		char out[64];
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -37;
		rc = anx_ed_eval(s,
				 "(progn"
				 " (setq sum 0)"
				 " (dolist (x (list 1 2 3 4))"
				 "   (setq sum (+ sum x)))"
				 " sum)",
				 true, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "10") != 0) {
			anx_ed_session_free(s); return -38;
		}
		anx_ed_session_free(s);
	}

	/* --- Test 6d: hooks (add-hook + run-hooks) --- */
	{
		struct anx_ed_session *s;
		char out[64];
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -39;
		rc = anx_ed_eval(s,
				 "(progn"
				 " (setq trace 0)"
				 " (defun bump () (setq trace (+ trace 1)))"
				 " (setq my-hook nil)"
				 " (add-hook 'my-hook 'bump)"
				 " (add-hook 'my-hook 'bump)"
				 " (run-hooks 'my-hook)"
				 " trace)",
				 true, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "2") != 0) {
			anx_ed_session_free(s); return -40;
		}
		anx_ed_session_free(s);
	}

	/* --- Test 7: cell dispatch editor-eval --- */
	{
		struct anx_so_create_params cp;
		struct anx_state_object    *src_obj, *form_obj, *res_obj;
		anx_oid_t                   src_oid, form_oid, out_oid;
		const char                 *src  = "hello world";
		const char                 *form = "(buffer-text (let ((b (buffer-create)))"
						   " (buffer-insert b \"X\") b))";

		anx_memset(&cp, 0, sizeof(cp));
		cp.object_type = ANX_OBJ_BYTE_DATA;
		cp.payload = src; cp.payload_size = anx_strlen(src);
		rc = anx_so_create(&cp, &src_obj);
		if (rc != ANX_OK) return -19;
		src_oid = src_obj->oid; anx_objstore_release(src_obj);

		anx_memset(&cp, 0, sizeof(cp));
		cp.object_type = ANX_OBJ_BYTE_DATA;
		cp.payload = form; cp.payload_size = anx_strlen(form);
		rc = anx_so_create(&cp, &form_obj);
		if (rc != ANX_OK) return -20;
		form_oid = form_obj->oid; anx_objstore_release(form_obj);

		anx_oid_t inputs[2] = { src_oid, form_oid };
		anx_memset(&out_oid, 0, sizeof(out_oid));
		rc = anx_ed_cell_dispatch("editor-eval", inputs, 2, &out_oid);
		if (rc != ANX_OK) return -21;

		res_obj = anx_objstore_lookup(&out_oid);
		if (!res_obj) return -22;
		if (res_obj->object_type != ANX_OBJ_BYTE_DATA) {
			anx_objstore_release(res_obj); return -23;
		}
		const char *p = (const char *)res_obj->payload;
		if (!strstarts(p, "\"X\"")) {
			anx_objstore_release(res_obj); return -24;
		}
		anx_objstore_release(res_obj);
	}

	return 0;
}
