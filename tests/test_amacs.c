/*
 * test_amacs.c — Tests for the editor (RFC-0023).
 */

#include <anx/types.h>
#include <anx/amacs.h>
#include <anx/input.h>
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

int test_amacs(void)
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

	/* --- Test 6e: markers track insertions and deletions --- */
	{
		struct anx_ed_buffer *b;
		struct anx_ed_marker *m_lo, *m_mid, *m_hi, *m_at;
		rc = anx_ed_buf_create(&b);
		if (rc != ANX_OK) return -41;
		anx_ed_buf_insert(b, "0123456789", 10);

		/* m_lo at 2 (before any edit) — should not move on inserts at >=5 */
		m_lo  = anx_ed_marker_new(b, 2);
		/* m_mid at 5 with default insertion_type=false — stays put when
		 *   we insert exactly at 5 */
		m_mid = anx_ed_marker_new(b, 5);
		/* m_at  at 5 with insertion_type=true — moves with the insert */
		m_at  = anx_ed_marker_new(b, 5);
		m_at->insertion_type = true;
		/* m_hi at 8 — pushed forward by inserts at 5 */
		m_hi  = anx_ed_marker_new(b, 8);

		anx_ed_buf_goto(b, 5);
		anx_ed_buf_insert(b, "ABC", 3);
		/* Buffer is now "01234ABC56789" */
		if (anx_ed_marker_position(m_lo)  != 2)  goto m_fail;
		if (anx_ed_marker_position(m_mid) != 5)  goto m_fail;
		if (anx_ed_marker_position(m_at)  != 8)  goto m_fail;
		if (anx_ed_marker_position(m_hi)  != 11) goto m_fail;

		/* Delete "ABC" — markers between 5 and 8 collapse to 5 */
		anx_ed_buf_goto(b, 5);
		anx_ed_buf_delete(b, 3);
		if (anx_ed_marker_position(m_lo)  != 2) goto m_fail;
		if (anx_ed_marker_position(m_mid) != 5) goto m_fail;
		if (anx_ed_marker_position(m_at)  != 5) goto m_fail;
		if (anx_ed_marker_position(m_hi)  != 8) goto m_fail;

		/* Free buffer — markers detach but stay live */
		anx_ed_buf_free(b);
		if (anx_ed_marker_attached(m_lo)) goto m_fail2;
		anx_ed_marker_release(m_lo);
		anx_ed_marker_release(m_mid);
		anx_ed_marker_release(m_at);
		anx_ed_marker_release(m_hi);
		goto m_ok;
	m_fail:
		anx_ed_buf_free(b);
		anx_ed_marker_release(m_lo);
		anx_ed_marker_release(m_mid);
		anx_ed_marker_release(m_at);
		anx_ed_marker_release(m_hi);
		return -42;
	m_fail2:
		anx_ed_marker_release(m_lo);
		anx_ed_marker_release(m_mid);
		anx_ed_marker_release(m_at);
		anx_ed_marker_release(m_hi);
		return -43;
	m_ok: ;
	}

	/* --- Test 6f: marker eLISP primitives --- */
	{
		struct anx_ed_session *s;
		char out[64];
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -44;

		/* make-marker is detached, position reads as nil */
		rc = anx_ed_eval(s, "(marker-position (make-marker))",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "nil") != 0) {
			anx_ed_session_free(s); return -45;
		}

		/* markerp recognizes its own */
		rc = anx_ed_eval(s, "(markerp (make-marker))",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "t") != 0) {
			anx_ed_session_free(s); return -46;
		}

		/* markerp says nil for an integer */
		rc = anx_ed_eval(s, "(markerp 5)",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "nil") != 0) {
			anx_ed_session_free(s); return -47;
		}
		anx_ed_session_free(s);
	}

	/* --- Test 6g: text property put/get/remove + insert/delete shifts --- */
	{
		struct anx_ed_buffer *b;
		const char *v;
		rc = anx_ed_buf_create(&b);
		if (rc != ANX_OK) return -48;
		anx_ed_buf_insert(b, "hello world", 11);

		/* Tag "hello" with face=red. */
		rc = anx_ed_buf_put_property(b, 0, 5, "face", "red");
		if (rc != ANX_OK) { anx_ed_buf_free(b); return -49; }
		v = anx_ed_buf_get_property(b, 0, "face");
		if (!v || anx_strcmp(v, "red") != 0) {
			anx_ed_buf_free(b); return -50;
		}
		v = anx_ed_buf_get_property(b, 4, "face");
		if (!v || anx_strcmp(v, "red") != 0) {
			anx_ed_buf_free(b); return -51;
		}
		v = anx_ed_buf_get_property(b, 5, "face");
		if (v) { anx_ed_buf_free(b); return -52; }

		/* Insert "X" at start — region shifts right by 1. */
		anx_ed_buf_goto(b, 0);
		anx_ed_buf_insert(b, "X", 1);
		v = anx_ed_buf_get_property(b, 0, "face");
		if (v) { anx_ed_buf_free(b); return -53; }
		v = anx_ed_buf_get_property(b, 1, "face");
		if (!v || anx_strcmp(v, "red") != 0) {
			anx_ed_buf_free(b); return -54;
		}
		v = anx_ed_buf_get_property(b, 5, "face");
		if (!v || anx_strcmp(v, "red") != 0) {
			anx_ed_buf_free(b); return -55;
		}

		/* Delete the inserted X — region snaps back. */
		anx_ed_buf_goto(b, 0);
		anx_ed_buf_delete(b, 1);
		v = anx_ed_buf_get_property(b, 0, "face");
		if (!v || anx_strcmp(v, "red") != 0) {
			anx_ed_buf_free(b); return -56;
		}

		/* Overlapping put with a different value. */
		anx_ed_buf_put_property(b, 3, 8, "face", "blue");
		v = anx_ed_buf_get_property(b, 2, "face");
		if (!v || anx_strcmp(v, "red") != 0) {
			anx_ed_buf_free(b); return -57;
		}
		v = anx_ed_buf_get_property(b, 4, "face");
		if (!v || anx_strcmp(v, "blue") != 0) {
			anx_ed_buf_free(b); return -58;
		}
		v = anx_ed_buf_get_property(b, 7, "face");
		if (!v || anx_strcmp(v, "blue") != 0) {
			anx_ed_buf_free(b); return -59;
		}

		/* Remove face from middle — leaves bookend intervals. */
		anx_ed_buf_remove_property(b, 4, 7, "face");
		v = anx_ed_buf_get_property(b, 3, "face");
		if (!v || anx_strcmp(v, "blue") != 0) {
			anx_ed_buf_free(b); return -60;
		}
		v = anx_ed_buf_get_property(b, 5, "face");
		if (v) { anx_ed_buf_free(b); return -61; }
		v = anx_ed_buf_get_property(b, 7, "face");
		if (!v || anx_strcmp(v, "blue") != 0) {
			anx_ed_buf_free(b); return -62;
		}

		anx_ed_buf_free(b);
	}

	/* --- Test 6h: faces (defface + lookup, hex literals in reader) --- */
	{
		struct anx_ed_session *s;
		char out[64];
		const struct anx_ed_face *f;
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -63;

		/* 0x hex literal parses as int. */
		rc = anx_ed_eval(s, "0xff8800", false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "16746496") != 0) {
			anx_ed_session_free(s); return -64;
		}

		/* defface stores into the registry. */
		rc = anx_ed_eval(s, "(defface 'unit-test-face 0x123456 0xabcdef)",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "t") != 0) {
			anx_ed_session_free(s); return -65;
		}
		f = anx_ed_face_lookup("unit-test-face");
		if (!f) { anx_ed_session_free(s); return -66; }
		if (f->fg != 0x123456 || f->bg != 0xabcdef) {
			anx_ed_session_free(s); return -67;
		}

		/* Built-in org-level-1 is registered by the defaults init. */
		f = anx_ed_face_lookup("org-level-1");
		if (!f) { anx_ed_session_free(s); return -68; }

		anx_ed_session_free(s);
	}

	/* --- Test 6i: defmacro — receives args unevaluated --- */
	{
		struct anx_ed_session *s;
		char out[64];
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -69;

		/* (my-when c body...) ⇒ (if c (progn body...) nil).
		 * Demonstrates that BODY arrives unevaluated — using cdr to
		 * splice it into a generated form. */
		rc = anx_ed_eval(s,
				 "(progn"
				 " (defmacro my-when (c x) (list 'if c x nil))"
				 " (my-when (= 1 1) (+ 10 32)))",
				 true, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "42") != 0) {
			anx_ed_session_free(s); return -70;
		}

		/* Confirm the macro short-circuits — the false branch never
		 * touches the side-effecting form. */
		rc = anx_ed_eval(s,
				 "(progn"
				 " (setq side 0)"
				 " (defmacro id-eval (x) x)"
				 " (defmacro skip (x) nil)"
				 " (skip (setq side 99))"
				 " side)",
				 true, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "0") != 0) {
			anx_ed_session_free(s); return -71;
		}

		anx_ed_session_free(s);
	}

	/* --- Test 6j: define-key parses descriptors and registers --- */
	{
		struct anx_ed_session *s;
		char out[64];
		uint32_t mods, key;
		const char *fn;
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -72;

		/* Parse a simple chord. */
		if (anx_ed_keymap_parse("C-c", &mods, &key) != ANX_OK ||
		    mods != ANX_MOD_CTRL || key != ANX_KEY_C) {
			anx_ed_session_free(s); return -73;
		}

		/* (define-key 'global "C-c" 'foo) registers it. */
		rc = anx_ed_eval(s, "(define-key 'global \"C-c\" 'foo)",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "t") != 0) {
			anx_ed_session_free(s); return -74;
		}
		fn = anx_ed_keymap_lookup(ANX_MOD_CTRL, ANX_KEY_C);
		if (!fn || anx_strcmp(fn, "foo") != 0) {
			anx_ed_session_free(s); return -75;
		}

		/* Re-binding overwrites. */
		rc = anx_ed_eval(s, "(global-set-key \"C-c\" 'bar)",
				 false, out, sizeof(out));
		if (rc != ANX_OK) { anx_ed_session_free(s); return -76; }
		fn = anx_ed_keymap_lookup(ANX_MOD_CTRL, ANX_KEY_C);
		if (!fn || anx_strcmp(fn, "bar") != 0) {
			anx_ed_session_free(s); return -77;
		}

		/* Named key parsing. */
		if (anx_ed_keymap_parse("M-RET", &mods, &key) != ANX_OK ||
		    mods != ANX_MOD_ALT || key != ANX_KEY_ENTER) {
			anx_ed_session_free(s); return -78;
		}

		anx_ed_session_free(s);
	}

  /* --- Test 6k: condition-case + unwind-protect --- */
	{
		struct anx_ed_session *s;
		char out[64];
		rc = anx_ed_session_create(&s);
		if (rc != ANX_OK) return -79;

		/* Error inside protected form is caught; handler runs and
		 * returns its value. */
		rc = anx_ed_eval(s,
				 "(condition-case e (error \"boom\")"
				 "  (error 99))",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "99") != 0) {
			anx_ed_session_free(s); return -80;
		}

		/* Without an error, condition-case returns the body's value. */
		rc = anx_ed_eval(s,
				 "(condition-case e (+ 2 3)"
				 "  (error 99))",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "5") != 0) {
			anx_ed_session_free(s); return -81;
		}

		/* Error data is bound to VAR inside the handler. */
		rc = anx_ed_eval(s,
				 "(condition-case e (error \"oops\")"
				 "  (error e))",
				 false, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "\"oops\"") != 0) {
			anx_ed_session_free(s); return -82;
		}

		/* unwind-protect runs the unwind forms even when body
		 * succeeds; the body's value is what's returned. */
		rc = anx_ed_eval(s,
				 "(progn"
				 " (setq cleanup 0)"
				 " (setq r (unwind-protect"
				 "           (+ 1 2)"
				 "           (setq cleanup 1)))"
				 " (list r cleanup))",
				 true, out, sizeof(out));
		if (rc != ANX_OK || anx_strcmp(out, "(3 1)") != 0) {
			anx_ed_session_free(s); return -83;
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
