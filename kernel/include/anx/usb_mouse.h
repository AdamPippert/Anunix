/*
 * anx/usb_mouse.h — USB HID mouse / pointing device driver.
 *
 * Supports USB HID boot-protocol mice (usage page 0x01, usage 0x02)
 * including PS/2 trackballs connected via USB adapters. These devices
 * present as standard USB HID boot-class mice: 3-byte report (buttons,
 * X delta, Y delta), or 4-byte with scroll wheel.
 *
 * The driver registers with the PCI xHCI / UHCI / OHCI controller at
 * boot, or can be driven from a synthetic report stream for testing via
 * anx_usb_mouse_report().
 */

#ifndef ANX_USB_MOUSE_H
#define ANX_USB_MOUSE_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Button mask bits (HID usage page 0x09)                             */
/* ------------------------------------------------------------------ */

#define ANX_MOUSE_BTN_LEFT    (1u << 0)
#define ANX_MOUSE_BTN_RIGHT   (1u << 1)
#define ANX_MOUSE_BTN_MIDDLE  (1u << 2)
#define ANX_MOUSE_BTN_BACK    (1u << 3)
#define ANX_MOUSE_BTN_FORWARD (1u << 4)

/* ------------------------------------------------------------------ */
/* USB HID boot-protocol mouse report (3 or 4 bytes)                  */
/* ------------------------------------------------------------------ */

struct anx_hid_mouse_report {
	uint8_t  buttons;  /* bitfield: bit 0 = left, 1 = right, 2 = middle */
	int8_t   x;        /* signed relative X movement */
	int8_t   y;        /* signed relative Y movement */
	int8_t   wheel;    /* signed scroll wheel (0 if 3-byte report) */
};

/* ------------------------------------------------------------------ */
/* Driver API                                                           */
/* ------------------------------------------------------------------ */

/*
 * Initialise the USB HID mouse driver.
 * Scans PCI for xHCI/OHCI/UHCI/EHCI controllers and probes for boot-
 * class HID mouse devices. Falls back to polling mode if no interrupt-
 * capable path is available.
 * Returns ANX_OK on success, ANX_ENOENT if no USB mouse found (non-fatal).
 */
int anx_usb_mouse_init(void);

/*
 * Process a raw HID boot-protocol report.
 * Called from the USB interrupt handler (or from test harness).
 * Translates the report into absolute screen coordinates and calls
 * anx_input_pointer_move() / anx_input_pointer_button().
 *
 * screen_w / screen_h are the display dimensions used to clamp
 * the accumulated cursor position.
 */
void anx_usb_mouse_report(const struct anx_hid_mouse_report *report,
                           uint32_t screen_w, uint32_t screen_h);

/* Set the initial cursor position (called after framebuffer init) */
void anx_usb_mouse_set_pos(int32_t x, int32_t y);

/* Get current cursor position */
void anx_usb_mouse_get_pos(int32_t *x, int32_t *y);

#endif /* ANX_USB_MOUSE_H */
