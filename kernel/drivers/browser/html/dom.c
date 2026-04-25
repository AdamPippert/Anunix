/*
 * dom.c — DOM tree builder.
 * Consumes tokens from the HTML5 tokenizer and builds a compact tree.
 */

#include "dom.h"
#include <anx/string.h>

/* Tags that must not push onto the element stack */
static bool is_void(const char *tag)
{
	static const char *void_tags[] = {
		"area","base","br","col","embed","hr","img","input",
		"link","meta","param","source","track","wbr", NULL
	};
	int i;
	for (i = 0; void_tags[i]; i++)
		if (anx_strcmp(tag, void_tags[i]) == 0) return true;
	return false;
}

void dom_builder_init(struct dom_builder *b, struct dom_doc *doc)
{
	anx_memset(b,   0, sizeof(*b));
	anx_memset(doc, 0, sizeof(*doc));
	b->doc = doc;
}

void dom_builder_token(const struct html_token *tok, void *ctx)
{
	struct dom_builder *b = (struct dom_builder *)ctx;
	struct dom_doc     *d = b->doc;

	switch (tok->type) {

	case HTML_TOK_START_TAG: {
		if (d->n_nodes >= DOM_MAX_NODES) break;

		struct dom_node *n = &d->nodes[d->n_nodes++];
		n->type = DOM_ELEMENT;
		anx_strlcpy(n->el.tag, tok->tag_name, HTML_MAX_TAG_NAME);
		anx_memcpy(n->el.attrs, tok->attrs,
			    tok->n_attrs * sizeof(struct html_attr));
		n->el.n_attrs = tok->n_attrs;

		/* Set root */
		if (!d->root && anx_strcmp(tok->tag_name, "html") == 0)
			d->root = n;

		/* Append to parent */
		if (b->depth > 0) {
			struct dom_node *parent = b->stack[b->depth - 1];
			if (parent->type == DOM_ELEMENT &&
			    parent->el.n_children < DOM_MAX_CHILDREN)
				parent->el.children[parent->el.n_children++] = n;
		}

		/* Check for <title> */
		if (anx_strcmp(tok->tag_name, "title") == 0) {
			b->in_title   = true;
			b->title_node = n;
		}

		if (!tok->self_closing && !is_void(tok->tag_name)) {
			if (b->depth < 63)
				b->stack[b->depth++] = n;
		}
		break;
	}

	case HTML_TOK_END_TAG: {
		/* Pop matching element from stack */
		int32_t i;
		for (i = (int32_t)b->depth - 1; i >= 0; i--) {
			if (anx_strcmp(b->stack[i]->el.tag, tok->tag_name) == 0) {
				b->depth = (uint32_t)i;
				break;
			}
		}
		if (anx_strcmp(tok->tag_name, "title") == 0)
			b->in_title = false;
		break;
	}

	case HTML_TOK_CHARACTER: {
		/* Skip pure whitespace outside elements */
		if (!b->in_title && tok->ch[0] <= ' ' && tok->ch[1] == '\0')
			break;

		/* Accumulate into title */
		if (b->in_title) {
			uint32_t tlen = (uint32_t)anx_strlen(d->title);
			if (tlen + 1 < sizeof(d->title))
				d->title[tlen] = tok->ch[0];
			break;
		}

		/* Append to last text node in parent, or create new one */
		if (b->depth > 0) {
			struct dom_node *parent = b->stack[b->depth - 1];

			/* See if the last child is a text node */
			if (parent->el.n_children > 0) {
				struct dom_node *last =
					parent->el.children[parent->el.n_children - 1];
				if (last->type == DOM_TEXT) {
					uint32_t tl = (uint32_t)anx_strlen(last->txt.text);
					if (tl + 2 < DOM_MAX_TEXT) {
						last->txt.text[tl]   = tok->ch[0];
						last->txt.text[tl+1] = '\0';
					}
					break;
				}
			}

			/* New text node */
			if (d->n_nodes < DOM_MAX_NODES &&
			    parent->el.n_children < DOM_MAX_CHILDREN) {
				struct dom_node *tn = &d->nodes[d->n_nodes++];
				tn->type = DOM_TEXT;
				tn->txt.text[0] = tok->ch[0];
				tn->txt.text[1] = '\0';
				parent->el.children[parent->el.n_children++] = tn;
			}
		}
		break;
	}

	default:
		break;
	}
}

const char *dom_attr(const struct dom_node *el, const char *name)
{
	uint32_t i;
	if (!el || el->type != DOM_ELEMENT) return NULL;
	for (i = 0; i < el->el.n_attrs; i++)
		if (anx_strcmp(el->el.attrs[i].name, name) == 0)
			return el->el.attrs[i].value;
	return NULL;
}
