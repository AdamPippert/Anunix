/*
 * usb_mouse.c — USB HID boot-protocol mouse driver.
 *
 * Supports USB mice and PS/2 trackballs connected via USB adapters.
 * Both present as HID boot-class mice: usage page 0x01, usage 0x02,
 * 3-byte report (buttons, dX, dY) or 4-byte with scroll wheel.
 *
 * Implementation notes:
 *   - The xHCI driver (drivers/usb/xhci.c) delivers boot mouse reports
 *     to anx_usb_mouse_report(); this file owns only the cursor.
 *   - The report parser is also callable from a synthetic stream for
 *     testing.
 *   - Cursor state is maintained here (absolute X,Y clamped to screen).
 */

#include <anx/usb_mouse.h>
#include <anx/input.h>
#include <anx/pci.h>
#include <anx/irq.h>
#include <anx/fb.h>
#include <anx/list.h>
#include <anx/kprintf.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* PCI class / subclass / prog-if values for USB controllers           */
/* ------------------------------------------------------------------ */

#define PCI_CLASS_SERIAL_BUS     0x0C
#define PCI_SUBCLASS_USB         0x03
#define PCI_PROGIF_UHCI          0x00
#define PCI_PROGIF_OHCI          0x10
#define PCI_PROGIF_EHCI          0x20
#define PCI_PROGIF_XHCI          0x30

/* ------------------------------------------------------------------ */
/* Cursor state                                                         */
/* ------------------------------------------------------------------ */

static int32_t  cur_x;
static int32_t  cur_y;
static uint32_t prev_buttons;
static bool     initialized;

/* Screen dimensions — updated from framebuffer at init */
static uint32_t scr_w = 1024;
static uint32_t scr_h = 768;

/* Sensitivity: movements are scaled by this factor / 256 */
#define MOUSE_SCALE  256u

/* ------------------------------------------------------------------ */
/* anx_usb_mouse_report — HID report parser                            */
/* ------------------------------------------------------------------ */

void
anx_usb_mouse_report(const struct anx_hid_mouse_report *report,
                      uint32_t screen_w, uint32_t screen_h)
{
	int32_t new_x, new_y;
	uint32_t changed;

	if (!report)
		return;

	/* Accumulate relative movement, clamped to screen bounds */
	new_x = cur_x + (int32_t)report->x;
	new_y = cur_y + (int32_t)report->y;

	if (new_x < 0)                    new_x = 0;
	if (new_y < 0)                    new_y = 0;
	if (new_x >= (int32_t)screen_w)   new_x = (int32_t)screen_w - 1;
	if (new_y >= (int32_t)screen_h)   new_y = (int32_t)screen_h - 1;

	cur_x = new_x;
	cur_y = new_y;

	/* Post move event whenever position changes */
	if (new_x != cur_x - (int32_t)report->x ||
	    new_y != cur_y - (int32_t)report->y ||
	    report->x != 0 || report->y != 0) {
		anx_input_pointer_move(cur_x, cur_y,
		                        (uint32_t)report->buttons);
	}

	/* Post button event when button state changes */
	changed = ((uint32_t)report->buttons) ^ prev_buttons;
	if (changed) {
		anx_input_pointer_button(cur_x, cur_y,
		                          (uint32_t)report->buttons, 0);
		prev_buttons = (uint32_t)report->buttons;
	}

	/* Post scroll event for wheel movement */
	if (report->wheel != 0)
		anx_input_pointer_scroll(cur_x, cur_y, (int32_t)report->wheel);
}

/* ------------------------------------------------------------------ */
/* Cursor position accessors                                            */
/* ------------------------------------------------------------------ */

void
anx_usb_mouse_set_pos(int32_t x, int32_t y)
{
	cur_x = x;
	cur_y = y;
}

void
anx_usb_mouse_get_pos(int32_t *x, int32_t *y)
{
	if (x) *x = cur_x;
	if (y) *y = cur_y;
}

/* ------------------------------------------------------------------ */
/* anx_usb_mouse_init                                                   */
/* ------------------------------------------------------------------ */

int
anx_usb_mouse_init(void)
{
	const struct anx_fb_info *fb;

	/* Centre cursor on screen using framebuffer dimensions */
	fb = anx_fb_get_info();
	if (fb && fb->available) {
		scr_w = fb->width;
		scr_h = fb->height;
	}
	cur_x = (int32_t)(scr_w / 2);
	cur_y = (int32_t)(scr_h / 2);
	prev_buttons = 0;

	/*
	 * This runs before the PCI scan, so it cannot look for controllers.
	 * The xHCI driver binds them from the driver table and feeds reports
	 * into anx_usb_mouse_report().
	 */
	initialized = true;
	kprintf("usb_mouse: initialized (report parser ready, "
	        "cursor at %dx%d, screen %ux%u)\n",
	        (int)cur_x, (int)cur_y,
	        (unsigned)scr_w, (unsigned)scr_h);
	return ANX_OK;
}
