/*
 * Temporary diagnostic harness — included into test_anunixmacs.c via
 * the build, run once at start of test to print actual eval results.
 */

#include <anx/anunixmacs.h>
#include <anx/anxml.h>
#include <anx/audio.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/types.h>

void anx_diag_dump(void)
{
	struct anx_ed_session *s;
	char out[64];
	if (anx_ed_session_create(&s) != ANX_OK) {
		kprintf("DIAG: session_create failed\n");
		return;
	}
	int rc = anx_ed_eval(s, "(+ 1 2 3)", false, out, sizeof(out));
	kprintf("DIAG plus123: rc=%d out=\"%s\"\n", rc, out);
	rc = anx_ed_eval(s, "(* 7 6)", false, out, sizeof(out));
	kprintf("DIAG mul76: rc=%d out=\"%s\"\n", rc, out);
	rc = anx_ed_eval(s, "(car (cons 1 2))", false, out, sizeof(out));
	kprintf("DIAG carcons: rc=%d out=\"%s\"\n", rc, out);
	anx_ed_session_free(s);

	struct anx_anxml_request req;
	struct anx_anxml_response r1, r2;
	anx_anxml_init();
	anx_memset(&req, 0, sizeof(req));
	anx_strlcpy(req.prompt, "anunix is ", sizeof(req.prompt));
	req.prompt_len = anx_strlen(req.prompt);
	req.max_tokens = 16;
	rc = anx_anxml_generate(&req, &r1);
	kprintf("DIAG anxml r1: rc=%d len=%u out=\"", rc, r1.output_len);
	for (uint32_t i = 0; i < r1.output_len && i < 32; i++) {
		char c = r1.output[i];
		if (c == '\n') kprintf("\\n");
		else kprintf("%c", c);
	}
	kprintf("\"\n");
	rc = anx_anxml_generate(&req, &r2);
	kprintf("DIAG anxml r2: rc=%d len=%u out=\"", rc, r2.output_len);
	for (uint32_t i = 0; i < r2.output_len && i < 32; i++) {
		char c = r2.output[i];
		if (c == '\n') kprintf("\\n");
		else kprintf("%c", c);
	}
	kprintf("\"\n");
}
