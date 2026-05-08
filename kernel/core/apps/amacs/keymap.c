/*
 * keymap.c — Global keymap for amacs (RFC-0023, Phase 2.7).
 *
 * The keymap is a flat array of (mods, key, fn-symbol-name) triples.
 * The editor's dispatch consults it before falling through to the
 * built-in C-/M-/arrow-key chords, so user code can re-bind any chord
 * via (define-key 'global "C-c f" 'fn).  Multi-key prefixes and
 * mode-local maps are not yet supported.
 */

#include <anx/amacs.h>
#include <anx/types.h>
#include <anx/string.h>
#include <anx/input.h>

struct key_binding {
	uint32_t mods;
	uint32_t key;
	char     fn_name[ANX_ED_KEY_FN_MAX];
	bool     in_use;
};

static struct key_binding g_keymap[ANX_ED_MAX_KEYBINDINGS];

int anx_ed_keymap_define(uint32_t mods, uint32_t key, const char *fn_name)
{
	uint32_t i, slot = ANX_ED_MAX_KEYBINDINGS;
	if (!fn_name || !*fn_name) return ANX_EINVAL;
	for (i = 0; i < ANX_ED_MAX_KEYBINDINGS; i++) {
		if (g_keymap[i].in_use &&
		    g_keymap[i].mods == mods && g_keymap[i].key == key) {
			slot = i;
			break;
		}
		if (!g_keymap[i].in_use && slot == ANX_ED_MAX_KEYBINDINGS)
			slot = i;
	}
	if (slot == ANX_ED_MAX_KEYBINDINGS) return ANX_ENOMEM;
	g_keymap[slot].mods = mods;
	g_keymap[slot].key  = key;
	g_keymap[slot].in_use = true;
	anx_strlcpy(g_keymap[slot].fn_name, fn_name,
		    sizeof(g_keymap[slot].fn_name));
	return ANX_OK;
}

const char *anx_ed_keymap_lookup(uint32_t mods, uint32_t key)
{
	uint32_t i;
	for (i = 0; i < ANX_ED_MAX_KEYBINDINGS; i++) {
		if (g_keymap[i].in_use &&
		    g_keymap[i].mods == mods && g_keymap[i].key == key)
			return g_keymap[i].fn_name;
	}
	return NULL;
}

/* Map an ASCII letter to its ANX_KEY_* code.  HID layout is contiguous
 * starting at 0x04 for 'a'.  Returns 0 for non-letters. */
static uint32_t letter_to_key(char c)
{
	if (c >= 'a' && c <= 'z') return ANX_KEY_A + (uint32_t)(c - 'a');
	if (c >= 'A' && c <= 'Z') return ANX_KEY_A + (uint32_t)(c - 'A');
	return 0;
}

static int parse_named_key(const char *name, uint32_t *out)
{
	if (anx_strcmp(name, "RET") == 0) { *out = ANX_KEY_ENTER; return 0; }
	if (anx_strcmp(name, "TAB") == 0) { *out = ANX_KEY_TAB;   return 0; }
	if (anx_strcmp(name, "ESC") == 0) { *out = ANX_KEY_ESC;   return 0; }
	if (anx_strcmp(name, "SPC") == 0) { *out = ANX_KEY_SPACE; return 0; }
	if (anx_strcmp(name, "DEL") == 0) { *out = ANX_KEY_BACKSPACE; return 0; }
	if (anx_strcmp(name, "BSP") == 0) { *out = ANX_KEY_BACKSPACE; return 0; }
	return -1;
}

int anx_ed_keymap_parse(const char *desc, uint32_t *mods_out, uint32_t *key_out)
{
	uint32_t mods = 0;
	const char *p;
	if (!desc || !mods_out || !key_out) return ANX_EINVAL;
	p = desc;
	/* Mod prefixes: "C-", "M-", "S-" */
	while (p[0] && p[1] == '-') {
		switch (p[0]) {
		case 'C': mods |= ANX_MOD_CTRL;  break;
		case 'M': mods |= ANX_MOD_ALT;   break;
		case 'S': mods |= ANX_MOD_SHIFT; break;
		default:  return ANX_EINVAL;
		}
		p += 2;
	}
	/* Remainder: a single ASCII letter or a named key. */
	if (p[0] == '\0') return ANX_EINVAL;
	if (p[1] == '\0') {
		uint32_t k = letter_to_key(p[0]);
		if (k == 0) return ANX_EINVAL;
		*mods_out = mods;
		*key_out  = k;
		return ANX_OK;
	}
	{
		uint32_t k;
		if (parse_named_key(p, &k) != 0) return ANX_EINVAL;
		*mods_out = mods;
		*key_out  = k;
		return ANX_OK;
	}
}
