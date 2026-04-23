/*
 * layout.c — Block/inline layout engine.
 *
 * Traverses the DOM tree depth-first and emits paint commands.
 * Block elements advance the y-cursor; inline content word-wraps
 * at the viewport edge using the 8×16 VGA font.
 *
 * Default styling follows browser UA stylesheet conventions:
 *   h1–h6 → bold, decreasing sizes
 *   p, div → block with margin
 *   a       → blue inline
 *   body    → white background, dark text
 */

#include "layout.h"
#include <anx/string.h>
#include <anx/alloc.h>

/* VGA font character dimensions */
#define FONT_W 8
#define FONT_H 16

/* Default colors */
#define COLOR_TEXT   0x001A2733U  /* ax-ink-900 */
#define COLOR_LINK   0x003A94A6U  /* ax-teal-500 */
#define COLOR_H1     0x000E2338U  /* ax-navy-900 */
#define COLOR_BG     0x00EFECe6U  /* ax-paper-100 */
#define COLOR_CODE   0x002F7A8CU
#define COLOR_MUTED  0x006A7683U

/* ── Default style computation ─────────────────────────────────────*/

static struct css_style default_style(const char *tag)
{
	struct css_style s;
	anx_memset(&s, 0, sizeof(s));
	s.display    = DISPLAY_BLOCK;
	s.color_fg   = COLOR_TEXT;
	s.color_bg   = 0;
	s.font_size  = 2;
	s.bold       = false;

	if (anx_strcmp(tag, "h1") == 0) {
		s.font_size = 3; s.bold = true;
		s.color_fg = COLOR_H1;
		s.margin_top = 24; s.margin_bottom = 16;
	} else if (anx_strcmp(tag, "h2") == 0) {
		s.font_size = 3; s.bold = true;
		s.margin_top = 20; s.margin_bottom = 12;
	} else if (anx_strcmp(tag, "h3") == 0 || anx_strcmp(tag, "h4") == 0) {
		s.bold = true;
		s.margin_top = 14; s.margin_bottom = 8;
	} else if (anx_strcmp(tag, "p") == 0) {
		s.margin_top = 10; s.margin_bottom = 10;
	} else if (anx_strcmp(tag, "ul") == 0 || anx_strcmp(tag, "ol") == 0) {
		s.margin_top = 4; s.margin_bottom = 4;
		s.padding_left = 24;
	} else if (anx_strcmp(tag, "li") == 0) {
		s.margin_top = 2; s.margin_bottom = 2;
		s.padding_left = 8;
	} else if (anx_strcmp(tag, "a") == 0) {
		s.display = DISPLAY_INLINE;
		s.color_fg = COLOR_LINK;
	} else if (anx_strcmp(tag, "span") == 0 ||
		   anx_strcmp(tag, "em")   == 0 ||
		   anx_strcmp(tag, "i")    == 0) {
		s.display = DISPLAY_INLINE;
	} else if (anx_strcmp(tag, "strong") == 0 ||
		   anx_strcmp(tag, "b")      == 0) {
		s.display = DISPLAY_INLINE;
		s.bold = true;
	} else if (anx_strcmp(tag, "code") == 0 ||
		   anx_strcmp(tag, "pre")  == 0 ||
		   anx_strcmp(tag, "tt")   == 0) {
		s.display = DISPLAY_INLINE;
		s.color_fg = COLOR_CODE;
		s.font_size = 1;
	} else if (anx_strcmp(tag, "br") == 0) {
		s.display = DISPLAY_BLOCK;
	} else if (anx_strcmp(tag, "hr") == 0) {
		s.display = DISPLAY_BLOCK;
		s.color_bg = 0x00CFCAC0U;  /* ax-paper-300 */
		s.margin_top = 8; s.margin_bottom = 8;
	} else if (anx_strcmp(tag, "head")   == 0 ||
		   anx_strcmp(tag, "meta")   == 0 ||
		   anx_strcmp(tag, "link")   == 0 ||
		   anx_strcmp(tag, "script") == 0 ||
		   anx_strcmp(tag, "style")  == 0 ||
		   anx_strcmp(tag, "title")  == 0 ||
		   anx_strcmp(tag, "noscript") == 0) {
		s.display = DISPLAY_NONE;
	}
	return s;
}

/* ── Paint command helpers ──────────────────────────────────────── */

static struct paint_cmd *emit_cmd(struct layout_ctx *ctx)
{
	if (ctx->n_cmds >= MAX_PAINT_CMDS) return NULL;
	struct paint_cmd *c = &ctx->cmds[ctx->n_cmds++];
	anx_memset(c, 0, sizeof(*c));
	return c;
}

static void emit_fill(struct layout_ctx *ctx, int32_t x, int32_t y,
		       uint32_t w, uint32_t h, uint32_t color)
{
	struct paint_cmd *c = emit_cmd(ctx);
	if (!c) return;
	c->type = PCMD_FILL_RECT;
	c->x = x; c->y = y; c->w = w; c->h = h; c->color = color;
}

static void emit_newline(struct layout_ctx *ctx)
{
	ctx->cursor_x  = (int32_t)ctx->indent;
	ctx->cursor_y += (int32_t)ctx->line_height;
	ctx->line_height = FONT_H;
}

/* Break text into word-wrapped runs and emit TEXT_RUN commands. */
static void emit_text(struct layout_ctx *ctx, const char *text,
		       uint32_t color, bool bold, uint8_t font_size)
{
	uint32_t glyph_w = (font_size == 1) ? FONT_W/2 : FONT_W;
	uint32_t glyph_h = (uint32_t)font_size * FONT_H;
	uint32_t max_x   = ctx->viewport_w - 8;
	const char *p = text;

	if (glyph_h > ctx->line_height)
		ctx->line_height = glyph_h;

	while (*p) {
		/* Find word boundary */
		const char *word_start = p;
		while (*p && *p != ' ' && *p != '\n') p++;
		uint32_t wlen = (uint32_t)(p - word_start);

		if (wlen == 0) {
			/* Space or newline */
			if (*p == '\n') emit_newline(ctx);
			else ctx->cursor_x += (int32_t)glyph_w;
			if (*p) p++;
			continue;
		}

		uint32_t pixel_w = wlen * glyph_w;

		/* Wrap if word doesn't fit */
		if ((uint32_t)ctx->cursor_x + pixel_w > max_x)
			emit_newline(ctx);

		struct paint_cmd *c = emit_cmd(ctx);
		if (!c) return;
		c->type      = PCMD_TEXT_RUN;
		c->x         = ctx->cursor_x;
		c->y         = ctx->cursor_y;
		c->color     = color;
		c->bold      = bold;
		c->font_size = font_size;

		uint32_t copy = (wlen < PAINT_MAX_TEXT - 1) ? wlen : PAINT_MAX_TEXT - 1;
		anx_memcpy(c->text, word_start, copy);
		c->text[copy] = '\0';

		ctx->cursor_x += (int32_t)pixel_w;
	}
}

/* ── Recursive tree walker ──────────────────────────────────────── */

struct walk_state {
	uint32_t fg_color;
	bool     bold;
	uint8_t  font_size;
};

static void walk_node(struct layout_ctx *ctx, const struct dom_node *n,
		       struct walk_state ws)
{
	if (!n) return;

	if (n->type == DOM_TEXT) {
		if (n->txt.text[0] != '\0')
			emit_text(ctx, n->txt.text, ws.fg_color,
				   ws.bold, ws.font_size);
		return;
	}

	/* DOM_ELEMENT */
	const char    *tag = n->el.tag;
	struct css_style st = default_style(tag);

	if (st.display == DISPLAY_NONE) return;

	bool is_block = (st.display == DISPLAY_BLOCK);

	if (is_block) {
		/* Newline + top margin */
		if (ctx->cursor_x != (int32_t)ctx->indent)
			emit_newline(ctx);
		ctx->cursor_y += (int32_t)st.margin_top;
		ctx->indent   += st.padding_left;
		ctx->cursor_x  = (int32_t)ctx->indent;

		/* <hr> special case */
		if (anx_strcmp(tag, "hr") == 0) {
			emit_fill(ctx, 0, ctx->cursor_y,
				   ctx->viewport_w, 2, 0x00CFCAC0U);
			ctx->cursor_y += 2 + st.margin_bottom;
			ctx->indent   -= st.padding_left;
			return;
		}
	}

	/* Merge styles down */
	struct walk_state child_ws = ws;
	if (st.color_fg) child_ws.fg_color = st.color_fg;
	if (st.bold)     child_ws.bold      = true;
	if (st.font_size > 1 || is_block)
		child_ws.font_size = st.font_size;

	/* Walk children */
	uint32_t i;
	for (i = 0; i < n->el.n_children; i++)
		walk_node(ctx, n->el.children[i], child_ws);

	if (is_block) {
		if (ctx->cursor_x != (int32_t)ctx->indent)
			emit_newline(ctx);
		ctx->cursor_y += (int32_t)st.margin_bottom;
		ctx->indent   -= st.padding_left;
		ctx->cursor_x  = (int32_t)ctx->indent;
	}
}

/* ── Public API ──────────────────────────────────────────────────── */

void layout_init(struct layout_ctx *ctx, uint32_t viewport_w)
{
	anx_memset(ctx, 0, sizeof(*ctx));
	ctx->viewport_w  = viewport_w;
	ctx->cursor_x    = 8;
	ctx->cursor_y    = 8;
	ctx->line_height = FONT_H;
	ctx->indent      = 8;
	ctx->bg_color    = COLOR_BG;
}

void layout_document(struct layout_ctx *ctx, const struct dom_doc *doc)
{
	/* Background fill */
	emit_fill(ctx, 0, 0, ctx->viewport_w, 16384, ctx->bg_color);

	if (!doc || !doc->root) {
		/* No DOM — render title only if available */
		return;
	}

	struct walk_state ws = {
		.fg_color  = COLOR_TEXT,
		.bold      = false,
		.font_size = 2,
	};

	/* Find <body> and walk it */
	uint32_t i;
	for (i = 0; i < doc->root->el.n_children; i++) {
		struct dom_node *child = doc->root->el.children[i];
		if (child && child->type == DOM_ELEMENT &&
		    anx_strcmp(child->el.tag, "body") == 0) {
			walk_node(ctx, child, ws);
			return;
		}
	}
	/* Fallback: walk root directly */
	walk_node(ctx, doc->root, ws);
}
