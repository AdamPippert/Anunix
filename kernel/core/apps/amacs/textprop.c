/*
 * textprop.c — Text property intervals for amacs (RFC-0023).
 *
 * Storage is a singly-linked list of non-overlapping intervals sorted by
 * start.  Each interval carries a list of (name, value) string pairs.
 *
 * put_property splits intervals at the [start, end) boundaries so the
 * range can be assigned uniformly, then writes (name, value) onto every
 * fully-contained interval, creating new intervals for any gap.
 *
 * After insert/delete, intervals are shifted/clipped so they continue to
 * track the same logical content.
 */

#include <anx/amacs.h>
#include <anx/types.h>
#include <anx/alloc.h>
#include <anx/string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static char *dup_str(const char *s)
{
	uint32_t n;
	char *out;
	if (!s) return NULL;
	n = (uint32_t)anx_strlen(s);
	out = (char *)anx_alloc(n + 1);
	if (!out) return NULL;
	if (n) anx_memcpy(out, s, n);
	out[n] = '\0';
	return out;
}

static void prop_free(struct anx_ed_textprop *p)
{
	if (!p) return;
	if (p->name)  anx_free(p->name);
	if (p->value) anx_free(p->value);
	anx_free(p);
}

static void interval_free(struct anx_ed_interval *iv)
{
	struct anx_ed_textprop *p;
	if (!iv) return;
	p = iv->props;
	while (p) {
		struct anx_ed_textprop *next = p->next;
		prop_free(p);
		p = next;
	}
	anx_free(iv);
}

static struct anx_ed_textprop *prop_find(struct anx_ed_interval *iv,
					 const char *name)
{
	struct anx_ed_textprop *p = iv->props;
	while (p) {
		if (anx_strcmp(p->name, name) == 0) return p;
		p = p->next;
	}
	return NULL;
}

static int prop_set(struct anx_ed_interval *iv,
		    const char *name, const char *value)
{
	struct anx_ed_textprop *p = prop_find(iv, name);
	char *new_val = dup_str(value);
	if (!new_val) return ANX_ENOMEM;
	if (p) {
		if (p->value) anx_free(p->value);
		p->value = new_val;
		return ANX_OK;
	}
	p = (struct anx_ed_textprop *)anx_zalloc(sizeof(*p));
	if (!p) { anx_free(new_val); return ANX_ENOMEM; }
	p->name  = dup_str(name);
	if (!p->name) { anx_free(new_val); anx_free(p); return ANX_ENOMEM; }
	p->value = new_val;
	p->next  = iv->props;
	iv->props = p;
	return ANX_OK;
}

static void prop_unset(struct anx_ed_interval *iv, const char *name)
{
	struct anx_ed_textprop **pp = &iv->props;
	while (*pp) {
		if (anx_strcmp((*pp)->name, name) == 0) {
			struct anx_ed_textprop *kill = *pp;
			*pp = kill->next;
			prop_free(kill);
			return;
		}
		pp = &(*pp)->next;
	}
}

/* Deep-copy an interval's property list.  Returns NULL on OOM. */
static struct anx_ed_textprop *props_dup(const struct anx_ed_textprop *src)
{
	struct anx_ed_textprop *head = NULL, **tail = &head;
	while (src) {
		struct anx_ed_textprop *p =
			(struct anx_ed_textprop *)anx_zalloc(sizeof(*p));
		if (!p) goto oom;
		p->name  = dup_str(src->name);
		p->value = dup_str(src->value);
		if (!p->name || !p->value) { prop_free(p); goto oom; }
		*tail = p;
		tail  = &p->next;
		src   = src->next;
	}
	return head;
oom: ;
	struct anx_ed_textprop *p = head;
	while (p) {
		struct anx_ed_textprop *next = p->next;
		prop_free(p);
		p = next;
	}
	return NULL;
}

/* Split the interval *pp at offset POS so [start, pos) and [pos, end)
 * are two intervals with identical property lists.  POS must satisfy
 * (*pp)->start < pos < (*pp)->end.  Returns the right-hand interval,
 * or NULL on OOM. */
static struct anx_ed_interval *split_at(struct anx_ed_interval **pp,
					uint32_t pos)
{
	struct anx_ed_interval *iv = *pp;
	struct anx_ed_interval *right;

	right = (struct anx_ed_interval *)anx_zalloc(sizeof(*right));
	if (!right) return NULL;
	right->start = pos;
	right->end   = iv->end;
	right->props = props_dup(iv->props);
	if (iv->props && !right->props) {
		anx_free(right);
		return NULL;
	}
	right->next  = iv->next;
	iv->end      = pos;
	iv->next     = right;
	return right;
}

/* ------------------------------------------------------------------ */
/* Adjustment after insert / delete                                    */
/* ------------------------------------------------------------------ */

void anx_ed_textprop_after_insert(struct anx_ed_buffer *buf,
				  uint32_t pos, uint32_t n);
void anx_ed_textprop_after_delete(struct anx_ed_buffer *buf,
				  uint32_t pos, uint32_t n);

void anx_ed_textprop_after_insert(struct anx_ed_buffer *buf,
				  uint32_t pos, uint32_t n)
{
	struct anx_ed_interval *iv;
	for (iv = buf->intervals; iv; iv = iv->next) {
		if (iv->start >= pos) {
			iv->start += n;
			iv->end   += n;
		} else if (iv->end > pos) {
			/* Insertion lands inside the interval — extend. */
			iv->end += n;
		}
	}
}

void anx_ed_textprop_after_delete(struct anx_ed_buffer *buf,
				  uint32_t pos, uint32_t n)
{
	struct anx_ed_interval **pp = &buf->intervals;
	uint32_t end = pos + n;
	while (*pp) {
		struct anx_ed_interval *iv = *pp;
		if (iv->end <= pos) {
			pp = &iv->next;
			continue;
		}
		if (iv->start >= end) {
			iv->start -= n;
			iv->end   -= n;
			pp = &iv->next;
			continue;
		}
		/* Overlap with [pos, end). */
		uint32_t new_start = (iv->start < pos) ? iv->start : pos;
		uint32_t new_end   = (iv->end > end)   ? iv->end - n : pos;
		if (new_end <= new_start) {
			*pp = iv->next;
			interval_free(iv);
			continue;
		}
		iv->start = new_start;
		iv->end   = new_end;
		pp = &iv->next;
	}
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void anx_ed_buf_clear_properties(struct anx_ed_buffer *buf)
{
	struct anx_ed_interval *iv;
	if (!buf) return;
	iv = buf->intervals;
	while (iv) {
		struct anx_ed_interval *next = iv->next;
		interval_free(iv);
		iv = next;
	}
	buf->intervals = NULL;
}

int anx_ed_buf_put_property(struct anx_ed_buffer *buf,
			    uint32_t start, uint32_t end,
			    const char *name, const char *value)
{
	struct anx_ed_interval **pp;
	uint32_t cursor;

	if (!buf || !name || !value || end <= start) return ANX_EINVAL;

	/* Step 1: split any interval that crosses [start, end) at the
	 * boundary so each interval is fully inside or fully outside. */
	pp = &buf->intervals;
	while (*pp) {
		struct anx_ed_interval *iv = *pp;
		if (iv->end <= start || iv->start >= end) {
			pp = &iv->next;
			continue;
		}
		if (iv->start < start && iv->end > start) {
			if (!split_at(pp, start)) return ANX_ENOMEM;
			/* Left half is fully outside; advance past it. */
			pp = &(*pp)->next;
			continue;
		}
		if (iv->start < end && iv->end > end) {
			if (!split_at(pp, end)) return ANX_ENOMEM;
		}
		pp = &(*pp)->next;
	}

	/* Step 2: walk again, setting NAME on intervals fully inside
	 * [start, end) and creating fresh intervals for any gap. */
	cursor = start;
	pp = &buf->intervals;
	while (*pp && (*pp)->end <= start) pp = &(*pp)->next;

	while (cursor < end) {
		if (*pp && (*pp)->start <= cursor && (*pp)->end <= end) {
			/* Existing interval inside range. */
			if (prop_set(*pp, name, value) != ANX_OK)
				return ANX_ENOMEM;
			cursor = (*pp)->end;
			pp = &(*pp)->next;
			continue;
		}
		/* Gap: from cursor to either next interval start, or end. */
		uint32_t gap_end = end;
		if (*pp && (*pp)->start < gap_end)
			gap_end = (*pp)->start;
		if (gap_end > cursor) {
			struct anx_ed_interval *iv =
				(struct anx_ed_interval *)anx_zalloc(sizeof(*iv));
			if (!iv) return ANX_ENOMEM;
			iv->start = cursor;
			iv->end   = gap_end;
			if (prop_set(iv, name, value) != ANX_OK) {
				interval_free(iv);
				return ANX_ENOMEM;
			}
			iv->next = *pp;
			*pp = iv;
			cursor = gap_end;
			pp = &iv->next;
		} else {
			/* Nothing left to do. */
			break;
		}
	}
	return ANX_OK;
}

const char *anx_ed_buf_get_property(const struct anx_ed_buffer *buf,
				    uint32_t pos, const char *name)
{
	struct anx_ed_interval *iv;
	if (!buf || !name) return NULL;
	for (iv = buf->intervals; iv; iv = iv->next) {
		if (iv->start > pos) break;
		if (iv->end <= pos) continue;
		struct anx_ed_textprop *p = prop_find(iv, name);
		if (p) return p->value;
	}
	return NULL;
}

int anx_ed_buf_remove_property(struct anx_ed_buffer *buf,
			       uint32_t start, uint32_t end,
			       const char *name)
{
	struct anx_ed_interval **pp;
	if (!buf || !name || end <= start) return ANX_EINVAL;

	/* Split at boundaries so we only mutate fully-inside intervals. */
	pp = &buf->intervals;
	while (*pp) {
		struct anx_ed_interval *iv = *pp;
		if (iv->end <= start || iv->start >= end) {
			pp = &iv->next;
			continue;
		}
		if (iv->start < start && iv->end > start) {
			if (!split_at(pp, start)) return ANX_ENOMEM;
			pp = &(*pp)->next;
			continue;
		}
		if (iv->start < end && iv->end > end) {
			if (!split_at(pp, end)) return ANX_ENOMEM;
		}
		pp = &(*pp)->next;
	}

	/* Now drop the property from intervals inside [start, end);
	 * remove intervals that end up empty. */
	pp = &buf->intervals;
	while (*pp) {
		struct anx_ed_interval *iv = *pp;
		if (iv->start >= start && iv->end <= end) {
			prop_unset(iv, name);
			if (!iv->props) {
				*pp = iv->next;
				interval_free(iv);
				continue;
			}
		}
		pp = &iv->next;
	}
	return ANX_OK;
}
