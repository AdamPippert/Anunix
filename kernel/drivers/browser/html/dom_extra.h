/*
 * dom_extra.h — Additional DOM helpers for JS bindings.
 *
 * These extend the minimal dom.h with query, mutation, and text
 * extraction APIs used by the JS engine.
 */

#ifndef ANX_DOM_EXTRA_H
#define ANX_DOM_EXTRA_H

#include <anx/types.h>
#include <anx/string.h>
#include "dom.h"

/* typedef for convenience in JS engine code */
typedef struct dom_doc dom_document;

/* ── Query ────────────────────────────────────────────────────────── */

/* Returns first element with matching id attribute, or NULL. */
struct dom_node *dom_get_element_by_id(struct dom_node *root,
                                       const char *id);

/* CSS simple selector: tag, #id, .class, or tag.class */
struct dom_node *dom_query_selector(struct dom_node *root,
                                    const char *selector);

/* ── Mutation ─────────────────────────────────────────────────────── */

/* Append child to parent element.  Silently drops if at capacity. */
static inline void dom_append_child(struct dom_node *parent,
                                    struct dom_node *child)
{
	if (!parent || !child) return;
	if (parent->type != DOM_ELEMENT) return;
	if (parent->el.n_children < DOM_MAX_CHILDREN)
		parent->el.children[parent->el.n_children++] = child;
}

/* Remove first matching child.  No-op if not found. */
static inline void dom_remove_child(struct dom_node *parent,
                                    struct dom_node *child)
{
	uint32_t i;
	if (!parent || !child || parent->type != DOM_ELEMENT) return;
	for (i = 0; i < parent->el.n_children; i++) {
		if (parent->el.children[i] == child) {
			uint32_t j;
			for (j = i + 1; j < parent->el.n_children; j++)
				parent->el.children[j-1] = parent->el.children[j];
			parent->el.n_children--;
			return;
		}
	}
}

/* Allocate a new element node from the doc arena.  Returns NULL if full. */
static inline struct dom_node *dom_create_element(struct dom_doc *doc,
                                                  const char *tag)
{
	if (!doc || doc->n_nodes >= DOM_MAX_NODES) return NULL;
	struct dom_node *n = &doc->nodes[doc->n_nodes++];
	n->type = DOM_ELEMENT;
	anx_strlcpy(n->el.tag, tag, sizeof(n->el.tag));
	n->el.n_children = 0;
	n->el.n_attrs    = 0;
	return n;
}

/* Allocate a new text node from the doc arena. */
static inline struct dom_node *dom_create_text(struct dom_doc *doc,
                                               const char *text)
{
	if (!doc || doc->n_nodes >= DOM_MAX_NODES) return NULL;
	struct dom_node *n = &doc->nodes[doc->n_nodes++];
	n->type = DOM_TEXT;
	anx_strlcpy(n->txt.text, text, sizeof(n->txt.text));
	return n;
}

/* ── Attribute mutation ───────────────────────────────────────────── */

/* Set or add an attribute on an element node. */
static inline void dom_attr_set(struct dom_node *el,
                                const char *name, const char *value)
{
	uint32_t i;
	if (!el || el->type != DOM_ELEMENT) return;
	/* Update existing */
	for (i = 0; i < el->el.n_attrs; i++) {
		if (anx_strcmp(el->el.attrs[i].name, name) == 0) {
			anx_strlcpy(el->el.attrs[i].value, value,
			            sizeof(el->el.attrs[i].value));
			return;
		}
	}
	/* Add new */
	if (el->el.n_attrs < HTML_MAX_ATTRS) {
		uint32_t idx = el->el.n_attrs++;
		anx_strlcpy(el->el.attrs[idx].name, name,
		            sizeof(el->el.attrs[idx].name));
		anx_strlcpy(el->el.attrs[idx].value, value,
		            sizeof(el->el.attrs[idx].value));
	}
}

/* ── Text extraction ──────────────────────────────────────────────── */

/* Concatenates all text-node descendants into out_buf[0..cap-1]. */
void dom_collect_text(const struct dom_node *node,
                      char *out_buf, uint32_t cap, uint32_t *pos);

/* Returns pointer to first text child, or "" if none. */
static inline const char *dom_text_content(const struct dom_node *n)
{
	uint32_t i;
	if (!n || n->type != DOM_ELEMENT) return "";
	for (i = 0; i < n->el.n_children; i++) {
		const struct dom_node *ch = n->el.children[i];
		if (ch && ch->type == DOM_TEXT)
			return ch->txt.text;
	}
	return "";
}

/* Returns tag name of element node, or "" if not an element. */
static inline const char *dom_tag_name(const struct dom_node *n)
{
	if (!n || n->type != DOM_ELEMENT) return "";
	return n->el.tag;
}

#endif /* ANX_DOM_EXTRA_H */
