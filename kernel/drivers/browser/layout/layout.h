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
#include "../forms/forms.h"

/* ── Paint commands ──────────────────────────────────────────────── */

#define PCMD_FILL_RECT    1
#define PCMD_TEXT_RUN     2
#define PCMD_IMAGE        3
#define PCMD_BORDER       4
#define PCMD_INPUT_FIELD  5   /* text / password / email / search input */
#define PCMD_BUTTON       6   /* <button> or <input type="submit|reset"> */
#define PCMD_CHECKBOX     7   /* <input type="checkbox"> */
#define PCMD_TEXTAREA     8   /* <textarea> */

#define PAINT_MAX_TEXT 256

struct paint_cmd {
	uint8_t  type;
	int32_t  x, y;
	uint32_t w, h;
	uint32_t color;               /* FILL_RECT, TEXT_RUN, BORDER */
	char     text[PAINT_MAX_TEXT]; /* TEXT_RUN, INPUT value, BUTTON label */
	bool     bold;
	bool     italic;
	uint8_t  font_size_px;        /* glyph height in pixels */
	uint8_t  border_width;        /* BORDER */
	uint8_t  border_style;        /* BORDER: CSS_BORDER_* */
	/* Form field fields */
	int32_t  field_idx;           /* INPUT/BUTTON/CHECKBOX: index into form_state */
	bool     focused;             /* INPUT/BUTTON: field has keyboard focus */
	bool     checked;             /* CHECKBOX: current state */
	uint32_t cursor_pos;          /* INPUT/TEXTAREA: byte offset of cursor */
	/* PCMD_IMAGE — source pixels borrowed from session image cache */
	const uint32_t *img_pixels;   /* XRGB8888 source, row-major; not freed here */
	uint32_t        img_src_w;
	uint32_t        img_src_h;
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

	/*
	 * Inline run accumulation: consecutive same-style words on the same
	 * line are merged into one PCMD_TEXT_RUN rather than one per word.
	 * Reduces paint command count by ~8-12× for typical paragraphs.
	 */
	struct {
		char     text[PAINT_MAX_TEXT];
		uint32_t len;
		int32_t  x, y;
		uint32_t color;
		uint8_t  font_size_px;
		bool     bold, italic;
	} inline_buf;
	bool inline_buf_active;
};

/* Decoded image entry passed to layout_document() for <img> rendering. */
struct layout_image {
	const char     *url;     /* src attribute value to match */
	const uint32_t *pixels;  /* XRGB8888, row-major */
	uint32_t        w, h;
};

void layout_init(struct layout_ctx *ctx, uint32_t viewport_w);

/* Flush any pending inline run — call at document end or block transitions. */
void layout_flush_inline(struct layout_ctx *ctx);

/*
 * Walk the DOM tree and generate paint commands.
 * author_idx: CSS selector index built from <style> blocks; may be NULL.
 * fs:         Form state to populate as form elements are discovered; may be NULL.
 * imgs/n_imgs: decoded image table for <img> rendering; may be NULL/0.
 */
void layout_document(struct layout_ctx *ctx,
		      const struct dom_doc *doc,
		      const struct css_selector_index *author_idx,
		      struct form_state *fs,
		      const struct layout_image *imgs,
		      uint32_t n_imgs);

#endif /* ANX_BROWSER_LAYOUT_H */
