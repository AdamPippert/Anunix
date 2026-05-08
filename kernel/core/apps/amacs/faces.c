/*
 * faces.c — Face registry for amacs (RFC-0023, Phase 2.2).
 *
 * A face is a (name, fg, bg) triple keyed by name.  The renderer looks
 * up the "face" text property at each character cell, fetches the face
 * by name, and applies the colors when blitting the glyph.
 *
 * The registry is a small fixed-size array — fine for the dozens of
 * faces a real Emacs config defines, and bounded so it can live in
 * static storage with no heap-allocator dependence at startup.
 */

#include <anx/amacs.h>
#include <anx/types.h>
#include <anx/alloc.h>
#include <anx/string.h>

static struct anx_ed_face g_faces[ANX_ED_MAX_FACES];
static bool               g_inited;

static int find_slot(const char *name)
{
	int i;
	for (i = 0; i < ANX_ED_MAX_FACES; i++) {
		if (g_faces[i].name && anx_strcmp(g_faces[i].name, name) == 0)
			return i;
	}
	return -1;
}

static int alloc_slot(void)
{
	int i;
	for (i = 0; i < ANX_ED_MAX_FACES; i++) {
		if (!g_faces[i].name) return i;
	}
	return -1;
}

int anx_ed_face_define(const char *name, uint32_t fg, uint32_t bg)
{
	int slot;
	uint32_t n;
	char *copy;
	if (!name) return ANX_EINVAL;
	slot = find_slot(name);
	if (slot < 0) {
		slot = alloc_slot();
		if (slot < 0) return ANX_ENOMEM;
		n = (uint32_t)anx_strlen(name);
		copy = (char *)anx_alloc(n + 1);
		if (!copy) return ANX_ENOMEM;
		anx_memcpy(copy, name, n);
		copy[n] = '\0';
		g_faces[slot].name = copy;
	}
	g_faces[slot].fg = fg;
	g_faces[slot].bg = bg;
	return ANX_OK;
}

const struct anx_ed_face *anx_ed_face_lookup(const char *name)
{
	int slot;
	if (!name) return NULL;
	if (!g_inited) anx_ed_face_init_defaults();
	slot = find_slot(name);
	if (slot < 0) return NULL;
	return &g_faces[slot];
}

void anx_ed_face_init_defaults(void)
{
	if (g_inited) return;
	g_inited = true;

	/* Editor base */
	anx_ed_face_define("default", 0x00e0e0e0, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("shadow",  0x00808080, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("region",  0x00000000, 0x004060a0);

	/* Generic source-code categories — pre-registered so user fontifiers
	 * have something to attach to without first calling defface. */
	anx_ed_face_define("comment",  0x00808080, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("string",   0x00b0e0a0, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("keyword",  0x00ffaa00, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("constant", 0x00d0a0ff, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("function", 0x0080c0ff, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("variable", 0x00ffe0a0, ANX_ED_FACE_INHERIT_BG);

	/* Org-mode palette — approximates the default Emacs Org colors so a
	 * user .amacs.el can put-text-property face=org-level-N and
	 * see something readable without configuration. */
	anx_ed_face_define("org-level-1", 0x0080d8ff, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("org-level-2", 0x00ffd080, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("org-level-3", 0x00b0e0a0, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("org-level-4", 0x00ffa0a0, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("org-level-5", 0x00d0a0ff, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("org-level-6", 0x0080d8c0, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("org-todo",    0x00ff6060, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("org-done",    0x0080d080, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("org-link",    0x008080ff, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("org-tag",     0x00d0d000, ANX_ED_FACE_INHERIT_BG);
	anx_ed_face_define("org-table",   0x00b0c0d0, ANX_ED_FACE_INHERIT_BG);
}
