/*
 * css_parser.c — CSS text tokenizer.
 */

#include "css_parser.h"
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* ── Whitespace / comment helpers ──────────────────────────────────── */

static void skip_ws(const char **p)
{
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
		(*p)++;
}

/* Skip whitespace and CSS block comments: /* ... * / */
static void skip_ws_comment(const char **p, const char *end)
{
	for (;;) {
		skip_ws(p);
		if (*p + 1 < end && **p == '/' && *(*p + 1) == '*') {
			*p += 2;
			while (*p + 1 < end &&
			       !(**p == '*' && *(*p + 1) == '/'))
				(*p)++;
			if (*p + 1 < end)
				*p += 2;
		} else {
			break;
		}
	}
}

/* ── Specificity ────────────────────────────────────────────────────── */

static uint32_t compute_specificity(const char *sel, size_t len)
{
	uint32_t    a = 0, b = 0, c = 0;
	const char *p   = sel;
	const char *end = sel + len;

	while (p < end) {
		if (*p == '#') {
			a++;
			p++;
			while (p < end && *p != '.' && *p != '#' &&
			       *p != ' ' && *p != '>' && *p != '+' && *p != '~' &&
			       *p != ':' && *p != '[')
				p++;
		} else if (*p == '.') {
			b++;
			p++;
			while (p < end && *p != '.' && *p != '#' &&
			       *p != ' ' && *p != '>' && *p != '+' && *p != '~' &&
			       *p != ':' && *p != '[')
				p++;
		} else if (*p == ':' || *p == '[') {
			/* pseudo-class / attr selector — count as class */
			b++;
			while (p < end && *p != ' ' && *p != '.' &&
			       *p != '#' && *p != ':' && *p != '[')
				p++;
		} else if (*p == ' ' || *p == '>' || *p == '+' || *p == '~') {
			p++;
		} else if (*p == '*') {
			p++;
		} else {
			/* tag name */
			c++;
			while (p < end && *p != '.' && *p != '#' &&
			       *p != ' ' && *p != '>' && *p != '+' && *p != '~' &&
			       *p != ':' && *p != '[')
				p++;
		}
	}
	return (a << 16) | (b << 8) | c;
}

/* ── Sheet management ───────────────────────────────────────────────── */

void css_sheet_init(struct css_sheet *sheet)
{
	anx_memset(sheet, 0, sizeof(*sheet));
}

void css_sheet_free(struct css_sheet *sheet)
{
	if (sheet->rules) {
		anx_free(sheet->rules);
		sheet->rules = NULL;
	}
	sheet->n_rules = 0;
	sheet->cap     = 0;
}

static size_t rtrim_len(const char *s, size_t len)
{
	while (len > 0 &&
	       (s[len-1] == ' ' || s[len-1] == '\t' ||
	        s[len-1] == '\n' || s[len-1] == '\r'))
		len--;
	return len;
}

static int sheet_add(struct css_sheet *sheet,
		      const char *sel,  size_t sel_len,
		      const char *decl, size_t decl_len,
		      uint8_t origin)
{
	if (sheet->n_rules >= CSS_MAX_RULES)
		return 0;  /* silently drop overflow rules */

	if (sheet->n_rules >= sheet->cap) {
		uint32_t         new_cap   = sheet->cap ? sheet->cap * 2 : 64;
		struct css_rule *new_rules =
			(struct css_rule *)anx_alloc(
				new_cap * sizeof(struct css_rule));
		if (!new_rules)
			return -1;
		if (sheet->rules) {
			anx_memcpy(new_rules, sheet->rules,
				    sheet->n_rules * sizeof(struct css_rule));
			anx_free(sheet->rules);
		}
		sheet->rules = new_rules;
		sheet->cap   = new_cap;
	}

	struct css_rule *r = &sheet->rules[sheet->n_rules++];
	anx_memset(r, 0, sizeof(*r));

	sel_len = rtrim_len(sel, sel_len);
	if (sel_len >= CSS_SELECTOR_MAX)
		sel_len = CSS_SELECTOR_MAX - 1;
	anx_memcpy(r->selector, sel, sel_len);
	r->selector[sel_len] = '\0';

	/* Strip leading whitespace from declarations */
	while (decl_len > 0 &&
	       (*decl == ' ' || *decl == '\t' || *decl == '\n' || *decl == '\r')) {
		decl++;
		decl_len--;
	}
	decl_len = rtrim_len(decl, decl_len);
	if (decl_len >= CSS_DECL_MAX)
		decl_len = CSS_DECL_MAX - 1;
	anx_memcpy(r->declarations, decl, decl_len);
	r->declarations[decl_len] = '\0';

	r->specificity = compute_specificity(r->selector, sel_len);
	r->origin      = origin;
	return 0;
}

/* ── Public parser ──────────────────────────────────────────────────── */

int css_parse(const char *text, size_t len,
	       struct css_sheet *sheet, uint8_t origin)
{
	const char *p   = text;
	const char *end = text + len;

	while (p < end) {
		skip_ws_comment(&p, end);
		if (p >= end)
			break;

		/* Skip @-rules */
		if (*p == '@') {
			while (p < end && *p != '{' && *p != ';')
				p++;
			if (p < end && *p == '{') {
				int depth = 1;
				p++;
				while (p < end && depth > 0) {
					if (*p == '{') depth++;
					else if (*p == '}') depth--;
					p++;
				}
			} else if (p < end) {
				p++; /* skip ';' */
			}
			continue;
		}

		/* Read selector block up to '{' */
		const char *sel_start = p;
		while (p < end && *p != '{')
			p++;
		if (p >= end)
			break;

		const char *sel_end = p;
		p++; /* skip '{' */

		/* Read declarations up to matching '}' */
		const char *decl_start = p;
		int depth = 1;
		while (p < end && depth > 0) {
			if (*p == '{')      depth++;
			else if (*p == '}') depth--;
			if (depth > 0) p++;
			else           break;
		}
		const char *decl_end = p;
		if (p < end) p++; /* skip '}' */

		/* Handle comma-separated selector list */
		const char *sp = sel_start;
		while (sp < sel_end) {
			const char *comma = sp;
			while (comma < sel_end && *comma != ',')
				comma++;

			/* Skip leading whitespace of this one selector */
			const char *ss   = sp;
			size_t      slen = (size_t)(comma - ss);
			while (slen > 0 &&
			       (*ss == ' ' || *ss == '\t' ||
			        *ss == '\n' || *ss == '\r')) {
				ss++;
				slen--;
			}

			if (slen > 0) {
				sheet_add(sheet, ss, slen,
					   decl_start,
					   (size_t)(decl_end - decl_start),
					   origin);
			}

			sp = (comma < sel_end) ? comma + 1 : sel_end;
		}
	}

	return 0;
}
