/*
 * tokenizer.h — HTML5 tokenizer (subset).
 *
 * Implements the tokenizer state machine from the HTML5 spec
 * (https://html.spec.whatwg.org/multipage/parsing.html#tokenization).
 *
 * Tokens are delivered to a callback. The caller never needs to buffer
 * the full document; the tokenizer processes chunks incrementally.
 */

#ifndef ANX_HTML_TOKENIZER_H
#define ANX_HTML_TOKENIZER_H

#include <anx/types.h>

/* ── Token types ─────────────────────────────────────────────────── */

#define HTML_TOK_DOCTYPE     1
#define HTML_TOK_START_TAG   2
#define HTML_TOK_END_TAG     3
#define HTML_TOK_COMMENT     4
#define HTML_TOK_CHARACTER   5
#define HTML_TOK_EOF         6

#define HTML_MAX_TAG_NAME   64
#define HTML_MAX_ATTR_NAME  64
#define HTML_MAX_ATTR_VAL  256
#define HTML_MAX_ATTRS      32

struct html_attr {
	char name[HTML_MAX_ATTR_NAME];
	char value[HTML_MAX_ATTR_VAL];
};

struct html_token {
	uint8_t type;             /* HTML_TOK_* */
	bool    self_closing;

	/* TAG tokens */
	char tag_name[HTML_MAX_TAG_NAME];
	struct html_attr attrs[HTML_MAX_ATTRS];
	uint32_t n_attrs;

	/* CHARACTER token: single Unicode code point as UTF-8 */
	char     ch[5];

	/* COMMENT / DOCTYPE: text content */
	char    *text;            /* points into tokenizer scratch; do not free */
	uint32_t text_len;
};

/* ── Tokenizer state ─────────────────────────────────────────────── */

#define HTML_TOK_STATE_COUNT 32

typedef void (*html_token_cb)(const struct html_token *tok, void *ctx);

struct html_tokenizer {
	/* Callback invoked for each complete token */
	html_token_cb  cb;
	void          *ctx;

	/* State machine */
	uint32_t       state;

	/* Work buffers (internal) */
	char           tag_name[HTML_MAX_TAG_NAME];
	uint32_t       tag_name_len;
	bool           is_end_tag;
	bool           self_closing;

	struct html_attr attrs[HTML_MAX_ATTRS];
	uint32_t         n_attrs;
	uint32_t         cur_attr;     /* index of attribute being parsed */
	bool             in_attr_val;

	char    scratch[4096];         /* comment/doctype text scratch */
	uint32_t scratch_len;

	/* Pending character(s) from character reference */
	char    char_buf[8];
	uint32_t char_len;

	/* Return-state for character references */
	uint32_t return_state;
};

/* Initialize a tokenizer and attach a callback. */
void html_tokenizer_init(struct html_tokenizer *t,
			  html_token_cb cb, void *ctx);

/*
 * Feed bytes to the tokenizer.
 * May invoke the callback zero or more times synchronously.
 */
void html_tokenizer_feed(struct html_tokenizer *t,
			  const char *data, size_t len);

/* Signal end of input; flushes any pending token. */
void html_tokenizer_eof(struct html_tokenizer *t);

#endif /* ANX_HTML_TOKENIZER_H */
