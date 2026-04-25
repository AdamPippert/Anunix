/*
 * css_selector.c — Rule index and selector matching.
 */

#include "css_selector.h"
#include <anx/string.h>
#include <anx/alloc.h>

/* ── FNV-1a hash ────────────────────────────────────────────────────── */

static uint32_t fnv1a(const char *s, size_t len)
{
	uint32_t h = 2166136261u;
	size_t   i;
	for (i = 0; i < len; i++) {
		h ^= (uint8_t)s[i];
		h *= 16777619u;
	}
	return h;
}

static uint32_t fnv1a_str(const char *s)
{
	return fnv1a(s, anx_strlen(s));
}

/* ── Bloom filter ───────────────────────────────────────────────────── */

void css_bloom_clear(struct css_bloom *b)
{
	anx_memset(b->bits, 0, sizeof(b->bits));
}

static void bloom_set(struct css_bloom *b, uint32_t hash)
{
	uint32_t idx  = (hash >> 5) & (CSS_BLOOM_WORDS - 1);
	uint32_t bit  = hash & 31u;
	b->bits[idx] |= (1u << bit);
}

void css_bloom_add(struct css_bloom *b, const char *str)
{
	uint32_t h = fnv1a_str(str);
	bloom_set(b, h);
	bloom_set(b, h ^ 0x5bd1e995u);
	bloom_set(b, h ^ 0x9e3779b9u);
}

bool css_bloom_test(const struct css_bloom *b, const char *str)
{
	uint32_t h = fnv1a_str(str);
	uint32_t seeds[3];
	uint32_t i;
	seeds[0] = h;
	seeds[1] = h ^ 0x5bd1e995u;
	seeds[2] = h ^ 0x9e3779b9u;
	for (i = 0; i < 3; i++) {
		uint32_t idx = (seeds[i] >> 5) & (CSS_BLOOM_WORDS - 1);
		uint32_t bit = seeds[i] & 31u;
		if (!(b->bits[idx] & (1u << bit)))
			return false;
	}
	return true;
}

/* ── Rightmost-key extraction ───────────────────────────────────────── */

/*
 * From a full selector like "div.foo .bar > p#baz", extract the key of
 * the rightmost simple selector component.
 *
 * Returns 'i' (id), 'c' (class), 't' (tag), or '*' (universal/unknown).
 * Writes the key name into out_key (NUL-terminated).
 */
static char rightmost_key(const char *sel, char *out_key, size_t key_cap)
{
	const char *p   = sel;
	const char *end = sel + anx_strlen(sel);

	/* Trim trailing whitespace */
	while (end > p && (end[-1] == ' ' || end[-1] == '\t' ||
	                   end[-1] == '\n' || end[-1] == '\r'))
		end--;

	/* Walk back to the start of the rightmost token */
	const char *tok_end = end;
	const char *q = end;
	while (q > p && q[-1] != ' ' && q[-1] != '>' &&
	       q[-1] != '+' && q[-1] != '~')
		q--;
	const char *tok = q;

	if (tok == tok_end) {
		out_key[0] = '\0';
		return '*';
	}

	/* Scan the token for #id first (highest priority), then .class, then tag */
	char   type       = '*';
	size_t name_len   = 0;
	const char *name  = NULL;

	q = tok;

	/* Initial tag */
	if (*q != '#' && *q != '.' && *q != '*' && *q != ':' && *q != '[') {
		name = q;
		while (q < tok_end && *q != '#' && *q != '.' && *q != ':' && *q != '[')
			q++;
		name_len = q - name;
		type = 't';
	} else if (*q == '*') {
		q++;
	}

	/* Scan rest for #id or .class — id wins */
	while (q < tok_end) {
		if (*q == '#') {
			q++;
			name = q;
			while (q < tok_end && *q != '.' && *q != '#' &&
			       *q != ':' && *q != '[')
				q++;
			name_len = q - name;
			type = 'i';
			break;  /* id is highest priority */
		} else if (*q == '.') {
			q++;
			if (type != 'i') {
				name = q;
				while (q < tok_end && *q != '.' && *q != '#' &&
				       *q != ':' && *q != '[')
					q++;
				name_len = q - name;
				type = 'c';
			} else {
				while (q < tok_end && *q != '.' && *q != '#' &&
				       *q != ':' && *q != '[')
					q++;
			}
		} else {
			q++;
		}
	}

	if (name && name_len > 0) {
		size_t copy = name_len < key_cap - 1 ? name_len : key_cap - 1;
		anx_memcpy(out_key, name, copy);
		out_key[copy] = '\0';
	} else {
		out_key[0] = '\0';
		type = '*';
	}
	return type;
}

/* ── Index build ────────────────────────────────────────────────────── */

void css_index_clear(struct css_selector_index *idx)
{
	anx_memset(idx, 0, sizeof(*idx));
}

static void bucket_add(struct css_bucket *b, const struct css_rule *r)
{
	if (b->n < CSS_BUCKET_MAX)
		b->rules[b->n++] = r;
}

void css_index_build(struct css_selector_index *idx,
		      const struct css_sheet *sheet)
{
	uint32_t i;
	char     key[64];

	for (i = 0; i < sheet->n_rules; i++) {
		const struct css_rule *r = &sheet->rules[i];
		char type = rightmost_key(r->selector, key, sizeof(key));

		if (type == 'i') {
			uint32_t slot = fnv1a_str(key) & (CSS_ID_BUCKETS - 1);
			bucket_add(&idx->by_id[slot], r);
		} else if (type == 'c') {
			uint32_t slot = fnv1a_str(key) & (CSS_CLASS_BUCKETS - 1);
			bucket_add(&idx->by_class[slot], r);
		} else if (type == 't') {
			uint32_t slot = fnv1a_str(key) & (CSS_TAG_BUCKETS - 1);
			bucket_add(&idx->by_tag[slot], r);
		} else {
			bucket_add(&idx->universal, r);
		}
	}
}

/* ── Simple selector matching ───────────────────────────────────────── */

/* Check if class list (space-separated) contains the given class name. */
static bool has_class(const char *class_list, const char *cls, size_t cls_len)
{
	const char *p = class_list;
	while (*p) {
		while (*p == ' ') p++;
		const char *tok = p;
		while (*p && *p != ' ') p++;
		size_t tok_len = (size_t)(p - tok);
		if (tok_len == cls_len &&
		    anx_strncmp(tok, cls, cls_len) == 0)
			return true;
	}
	return false;
}

/*
 * Does element el match the simple (no-combinator) selector described by
 * the string sel[0..len-1]?
 */
static bool match_simple(const char *sel, size_t len,
			   const struct dom_node *el)
{
	const char *p   = sel;
	const char *end = sel + len;

	if (!el || el->type != DOM_ELEMENT)
		return false;

	/* Tag name check (must come first if present) */
	if (p < end && *p != '#' && *p != '.' &&
	    *p != '*' && *p != ':' && *p != '[') {
		const char *tag_s = p;
		while (p < end && *p != '#' && *p != '.' &&
		       *p != ':' && *p != '[')
			p++;
		size_t      tag_l = (size_t)(p - tag_s);
		const char *el_tag = el->el.tag;
		size_t      el_tl  = anx_strlen(el_tag);
		if (el_tl != tag_l || anx_strncmp(el_tag, tag_s, tag_l) != 0)
			return false;
	} else if (p < end && *p == '*') {
		p++;
	}

	/* Class and ID checks */
	while (p < end) {
		if (*p == '#') {
			p++;
			const char *id_s = p;
			while (p < end && *p != '#' && *p != '.' &&
			       *p != ':' && *p != '[')
				p++;
			size_t      id_l  = (size_t)(p - id_s);
			const char *el_id = dom_attr(el, "id");
			if (!el_id)
				return false;
			size_t el_il = anx_strlen(el_id);
			if (el_il != id_l || anx_strncmp(el_id, id_s, id_l) != 0)
				return false;
		} else if (*p == '.') {
			p++;
			const char *cls_s = p;
			while (p < end && *p != '#' && *p != '.' &&
			       *p != ':' && *p != '[')
				p++;
			size_t      cls_l  = (size_t)(p - cls_s);
			const char *el_cls = dom_attr(el, "class");
			if (!el_cls || !has_class(el_cls, cls_s, cls_l))
				return false;
		} else if (*p == ':') {
			/* pseudo-class: skip for now (always passes) */
			while (p < end && *p != '.' && *p != '#')
				p++;
		} else if (*p == '[') {
			/* attribute selector: skip for now */
			while (p < end && *p != ']')
				p++;
			if (p < end) p++;
		} else {
			p++;
		}
	}
	return true;
}

/* ── Compound selector matching with ancestor context ──────────────── */

#define SEG_MAX 8

struct seg {
	const char *text;
	size_t      len;
	char        comb;  /* combinator BEFORE this segment: ' ', '>', '+', '~', or 0 */
};

/*
 * Parse a full selector string into segments.
 * segs[i].comb is the combinator that relates segs[i] to segs[i-1].
 * segs[0].comb is always 0 (no predecessor).
 * Returns number of segments found.
 */
static int parse_segments(const char *sel, struct seg *segs, int max)
{
	const char *p         = sel;
	int         n         = 0;
	char        next_comb = 0;  /* combinator to assign to the next segment */

	while (*p && n < max) {
		/* Skip whitespace */
		while (*p == ' ') p++;
		if (!*p) break;

		/* Explicit combinator character */
		if (*p == '>' || *p == '+' || *p == '~') {
			next_comb = *p++;
			while (*p == ' ') p++;
			continue;
		}

		/* Simple selector: everything up to whitespace or combinator */
		const char *start = p;
		while (*p && *p != ' ' && *p != '>' &&
		       *p != '+' && *p != '~')
			p++;
		size_t len = (size_t)(p - start);
		if (len == 0)
			continue;

		segs[n].text = start;
		segs[n].len  = len;
		segs[n].comb = next_comb;
		n++;

		/* Default combinator for the next segment is descendant */
		next_comb = ' ';
	}
	return n;
}

/*
 * Recursively match selector segments against element and ancestor chain.
 * seg_idx: index of the segment we're trying to match right now (0-based
 * from left; we match right-to-left by calling with seg_idx = n_segs-1).
 */
static bool match_segments(const struct seg *segs, int seg_idx,
			     const struct dom_node *el,
			     const struct dom_node *const *ancs,
			     uint32_t n_ancs)
{
	if (seg_idx < 0)
		return true;  /* all segments satisfied */

	if (!match_simple(segs[seg_idx].text, segs[seg_idx].len, el))
		return false;

	if (seg_idx == 0)
		return true;  /* leftmost segment matched */

	char        comb      = segs[seg_idx].comb;
	const struct seg *prev = &segs[seg_idx - 1];

	if (comb == '>') {
		/* Immediate parent must match segs[seg_idx-1] */
		if (n_ancs == 0)
			return false;
		return match_segments(segs, seg_idx - 1,
				       ancs[0], ancs + 1, n_ancs - 1);
	} else {
		/* Descendant: any ancestor must satisfy segs[0..seg_idx-1] */
		uint32_t i;
		for (i = 0; i < n_ancs; i++) {
			if (match_segments(segs, seg_idx - 1,
					    ancs[i], ancs + i + 1,
					    n_ancs - i - 1))
				return true;
		}
		return false;
	}
	(void)prev;
}

static bool rule_matches(const struct css_rule *r,
			   const struct dom_node *el,
			   const struct dom_node *const *ancs,
			   uint32_t n_ancs,
			   const struct css_bloom *bloom)
{
	struct seg segs[SEG_MAX];
	int n_segs = parse_segments(r->selector, segs, SEG_MAX);
	if (n_segs == 0)
		return false;

	/* Bloom fast-reject: if the selector requires an ancestor tag/class
	 * that is definitely not in the chain, skip. */
	int i;
	for (i = 0; i < n_segs - 1; i++) {
		/* For each non-rightmost segment, check the Bloom filter */
		/* We check the tag portion (before any '.' or '#') */
		const char *s = segs[i].text;
		size_t      l = segs[i].len;
		if (l > 0 && *s != '#' && *s != '.' && *s != '*') {
			/* Has a tag requirement — check ancestor bloom */
			char tag[32];
			size_t tl = 0;
			while (tl < l && s[tl] != '.' && s[tl] != '#' && tl < 31) {
				tag[tl] = s[tl];
				tl++;
			}
			tag[tl] = '\0';
			if (!css_bloom_test(bloom, tag))
				return false;
		}
	}

	return match_segments(segs, n_segs - 1, el, ancs, n_ancs);
}

/* ── Sort helpers ───────────────────────────────────────────────────── */

static uint32_t rule_sort_key(const struct css_rule *r)
{
	return ((uint32_t)r->origin << 24) | r->specificity;
}

/* Simple insertion sort on a small array of rule pointers */
static void sort_rules(const struct css_rule **rules, uint32_t n)
{
	uint32_t i, j;
	for (i = 1; i < n; i++) {
		const struct css_rule *key = rules[i];
		uint32_t               kv  = rule_sort_key(key);
		j = i;
		while (j > 0 && rule_sort_key(rules[j-1]) > kv) {
			rules[j] = rules[j-1];
			j--;
		}
		rules[j] = key;
	}
}

/* ── Public match API ───────────────────────────────────────────────── */

static void collect_from_bucket(const struct css_bucket *b,
				  const struct dom_node *el,
				  const struct dom_node *const *ancs,
				  uint32_t n_ancs,
				  const struct css_bloom *bloom,
				  const struct css_rule **out,
				  uint32_t *n_out,
				  uint32_t max_out)
{
	uint32_t i;
	for (i = 0; i < b->n && *n_out < max_out; i++) {
		if (rule_matches(b->rules[i], el, ancs, n_ancs, bloom))
			out[(*n_out)++] = b->rules[i];
	}
}

uint32_t css_match(const struct css_selector_index *idx,
		    const struct dom_node *el,
		    const struct dom_node *const *ancestors,
		    uint32_t n_ancestors,
		    const struct css_bloom *bloom,
		    const struct css_rule **out,
		    uint32_t max_out)
{
	uint32_t n = 0;

	if (!el || el->type != DOM_ELEMENT || !idx)
		return 0;

	/* Universal bucket */
	collect_from_bucket(&idx->universal, el, ancestors, n_ancestors,
			     bloom, out, &n, max_out);

	/* Tag bucket */
	{
		uint32_t slot = fnv1a_str(el->el.tag) & (CSS_TAG_BUCKETS - 1);
		collect_from_bucket(&idx->by_tag[slot], el, ancestors,
				     n_ancestors, bloom, out, &n, max_out);
	}

	/* Class buckets */
	{
		const char *cls = dom_attr(el, "class");
		if (cls) {
			const char *p = cls;
			while (*p) {
				while (*p == ' ') p++;
				const char *tok = p;
				while (*p && *p != ' ') p++;
				size_t tok_len = (size_t)(p - tok);
				if (tok_len > 0) {
					uint32_t slot = fnv1a(tok, tok_len) &
						        (CSS_CLASS_BUCKETS - 1);
					collect_from_bucket(&idx->by_class[slot],
							     el, ancestors,
							     n_ancestors, bloom,
							     out, &n, max_out);
				}
			}
		}
	}

	/* ID bucket */
	{
		const char *id = dom_attr(el, "id");
		if (id) {
			uint32_t slot = fnv1a_str(id) & (CSS_ID_BUCKETS - 1);
			collect_from_bucket(&idx->by_id[slot], el, ancestors,
					     n_ancestors, bloom, out, &n,
					     max_out);
		}
	}

	sort_rules(out, n);
	return n;
}
