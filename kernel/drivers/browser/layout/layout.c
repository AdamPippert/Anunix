/*
 * layout.c — Block/inline layout engine.
 *
 * Traverses the DOM tree depth-first and emits paint commands.  Style for
 * each element is resolved via the CSS cascade (css/css_cascade.h), so
 * user-authored <style> blocks are honoured automatically.
 *
 * Block elements advance the y-cursor; inline content word-wraps at the
 * viewport edge.  Font rendering uses the 8×16 VGA bitmap font.
 */

#include "layout.h"
#include "../css/css_cascade.h"
#include <anx/string.h>
#include <anx/alloc.h>

/* VGA font character dimensions */
#define FONT_W 8
#define FONT_H 16

/* ── Paint command helpers ──────────────────────────────────────── */

static struct paint_cmd *emit_cmd(struct layout_ctx *ctx)
{
	if (ctx->n_cmds >= MAX_PAINT_CMDS) return NULL;
	struct paint_cmd *c = &ctx->cmds[ctx->n_cmds++];
	anx_memset(c, 0, sizeof(*c));
	return c;
}

static void emit_fill(struct layout_ctx *ctx,
		       int32_t x, int32_t y,
		       uint32_t w, uint32_t h, uint32_t color)
{
	struct paint_cmd *c = emit_cmd(ctx);
	if (!c) return;
	c->type = PCMD_FILL_RECT;
	c->x = x; c->y = y; c->w = w; c->h = h; c->color = color;
}

static void emit_border(struct layout_ctx *ctx,
			  int32_t x, int32_t y,
			  uint32_t w, uint32_t h,
			  uint32_t color, uint8_t bw, uint8_t bstyle)
{
	struct paint_cmd *c = emit_cmd(ctx);
	if (!c) return;
	c->type         = PCMD_BORDER;
	c->x            = x; c->y = y; c->w = w; c->h = h;
	c->color        = color;
	c->border_width = bw;
	c->border_style = bstyle;
}

static void emit_newline(struct layout_ctx *ctx)
{
	ctx->cursor_x   = (int32_t)ctx->indent;
	ctx->cursor_y  += (int32_t)ctx->line_height;
	ctx->line_height = FONT_H;
}

/*
 * Break text into word-wrapped runs and emit TEXT_RUN commands.
 * font_size_px: pixel height (use FONT_H multiples for VGA font).
 */
static void emit_text(struct layout_ctx *ctx,
		       const char *text,
		       uint32_t color,
		       bool bold, bool italic,
		       uint8_t font_size_px)
{
	uint32_t glyph_w = (font_size_px <= 10) ? (uint32_t)(FONT_W / 2)
	                                         : (uint32_t)FONT_W;
	uint32_t glyph_h = font_size_px;
	uint32_t max_x   = ctx->viewport_w > 16 ? ctx->viewport_w - 8 : 8;
	const char *p    = text;

	if (glyph_h > ctx->line_height)
		ctx->line_height = glyph_h;

	while (*p) {
		const char *word_start = p;
		while (*p && *p != ' ' && *p != '\n') p++;
		uint32_t wlen = (uint32_t)(p - word_start);

		if (wlen == 0) {
			if (*p == '\n') emit_newline(ctx);
			else ctx->cursor_x += (int32_t)glyph_w;
			if (*p) p++;
			continue;
		}

		uint32_t pixel_w = wlen * glyph_w;

		if ((uint32_t)ctx->cursor_x + pixel_w > max_x)
			emit_newline(ctx);

		struct paint_cmd *c = emit_cmd(ctx);
		if (!c) return;
		c->type        = PCMD_TEXT_RUN;
		c->x           = ctx->cursor_x;
		c->y           = ctx->cursor_y;
		c->color       = color;
		c->bold        = bold;
		c->italic      = italic;
		c->font_size_px = font_size_px;

		uint32_t copy = (wlen < PAINT_MAX_TEXT - 1) ? wlen : PAINT_MAX_TEXT - 1;
		anx_memcpy(c->text, word_start, copy);
		c->text[copy] = '\0';

		ctx->cursor_x += (int32_t)pixel_w;
	}
}

/* ── Recursive tree walker ──────────────────────────────────────── */

#define ANCESTOR_MAX 64

struct walk_state {
	/* CSS context */
	const struct css_selector_index *author_idx;
	struct computed_style            parent_style;
	/* Ancestor chain for selector matching */
	const struct dom_node           *ancestors[ANCESTOR_MAX];
	uint32_t                         n_ancestors;
	struct css_bloom                 bloom;
};

static void walk_node(struct layout_ctx *ctx,
		       const struct dom_node *n,
		       struct walk_state *ws)
{
	if (!n) return;

	/* ── Text nodes ──────────────────────────────────────────────── */
	if (n->type == DOM_TEXT) {
		if (n->txt.text[0] != '\0') {
			const struct computed_style *p = &ws->parent_style;
			emit_text(ctx, n->txt.text,
				   p->color_fg,
				   p->font_weight == CSS_FONT_WEIGHT_BOLD,
				   p->font_style == 1,
				   p->font_size_px);
		}
		return;
	}

	/* ── Element nodes ───────────────────────────────────────────── */
	struct computed_style st;
	css_resolve(&st, n,
		     ws->author_idx,
		     &ws->parent_style,
		     ws->ancestors,
		     ws->n_ancestors,
		     &ws->bloom);

	if (st.display == CSS_DISPLAY_NONE) return;

	bool is_block = (st.display == CSS_DISPLAY_BLOCK ||
	                 st.display == CSS_DISPLAY_FLEX);

	if (is_block) {
		if (ctx->cursor_x != (int32_t)ctx->indent)
			emit_newline(ctx);
		ctx->cursor_y += (int32_t)st.margin_top;

		/* <hr>: horizontal rule */
		if (anx_strcmp(n->el.tag, "hr") == 0) {
			emit_fill(ctx, (int32_t)ctx->indent, ctx->cursor_y,
				   ctx->viewport_w - ctx->indent * 2, 2,
				   st.color_bg ? st.color_bg : 0x00cfcac0u);
			ctx->cursor_y += 2 + st.margin_bottom;
			return;
		}

		/* Background fill */
		if (st.color_bg) {
			/* We use a generous height estimate; paint clips at viewport */
			emit_fill(ctx, (int32_t)ctx->indent, ctx->cursor_y,
				   ctx->viewport_w - ctx->indent,
				   (uint32_t)(st.font_size_px + st.padding_top +
				               st.padding_bottom + 4),
				   st.color_bg);
		}

		ctx->indent  += st.padding_left;
		ctx->cursor_x = (int32_t)ctx->indent;
	}

	/* Push this element onto the ancestor stack for children */
	struct walk_state child_ws = *ws;
	if (child_ws.n_ancestors < ANCESTOR_MAX) {
		/* Shift ancestors: newest parent goes to index 0 */
		uint32_t i;
		for (i = child_ws.n_ancestors; i > 0; i--)
			child_ws.ancestors[i] = child_ws.ancestors[i-1];
		child_ws.ancestors[0]  = n;
		child_ws.n_ancestors++;
	}
	/* Add this element's tag and classes to the Bloom filter */
	css_bloom_add(&child_ws.bloom, n->el.tag);
	{
		const char *cls = dom_attr(n, "class");
		if (cls) {
			const char *p = cls;
			while (*p) {
				while (*p == ' ') p++;
				const char *tok = p;
				while (*p && *p != ' ') p++;
				size_t tl = (size_t)(p - tok);
				if (tl > 0) {
					char tmp[64];
					if (tl >= sizeof(tmp)) tl = sizeof(tmp)-1;
					anx_memcpy(tmp, tok, tl);
					tmp[tl] = '\0';
					css_bloom_add(&child_ws.bloom, tmp);
				}
			}
		}
	}
	child_ws.parent_style = st;

	/* Walk children */
	uint32_t i;
	for (i = 0; i < n->el.n_children; i++)
		walk_node(ctx, n->el.children[i], &child_ws);

	if (is_block) {
		if (ctx->cursor_x != (int32_t)ctx->indent)
			emit_newline(ctx);
		ctx->cursor_y += (int32_t)st.margin_bottom;
		ctx->indent   -= st.padding_left;
		ctx->cursor_x  = (int32_t)ctx->indent;

		/* Border */
		if (st.border_width > 0 && st.border_style != CSS_BORDER_NONE) {
			/* Simple top/bottom border lines only */
			emit_border(ctx,
				     (int32_t)ctx->indent,
				     ctx->cursor_y,
				     ctx->viewport_w - ctx->indent,
				     1,
				     st.border_color ? st.border_color
				                     : 0x00cfcac0u,
				     st.border_width,
				     st.border_style);
		}
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
	ctx->bg_color    = 0x00efece6u; /* ax-paper-100 */
}

void layout_document(struct layout_ctx *ctx,
		      const struct dom_doc *doc,
		      const struct css_selector_index *author_idx)
{
	/* Ensure UA stylesheet is loaded */
	css_ua_init();

	/* Background fill */
	emit_fill(ctx, 0, 0, ctx->viewport_w, 16384, ctx->bg_color);

	if (!doc || !doc->root) return;

	struct walk_state ws;
	anx_memset(&ws, 0, sizeof(ws));
	ws.author_idx = author_idx;
	/* Initial parent style: body-like defaults */
	ws.parent_style.display      = CSS_DISPLAY_BLOCK;
	ws.parent_style.font_size_px = 14;
	ws.parent_style.color_fg     = 0x001a2733u;
	ws.parent_style.color_bg     = 0;
	ws.parent_style.width        = CSS_DIM_AUTO;
	ws.parent_style.height       = CSS_DIM_AUTO;
	css_bloom_clear(&ws.bloom);

	/* Find <body> and walk it */
	uint32_t i;
	for (i = 0; i < doc->root->el.n_children; i++) {
		struct dom_node *child = doc->root->el.children[i];
		if (child && child->type == DOM_ELEMENT &&
		    anx_strcmp(child->el.tag, "body") == 0) {
			walk_node(ctx, child, &ws);
			return;
		}
	}
	walk_node(ctx, doc->root, &ws);
}
