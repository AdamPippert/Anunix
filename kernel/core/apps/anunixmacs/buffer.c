/*
 * buffer.c — Gap buffer for anunixmacs (RFC-0023).
 */

#include <anx/anunixmacs.h>
#include <anx/types.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Internals                                                            */
/* ------------------------------------------------------------------ */

static int ensure_capacity(struct anx_ed_buffer *buf, uint32_t extra)
{
	uint32_t gap = buf->gap_end - buf->gap_start;
	uint32_t need;
	uint32_t newsz;
	char    *nd;

	if (gap >= extra)
		return ANX_OK;
	need = buf->size + extra - gap;
	newsz = buf->size ? buf->size : ANX_ED_BUF_INITIAL;
	while (newsz < need) {
		if (newsz > ANX_ED_BUF_MAX / 2)
			return ANX_ENOMEM;
		newsz *= 2;
	}
	if (newsz > ANX_ED_BUF_MAX)
		return ANX_ENOMEM;
	nd = (char *)anx_alloc(newsz);
	if (!nd)
		return ANX_ENOMEM;

	/* Copy [0, gap_start) and [gap_end, size) into the new buffer
	 * such that the gap grows in place. */
	uint32_t tail_len = buf->size - buf->gap_end;
	uint32_t new_gap_end = newsz - tail_len;
	if (buf->gap_start)
		anx_memcpy(nd, buf->data, buf->gap_start);
	if (tail_len)
		anx_memcpy(nd + new_gap_end, buf->data + buf->gap_end, tail_len);
	if (buf->data)
		anx_free(buf->data);
	buf->data    = nd;
	buf->size    = newsz;
	buf->gap_end = new_gap_end;
	return ANX_OK;
}

static void move_gap_to(struct anx_ed_buffer *buf, uint32_t pos)
{
	if (pos == buf->gap_start)
		return;
	uint32_t gap_size = buf->gap_end - buf->gap_start;
	if (pos < buf->gap_start) {
		uint32_t move = buf->gap_start - pos;
		anx_memmove(buf->data + buf->gap_end - move,
			    buf->data + pos, move);
		buf->gap_start -= move;
		buf->gap_end   -= move;
	} else {
		uint32_t move = pos - buf->gap_start;
		anx_memmove(buf->data + buf->gap_start,
			    buf->data + buf->gap_end, move);
		buf->gap_start += move;
		buf->gap_end   += move;
	}
	(void)gap_size;
}

/* Convert a logical 0-indexed position to the underlying offset. */
static uint32_t log_to_off(const struct anx_ed_buffer *buf, uint32_t pos)
{
	if (pos <= buf->gap_start)
		return pos;
	return pos + (buf->gap_end - buf->gap_start);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int anx_ed_buf_create(struct anx_ed_buffer **out)
{
	struct anx_ed_buffer *buf;

	if (!out)
		return ANX_EINVAL;
	buf = (struct anx_ed_buffer *)anx_zalloc(sizeof(*buf));
	if (!buf)
		return ANX_ENOMEM;
	buf->data = (char *)anx_alloc(ANX_ED_BUF_INITIAL);
	if (!buf->data) {
		anx_free(buf);
		return ANX_ENOMEM;
	}
	buf->size      = ANX_ED_BUF_INITIAL;
	buf->gap_start = 0;
	buf->gap_end   = ANX_ED_BUF_INITIAL;
	buf->point     = 0;
	*out = buf;
	return ANX_OK;
}

int anx_ed_buf_create_from_bytes(const char *bytes, uint32_t len,
				 struct anx_ed_buffer **out)
{
	struct anx_ed_buffer *buf;
	int rc = anx_ed_buf_create(&buf);
	if (rc != ANX_OK) return rc;
	if (len > 0) {
		rc = anx_ed_buf_insert(buf, bytes, len);
		if (rc != ANX_OK) {
			anx_ed_buf_free(buf);
			return rc;
		}
	}
	buf->point = 0;
	*out = buf;
	return ANX_OK;
}

void anx_ed_buf_free(struct anx_ed_buffer *buf)
{
	if (!buf) return;
	if (buf->data) anx_free(buf->data);
	anx_free(buf);
}

uint32_t anx_ed_buf_length(const struct anx_ed_buffer *buf)
{
	if (!buf) return 0;
	return buf->size - (buf->gap_end - buf->gap_start);
}

int anx_ed_buf_goto(struct anx_ed_buffer *buf, uint32_t pos)
{
	uint32_t len;
	if (!buf) return ANX_EINVAL;
	len = anx_ed_buf_length(buf);
	if (pos > len) pos = len;
	buf->point = pos;
	return ANX_OK;
}

int anx_ed_buf_insert(struct anx_ed_buffer *buf, const char *s, uint32_t n)
{
	int rc;
	if (!buf || !s) return ANX_EINVAL;
	if (n == 0) return ANX_OK;
	rc = ensure_capacity(buf, n);
	if (rc != ANX_OK) return rc;
	move_gap_to(buf, buf->point);
	anx_memcpy(buf->data + buf->gap_start, s, n);
	buf->gap_start += n;
	buf->point     += n;
	buf->dirty     = true;
	return ANX_OK;
}

int anx_ed_buf_delete(struct anx_ed_buffer *buf, uint32_t n)
{
	uint32_t len;
	if (!buf) return ANX_EINVAL;
	if (n == 0) return ANX_OK;
	len = anx_ed_buf_length(buf);
	if (buf->point + n > len) n = len - buf->point;
	move_gap_to(buf, buf->point);
	buf->gap_end += n;
	buf->dirty   = true;
	return ANX_OK;
}

int anx_ed_buf_text(const struct anx_ed_buffer *buf, char *out,
		    uint32_t out_size, uint32_t *written)
{
	uint32_t len, copy;
	if (!buf || !out) return ANX_EINVAL;
	len = anx_ed_buf_length(buf);
	copy = (len < out_size - 1) ? len : (out_size - 1);

	/* Two segments: [0, gap_start) then [gap_end, size). */
	if (copy <= buf->gap_start) {
		anx_memcpy(out, buf->data, copy);
	} else {
		uint32_t first = buf->gap_start;
		uint32_t rest  = copy - first;
		anx_memcpy(out, buf->data, first);
		anx_memcpy(out + first, buf->data + buf->gap_end, rest);
	}
	out[copy] = '\0';
	if (written) *written = copy;
	return ANX_OK;
}

int anx_ed_buf_search(const struct anx_ed_buffer *buf, const char *needle,
		      uint32_t *match_pos)
{
	uint32_t nl, blen, i, j;
	if (!buf || !needle || !match_pos) return ANX_EINVAL;
	nl = (uint32_t)anx_strlen(needle);
	if (nl == 0) return ANX_EINVAL;
	blen = anx_ed_buf_length(buf);
	if (nl > blen) return ANX_ENOENT;

	/* Naive byte search across the gap-aware logical view. */
	for (i = 0; i + nl <= blen; i++) {
		bool match = true;
		for (j = 0; j < nl; j++) {
			uint32_t off = log_to_off(buf, i + j);
			if (buf->data[off] != needle[j]) {
				match = false;
				break;
			}
		}
		if (match) {
			*match_pos = i;
			return ANX_OK;
		}
	}
	return ANX_ENOENT;
}

int anx_ed_buf_replace_all(struct anx_ed_buffer *buf, const char *needle,
			   const char *replacement, uint32_t *count)
{
	uint32_t pos = 0;
	uint32_t nl = needle ? (uint32_t)anx_strlen(needle) : 0;
	uint32_t rl = replacement ? (uint32_t)anx_strlen(replacement) : 0;
	uint32_t replaced = 0;
	int      rc;

	if (!buf || !needle || nl == 0) return ANX_EINVAL;
	while (1) {
		uint32_t found;
		anx_ed_buf_goto(buf, pos);
		rc = anx_ed_buf_search(buf, needle, &found);
		if (rc != ANX_OK) break;
		anx_ed_buf_goto(buf, found);
		anx_ed_buf_delete(buf, nl);
		if (rl) anx_ed_buf_insert(buf, replacement, rl);
		pos = found + rl;
		replaced++;
	}
	if (count) *count = replaced;
	return ANX_OK;
}
