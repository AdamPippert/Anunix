/*
 * forms.c — HTML form element state and editing operations.
 */

#include "forms.h"
#include <anx/string.h>
#include <anx/alloc.h>

/* ── UTF-8 helpers ──────────────────────────────────────────────── */

/* Length in bytes of the UTF-8 character starting at p. */
static uint32_t utf8_char_len(const char *p)
{
	uint8_t c = (uint8_t)*p;
	if (c == 0)               return 0;
	if ((c & 0x80) == 0)      return 1;
	if ((c & 0xE0) == 0xC0)   return 2;
	if ((c & 0xF0) == 0xE0)   return 3;
	return 4;
}

/* Move byte offset one UTF-8 character forward in s (NUL-terminated). */
static uint32_t utf8_next(const char *s, uint32_t pos)
{
	uint32_t len = utf8_char_len(s + pos);
	return len > 0 ? pos + len : pos;
}

/* Move byte offset one UTF-8 character backward in s. */
static uint32_t utf8_prev(const char *s, uint32_t pos)
{
	if (pos == 0) return 0;
	uint32_t p = pos - 1;
	while (p > 0 && ((uint8_t)s[p] & 0xC0) == 0x80)
		p--;
	return p;
}

/* ── Lifecycle ──────────────────────────────────────────────────── */

void form_state_reset(struct form_state *fs)
{
	anx_memset(fs, 0, sizeof(*fs));
	fs->focused_idx = -1;
}

int32_t form_field_register(struct form_state *fs,
			      uint8_t type,
			      const char *name,
			      const char *value,
			      const char *placeholder,
			      const char *label,
			      bool checked,
			      int32_t bbox_x, int32_t bbox_y,
			      uint32_t bbox_w, uint32_t bbox_h)
{
	if (fs->n_fields >= FORM_FIELDS_MAX)
		return -1;

	uint32_t idx      = fs->n_fields++;
	struct form_field *f = &fs->fields[idx];
	anx_memset(f, 0, sizeof(*f));

	f->type    = type;
	f->active  = true;
	f->checked = checked;

	if (name)        anx_strlcpy(f->name,        name,        FORM_NAME_MAX);
	if (placeholder) anx_strlcpy(f->placeholder, placeholder, FORM_NAME_MAX);
	if (label)       anx_strlcpy(f->label,       label,       FORM_VALUE_MAX);
	if (value) {
		anx_strlcpy(f->value, value, FORM_VALUE_MAX);
		f->cursor_pos = (uint32_t)anx_strlen(f->value);
		f->sel_anchor = f->cursor_pos;
	}

	f->bbox_x = bbox_x; f->bbox_y = bbox_y;
	f->bbox_w = bbox_w; f->bbox_h = bbox_h;

	return (int32_t)idx;
}

/* ── Focus ──────────────────────────────────────────────────────── */

static void set_focus(struct form_state *fs, int32_t idx)
{
	if (fs->focused_idx >= 0 &&
	    fs->focused_idx < (int32_t)fs->n_fields)
		fs->fields[fs->focused_idx].focused = false;

	fs->focused_idx = idx;

	if (idx >= 0 && idx < (int32_t)fs->n_fields)
		fs->fields[idx].focused = true;
}

/* ── Interaction ────────────────────────────────────────────────── */

int32_t form_click(struct form_state *fs, int32_t x, int32_t y)
{
	uint32_t i;
	for (i = 0; i < fs->n_fields; i++) {
		struct form_field *f = &fs->fields[i];
		if (!f->active || f->type == FORM_TYPE_HIDDEN)
			continue;
		if (x >= f->bbox_x && x < f->bbox_x + (int32_t)f->bbox_w &&
		    y >= f->bbox_y && y < f->bbox_y + (int32_t)f->bbox_h) {
			set_focus(fs, (int32_t)i);
			/* Toggle checkbox/radio */
			if (f->type == FORM_TYPE_CHECKBOX)
				f->checked = !f->checked;
			else if (f->type == FORM_TYPE_RADIO)
				f->checked = true;
			return (int32_t)i;
		}
	}
	set_focus(fs, -1);
	return -1;
}

bool form_key(struct form_state *fs, const char *key)
{
	if (fs->focused_idx < 0)
		return false;
	struct form_field *f = &fs->fields[fs->focused_idx];
	if (!f->active)
		return false;

	/* Non-text fields only handle Tab/Enter */
	if (f->type == FORM_TYPE_SUBMIT || f->type == FORM_TYPE_RESET ||
	    f->type == FORM_TYPE_CHECKBOX || f->type == FORM_TYPE_RADIO ||
	    f->type == FORM_TYPE_SELECT) {
		if (anx_strcmp(key, "Tab") == 0) {
			form_focus_next(fs, false);
			return true;
		}
		return false;
	}

	if (f->type != FORM_TYPE_TEXT   && f->type != FORM_TYPE_PASSWORD &&
	    f->type != FORM_TYPE_TEXTAREA)
		return false;

	size_t vlen = anx_strlen(f->value);

	/* ── Navigation ────────────────────────────────────────────── */
	if (anx_strcmp(key, "ArrowLeft") == 0) {
		f->cursor_pos = utf8_prev(f->value, f->cursor_pos);
		f->sel_anchor = f->cursor_pos;
		return true;
	}
	if (anx_strcmp(key, "ArrowRight") == 0) {
		f->cursor_pos = utf8_next(f->value, f->cursor_pos);
		if (f->cursor_pos > vlen) f->cursor_pos = (uint32_t)vlen;
		f->sel_anchor = f->cursor_pos;
		return true;
	}
	if (anx_strcmp(key, "Home") == 0) {
		f->cursor_pos = 0;
		f->sel_anchor = 0;
		return true;
	}
	if (anx_strcmp(key, "End") == 0) {
		f->cursor_pos = (uint32_t)vlen;
		f->sel_anchor = (uint32_t)vlen;
		return true;
	}
	if (anx_strcmp(key, "Tab") == 0) {
		form_focus_next(fs, false);
		return true;
	}

	/* ── Deletion ───────────────────────────────────────────────── */
	if (anx_strcmp(key, "Backspace") == 0) {
		if (f->cursor_pos == 0) return true;
		uint32_t prev = utf8_prev(f->value, f->cursor_pos);
		/* Shift tail left */
		anx_memmove(f->value + prev,
			     f->value + f->cursor_pos,
			     vlen - f->cursor_pos + 1);
		f->cursor_pos = prev;
		f->sel_anchor = prev;
		return true;
	}
	if (anx_strcmp(key, "Delete") == 0) {
		if (f->cursor_pos >= (uint32_t)vlen) return true;
		uint32_t next = utf8_next(f->value, f->cursor_pos);
		anx_memmove(f->value + f->cursor_pos,
			     f->value + next,
			     vlen - next + 1);
		return true;
	}

	/* ── Printable character insertion ──────────────────────────── */
	/* Ignore control keys that aren't printable */
	if (anx_strcmp(key, "Enter") == 0) {
		if (f->type == FORM_TYPE_TEXTAREA) {
			/* Insert newline */
			key = "\n";
		} else {
			return false;  /* let caller handle submit */
		}
	}
	if (anx_strcmp(key, "Escape") == 0 ||
	    anx_strcmp(key, "Shift")  == 0 ||
	    anx_strcmp(key, "Control") == 0 ||
	    anx_strcmp(key, "Alt")    == 0 ||
	    anx_strcmp(key, "Meta")   == 0 ||
	    anx_strncmp(key, "F", 1) == 0)  /* function keys */
		return true;

	size_t key_len = anx_strlen(key);
	if (key_len == 0 || key_len > 4)   /* not a printable UTF-8 char */
		return true;

	/* Check multi-char special key names (longer than any printable char) */
	if (key_len > 4) return true;

	if (vlen + key_len >= FORM_VALUE_MAX)
		return true;  /* field full */

	/* Shift the tail right to make room */
	anx_memmove(f->value + f->cursor_pos + key_len,
		     f->value + f->cursor_pos,
		     vlen - f->cursor_pos + 1);
	anx_memcpy(f->value + f->cursor_pos, key, key_len);
	f->cursor_pos += (uint32_t)key_len;
	f->sel_anchor  = f->cursor_pos;
	return true;
}

void form_focus_next(struct form_state *fs, bool reverse)
{
	uint32_t count = fs->n_fields;
	if (count == 0) return;

	int32_t start = fs->focused_idx;
	int32_t step  = reverse ? -1 : 1;
	int32_t idx   = (start + step + (int32_t)count) % (int32_t)count;

	while (idx != start) {
		struct form_field *f = &fs->fields[idx];
		if (f->active && f->type != FORM_TYPE_HIDDEN) {
			set_focus(fs, idx);
			return;
		}
		idx = (idx + step + (int32_t)count) % (int32_t)count;
	}
}

/* ── URL encoding ───────────────────────────────────────────────── */

static const char HEX[] = "0123456789ABCDEF";

static size_t url_encode(const char *src, char *dst, size_t dst_cap)
{
	size_t out = 0;
	for (; *src && out + 4 < dst_cap; src++) {
		uint8_t c = (uint8_t)*src;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.' || c == '~') {
			dst[out++] = (char)c;
		} else if (c == ' ') {
			dst[out++] = '+';
		} else {
			dst[out++] = '%';
			dst[out++] = HEX[(c >> 4) & 0xF];
			dst[out++] = HEX[c & 0xF];
		}
	}
	if (out < dst_cap) dst[out] = '\0';
	return out;
}

size_t form_collect(const struct form_state *fs, char *buf, size_t cap)
{
	size_t   pos  = 0;
	bool     first = true;
	uint32_t i;

	for (i = 0; i < fs->n_fields; i++) {
		const struct form_field *f = &fs->fields[i];
		if (!f->active) continue;
		if (f->type == FORM_TYPE_SUBMIT   ||
		    f->type == FORM_TYPE_RESET    ||
		    f->type == FORM_TYPE_HIDDEN)   continue;
		if (f->name[0] == '\0') continue;
		if (f->type == FORM_TYPE_CHECKBOX && !f->checked) continue;
		if (f->type == FORM_TYPE_RADIO    && !f->checked) continue;

		if (!first && pos + 1 < cap) buf[pos++] = '&';
		first = false;

		/* Encode name */
		pos += url_encode(f->name, buf + pos, cap - pos);
		if (pos + 1 < cap) buf[pos++] = '=';
		/* Encode value */
		const char *val = (f->type == FORM_TYPE_CHECKBOX ||
		                   f->type == FORM_TYPE_RADIO)
		                  ? "on" : f->value;
		pos += url_encode(val, buf + pos, cap - pos);
	}
	if (pos < cap) buf[pos] = '\0';
	return pos;
}

bool form_submit_action(const struct form_state *fs __attribute__((unused)),
			  char *action_buf, size_t action_cap,
			  char *method_buf, size_t method_cap)
{
	/*
	 * In the current single-page model, form action comes from the DOM
	 * <form action="..." method="..."> element.  Without a DOM back-
	 * reference stored here, we return sensible defaults.  The browser
	 * driver augments this during dispatch.
	 */
	if (action_buf && action_cap > 0) action_buf[0] = '\0';
	if (method_buf && method_cap > 0) anx_strlcpy(method_buf, "GET", method_cap);
	return true;
}
