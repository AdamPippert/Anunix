/*
 * css_cascade.h — Computed style and cascade pipeline.
 *
 * css_resolve() runs the full CSS cascade for one DOM element:
 *   1. UA defaults (via built-in stylesheet)
 *   2. Author rules (matched from the page's <style> blocks)
 *   3. Inline style="" attribute
 *
 * Inherited properties flow from parent to child automatically.
 */

#ifndef ANX_BROWSER_CSS_CASCADE_H
#define ANX_BROWSER_CSS_CASCADE_H

#include <anx/types.h>
#include "css_selector.h"
#include "../html/dom.h"

/* ── Display & layout constants ──────────────────────────────────── */

#define CSS_DISPLAY_BLOCK        0
#define CSS_DISPLAY_INLINE       1
#define CSS_DISPLAY_INLINE_BLOCK 2
#define CSS_DISPLAY_FLEX         3
#define CSS_DISPLAY_NONE         4

#define CSS_POSITION_STATIC   0
#define CSS_POSITION_RELATIVE 1
#define CSS_POSITION_ABSOLUTE 2
#define CSS_POSITION_FIXED    3

#define CSS_FLOAT_NONE  0
#define CSS_FLOAT_LEFT  1
#define CSS_FLOAT_RIGHT 2

#define CSS_TEXT_ALIGN_LEFT   0
#define CSS_TEXT_ALIGN_CENTER 1
#define CSS_TEXT_ALIGN_RIGHT  2

#define CSS_FONT_WEIGHT_NORMAL 0
#define CSS_FONT_WEIGHT_BOLD   1

#define CSS_TEXT_DECO_NONE      0
#define CSS_TEXT_DECO_UNDERLINE 1

#define CSS_BORDER_NONE   0
#define CSS_BORDER_SOLID  1
#define CSS_BORDER_DASHED 2

/* Sentinel value for width/height: means "auto". */
#define CSS_DIM_AUTO 0xFFFFU

/* ── Computed style (48 bytes) ───────────────────────────────────── */

struct computed_style {
	/* Display & position (4 bytes) */
	uint8_t  display;
	uint8_t  position;
	uint8_t  float_val;
	uint8_t  text_align;

	/* Colors (8 bytes) */
	uint32_t color_fg;
	uint32_t color_bg;

	/* Font (4 bytes) */
	uint8_t  font_size_px;    /* pixel height: 8 10 12 14 16 18 20 24 32 */
	uint8_t  font_weight;     /* CSS_FONT_WEIGHT_* */
	uint8_t  font_style;      /* 0=normal, 1=italic */
	uint8_t  text_decoration; /* CSS_TEXT_DECO_* */

	/* Box model (16 bytes) */
	uint16_t margin_top,    margin_right;
	uint16_t margin_bottom, margin_left;
	uint16_t padding_top,   padding_right;
	uint16_t padding_bottom, padding_left;

	/* Explicit dimensions (4 bytes): CSS_DIM_AUTO = not set */
	uint16_t width, height;

	/* Border (6 bytes) */
	uint8_t  border_width;
	uint8_t  border_style;  /* CSS_BORDER_* */
	uint32_t border_color;

	/* Dirty bits — set by layout engine, not by cascade */
	uint8_t  dirty;  /* bit 0=layout, 1=paint, 2=style */
};

/* ── Cascade API ─────────────────────────────────────────────────── */

/*
 * Initialise the UA (user-agent) stylesheet.  Call once at browser init.
 * The UA sheet is stored in a module-level css_sheet; it persists for the
 * lifetime of the browser driver.
 */
void css_ua_init(void);

/*
 * Resolve computed style for one DOM element.
 *
 * author:   Selector index built from parsed <style> blocks.  May be NULL.
 * parent:   Inherited computed style of parent element.  May be NULL for root.
 * ancestors / n_ancestors: ancestor DOM node chain (parent first).
 * bloom:    Bloom filter built from ancestor tags+classes.
 */
void css_resolve(struct computed_style             *out,
		  const struct dom_node            *el,
		  const struct css_selector_index  *author,
		  const struct computed_style      *parent,
		  const struct dom_node   *const   *ancestors,
		  uint32_t                          n_ancestors,
		  const struct css_bloom           *bloom);

#endif /* ANX_BROWSER_CSS_CASCADE_H */
