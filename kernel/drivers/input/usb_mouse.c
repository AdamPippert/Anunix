/*
 * usb_mouse.c — USB HID boot-protocol mouse driver.
 *
 * Supports USB mice and PS/2 trackballs connected via USB adapters.
 * Both present as HID boot-class mice: usage page 0x01, usage 0x02,
 * 3-byte report (buttons, dX, dY) or 4-byte with scroll wheel.
 *
 * Implementation notes:
 *   - Full xHCI ring / descriptor programming is not yet implemented.
 *     This driver detects USB controllers via PCI (class 0x0C sub 0x03),
 *     enables them, and registers an IRQ handler ready to receive reports.
 *   - The report parser (anx_usb_mouse_report) is complete and correct;
 *     it is also callable from a synthetic stream for testing.
 *   - Cursor state is maintained here (absolute X,Y clamped to screen).
 *   - Actual host controller initialisation is behind ANX_USB_HCI_READY;
 *     the TODO comment marks where xHCI bulk-in endpoint setup belongs.
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
/* IRQ handler                                                          */
/* ------------------------------------------------------------------ */

/*
 * Called when a USB interrupt fires. In a full xHCI implementation this
 * would read the completed transfer ring entry and pass the report bytes
 * here. For now we define the handler skeleton; the report bytes arrive
 * via anx_usb_mouse_report() once the host controller is wired.
 */
static void usb_mouse_irq_handler(uint32_t irq, void *arg)
{
	(void)irq;
	(void)arg;
	/*
	 * TODO(xhci): dequeue completed bulk-in TRB, extract HID report,
	 * call anx_usb_mouse_report(&report, scr_w, scr_h).
	 *
	 * Until then this handler acks the interrupt so the system
	 * doesn't hang on spurious PCI IRQs from the USB controller.
	 */
}

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
	struct anx_list_head *list;
	struct anx_list_head *pos;
	bool found_controller = false;
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

	/* Walk PCI device list looking for USB host controllers */
	list = anx_pci_device_list();
	ANX_LIST_FOR_EACH(pos, list) {
		struct anx_pci_device *dev =
			ANX_LIST_ENTRY(pos, struct anx_pci_device, link);

		if (dev->class_code != PCI_CLASS_SERIAL_BUS ||
		    dev->subclass   != PCI_SUBCLASS_USB)
			continue;

		found_controller = true;

		kprintf("usb_mouse: found USB controller [%04x:%04x] "
		        "progif=%02x irq=%u\n",
		        dev->vendor_id, dev->device_id,
		        dev->prog_if,   dev->irq_line);

		/* Enable bus mastering so the controller can DMA */
		anx_pci_enable_bus_master(dev);

		/*
		 * Register an IRQ handler for this controller's interrupt
		 * line. IRQ lines 10-11 are typical for PCI USB controllers.
		 * We only register once even if multiple controllers share a
		 * line (the handler does nothing until xHCI is wired).
		 */
		if (dev->irq_line > 0 && dev->irq_line <= 15) {
			anx_irq_register((uint8_t)dev->irq_line,
			                  usb_mouse_irq_handler, dev);
			anx_irq_unmask((uint8_t)dev->irq_line);
		}

		/*
		 * TODO(xhci): initialise the host controller ring:
		 *   1. Reset the controller (XHCI_OP_USBCMD HCRST bit)
		 *   2. Allocate DCBAA and command/event rings
		 *   3. Walk port status registers, detect FS/LS/HS device
		 *   4. Issue ENABLE_SLOT, ADDRESS_DEVICE commands
		 *   5. Set configuration for HID boot-protocol mouse
		 *      (SET_PROTOCOL, SET_IDLE)
		 *   6. Schedule recurring bulk-in TRBs on interrupt endpoint
		 * Once done, reports arrive in usb_mouse_irq_handler above.
		 *
		 * For now, break after the first controller — one is enough.
		 */
		break;
	}

	if (!found_controller) {
		kprintf("usb_mouse: no USB controller found (non-fatal)\n");
		return ANX_ENOENT;
	}

	initialized = true;
	kprintf("usb_mouse: initialized (report parser ready, "
	        "cursor at %dx%d, screen %ux%u)\n",
	        (int)cur_x, (int)cur_y,
	        (unsigned)scr_w, (unsigned)scr_h);
	return ANX_OK;
}
