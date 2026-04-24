/*
 * test_clipboard.c — Deterministic tests for P1-003: clipboard,
 * drag-and-drop, and file-picker interface contracts.
 *
 * Tests:
 * - P1-003-U01: unauthorized clipboard access denied
 * - P1-003-I02: copy/paste roundtrip for text payload
 * - P1-003-I03: drag/drop file metadata reaches target surface
 */

#include <anx/types.h>
#include <anx/interface_plane.h>
#include <anx/clipboard.h>
#include <anx/input.h>
#include <anx/uuid.h>
#include <anx/string.h>

#define ASSERT(cond, code)    do { if (!(cond)) return (code); } while (0)
#define ASSERT_EQ(a, b, code) do { if ((a) != (b)) return (code); } while (0)

static int noop_map(struct anx_surface *s)    { (void)s; return ANX_OK; }
static int noop_commit(struct anx_surface *s) { (void)s; return ANX_OK; }
static void noop_damage(struct anx_surface *s,
                         int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	(void)s; (void)x; (void)y; (void)w; (void)h;
}
static void noop_unmap(struct anx_surface *s) { (void)s; }

static const struct anx_renderer_ops test_ops = {
	.map    = noop_map,
	.commit = noop_commit,
	.damage = noop_damage,
	.unmap  = noop_unmap,
};

static int test_setup(void)
{
	int rc;

	rc = anx_iface_init();  /* also calls anx_clipboard_init() */
	if (rc != ANX_OK)
		return rc;

	rc = anx_iface_renderer_register(ANX_ENGINE_RENDERER_HEADLESS,
	                                  &test_ops, "test-headless");
	if (rc != ANX_OK)
		return rc;

	rc = anx_iface_env_define("visual-desktop", "anx:schema/env/v1", 0);
	if (rc != ANX_OK && rc != ANX_EEXIST)
		return rc;

	anx_iface_event_reset();
	anx_input_stats_reset();
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* P1-003-U01: unauthorized clipboard access denied                    */
/* ------------------------------------------------------------------ */

static int test_clipboard_access_control(void)
{
	anx_cid_t cid_none, cid_write, cid_both;
	char mime[ANX_CLIPBOARD_MIME_MAX];
	uint8_t buf[64];
	uint32_t len;
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -100);

	/* Three cells: no grants, write-only, read+write. */
	anx_uuid_generate(&cid_none);
	anx_uuid_generate(&cid_write);
	anx_uuid_generate(&cid_both);

	rc = anx_clipboard_grant(cid_write, ANX_CLIPBOARD_FLAG_WRITE);
	ASSERT_EQ(rc, ANX_OK, -101);
	rc = anx_clipboard_grant(cid_both,
	                          ANX_CLIPBOARD_FLAG_READ |
	                          ANX_CLIPBOARD_FLAG_WRITE);
	ASSERT_EQ(rc, ANX_OK, -102);

	/* cid_none: write denied */
	rc = anx_clipboard_write(cid_none, "text/plain", "hello", 5);
	ASSERT_EQ(rc, ANX_EPERM, -103);

	/* cid_none: read denied */
	rc = anx_clipboard_read(cid_none, mime, sizeof(mime),
	                         buf, sizeof(buf), &len);
	ASSERT_EQ(rc, ANX_EPERM, -104);

	/* cid_write: write succeeds */
	rc = anx_clipboard_write(cid_write, "text/plain", "hello", 5);
	ASSERT_EQ(rc, ANX_OK, -105);

	/* cid_write: read denied (no READ flag) */
	rc = anx_clipboard_read(cid_write, mime, sizeof(mime),
	                         buf, sizeof(buf), &len);
	ASSERT_EQ(rc, ANX_EPERM, -106);

	/* cid_both: read succeeds — gets what cid_write wrote */
	rc = anx_clipboard_read(cid_both, mime, sizeof(mime),
	                         buf, sizeof(buf), &len);
	ASSERT_EQ(rc, ANX_OK, -107);
	ASSERT_EQ(len, 5u, -108);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-003-I02: copy/paste roundtrip for text payload                   */
/* ------------------------------------------------------------------ */

static int test_clipboard_copy_paste(void)
{
	anx_cid_t cid;
	char mime[ANX_CLIPBOARD_MIME_MAX];
	uint8_t buf[128];
	uint32_t len;
	int rc;

	static const char text_a[] = "clipboard text payload";
	static const char text_b[] = "second clipboard write";

	rc = test_setup();
	ASSERT(rc == ANX_OK, -200);

	anx_uuid_generate(&cid);
	rc = anx_clipboard_grant(cid,
	                          ANX_CLIPBOARD_FLAG_READ |
	                          ANX_CLIPBOARD_FLAG_WRITE);
	ASSERT_EQ(rc, ANX_OK, -201);

	/* Write first payload. */
	rc = anx_clipboard_write(cid, "text/plain",
	                          text_a, (uint32_t)anx_strlen(text_a));
	ASSERT_EQ(rc, ANX_OK, -202);

	/* Read back; verify content and MIME type. */
	rc = anx_clipboard_read(cid, mime, sizeof(mime),
	                         buf, sizeof(buf), &len);
	ASSERT_EQ(rc, ANX_OK, -203);
	ASSERT_EQ(len, (uint32_t)anx_strlen(text_a), -204);
	ASSERT(anx_strcmp(mime, "text/plain") == 0, -205);
	ASSERT(anx_memcmp(buf, text_a, len) == 0, -206);

	/* Overwrite with second payload. */
	rc = anx_clipboard_write(cid, "text/html",
	                          text_b, (uint32_t)anx_strlen(text_b));
	ASSERT_EQ(rc, ANX_OK, -207);

	rc = anx_clipboard_read(cid, mime, sizeof(mime),
	                         buf, sizeof(buf), &len);
	ASSERT_EQ(rc, ANX_OK, -208);
	ASSERT_EQ(len, (uint32_t)anx_strlen(text_b), -209);
	ASSERT(anx_strcmp(mime, "text/html") == 0, -210);
	ASSERT(anx_memcmp(buf, text_b, len) == 0, -211);

	/* Clear and verify subsequent read returns ENOENT. */
	anx_clipboard_clear();
	rc = anx_clipboard_read(cid, mime, sizeof(mime),
	                         buf, sizeof(buf), &len);
	ASSERT_EQ(rc, ANX_ENOENT, -212);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-003-I03: drag/drop file metadata reaches target surface          */
/* ------------------------------------------------------------------ */

static int test_drag_drop_delivery(void)
{
	struct anx_surface *src, *dst;
	struct anx_drag_payload payload;
	static const char uri[] = "/tmp/dropped-file.txt";
	int rc;

	rc = test_setup();
	ASSERT(rc == ANX_OK, -300);

	rc = anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS,
	                               NULL, 0, 0, 100, 100, &src);
	ASSERT(rc == ANX_OK, -301);
	rc = anx_iface_surface_map(src);
	ASSERT(rc == ANX_OK, -302);

	rc = anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS,
	                               NULL, 0, 0, 100, 100, &dst);
	ASSERT(rc == ANX_OK, -303);
	rc = anx_iface_surface_map(dst);
	ASSERT(rc == ANX_OK, -304);

	/* Begin drag from src with URI list payload. */
	rc = anx_iface_drag_begin(src, "text/uri-list",
	                           uri, (uint32_t)anx_strlen(uri));
	ASSERT_EQ(rc, ANX_OK, -305);

	/* A second begin while one is active must fail. */
	rc = anx_iface_drag_begin(src, "text/plain", "x", 1);
	ASSERT_EQ(rc, ANX_EBUSY, -306);

	/* Deliver to target. */
	rc = anx_iface_drag_deliver(dst, &payload);
	ASSERT_EQ(rc, ANX_OK, -307);

	/* Verify metadata reaches destination intact. */
	ASSERT(anx_strcmp(payload.mime_type, "text/uri-list") == 0, -308);
	ASSERT_EQ(payload.data_len, (uint32_t)anx_strlen(uri), -309);
	ASSERT(anx_memcmp(payload.data, uri, payload.data_len) == 0, -310);
	ASSERT(anx_uuid_compare(&payload.source_surf, &src->oid) == 0, -311);

	/* Deliver with no active drag must return ENOENT. */
	rc = anx_iface_drag_deliver(dst, &payload);
	ASSERT_EQ(rc, ANX_ENOENT, -312);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Test suite entry point                                               */
/* ------------------------------------------------------------------ */

int test_clipboard(void)
{
	int rc;

	rc = test_clipboard_access_control();
	if (rc != 0)
		return rc;

	rc = test_clipboard_copy_paste();
	if (rc != 0)
		return rc;

	rc = test_drag_drop_delivery();
	if (rc != 0)
		return rc;

	return 0;
}
