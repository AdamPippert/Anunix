/*
 * hid_boot.c — HID boot-protocol keyboard and mouse report translation.
 *
 * A boot keyboard report carries a modifier bitmap and up to six pressed
 * key usages. It states what is held now, not what changed, so this layer
 * keeps the previous report and emits an edge for every difference.
 *
 * Characters follow the US layout the PS/2 path uses (arch_init.c), so a
 * USB keyboard and a PS/2 keyboard produce the same events.
 */

#include <anx/hid_boot.h>
#include <anx/input.h>
#include <anx/string.h>

#define KEY_FIRST		2	/* report bytes 2..7 hold key usages */
#define USAGE_TABLE_BASE	0x04	/* Keyboard a and A */
#define USAGE_TABLE_LEN		0x35	/* through 0x38, Keyboard / and ? */
#define MOD_SHIFT_BITS		0x22	/* LeftShift (bit 1) | RightShift (bit 5) */

/* Usages 0x01..0x03 report rollover or an error, not keys. */
#define USAGE_ERROR_LAST	0x03

static const char usage_plain[USAGE_TABLE_LEN] = {
	'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
	'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
	'1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
	'\n', 0x1B, '\b', '\t', ' ',
	'-', '=', '[', ']', '\\',
	0,				/* 0x32 non-US # */
	';', '\'', '`', ',', '.', '/',
};

static const char usage_shift[USAGE_TABLE_LEN] = {
	'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
	'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
	'!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
	'\n', 0x1B, '\b', '\t', ' ',
	'_', '+', '{', '}', '|',
	0,				/* 0x32 non-US # */
	':', '"', '~', '<', '>', '?',
};

void anx_hid_kbd_reset(struct anx_hid_kbd_state *st)
{
	if (st)
		anx_memset(st->prev, 0, sizeof(st->prev));
}

uint32_t anx_hid_usage_to_unicode(uint32_t usage, bool shift)
{
	if (usage < USAGE_TABLE_BASE ||
	    usage >= USAGE_TABLE_BASE + USAGE_TABLE_LEN)
		return 0;
	return (uint32_t)(uint8_t)(shift ? usage_shift : usage_plain)
		[usage - USAGE_TABLE_BASE];
}

static bool report_holds(const uint8_t *r, uint8_t usage)
{
	uint32_t i;

	for (i = KEY_FIRST; i < ANX_HID_BOOT_KBD_LEN; i++)
		if (r[i] == usage)
			return true;
	return false;
}

int anx_hid_kbd_report(struct anx_hid_kbd_state *st, const uint8_t *report,
		       uint32_t len, anx_hid_key_sink sink, void *arg)
{
	uint8_t cur[ANX_HID_BOOT_KBD_LEN];
	uint32_t i;
	int edges = 0;
	bool shift;

	if (!st || !report || !sink || len < KEY_FIRST + 1)
		return ANX_EINVAL;

	anx_memset(cur, 0, sizeof(cur));
	anx_memcpy(cur, report,
		   len < ANX_HID_BOOT_KBD_LEN ? len : ANX_HID_BOOT_KBD_LEN);

	/*
	 * Too many keys held: the keyboard reports an error usage in every
	 * slot and no longer says which keys are down. Keep the last known
	 * state rather than releasing everything.
	 */
	for (i = KEY_FIRST; i < ANX_HID_BOOT_KBD_LEN; i++)
		if (cur[i] != 0 && cur[i] <= USAGE_ERROR_LAST)
			return 0;

	/* Releases first, so a key that moves between chords stays ordered. */
	for (i = 0; i < 8; i++) {
		uint8_t bit = (uint8_t)(1u << i);

		if ((st->prev[0] & bit) && !(cur[0] & bit)) {
			sink(ANX_HID_KEY_UP, ANX_HID_USAGE_MOD_BASE + i, 0, arg);
			edges++;
		}
	}
	for (i = KEY_FIRST; i < ANX_HID_BOOT_KBD_LEN; i++) {
		if (st->prev[i] == 0 || report_holds(cur, st->prev[i]))
			continue;
		sink(ANX_HID_KEY_UP, st->prev[i], 0, arg);
		edges++;
	}

	for (i = 0; i < 8; i++) {
		uint8_t bit = (uint8_t)(1u << i);

		if (!(st->prev[0] & bit) && (cur[0] & bit)) {
			sink(ANX_HID_KEY_DOWN, ANX_HID_USAGE_MOD_BASE + i, 0, arg);
			edges++;
		}
	}
	shift = (cur[0] & MOD_SHIFT_BITS) != 0;
	for (i = KEY_FIRST; i < ANX_HID_BOOT_KBD_LEN; i++) {
		if (cur[i] == 0 || report_holds(st->prev, cur[i]))
			continue;
		sink(ANX_HID_KEY_DOWN, cur[i],
		     anx_hid_usage_to_unicode(cur[i], shift), arg);
		edges++;
	}

	anx_memcpy(st->prev, cur, sizeof(cur));
	return edges;
}

void anx_hid_key_to_input(enum anx_hid_key_edge edge, uint32_t usage,
			  uint32_t unicode, void *arg)
{
	(void)arg;

	if (edge == ANX_HID_KEY_DOWN)
		anx_input_key_down(usage, anx_input_get_modifiers(), unicode);
	else
		anx_input_key_up(usage, anx_input_get_modifiers());
}

int anx_hid_mouse_report(const uint8_t *report, uint32_t len,
			 struct anx_hid_mouse_delta *out)
{
	if (!report || !out || len < 3)
		return ANX_EINVAL;

	out->buttons = report[0] & 0x07;
	out->dx      = (int8_t)report[1];
	out->dy      = (int8_t)report[2];
	out->wheel   = len >= 4 ? (int8_t)report[3] : 0;
	return ANX_OK;
}
