/*
 * anx/hid_boot.h — HID boot-protocol keyboard and mouse reports.
 *
 * The boot protocol fixes the report layout (USB HID 1.11, Appendix B), so
 * a keyboard or mouse works before a report descriptor parser exists. This
 * layer only translates: reports in, key edges and pointer deltas out. It
 * touches no hardware, so the USB and I2C transports share it and the host
 * test build covers it.
 */

#ifndef ANX_HID_BOOT_H
#define ANX_HID_BOOT_H

#include <anx/types.h>

#define ANX_HID_BOOT_KBD_LEN	8	/* modifiers, reserved, six key usages */
#define ANX_HID_USAGE_MOD_BASE	0xE0	/* bit n of the modifier byte is 0xE0 + n */

enum anx_hid_key_edge {
	ANX_HID_KEY_UP   = 0,
	ANX_HID_KEY_DOWN = 1,
};

/* Receives one key edge. unicode is 0 for releases and non-printing keys. */
typedef void (*anx_hid_key_sink)(enum anx_hid_key_edge edge, uint32_t usage,
				 uint32_t unicode, void *arg);

/* Last report seen from one keyboard. */
struct anx_hid_kbd_state {
	uint8_t prev[ANX_HID_BOOT_KBD_LEN];
};

/* One boot mouse report. */
struct anx_hid_mouse_delta {
	uint8_t buttons;	/* bit 0 left, bit 1 right, bit 2 middle */
	int8_t  dx;
	int8_t  dy;
	int8_t  wheel;
};

/* Forget all held keys. */
void anx_hid_kbd_reset(struct anx_hid_kbd_state *st);

/* US-layout character for a keyboard usage, or 0 when it has none. */
uint32_t anx_hid_usage_to_unicode(uint32_t usage, bool shift);

/*
 * Compare a keyboard report with the previous one and emit an edge per key
 * that changed: releases first, then presses, modifiers before keys.
 * Returns the number of edges, or ANX_EINVAL for a malformed call.
 */
int anx_hid_kbd_report(struct anx_hid_kbd_state *st, const uint8_t *report,
		       uint32_t len, anx_hid_key_sink sink, void *arg);

/* Key sink that posts edges to the input layer. */
void anx_hid_key_to_input(enum anx_hid_key_edge edge, uint32_t usage,
			  uint32_t unicode, void *arg);

/* Decode a boot mouse report. Returns ANX_OK or ANX_EINVAL. */
int anx_hid_mouse_report(const uint8_t *report, uint32_t len,
			 struct anx_hid_mouse_delta *out);

#endif /* ANX_HID_BOOT_H */
