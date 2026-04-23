/*
 * css_cascade.c — CSS cascade pipeline and property value parser.
 *
 * Flow:
 *   1. Start with UA defaults for this element's tag.
 *   2. Inherit inheritable properties from parent computed style.
 *   3. Apply matched author rules in ascending (origin, specificity) order.
 *   4. Apply inline style="" attribute at highest author specificity.
 */

#include "css_cascade.h"
#include "css_parser.h"
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>

/* ── UA stylesheet (hardcoded as CSS text) ───────────────────────── */

static const char UA_CSS[] =
	"head,script,style,meta,link,noscript,title{display:none}"
	"html,body{display:block}"
	"body{color:#1a2733;background-color:#efece6}"
	"div,p,section,article,header,footer,main,nav,aside{"
	  "display:block}"
	"h1{display:block;font-size:24px;font-weight:bold;"
	    "color:#0e2338;margin-top:24px;margin-bottom:16px}"
	"h2{display:block;font-size:20px;font-weight:bold;"
	    "margin-top:20px;margin-bottom:12px}"
	"h3{display:block;font-size:18px;font-weight:bold;"
	    "margin-top:14px;margin-bottom:8px}"
	"h4,h5,h6{display:block;font-weight:bold;"
	          "margin-top:12px;margin-bottom:6px}"
	"p{margin-top:10px;margin-bottom:10px}"
	"ul,ol{display:block;margin-top:4px;margin-bottom:4px;"
	       "padding-left:24px}"
	"li{display:block;margin-top:2px;margin-bottom:2px;"
	    "padding-left:8px}"
	"table,tr,td,th{display:block}"
	"blockquote{display:block;margin-left:24px;"
	            "padding-left:12px;border-left:3px solid #cfcac0}"
	"a{display:inline;color:#3a94a6}"
	"span,em,i,cite{display:inline}"
	"strong,b{display:inline;font-weight:bold}"
	"code,tt,kbd,samp{display:inline;color:#2f7a8c;font-size:12px}"
	"pre{display:block;color:#2f7a8c;font-size:12px;"
	     "margin-top:8px;margin-bottom:8px}"
	"br{display:block}"
	"hr{display:block;background-color:#cfcac0;"
	    "margin-top:8px;margin-bottom:8px}"
	"small{display:inline;font-size:10px}"
	"sub,sup{display:inline;font-size:10px}"
	"abbr,acronym{display:inline}"
	;

static struct css_sheet       s_ua_sheet;
static struct css_selector_index s_ua_index;
static bool                   s_ua_ready;

void css_ua_init(void)
{
	if (s_ua_ready) return;
	css_sheet_init(&s_ua_sheet);
	css_parse(UA_CSS, anx_strlen(UA_CSS), &s_ua_sheet, CSS_ORIGIN_UA);
	css_index_clear(&s_ua_index);
	css_index_build(&s_ua_index, &s_ua_sheet);
	s_ua_ready = true;
}

/* ── Color name table ────────────────────────────────────────────── */

struct named_color { const char *name; uint32_t xrgb; };

static const struct named_color NAMED_COLORS[] = {
	{"black",   0x00000000u}, {"white",   0x00ffffffu},
	{"red",     0x00ff0000u}, {"green",   0x00008000u},
	{"lime",    0x0000ff00u}, {"blue",    0x000000ffu},
	{"navy",    0x00000080u}, {"teal",    0x00008080u},
	{"aqua",    0x0000ffffu}, {"cyan",    0x0000ffffu},
	{"fuchsia", 0x00ff00ffu}, {"magenta", 0x00ff00ffu},
	{"yellow",  0x00ffff00u}, {"orange",  0x00ffa500u},
	{"purple",  0x00800080u}, {"maroon",  0x00800000u},
	{"olive",   0x00808000u}, {"silver",  0x00c0c0c0u},
	{"gray",    0x00808080u}, {"grey",    0x00808080u},
	{"transparent", 0xff000000u},
	{NULL, 0}
};

/* ── Value parsers ───────────────────────────────────────────────── */

static void skip_ws_p(const char **p)
{
	while (**p == ' ' || **p == '\t') (*p)++;
}

static bool parse_hex2(const char **p, uint8_t *out)
{
	uint8_t v = 0;
	int     i;
	for (i = 0; i < 2; i++) {
		char c = **p;
		if      (c >= '0' && c <= '9') v = (uint8_t)(v * 16 + (c - '0'));
		else if (c >= 'a' && c <= 'f') v = (uint8_t)(v * 16 + (c - 'a' + 10));
		else if (c >= 'A' && c <= 'F') v = (uint8_t)(v * 16 + (c - 'A' + 10));
		else return false;
		(*p)++;
	}
	*out = v;
	return true;
}

/*
 * Parse a CSS color value into XRGB8888.
 * Returns false if the value is not a recognisable color.
 */
static bool parse_color(const char *val, uint32_t *out)
{
	const char *p = val;
	skip_ws_p(&p);

	if (*p == '#') {
		p++;
		uint8_t r, g, b;
		size_t  hex_len = anx_strlen(p);

		if (hex_len >= 6) {
			/* #rrggbb */
			if (!parse_hex2(&p, &r)) return false;
			if (!parse_hex2(&p, &g)) return false;
			if (!parse_hex2(&p, &b)) return false;
		} else if (hex_len >= 3) {
			/* #rgb → #rrggbb */
			char c;
			c = *p++; r = (uint8_t)(c >= 'a' ? (c - 'a' + 10) : (c >= 'A' ? (c-'A'+10) : (c-'0'))); r |= (uint8_t)(r << 4);
			c = *p++; g = (uint8_t)(c >= 'a' ? (c - 'a' + 10) : (c >= 'A' ? (c-'A'+10) : (c-'0'))); g |= (uint8_t)(g << 4);
			c = *p++; b = (uint8_t)(c >= 'a' ? (c - 'a' + 10) : (c >= 'A' ? (c-'A'+10) : (c-'0'))); b |= (uint8_t)(b << 4);
		} else {
			return false;
		}
		*out = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
		return true;
	}

	if (anx_strncmp(p, "rgb(", 4) == 0) {
		p += 4;
		uint32_t r = 0, g = 0, b = 0;
		while (*p >= '0' && *p <= '9') { r = r*10 + (*p - '0'); p++; }
		while (*p == ',' || *p == ' ') p++;
		while (*p >= '0' && *p <= '9') { g = g*10 + (*p - '0'); p++; }
		while (*p == ',' || *p == ' ') p++;
		while (*p >= '0' && *p <= '9') { b = b*10 + (*p - '0'); p++; }
		*out = ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
		return true;
	}

	/* Named color */
	const struct named_color *nc = NAMED_COLORS;
	while (nc->name) {
		if (anx_strcmp(p, nc->name) == 0) {
			*out = nc->xrgb;
			return true;
		}
		nc++;
	}
	return false;
}

/*
 * Parse a CSS length into pixels.
 * parent_font_px: current font size (for em units).
 * Returns 0xFFFF (CSS_DIM_AUTO) for "auto".
 */
static uint16_t parse_length(const char *val, uint8_t parent_font_px)
{
	const char *p = val;
	skip_ws_p(&p);

	if (anx_strcmp(p, "auto") == 0)
		return CSS_DIM_AUTO;
	if (anx_strcmp(p, "0") == 0)
		return 0;

	uint32_t n = 0;
	bool     frac = false;
	uint32_t frac_div = 1;
	uint32_t frac_val = 0;

	while (*p >= '0' && *p <= '9') { n = n*10 + (uint32_t)(*p - '0'); p++; }
	if (*p == '.') {
		frac = true;
		p++;
		while (*p >= '0' && *p <= '9') {
			frac_val = frac_val*10 + (uint32_t)(*p - '0');
			frac_div *= 10;
			p++;
		}
	}

	if (anx_strncmp(p, "px", 2) == 0)
		return (uint16_t)(n > 0xFFFEu ? 0xFFFEu : n);
	if (anx_strncmp(p, "em", 2) == 0) {
		uint32_t px = n * parent_font_px;
		if (frac) px += (frac_val * parent_font_px) / frac_div;
		return (uint16_t)(px > 0xFFFEu ? 0xFFFEu : px);
	}
	if (anx_strncmp(p, "rem", 3) == 0) {
		uint32_t px = n * 14;  /* default root font size */
		return (uint16_t)(px > 0xFFFEu ? 0xFFFEu : px);
	}
	/* Unitless or unknown: treat as pixels */
	return (uint16_t)(n > 0xFFFEu ? 0xFFFEu : n);
}

static uint8_t parse_font_size(const char *val, uint8_t parent_px)
{
	if (anx_strcmp(val, "xx-small") == 0) return 8;
	if (anx_strcmp(val, "x-small")  == 0) return 10;
	if (anx_strcmp(val, "small")    == 0) return 12;
	if (anx_strcmp(val, "medium")   == 0) return 14;
	if (anx_strcmp(val, "large")    == 0) return 18;
	if (anx_strcmp(val, "x-large")  == 0) return 24;
	if (anx_strcmp(val, "xx-large") == 0) return 32;
	if (anx_strcmp(val, "smaller")  == 0) return parent_px > 2 ? (uint8_t)(parent_px - 2) : 8;
	if (anx_strcmp(val, "larger")   == 0) return (uint8_t)(parent_px + 2 < 32 ? parent_px + 2 : 32);

	uint16_t px = parse_length(val, parent_px);
	if (px == CSS_DIM_AUTO || px == 0) return parent_px;
	return (uint8_t)(px > 32 ? 32 : px);
}

/* ── Single-declaration applier ─────────────────────────────────── */

/* Trim trailing whitespace into a stack buffer */
static void trim_copy(const char *src, size_t src_len, char *dst, size_t dst_cap)
{
	while (src_len > 0 &&
	       (src[src_len-1] == ' ' || src[src_len-1] == '\t' ||
	        src[src_len-1] == '\n' || src[src_len-1] == '\r'))
		src_len--;
	if (src_len >= dst_cap) src_len = dst_cap - 1;
	anx_memcpy(dst, src, src_len);
	dst[src_len] = '\0';
}

/*
 * Parse a CSS shorthand "1px solid red" for border.
 * Sets width, style, color on s.
 */
static void apply_border_shorthand(struct computed_style *s, const char *val)
{
	char  tok[64];
	const char *p = val;

	while (*p) {
		while (*p == ' ') p++;
		const char *start = p;
		while (*p && *p != ' ') p++;
		size_t tlen = (size_t)(p - start);
		if (tlen == 0) break;
		if (tlen >= sizeof(tok)) tlen = sizeof(tok) - 1;
		anx_memcpy(tok, start, tlen);
		tok[tlen] = '\0';

		if (anx_strcmp(tok, "none") == 0) {
			s->border_width = 0;
			s->border_style = CSS_BORDER_NONE;
		} else if (anx_strcmp(tok, "solid") == 0) {
			s->border_style = CSS_BORDER_SOLID;
			if (s->border_width == 0) s->border_width = 1;
		} else if (anx_strcmp(tok, "dashed") == 0) {
			s->border_style = CSS_BORDER_DASHED;
			if (s->border_width == 0) s->border_width = 1;
		} else {
			/* Try as length */
			uint16_t px = parse_length(tok, 14);
			if (px != CSS_DIM_AUTO && px > 0) {
				s->border_width = (uint8_t)(px > 16 ? 16 : px);
			} else {
				/* Try as color */
				uint32_t c;
				if (parse_color(tok, &c))
					s->border_color = c;
			}
		}
	}
}

/* Apply a single property:value pair to the computed style. */
static void apply_one(struct computed_style *s,
		        const char *prop, size_t prop_len,
		        const char *val,  size_t val_len)
{
	char  pname[64], pval[256];

	if (prop_len >= sizeof(pname)) return;
	if (val_len  >= sizeof(pval))  val_len = sizeof(pval) - 1;

	anx_memcpy(pname, prop, prop_len);
	pname[prop_len] = '\0';
	trim_copy(val, val_len, pval, sizeof(pval));

	/* Skip empty values */
	if (!pval[0]) return;

	/* --- display --- */
	if (anx_strcmp(pname, "display") == 0) {
		if      (anx_strcmp(pval, "block")        == 0) s->display = CSS_DISPLAY_BLOCK;
		else if (anx_strcmp(pval, "inline")        == 0) s->display = CSS_DISPLAY_INLINE;
		else if (anx_strcmp(pval, "inline-block")  == 0) s->display = CSS_DISPLAY_INLINE_BLOCK;
		else if (anx_strcmp(pval, "flex")          == 0) s->display = CSS_DISPLAY_FLEX;
		else if (anx_strcmp(pval, "none")          == 0) s->display = CSS_DISPLAY_NONE;
		return;
	}

	/* --- position --- */
	if (anx_strcmp(pname, "position") == 0) {
		if      (anx_strcmp(pval, "static")   == 0) s->position = CSS_POSITION_STATIC;
		else if (anx_strcmp(pval, "relative") == 0) s->position = CSS_POSITION_RELATIVE;
		else if (anx_strcmp(pval, "absolute") == 0) s->position = CSS_POSITION_ABSOLUTE;
		else if (anx_strcmp(pval, "fixed")    == 0) s->position = CSS_POSITION_FIXED;
		return;
	}

	/* --- float --- */
	if (anx_strcmp(pname, "float") == 0) {
		if      (anx_strcmp(pval, "left")  == 0) s->float_val = CSS_FLOAT_LEFT;
		else if (anx_strcmp(pval, "right") == 0) s->float_val = CSS_FLOAT_RIGHT;
		else                                       s->float_val = CSS_FLOAT_NONE;
		return;
	}

	/* --- color --- */
	if (anx_strcmp(pname, "color") == 0) {
		uint32_t c;
		if (parse_color(pval, &c)) s->color_fg = c;
		return;
	}

	/* --- background-color --- */
	if (anx_strcmp(pname, "background-color") == 0 ||
	    anx_strcmp(pname, "background") == 0) {
		uint32_t c;
		if (parse_color(pval, &c)) s->color_bg = c;
		return;
	}

	/* --- font-size --- */
	if (anx_strcmp(pname, "font-size") == 0) {
		s->font_size_px = parse_font_size(pval, s->font_size_px);
		return;
	}

	/* --- font-weight --- */
	if (anx_strcmp(pname, "font-weight") == 0) {
		if (anx_strcmp(pval, "bold") == 0 ||
		    anx_strcmp(pval, "bolder") == 0 ||
		    (pval[0] >= '5' && pval[0] <= '9' && pval[1] == '0' && pval[2] == '0'))
			s->font_weight = CSS_FONT_WEIGHT_BOLD;
		else
			s->font_weight = CSS_FONT_WEIGHT_NORMAL;
		return;
	}

	/* --- font-style --- */
	if (anx_strcmp(pname, "font-style") == 0) {
		s->font_style = (anx_strcmp(pval, "italic") == 0 ||
		                 anx_strcmp(pval, "oblique") == 0) ? 1 : 0;
		return;
	}

	/* --- text-align --- */
	if (anx_strcmp(pname, "text-align") == 0) {
		if      (anx_strcmp(pval, "center") == 0) s->text_align = CSS_TEXT_ALIGN_CENTER;
		else if (anx_strcmp(pval, "right")  == 0) s->text_align = CSS_TEXT_ALIGN_RIGHT;
		else                                        s->text_align = CSS_TEXT_ALIGN_LEFT;
		return;
	}

	/* --- text-decoration --- */
	if (anx_strcmp(pname, "text-decoration") == 0) {
		s->text_decoration =
			(anx_strstr(pval, "underline") != NULL) ?
			CSS_TEXT_DECO_UNDERLINE : CSS_TEXT_DECO_NONE;
		return;
	}

	/* --- margin shorthand & longhands --- */
	if (anx_strcmp(pname, "margin") == 0) {
		uint16_t v = parse_length(pval, s->font_size_px);
		if (v == CSS_DIM_AUTO) v = 0;
		s->margin_top = s->margin_right =
		s->margin_bottom = s->margin_left = v;
		return;
	}
	if (anx_strcmp(pname, "margin-top")    == 0) { uint16_t v = parse_length(pval, s->font_size_px); s->margin_top    = (v == CSS_DIM_AUTO) ? 0 : v; return; }
	if (anx_strcmp(pname, "margin-right")  == 0) { uint16_t v = parse_length(pval, s->font_size_px); s->margin_right  = (v == CSS_DIM_AUTO) ? 0 : v; return; }
	if (anx_strcmp(pname, "margin-bottom") == 0) { uint16_t v = parse_length(pval, s->font_size_px); s->margin_bottom = (v == CSS_DIM_AUTO) ? 0 : v; return; }
	if (anx_strcmp(pname, "margin-left")   == 0) { uint16_t v = parse_length(pval, s->font_size_px); s->margin_left   = (v == CSS_DIM_AUTO) ? 0 : v; return; }

	/* --- padding shorthand & longhands --- */
	if (anx_strcmp(pname, "padding") == 0) {
		uint16_t v = parse_length(pval, s->font_size_px);
		if (v == CSS_DIM_AUTO) v = 0;
		s->padding_top = s->padding_right =
		s->padding_bottom = s->padding_left = v;
		return;
	}
	if (anx_strcmp(pname, "padding-top")    == 0) { uint16_t v = parse_length(pval, s->font_size_px); s->padding_top    = (v == CSS_DIM_AUTO) ? 0 : v; return; }
	if (anx_strcmp(pname, "padding-right")  == 0) { uint16_t v = parse_length(pval, s->font_size_px); s->padding_right  = (v == CSS_DIM_AUTO) ? 0 : v; return; }
	if (anx_strcmp(pname, "padding-bottom") == 0) { uint16_t v = parse_length(pval, s->font_size_px); s->padding_bottom = (v == CSS_DIM_AUTO) ? 0 : v; return; }
	if (anx_strcmp(pname, "padding-left")   == 0) { uint16_t v = parse_length(pval, s->font_size_px); s->padding_left   = (v == CSS_DIM_AUTO) ? 0 : v; return; }

	/* --- width / height --- */
	if (anx_strcmp(pname, "width")      == 0) { s->width  = parse_length(pval, s->font_size_px); return; }
	if (anx_strcmp(pname, "height")     == 0) { s->height = parse_length(pval, s->font_size_px); return; }
	if (anx_strcmp(pname, "min-width")  == 0) { return; }  /* acknowledge but ignore */
	if (anx_strcmp(pname, "min-height") == 0) { return; }

	/* --- border shorthand & longhands --- */
	if (anx_strcmp(pname, "border") == 0 ||
	    anx_strcmp(pname, "border-left") == 0) {
		apply_border_shorthand(s, pval);
		return;
	}
	if (anx_strcmp(pname, "border-width") == 0) {
		uint16_t v = parse_length(pval, s->font_size_px);
		if (v != CSS_DIM_AUTO) s->border_width = (uint8_t)(v > 16 ? 16 : v);
		return;
	}
	if (anx_strcmp(pname, "border-style") == 0) {
		if      (anx_strcmp(pval, "solid")  == 0) s->border_style = CSS_BORDER_SOLID;
		else if (anx_strcmp(pval, "dashed") == 0) s->border_style = CSS_BORDER_DASHED;
		else                                        s->border_style = CSS_BORDER_NONE;
		return;
	}
	if (anx_strcmp(pname, "border-color") == 0) {
		uint32_t c;
		if (parse_color(pval, &c)) s->border_color = c;
		return;
	}
}

/*
 * Parse a CSS declarations string ("color:red; margin:10px") and apply
 * each property to the computed style.
 */
static void apply_declarations(struct computed_style *s, const char *decls)
{
	const char *p = decls;

	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ';')
			p++;
		if (!*p) break;

		/* Find property name */
		const char *prop_start = p;
		while (*p && *p != ':' && *p != ';') p++;
		if (!*p || *p == ';') continue;
		size_t prop_len = (size_t)(p - prop_start);
		/* Trim trailing whitespace from prop */
		while (prop_len > 0 &&
		       (prop_start[prop_len-1] == ' ' ||
		        prop_start[prop_len-1] == '\t'))
			prop_len--;

		p++; /* skip ':' */
		while (*p == ' ' || *p == '\t') p++;

		/* Find value (up to ';') */
		const char *val_start = p;
		while (*p && *p != ';') p++;
		size_t val_len = (size_t)(p - val_start);

		if (prop_len > 0 && val_len > 0)
			apply_one(s, prop_start, prop_len, val_start, val_len);
	}
}

/* ── Default initial values ─────────────────────────────────────── */

static void style_initial(struct computed_style *s)
{
	anx_memset(s, 0, sizeof(*s));
	s->display      = CSS_DISPLAY_INLINE;  /* HTML default for unknown tags */
	s->position     = CSS_POSITION_STATIC;
	s->font_size_px = 14;                  /* 14px default (medium) */
	s->color_fg     = 0x001a2733u;         /* ax-ink-900 */
	s->color_bg     = 0;                   /* transparent */
	s->width        = CSS_DIM_AUTO;
	s->height       = CSS_DIM_AUTO;
}

/* Inherit inheritable properties from parent. */
static void style_inherit(struct computed_style *s,
			    const struct computed_style *parent)
{
	if (!parent) return;
	s->color_fg        = parent->color_fg;
	s->font_size_px    = parent->font_size_px;
	s->font_weight     = parent->font_weight;
	s->font_style      = parent->font_style;
	s->text_decoration = parent->text_decoration;
	s->text_align      = parent->text_align;
}

/* ── Public cascade pipeline ─────────────────────────────────────── */

#define MAX_MATCHED 64

void css_resolve(struct computed_style             *out,
		  const struct dom_node            *el,
		  const struct css_selector_index  *author,
		  const struct computed_style      *parent,
		  const struct dom_node   *const   *ancestors,
		  uint32_t                          n_ancestors,
		  const struct css_bloom           *bloom)
{
	const struct css_rule *matched[MAX_MATCHED];
	uint32_t               n_matched = 0;
	uint32_t               i;

	style_initial(out);
	style_inherit(out, parent);

	/* Ensure UA index is ready */
	if (!s_ua_ready) css_ua_init();

	/* 1. UA rules */
	n_matched = css_match(&s_ua_index, el,
			       ancestors, n_ancestors,
			       bloom, matched, MAX_MATCHED);

	/* 2. Author rules — append (already sorted within each call) */
	if (author) {
		const struct css_rule *author_matched[MAX_MATCHED];
		uint32_t n_author = css_match(author, el,
					       ancestors, n_ancestors,
					       bloom, author_matched,
					       MAX_MATCHED);
		for (i = 0; i < n_author && n_matched < MAX_MATCHED; i++)
			matched[n_matched++] = author_matched[i];
	}

	/* Apply all matched rules in order (UA first, author last, last wins) */
	for (i = 0; i < n_matched; i++)
		apply_declarations(out, matched[i]->declarations);

	/* 3. Inline style="" (highest author priority) */
	{
		const char *inline_style = dom_attr(el, "style");
		if (inline_style && inline_style[0])
			apply_declarations(out, inline_style);
	}
}
