/*
 * layout.c — Block/inline layout engine.
 *
 * Traverses the DOM tree depth-first and emits paint commands.  Style for
 * each element is resolved via the CSS cascade (css/css_cascade.h), so
 * user-authored <style> blocks are honoured automatically.
 *
 * Inline text uses a run-accumulation buffer: consecutive same-style words
 * on the same line are merged into a single PCMD_TEXT_RUN.  This cuts paint
 * command count from O(words) to O(lines × style-changes), reducing renderer
 * work ~10× for typical paragraphs.
 *
 * Font metrics: VGA 8×16 bitmap font.  Small (≤10 px) glyphs use 4-px advance.
 */

#include "layout.h"
#include "../css/css_cascade.h"
#include "../forms/forms.h"
#include <anx/string.h>
#include <anx/alloc.h>

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

/* ── Inline run buffer ───────────────────────────────────────────── */

static void flush_inline(struct layout_ctx *ctx)
{
	if (!ctx->inline_buf_active || ctx->inline_buf.len == 0) return;
	struct paint_cmd *c = emit_cmd(ctx);
	if (c) {
		c->type         = PCMD_TEXT_RUN;
		c->x            = ctx->inline_buf.x;
		c->y            = ctx->inline_buf.y;
		c->color        = ctx->inline_buf.color;
		c->bold         = ctx->inline_buf.bold;
		c->italic       = ctx->inline_buf.italic;
		c->font_size_px = ctx->inline_buf.font_size_px;
		anx_memcpy(c->text, ctx->inline_buf.text,
			    ctx->inline_buf.len + 1);
	}
	ctx->inline_buf_active = false;
	ctx->inline_buf.len    = 0;
}

void layout_flush_inline(struct layout_ctx *ctx)
{
	flush_inline(ctx);
}

static void emit_newline(struct layout_ctx *ctx)
{
	flush_inline(ctx);
	ctx->cursor_x   = (int32_t)ctx->indent;
	ctx->cursor_y  += (int32_t)ctx->line_height;
	ctx->line_height = FONT_H;
}

/*
 * Emit inline text with run-accumulation.
 *
 * Words sharing style on the same line are appended to inline_buf and
 * emitted as a single PCMD_TEXT_RUN.  flush_inline() is called whenever
 * a line wraps or style changes.
 */
static void emit_text(struct layout_ctx *ctx,
		       const char *text,
		       uint32_t color,
		       bool bold, bool italic,
		       uint8_t font_size_px)
{
	const uint32_t glyph_w = (font_size_px <= 10) ? 4u : (uint32_t)FONT_W;
	const uint32_t glyph_h = font_size_px;
	const uint32_t max_x   = ctx->viewport_w > 16
				   ? ctx->viewport_w - 8u : 8u;
	const char    *p       = text;
	bool           need_sp = false;

	if (glyph_h > ctx->line_height)
		ctx->line_height = glyph_h;

	while (*p) {
		/* Whitespace: advance cursor, flag that next word needs a space */
		if (*p == ' ' || *p == '\t') {
			ctx->cursor_x += (int32_t)glyph_w;
			need_sp = true;
			p++;
			continue;
		}
		if (*p == '\n') {
			flush_inline(ctx);
			need_sp = false;
			emit_newline(ctx);
			p++;
			continue;
		}

		/* Scan to word end */
		const char *w = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
		uint32_t wlen = (uint32_t)(p - w);
		uint32_t wpx  = wlen * glyph_w;

		/* Word-wrap: only wrap if word is not at indent (avoids loop) */
		if ((uint32_t)ctx->cursor_x + wpx > max_x &&
		    ctx->cursor_x > (int32_t)ctx->indent) {
			flush_inline(ctx);
			need_sp = false;
			emit_newline(ctx);
		}

		/* Extend current run if same style on same line; else flush+new */
		bool extend = ctx->inline_buf_active
			   && ctx->inline_buf.color        == color
			   && ctx->inline_buf.bold         == bold
			   && ctx->inline_buf.italic       == italic
			   && ctx->inline_buf.font_size_px == font_size_px
			   && ctx->inline_buf.y            == ctx->cursor_y;

		if (extend) {
			uint32_t rem = (PAINT_MAX_TEXT - 1) - ctx->inline_buf.len;
			if (need_sp && rem > 0) {
				/* cursor_x already advanced for this space */
				ctx->inline_buf.text[ctx->inline_buf.len++] = ' ';
				rem--;
			}
			uint32_t copy = (wlen < rem) ? wlen : rem;
			anx_memcpy(ctx->inline_buf.text + ctx->inline_buf.len,
				    w, copy);
			ctx->inline_buf.len += copy;
			ctx->inline_buf.text[ctx->inline_buf.len] = '\0';
		} else {
			flush_inline(ctx);
			uint32_t copy = (wlen < PAINT_MAX_TEXT - 1)
					 ? wlen : PAINT_MAX_TEXT - 1;
			anx_memcpy(ctx->inline_buf.text, w, copy);
			ctx->inline_buf.len          = copy;
			ctx->inline_buf.text[copy]   = '\0';
			ctx->inline_buf.x            = ctx->cursor_x;
			ctx->inline_buf.y            = ctx->cursor_y;
			ctx->inline_buf.color        = color;
			ctx->inline_buf.bold         = bold;
			ctx->inline_buf.italic       = italic;
			ctx->inline_buf.font_size_px = font_size_px;
			ctx->inline_buf_active       = true;
		}

		ctx->cursor_x += (int32_t)wpx;
		need_sp = false;
	}
}

/* ── Recursive tree walker ──────────────────────────────────────── */

#define ANCESTOR_MAX 64

struct walk_state {
	const struct css_selector_index *author_idx;
	struct computed_style            parent_style;
	const struct dom_node           *ancestors[ANCESTOR_MAX];
	uint32_t                         n_ancestors;
	struct form_state               *fs;
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

	const char *tag = n->el.tag;

	/* Form elements: render specially */
	if ((anx_strcmp(tag, "input")    == 0 ||
	     anx_strcmp(tag, "textarea") == 0 ||
	     anx_strcmp(tag, "button")   == 0 ||
	     anx_strcmp(tag, "select")   == 0) && ws->fs) {
		if (ctx->cursor_x != (int32_t)ctx->indent)
			emit_newline(ctx);

		const char *ftype   = dom_attr(n, "type") ? dom_attr(n, "type") : "text";
		const char *fname   = dom_attr(n, "name")        ? dom_attr(n, "name")        : "";
		const char *fvalue  = dom_attr(n, "value")       ? dom_attr(n, "value")       : "";
		const char *fpholder= dom_attr(n, "placeholder") ? dom_attr(n, "placeholder") : "";

		uint8_t field_type = FORM_TYPE_TEXT;
		uint8_t pcmd_type  = PCMD_INPUT_FIELD;

		if (anx_strcmp(tag, "textarea") == 0) {
			field_type = FORM_TYPE_TEXTAREA;
			pcmd_type  = PCMD_TEXTAREA;
		} else if (anx_strcmp(tag, "select") == 0) {
			field_type = FORM_TYPE_SELECT;
			pcmd_type  = PCMD_INPUT_FIELD;
		} else if (anx_strcmp(tag, "button") == 0) {
			field_type = FORM_TYPE_SUBMIT;
			pcmd_type  = PCMD_BUTTON;
		} else if (anx_strcmp(ftype, "submit") == 0 ||
			    anx_strcmp(ftype, "button") == 0) {
			field_type = FORM_TYPE_SUBMIT;
			pcmd_type  = PCMD_BUTTON;
		} else if (anx_strcmp(ftype, "reset") == 0) {
			field_type = FORM_TYPE_RESET;
			pcmd_type  = PCMD_BUTTON;
		} else if (anx_strcmp(ftype, "checkbox") == 0) {
			field_type = FORM_TYPE_CHECKBOX;
			pcmd_type  = PCMD_CHECKBOX;
		} else if (anx_strcmp(ftype, "radio") == 0) {
			field_type = FORM_TYPE_RADIO;
			pcmd_type  = PCMD_CHECKBOX;
		} else if (anx_strcmp(ftype, "password") == 0) {
			field_type = FORM_TYPE_PASSWORD;
		} else if (anx_strcmp(ftype, "hidden") == 0) {
			form_field_register(ws->fs, FORM_TYPE_HIDDEN,
					     fname, fvalue, "", "", false,
					     0, 0, 0, 0);
			return;
		}

		uint32_t fw = (field_type == FORM_TYPE_CHECKBOX ||
			        field_type == FORM_TYPE_RADIO) ? 16 :
			       (field_type == FORM_TYPE_TEXTAREA) ? 360 : 200;
		uint32_t fh = (field_type == FORM_TYPE_TEXTAREA) ? 80 : 24;

		char btn_label[256];
		btn_label[0] = '\0';
		if (pcmd_type == PCMD_BUTTON) {
			if (fvalue[0])
				anx_strlcpy(btn_label, fvalue, sizeof(btn_label));
			else if (anx_strcmp(tag, "button") == 0) {
				uint32_t ci;
				size_t bpos = 0;
				for (ci = 0; ci < n->el.n_children &&
				             bpos < sizeof(btn_label) - 1; ci++) {
					struct dom_node *ch = n->el.children[ci];
					if (ch && ch->type == DOM_TEXT) {
						size_t cl = anx_strlen(ch->txt.text);
						if (cl > sizeof(btn_label) - bpos - 1)
							cl = sizeof(btn_label) - bpos - 1;
						anx_memcpy(btn_label + bpos,
							   ch->txt.text, cl);
						bpos += cl;
					}
				}
				btn_label[bpos] = '\0';
			}
			if (!btn_label[0])
				anx_strlcpy(btn_label,
					     field_type == FORM_TYPE_RESET ?
					       "Reset" : "Submit",
					     sizeof(btn_label));
		}

		bool    is_checked = (dom_attr(n, "checked") != NULL);
		int32_t fidx = form_field_register(ws->fs, field_type,
						    fname, fvalue, fpholder,
						    btn_label, is_checked,
						    ctx->cursor_x,
						    ctx->cursor_y,
						    fw, fh);

		struct paint_cmd *c = emit_cmd(ctx);
		if (c) {
			c->type       = pcmd_type;
			c->x          = ctx->cursor_x;
			c->y          = ctx->cursor_y;
			c->w          = fw;
			c->h          = fh;
			c->field_idx  = fidx;
			c->focused    = false;
			c->checked    = is_checked;
			c->cursor_pos = 0;
			if (pcmd_type == PCMD_BUTTON)
				anx_strlcpy(c->text, btn_label, PAINT_MAX_TEXT);
			else if (fvalue[0])
				anx_strlcpy(c->text, fvalue, PAINT_MAX_TEXT);
			else
				anx_strlcpy(c->text, fpholder, PAINT_MAX_TEXT);
			c->color = 0x001a2733u;
		}

		ctx->cursor_y += (int32_t)fh + 4;
		ctx->cursor_x  = (int32_t)ctx->indent;
		return;
	}

	/* ── Standard element style resolution ───────────────────────── */
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

		if (anx_strcmp(n->el.tag, "hr") == 0) {
			emit_fill(ctx, (int32_t)ctx->indent, ctx->cursor_y,
				   ctx->viewport_w - ctx->indent * 2, 2,
				   st.color_bg ? st.color_bg : 0x00cfcac0u);
			ctx->cursor_y += 2 + st.margin_bottom;
			return;
		}

		if (st.color_bg) {
			emit_fill(ctx, (int32_t)ctx->indent, ctx->cursor_y,
				   ctx->viewport_w - ctx->indent,
				   (uint32_t)(st.font_size_px + st.padding_top +
				               st.padding_bottom + 4),
				   st.color_bg);
		}

		ctx->indent  += st.padding_left;
		ctx->cursor_x = (int32_t)ctx->indent;
	}

	/* Push ancestor for child selector matching */
	struct walk_state child_ws = *ws;
	if (child_ws.n_ancestors < ANCESTOR_MAX) {
		uint32_t i;
		for (i = child_ws.n_ancestors; i > 0; i--)
			child_ws.ancestors[i] = child_ws.ancestors[i-1];
		child_ws.ancestors[0] = n;
		child_ws.n_ancestors++;
	}
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

	uint32_t i;
	for (i = 0; i < n->el.n_children; i++)
		walk_node(ctx, n->el.children[i], &child_ws);

	if (is_block) {
		if (ctx->cursor_x != (int32_t)ctx->indent)
			emit_newline(ctx);
		ctx->cursor_y += (int32_t)st.margin_bottom;
		ctx->indent   -= st.padding_left;
		ctx->cursor_x  = (int32_t)ctx->indent;

		if (st.border_width > 0 && st.border_style != CSS_BORDER_NONE) {
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
	ctx->bg_color    = 0x00efece6u;
}

void layout_document(struct layout_ctx *ctx,
		      const struct dom_doc *doc,
		      const struct css_selector_index *author_idx,
		      struct form_state *fs)
{
	css_ua_init();

	if (fs) form_state_reset(fs);

	emit_fill(ctx, 0, 0, ctx->viewport_w, 16384, ctx->bg_color);

	if (!doc || !doc->root) return;

	struct walk_state ws;
	anx_memset(&ws, 0, sizeof(ws));
	ws.author_idx = author_idx;
	ws.fs         = fs;
	ws.parent_style.display      = CSS_DISPLAY_BLOCK;
	ws.parent_style.font_size_px = 14;
	ws.parent_style.color_fg     = 0x001a2733u;
	ws.parent_style.color_bg     = 0;
	ws.parent_style.width        = CSS_DIM_AUTO;
	ws.parent_style.height       = CSS_DIM_AUTO;
	css_bloom_clear(&ws.bloom);

	uint32_t i;
	for (i = 0; i < doc->root->el.n_children; i++) {
		struct dom_node *child = doc->root->el.children[i];
		if (child && child->type == DOM_ELEMENT &&
		    anx_strcmp(child->el.tag, "body") == 0) {
			walk_node(ctx, child, &ws);
			flush_inline(ctx);
			return;
		}
	}
	walk_node(ctx, doc->root, &ws);
	flush_inline(ctx);
}
