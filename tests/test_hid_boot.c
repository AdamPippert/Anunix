/*
 * test_hid_boot.c — HID boot-protocol report translation.
 *
 * A boot keyboard report says which keys are held, not which changed.
 * These tests assert that the translation emits exactly one edge per
 * change, in a stable order, and that a rollover report changes nothing.
 * A missed release leaves a key stuck down, which is worse than a missed
 * press.
 */

#include <anx/types.h>
#include <anx/hid_boot.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			kprintf("  FAIL: %s\n", (msg));			\
			return -1;					\
		}							\
	} while (0)

#define MAX_EDGES	32

struct edge_log {
	uint32_t n;
	struct {
		enum anx_hid_key_edge edge;
		uint32_t usage;
		uint32_t unicode;
	} e[MAX_EDGES];
};

static void record(enum anx_hid_key_edge edge, uint32_t usage,
		   uint32_t unicode, void *arg)
{
	struct edge_log *log = arg;

	if (log->n < MAX_EDGES) {
		log->e[log->n].edge    = edge;
		log->e[log->n].usage   = usage;
		log->e[log->n].unicode = unicode;
	}
	log->n++;
}

static int feed(struct anx_hid_kbd_state *st, struct edge_log *log,
		const uint8_t *report)
{
	anx_memset(log, 0, sizeof(*log));
	return anx_hid_kbd_report(st, report, ANX_HID_BOOT_KBD_LEN, record, log);
}

static int press_and_release(void)
{
	static const uint8_t down[8] = { 0, 0, 0x04 };
	static const uint8_t up[8]   = { 0 };
	struct anx_hid_kbd_state st;
	struct edge_log log;

	anx_hid_kbd_reset(&st);

	CHECK(feed(&st, &log, down) == 1, "one edge for a press");
	CHECK(log.e[0].edge == ANX_HID_KEY_DOWN, "press is a down edge");
	CHECK(log.e[0].usage == 0x04 && log.e[0].unicode == 'a',
	      "usage 0x04 is 'a'");

	CHECK(feed(&st, &log, down) == 0, "a repeated report emits nothing");

	CHECK(feed(&st, &log, up) == 1, "one edge for a release");
	CHECK(log.e[0].edge == ANX_HID_KEY_UP && log.e[0].usage == 0x04,
	      "release names the key");
	CHECK(log.e[0].unicode == 0, "a release carries no character");
	return 0;
}

static int shift_orders_modifier_first(void)
{
	static const uint8_t shifted[8] = { 0x02, 0, 0x04 };
	static const uint8_t none[8]    = { 0 };
	struct anx_hid_kbd_state st;
	struct edge_log log;

	anx_hid_kbd_reset(&st);

	CHECK(feed(&st, &log, shifted) == 2, "shift and key are two edges");
	CHECK(log.e[0].usage == 0xE1 && log.e[0].edge == ANX_HID_KEY_DOWN,
	      "LeftShift goes down before the key");
	CHECK(log.e[1].usage == 0x04 && log.e[1].unicode == 'A',
	      "the key sees shift and reads 'A'");

	CHECK(feed(&st, &log, none) == 2, "both release together");
	CHECK(log.e[0].usage == 0xE1 && log.e[1].usage == 0x04,
	      "the modifier releases before the key");
	return 0;
}

static int rollover_keeps_state(void)
{
	static const uint8_t held[8]     = { 0, 0, 0x04, 0x05 };
	static const uint8_t rollover[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };
	static const uint8_t none[8]     = { 0 };
	struct anx_hid_kbd_state st;
	struct edge_log log;

	anx_hid_kbd_reset(&st);

	CHECK(feed(&st, &log, held) == 2, "two keys down");
	CHECK(feed(&st, &log, rollover) == 0,
	      "a rollover report changes nothing");
	CHECK(feed(&st, &log, none) == 2,
	      "the keys held before rollover still release");
	return 0;
}

static int chord_change(void)
{
	static const uint8_t ab[8] = { 0, 0, 0x04, 0x05 };
	static const uint8_t bc[8] = { 0, 0, 0x06, 0x05 };
	struct anx_hid_kbd_state st;
	struct edge_log log;

	anx_hid_kbd_reset(&st);
	feed(&st, &log, ab);

	CHECK(feed(&st, &log, bc) == 2,
	      "moving slots is not a change; only a and c differ");
	CHECK(log.e[0].edge == ANX_HID_KEY_UP && log.e[0].usage == 0x04,
	      "a releases first");
	CHECK(log.e[1].edge == ANX_HID_KEY_DOWN && log.e[1].usage == 0x06,
	      "then c goes down");
	return 0;
}

static int character_table(void)
{
	CHECK(anx_hid_usage_to_unicode(0x28, false) == '\n', "Enter");
	CHECK(anx_hid_usage_to_unicode(0x2A, false) == '\b', "Backspace");
	CHECK(anx_hid_usage_to_unicode(0x2B, false) == '\t', "Tab");
	CHECK(anx_hid_usage_to_unicode(0x29, false) == 0x1B, "Escape");
	CHECK(anx_hid_usage_to_unicode(0x1E, true) == '!', "shift 1");
	CHECK(anx_hid_usage_to_unicode(0x38, true) == '?', "shift slash");
	CHECK(anx_hid_usage_to_unicode(0x32, false) == 0, "non-US # has none");
	CHECK(anx_hid_usage_to_unicode(0x4F, false) == 0, "arrows have none");
	CHECK(anx_hid_usage_to_unicode(0xE1, false) == 0, "modifiers have none");
	return 0;
}

static int malformed_reports(void)
{
	static const uint8_t two[2] = { 0, 0 };
	struct anx_hid_kbd_state st;
	struct edge_log log;
	struct anx_hid_mouse_delta m;

	anx_hid_kbd_reset(&st);
	anx_memset(&log, 0, sizeof(log));

	CHECK(anx_hid_kbd_report(&st, two, 2, record, &log) == ANX_EINVAL,
	      "a keyboard report needs a key byte");
	CHECK(anx_hid_mouse_report(two, 2, &m) == ANX_EINVAL,
	      "a mouse report needs X and Y");
	return 0;
}

static int mouse_report(void)
{
	static const uint8_t three[3] = { 0x09, 0xFE, 0x05 };
	static const uint8_t four[4]  = { 0x02, 0x01, 0x80, 0xFF };
	struct anx_hid_mouse_delta m;

	CHECK(anx_hid_mouse_report(three, 3, &m) == ANX_OK, "3-byte report");
	CHECK(m.buttons == 0x01, "padding bits above the buttons are dropped");
	CHECK(m.dx == -2 && m.dy == 5 && m.wheel == 0, "signed deltas");

	CHECK(anx_hid_mouse_report(four, 4, &m) == ANX_OK, "4-byte report");
	CHECK(m.buttons == 0x02 && m.dy == -128 && m.wheel == -1,
	      "wheel is the fourth byte");
	return 0;
}

int test_hid_boot(void)
{
	if (press_and_release())
		return -1;
	if (shift_orders_modifier_first())
		return -1;
	if (rollover_keeps_state())
		return -1;
	if (chord_change())
		return -1;
	if (character_table())
		return -1;
	if (malformed_reports())
		return -1;
	if (mouse_report())
		return -1;
	return 0;
}
