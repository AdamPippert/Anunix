/*
 * css_selector.h — Rule index for fast selector matching.
 *
 * Rules are indexed by the tag/class/id of their rightmost simple
 * selector component.  Lookup is O(1) average via FNV-1a hash buckets.
 *
 * A 4096-bit Bloom filter tracks which tags and classes appear in the
 * ancestor chain, allowing fast rejection of rules whose ancestor
 * requirements cannot possibly be satisfied.
 */

#ifndef ANX_BROWSER_CSS_SELECTOR_H
#define ANX_BROWSER_CSS_SELECTOR_H

#include <anx/types.h>
#include "css_parser.h"
#include "../html/dom.h"

/* ── Bloom filter ─────────────────────────────────────────────────── */

#define CSS_BLOOM_WORDS 128   /* 128 × 32 = 4096 bits */

struct css_bloom {
	uint32_t bits[CSS_BLOOM_WORDS];
};

void css_bloom_clear(struct css_bloom *b);
/* Add a string (tag name or class name) to the filter. */
void css_bloom_add(struct css_bloom *b, const char *str);
/* Returns true if the string MAY be in the set (false ⇒ definitely absent). */
bool css_bloom_test(const struct css_bloom *b, const char *str);

/* ── Hash-bucket index ────────────────────────────────────────────── */

#define CSS_TAG_BUCKETS   128
#define CSS_CLASS_BUCKETS 256
#define CSS_ID_BUCKETS    256
#define CSS_BUCKET_MAX     64   /* rules per bucket (extras go to universal) */

struct css_bucket {
	const struct css_rule *rules[CSS_BUCKET_MAX];
	uint32_t               n;
};

struct css_selector_index {
	struct css_bucket by_tag  [CSS_TAG_BUCKETS];
	struct css_bucket by_class[CSS_CLASS_BUCKETS];
	struct css_bucket by_id   [CSS_ID_BUCKETS];
	struct css_bucket universal;   /* tag='*' or overflow */
};

/* Build the index from a parsed stylesheet. */
void css_index_clear(struct css_selector_index *idx);
void css_index_build(struct css_selector_index *idx,
		      const struct css_sheet *sheet);

/*
 * Find all rules matching el (with ancestor context).
 *
 * ancestors[0] is the immediate parent, ancestors[n_ancestors-1] is the root.
 * bloom is built from the ancestor chain by the caller.
 *
 * Writes matching rule pointers into out (max_out slots), sorted ascending
 * by (origin, specificity) so the last writer wins in a simple loop.
 * Returns the count written.
 */
uint32_t css_match(const struct css_selector_index *idx,
		    const struct dom_node *el,
		    const struct dom_node *const *ancestors,
		    uint32_t n_ancestors,
		    const struct css_bloom *bloom,
		    const struct css_rule **out,
		    uint32_t max_out);

#endif /* ANX_BROWSER_CSS_SELECTOR_H */
