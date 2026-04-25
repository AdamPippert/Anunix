/*
 * dom_extra.c — DOM query and text extraction.
 */

#include "dom_extra.h"
#include <anx/string.h>

static int str_icmp(const char *a, const char *b)
{
	while (*a && *b) {
		char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
		if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
		a++; b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ── Simple selector matching ─────────────────────────────────────── */

/* Parse selector into (tag, id, class) components */
static void parse_selector(const char *sel,
                           char *tag, uint32_t tag_cap,
                           char *id,  uint32_t id_cap,
                           char *cls, uint32_t cls_cap)
{
	tag[0] = id[0] = cls[0] = '\0';
	uint32_t i = 0, tpos = 0, ipos = 0, cpos = 0;
	char mode = 't'; /* t=tag, i=id, c=class */
	for (; sel[i]; i++) {
		char c = sel[i];
		if (c == '#') { mode = 'i'; }
		else if (c == '.') { mode = 'c'; }
		else if (mode == 't' && tpos + 1 < tag_cap) tag[tpos++] = c;
		else if (mode == 'i' && ipos + 1 < id_cap)  id[ipos++]  = c;
		else if (mode == 'c' && cpos + 1 < cls_cap) cls[cpos++] = c;
	}
	tag[tpos] = id[ipos] = cls[cpos] = '\0';
}

static bool node_matches(const struct dom_node *n,
                         const char *tag, const char *id, const char *cls)
{
	if (n->type != DOM_ELEMENT) return false;
	if (tag[0] && str_icmp(n->el.tag, tag) != 0) return false;
	if (id[0]) {
		const char *nid = dom_attr(n, "id");
		if (!nid || anx_strcmp(nid, id) != 0) return false;
	}
	if (cls[0]) {
		const char *ncls = dom_attr(n, "class");
		if (!ncls) return false;
		/* Check if cls is a word in the class string */
		const char *p = ncls;
		uint32_t clen = anx_strlen(cls);
		while (*p) {
			while (*p == ' ') p++;
			const char *start = p;
			while (*p && *p != ' ') p++;
			uint32_t wlen = (uint32_t)(p - start);
			if (wlen == clen && anx_strncmp(start, cls, clen) == 0)
				return true;
		}
		return false;
	}
	return true;
}

static struct dom_node *qs_walk(struct dom_node *node,
                                const char *tag, const char *id,
                                const char *cls)
{
	if (!node) return NULL;
	if (node_matches(node, tag, id, cls)) return node;
	if (node->type == DOM_ELEMENT) {
		uint32_t i;
		for (i = 0; i < node->el.n_children; i++) {
			struct dom_node *r = qs_walk(node->el.children[i], tag, id, cls);
			if (r) return r;
		}
	}
	return NULL;
}

struct dom_node *dom_query_selector(struct dom_node *root, const char *selector)
{
	char tag[32], id[64], cls[64];
	parse_selector(selector, tag, sizeof(tag), id, sizeof(id), cls, sizeof(cls));
	return qs_walk(root, tag, id, cls);
}

struct dom_node *dom_get_element_by_id(struct dom_node *root, const char *id)
{
	char sel[66];
	sel[0] = '#';
	anx_strlcpy(sel + 1, id, sizeof(sel) - 1);
	return dom_query_selector(root, sel);
}

/* ── Text content ─────────────────────────────────────────────────── */

void dom_collect_text(const struct dom_node *node,
                      char *out_buf, uint32_t cap, uint32_t *pos)
{
	if (!node || *pos >= cap - 1) return;
	if (node->type == DOM_TEXT) {
		const char *t = node->txt.text;
		while (*t && *pos < cap - 1)
			out_buf[(*pos)++] = *t++;
	} else if (node->type == DOM_ELEMENT) {
		uint32_t i;
		for (i = 0; i < node->el.n_children; i++)
			dom_collect_text(node->el.children[i], out_buf, cap, pos);
	}
	out_buf[*pos] = '\0';
}
