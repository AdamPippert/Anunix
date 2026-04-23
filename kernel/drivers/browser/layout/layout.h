/*
 * layout.h — Block/inline layout engine.
 *
 * Converts a DOM tree into a flat list of paint commands (boxes + text runs).
 * Block elements stack vertically; inline content word-wraps.
 *
 * Style resolution is delegated to the CSS engine (css/css_cascade.h).
 */

#ifndef ANX_BROWSER_LAYOUT_H
#define ANX_BROWSER_LAYOUT_H

#include <anx/types.h>
#include "../html/dom.h"
#include "../css/css_cascade.h"
#include "../css/css_selector.h"

/* ── Paint commands ──────────────────────────────────────────────── */

#define PCMD_FILL_RECT 1
#define PCMD_TEXT_RUN  2
#define PCMD_IMAGE     3
#define PCMD_BORDER    4

#define PAINT_MAX_TEXT 256

struct paint_cmd {
	uint8_t  type;
	int32_t  x, y;
	uint32_t w, h;
	uint32_t color;               /* FILL_RECT, TEXT_RUN, BORDER */
	char     text[PAINT_MAX_TEXT]; /* TEXT_RUN */
	bool     bold;
	bool     italic;
	uint8_t  font_size_px;        /* glyph height in pixels */
	uint8_t  border_width;        /* BORDER */
	uint8_t  border_style;        /* BORDER: CSS_BORDER_* */
};

/* ── Layout context ──────────────────────────────────────────────── */

#define MAX_PAINT_CMDS 4096

struct layout_ctx {
	struct paint_cmd cmds[MAX_PAINT_CMDS];
	uint32_t         n_cmds;
	int32_t          cursor_x, cursor_y;  /* current pen position */
	uint32_t         viewport_w;
	uint32_t         line_height;
	uint32_t         indent;              /* current left indent (px) */
	uint32_t         bg_color;            /* page background color */
};

void layout_init(struct layout_ctx *ctx, uint32_t viewport_w);

/*
 * Walk the DOM tree and generate paint commands.
 * author_idx: CSS selector index built from <style> blocks; may be NULL.
 */
void layout_document(struct layout_ctx *ctx,
		      const struct dom_doc *doc,
		      const struct css_selector_index *author_idx);

#endif /* ANX_BROWSER_LAYOUT_H */
