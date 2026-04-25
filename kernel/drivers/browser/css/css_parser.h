/*
 * css_parser.h — CSS text tokenizer: produces flat rule arrays.
 *
 * Parses CSS source (e.g. contents of a <style> block) into a list of
 * {selector, declarations, specificity} tuples.  Comma-separated selector
 * groups are split into individual rules.  @-rules are silently skipped.
 */

#ifndef ANX_BROWSER_CSS_PARSER_H
#define ANX_BROWSER_CSS_PARSER_H

#include <anx/types.h>

#define CSS_MAX_RULES      512
#define CSS_SELECTOR_MAX   128
#define CSS_DECL_MAX      1024

/* Cascade origin — lower numeric value = lower priority in cascade */
#define CSS_ORIGIN_UA     0   /* built-in user-agent stylesheet */
#define CSS_ORIGIN_AUTHOR 1   /* <style> block or linked sheet */
#define CSS_ORIGIN_INLINE 2   /* style="" attribute */

struct css_rule {
	char     selector[CSS_SELECTOR_MAX];  /* raw selector text, e.g. ".foo > p" */
	char     declarations[CSS_DECL_MAX];  /* raw decls, e.g. "color:red;margin:10px" */
	uint32_t specificity;                  /* packed: (ids<<16)|(classes<<8)|tags */
	uint8_t  origin;                       /* CSS_ORIGIN_* */
};

struct css_sheet {
	struct css_rule *rules;  /* heap-allocated array */
	uint32_t         n_rules;
	uint32_t         cap;
};

/* Initialise / free a css_sheet. */
void css_sheet_init(struct css_sheet *sheet);
void css_sheet_free(struct css_sheet *sheet);

/*
 * Parse CSS text and append resulting rules to sheet.
 * Returns 0 on success, -1 on allocation failure.
 */
int css_parse(const char *text, size_t len,
	       struct css_sheet *sheet, uint8_t origin);

#endif /* ANX_BROWSER_CSS_PARSER_H */
