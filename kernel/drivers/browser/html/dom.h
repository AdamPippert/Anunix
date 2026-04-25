/*
 * dom.h — DOM tree (subset).
 *
 * A minimal tree representation sufficient for block/inline layout.
 * Nodes are allocated from a per-session arena and freed all at once
 * when the page is replaced.
 */

#ifndef ANX_HTML_DOM_H
#define ANX_HTML_DOM_H

#include <anx/types.h>
#include <anx/alloc.h>
#include "tokenizer.h"

/* ── Node types ──────────────────────────────────────────────────── */

#define DOM_TEXT    1
#define DOM_ELEMENT 2

#define DOM_MAX_TEXT 512
#define DOM_MAX_CHILDREN 128

struct dom_node;

struct dom_element {
	char             tag[HTML_MAX_TAG_NAME];
	struct html_attr attrs[HTML_MAX_ATTRS];
	uint32_t         n_attrs;
	struct dom_node *children[DOM_MAX_CHILDREN];
	uint32_t         n_children;
};

struct dom_text {
	char text[DOM_MAX_TEXT];
};

struct dom_node {
	uint8_t type;    /* DOM_TEXT or DOM_ELEMENT */
	union {
		struct dom_element el;
		struct dom_text    txt;
	};
};

/* ── Document ────────────────────────────────────────────────────── */

#define DOM_MAX_NODES  4096

struct dom_doc {
	struct dom_node  nodes[DOM_MAX_NODES];
	uint32_t         n_nodes;
	struct dom_node *root;  /* <html> node */
	char             title[256];
};

/* ── Builder ─────────────────────────────────────────────────────── */

struct dom_builder {
	struct dom_doc  *doc;
	struct dom_node *stack[64];  /* element stack */
	uint32_t         depth;
	struct dom_node *title_node; /* <title> element if found */
	bool             in_title;
};

void dom_builder_init(struct dom_builder *b, struct dom_doc *doc);

/* Token callback: pass to html_tokenizer_init as the cb */
void dom_builder_token(const struct html_token *tok, void *ctx);

/* Get attribute value by name, or NULL */
const char *dom_attr(const struct dom_node *el, const char *name);

#endif /* ANX_HTML_DOM_H */
