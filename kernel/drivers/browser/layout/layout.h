/*
 * layout.h — Block/inline layout engine.
 *
 * Converts a DOM tree into a flat list of paint commands (boxes + text runs).
 * Block elements stack vertically; inline content word-wraps.
 */

#ifndef ANX_BROWSER_LAYOUT_H
#define ANX_BROWSER_LAYOUT_H

#include <anx/types.h>
#include "../html/dom.h"

/* ── Computed styles ─────────────────────────────────────────────── */

#define DISPLAY_BLOCK  0
#define DISPLAY_INLINE 1
#define DISPLAY_NONE   2

struct css_style {
	uint8_t  display;    /* DISPLAY_* */
	uint32_t color_fg;   /* XRGB8888 */
	uint32_t color_bg;   /* XRGB8888, 0 = transparent */
	uint8_t  font_size;  /* 1=small(8), 2=normal(16), 3=large(24) */
	bool     bold;
	bool     italic;
	uint16_t margin_top, margin_bottom, margin_left, margin_right;
	uint16_t padding_top, padding_bottom, padding_left, padding_right;
};

/* ── Paint commands ──────────────────────────────────────────────── */

#define PCMD_FILL_RECT 1
#define PCMD_TEXT_RUN  2
#define PCMD_IMAGE     3

#define PAINT_MAX_TEXT 256

struct paint_cmd {
	uint8_t  type;
	int32_t  x, y;
	uint32_t w, h;
	uint32_t color;           /* for FILL_RECT and TEXT_RUN */
	char     text[PAINT_MAX_TEXT]; /* TEXT_RUN */
	bool     bold;
	uint8_t  font_size;       /* glyph height in pixels */
};

/* ── Layout context ──────────────────────────────────────────────── */

#define MAX_PAINT_CMDS 2048

struct layout_ctx {
	struct paint_cmd cmds[MAX_PAINT_CMDS];
	uint32_t         n_cmds;
	int32_t          cursor_x, cursor_y;  /* current pen position */
	uint32_t         viewport_w;
	uint32_t         line_height;
	uint32_t         indent;              /* current left indent */
	uint32_t         bg_color;            /* page background */
};

void layout_init(struct layout_ctx *ctx, uint32_t viewport_w);

/* Walk the DOM tree and generate paint commands. */
void layout_document(struct layout_ctx *ctx, const struct dom_doc *doc);

#endif /* ANX_BROWSER_LAYOUT_H */
