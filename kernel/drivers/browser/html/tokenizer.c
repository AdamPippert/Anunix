/*
 * tokenizer.c — HTML5 tokenizer state machine.
 *
 * Implements the subset of HTML5 tokenizer states needed to correctly
 * parse the structure of real web pages: start/end tags, attributes,
 * text content, comments, and DOCTYPE.
 *
 * State names map directly to the HTML5 spec sections.
 */

#include "tokenizer.h"
#include <anx/string.h>
#include <anx/alloc.h>

/* ── State constants ─────────────────────────────────────────────── */

#define S_DATA                    0
#define S_TAG_OPEN                1
#define S_END_TAG_OPEN            2
#define S_TAG_NAME                3
#define S_BEFORE_ATTR_NAME        4
#define S_ATTR_NAME               5
#define S_AFTER_ATTR_NAME         6
#define S_BEFORE_ATTR_VALUE       7
#define S_ATTR_VALUE_DQ           8   /* double-quoted */
#define S_ATTR_VALUE_SQ           9   /* single-quoted */
#define S_ATTR_VALUE_UQ          10   /* unquoted */
#define S_AFTER_ATTR_VALUE       11
#define S_SELF_CLOSING           12
#define S_BOGUS_COMMENT          13
#define S_MARKUP_DECL_OPEN       14
#define S_COMMENT_START          15
#define S_COMMENT                16
#define S_COMMENT_END_DASH       17
#define S_COMMENT_END            18
#define S_DOCTYPE                19
#define S_RAWTEXT                20   /* <script>, <style> content */
#define S_RAWTEXT_LT             21
#define S_RAWTEXT_END_TAG_OPEN   22
#define S_RAWTEXT_END_TAG_NAME   23

/* ── Helpers ─────────────────────────────────────────────────────── */

static bool is_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static char to_lower(char c)
{
	if (c >= 'A' && c <= 'Z') return c + 32;
	return c;
}

static void scratch_append(struct html_tokenizer *t, char c)
{
	if (t->scratch_len + 1 < sizeof(t->scratch))
		t->scratch[t->scratch_len++] = c;
}

static void tag_name_append(struct html_tokenizer *t, char c)
{
	if (t->tag_name_len + 1 < HTML_MAX_TAG_NAME)
		t->tag_name[t->tag_name_len++] = to_lower(c);
}

static void attr_name_append(struct html_tokenizer *t, char c)
{
	struct html_attr *a = &t->attrs[t->cur_attr];
	uint32_t len = (uint32_t)anx_strlen(a->name);
	if (len + 1 < HTML_MAX_ATTR_NAME)
		a->name[len] = to_lower(c);
}

static void attr_val_append(struct html_tokenizer *t, char c)
{
	struct html_attr *a = &t->attrs[t->cur_attr];
	uint32_t len = (uint32_t)anx_strlen(a->value);
	if (len + 1 < HTML_MAX_ATTR_VAL)
		a->value[len] = c;
}

static void new_attr(struct html_tokenizer *t)
{
	if (t->n_attrs < HTML_MAX_ATTRS) {
		t->cur_attr = t->n_attrs++;
		anx_memset(&t->attrs[t->cur_attr], 0, sizeof(struct html_attr));
	}
}

static void emit_current_tag(struct html_tokenizer *t)
{
	struct html_token tok;

	anx_memset(&tok, 0, sizeof(tok));
	tok.type = t->is_end_tag ? HTML_TOK_END_TAG : HTML_TOK_START_TAG;
	tok.self_closing = t->self_closing;
	anx_strlcpy(tok.tag_name, t->tag_name, HTML_MAX_TAG_NAME);
	anx_memcpy(tok.attrs, t->attrs,
		    t->n_attrs * sizeof(struct html_attr));
	tok.n_attrs = t->n_attrs;

	t->cb(&tok, t->ctx);

	/* Check if we're entering a raw-text element */
	if (!t->is_end_tag) {
		if (anx_strcmp(t->tag_name, "script") == 0 ||
		    anx_strcmp(t->tag_name, "style")  == 0) {
			/* Save the tag name for rawtext end-tag matching */
			t->state = S_RAWTEXT;
			return;
		}
	}
	t->state = S_DATA;
}

static void emit_char(struct html_tokenizer *t, char c)
{
	struct html_token tok;
	anx_memset(&tok, 0, sizeof(tok));
	tok.type  = HTML_TOK_CHARACTER;
	tok.ch[0] = c;
	tok.ch[1] = '\0';
	t->cb(&tok, t->ctx);
}

static void emit_comment(struct html_tokenizer *t)
{
	struct html_token tok;
	anx_memset(&tok, 0, sizeof(tok));
	tok.type     = HTML_TOK_COMMENT;
	tok.text     = t->scratch;
	tok.text_len = t->scratch_len;
	t->scratch[t->scratch_len] = '\0';
	t->cb(&tok, t->ctx);
	t->scratch_len = 0;
}

static void reset_tag(struct html_tokenizer *t, bool is_end)
{
	t->tag_name_len = 0;
	t->tag_name[0]  = '\0';
	t->is_end_tag   = is_end;
	t->self_closing = false;
	t->n_attrs      = 0;
	t->cur_attr     = 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

void html_tokenizer_init(struct html_tokenizer *t,
			  html_token_cb cb, void *ctx)
{
	anx_memset(t, 0, sizeof(*t));
	t->cb    = cb;
	t->ctx   = ctx;
	t->state = S_DATA;
}

void html_tokenizer_feed(struct html_tokenizer *t,
			  const char *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		char c = data[i];

		switch (t->state) {

		/* ── DATA ─────────────────────────────────────────── */
		case S_DATA:
			if (c == '<') {
				t->state = S_TAG_OPEN;
			} else if (c == '&') {
				/* Simplified: emit '&' literally (no entity expand) */
				emit_char(t, c);
			} else {
				emit_char(t, c);
			}
			break;

		/* ── TAG OPEN ─────────────────────────────────────── */
		case S_TAG_OPEN:
			if (c == '/') {
				t->state = S_END_TAG_OPEN;
			} else if (c == '!') {
				t->state = S_MARKUP_DECL_OPEN;
				t->scratch_len = 0;
			} else if (c == '?') {
				/* Processing instruction — treat as bogus comment */
				t->state = S_BOGUS_COMMENT;
				t->scratch_len = 0;
			} else if (is_alpha(c)) {
				reset_tag(t, false);
				tag_name_append(t, c);
				t->state = S_TAG_NAME;
			} else {
				/* Anything else: emit '<' and reconsume */
				emit_char(t, '<');
				t->state = S_DATA;
				i--;  /* reconsume */
			}
			break;

		/* ── END TAG OPEN ─────────────────────────────────── */
		case S_END_TAG_OPEN:
			if (is_alpha(c)) {
				reset_tag(t, true);
				tag_name_append(t, c);
				t->state = S_TAG_NAME;
			} else if (c == '>') {
				t->state = S_DATA;
			} else {
				t->state = S_BOGUS_COMMENT;
				t->scratch_len = 0;
			}
			break;

		/* ── TAG NAME ─────────────────────────────────────── */
		case S_TAG_NAME:
			if (is_ws(c)) {
				t->state = S_BEFORE_ATTR_NAME;
			} else if (c == '/') {
				t->state = S_SELF_CLOSING;
			} else if (c == '>') {
				emit_current_tag(t);
			} else {
				tag_name_append(t, c);
			}
			break;

		/* ── BEFORE ATTRIBUTE NAME ────────────────────────── */
		case S_BEFORE_ATTR_NAME:
			if (is_ws(c)) {
				/* stay */
			} else if (c == '/' || c == '>') {
				t->state = S_AFTER_ATTR_NAME;
				i--;  /* reconsume */
			} else {
				new_attr(t);
				attr_name_append(t, c);
				t->state = S_ATTR_NAME;
			}
			break;

		/* ── ATTRIBUTE NAME ───────────────────────────────── */
		case S_ATTR_NAME:
			if (is_ws(c)) {
				t->state = S_AFTER_ATTR_NAME;
			} else if (c == '=') {
				t->state = S_BEFORE_ATTR_VALUE;
			} else if (c == '/' || c == '>') {
				t->state = S_AFTER_ATTR_NAME;
				i--;
			} else {
				attr_name_append(t, c);
			}
			break;

		/* ── AFTER ATTRIBUTE NAME ─────────────────────────── */
		case S_AFTER_ATTR_NAME:
			if (is_ws(c)) {
				/* stay */
			} else if (c == '=') {
				t->state = S_BEFORE_ATTR_VALUE;
			} else if (c == '/') {
				t->state = S_SELF_CLOSING;
			} else if (c == '>') {
				emit_current_tag(t);
			} else {
				new_attr(t);
				attr_name_append(t, c);
				t->state = S_ATTR_NAME;
			}
			break;

		/* ── BEFORE ATTRIBUTE VALUE ───────────────────────── */
		case S_BEFORE_ATTR_VALUE:
			if (is_ws(c)) {
				/* stay */
			} else if (c == '"') {
				t->state = S_ATTR_VALUE_DQ;
			} else if (c == '\'') {
				t->state = S_ATTR_VALUE_SQ;
			} else if (c == '>') {
				emit_current_tag(t);
			} else {
				attr_val_append(t, c);
				t->state = S_ATTR_VALUE_UQ;
			}
			break;

		case S_ATTR_VALUE_DQ:
			if (c == '"') t->state = S_AFTER_ATTR_VALUE;
			else attr_val_append(t, c);
			break;

		case S_ATTR_VALUE_SQ:
			if (c == '\'') t->state = S_AFTER_ATTR_VALUE;
			else attr_val_append(t, c);
			break;

		case S_ATTR_VALUE_UQ:
			if (is_ws(c))  t->state = S_BEFORE_ATTR_NAME;
			else if (c == '>') emit_current_tag(t);
			else attr_val_append(t, c);
			break;

		case S_AFTER_ATTR_VALUE:
			if (is_ws(c))  t->state = S_BEFORE_ATTR_NAME;
			else if (c == '/') t->state = S_SELF_CLOSING;
			else if (c == '>') emit_current_tag(t);
			break;

		/* ── SELF-CLOSING ─────────────────────────────────── */
		case S_SELF_CLOSING:
			if (c == '>') {
				t->self_closing = true;
				emit_current_tag(t);
			} else {
				t->state = S_BEFORE_ATTR_NAME;
				i--;
			}
			break;

		/* ── MARKUP DECLARATION OPEN (after '<!') ─────────── */
		case S_MARKUP_DECL_OPEN:
			scratch_append(t, c);
			if (t->scratch_len == 2 &&
			    t->scratch[0] == '-' && t->scratch[1] == '-') {
				t->scratch_len = 0;
				t->state = S_COMMENT_START;
			} else if (t->scratch_len == 7) {
				/* DOCTYPE or CDATA — treat as bogus */
				t->state = S_BOGUS_COMMENT;
			}
			break;

		/* ── COMMENT ──────────────────────────────────────── */
		case S_COMMENT_START:
			if (c == '-') t->state = S_COMMENT_END_DASH;
			else if (c == '>') {
				emit_comment(t);
				t->state = S_DATA;
			} else {
				scratch_append(t, c);
				t->state = S_COMMENT;
			}
			break;

		case S_COMMENT:
			if (c == '-') t->state = S_COMMENT_END_DASH;
			else scratch_append(t, c);
			break;

		case S_COMMENT_END_DASH:
			if (c == '-') t->state = S_COMMENT_END;
			else {
				scratch_append(t, '-');
				scratch_append(t, c);
				t->state = S_COMMENT;
			}
			break;

		case S_COMMENT_END:
			if (c == '>') {
				emit_comment(t);
				t->state = S_DATA;
			} else if (c == '-') {
				scratch_append(t, '-');
			} else {
				scratch_append(t, '-');
				scratch_append(t, '-');
				scratch_append(t, c);
				t->state = S_COMMENT;
			}
			break;

		/* ── BOGUS COMMENT ────────────────────────────────── */
		case S_BOGUS_COMMENT:
			if (c == '>') {
				emit_comment(t);
				t->state = S_DATA;
			} else {
				scratch_append(t, c);
			}
			break;

		/* ── DOCTYPE ──────────────────────────────────────── */
		case S_DOCTYPE:
			if (c == '>') t->state = S_DATA;
			break;

		/* ── RAWTEXT (<script>, <style>) ──────────────────── */
		case S_RAWTEXT:
			if (c == '<') {
				t->state = S_RAWTEXT_LT;
				t->scratch_len = 0;
			}
			/* Discard rawtext content — we don't execute JS/CSS inline */
			break;

		case S_RAWTEXT_LT:
			if (c == '/') {
				t->state = S_RAWTEXT_END_TAG_OPEN;
			} else {
				t->state = S_RAWTEXT;
			}
			break;

		case S_RAWTEXT_END_TAG_OPEN:
			if (is_alpha(c)) {
				t->scratch_len = 0;
				scratch_append(t, to_lower(c));
				t->state = S_RAWTEXT_END_TAG_NAME;
			} else {
				t->state = S_RAWTEXT;
			}
			break;

		case S_RAWTEXT_END_TAG_NAME:
			if (c == '>') {
				/* Accept any end-tag to exit rawtext for simplicity */
				t->state = S_DATA;
			} else if (is_ws(c)) {
				t->state = S_DATA;
			} else if (c == '/') {
				t->state = S_DATA;
			} else {
				scratch_append(t, to_lower(c));
			}
			break;

		default:
			t->state = S_DATA;
			break;
		}
	}
}

void html_tokenizer_eof(struct html_tokenizer *t)
{
	struct html_token tok;
	anx_memset(&tok, 0, sizeof(tok));
	tok.type = HTML_TOK_EOF;
	t->cb(&tok, t->ctx);
}
