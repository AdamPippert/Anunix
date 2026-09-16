/*
 * xhci.c — Polled xHCI host controller driver.
 *
 * Brings up every xHCI controller on the PCI bus without interrupts:
 * firmware handoff, reset, a command ring, one event ring, and the device
 * context array. It enumerates devices on root ports and behind USB 2.0
 * hubs, and binds HID boot-protocol keyboards and mice.
 *
 * Register and field positions follow the xHCI specification, revision
 * 1.2. The firmware handoff follows the sequence Linux uses in
 * drivers/usb/host/pci-quirks.c.
 *
 * Scope: devices present at probe time. Hot-plug, USB 3 hubs, and bulk,
 * isochronous, and stream endpoints are not supported.
 */

#include <anx/types.h>
#include <anx/xhci.h>
#include <anx/hid_boot.h>
#include <anx/usb_mouse.h>
#include <anx/fb.h>
#include <anx/pci.h>
#include <anx/mmio.h>
#include <anx/page.h>
#include <anx/list.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/delay.h>
#include <anx/perf.h>

/* PCI identity: serial bus controller, USB, xHCI programming interface */
#define XHCI_PCI_CLASS		0x0C
#define XHCI_PCI_SUBCLASS	0x03
#define XHCI_PCI_PROGIF		0x30
#define PCI_COMMAND		0x04
#define PCI_CMD_MEM		(1u << 1)
#define PCI_CMD_BUS_MASTER	(1u << 2)

/* Capability registers */
#define CAP_CAPLENGTH		0x00
#define CAP_HCSPARAMS1		0x04
#define CAP_HCSPARAMS2		0x08
#define CAP_HCCPARAMS1		0x10
#define CAP_DBOFF		0x14
#define CAP_RTSOFF		0x18

#define HCS1_MAX_SLOTS(p)	((p) & 0xFF)
#define HCS1_MAX_PORTS(p)	(((p) >> 24) & 0xFF)
#define HCS2_MAX_SCRATCH(p)	(((((p) >> 21) & 0x1F) << 5) | (((p) >> 27) & 0x1F))
#define HCC1_CSZ		(1u << 2)
#define HCC1_XECP(p)		(((p) >> 16) & 0xFFFF)

/* Operational registers, offset by CAPLENGTH */
#define OP_USBCMD		0x00
#define OP_USBSTS		0x04
#define OP_PAGESIZE		0x08
#define OP_CRCR			0x18
#define OP_DCBAAP		0x30
#define OP_CONFIG		0x38
#define OP_PORTSC(port)		(0x400 + ((port) - 1) * 0x10)

#define CMD_RUN			(1u << 0)
#define CMD_HCRST		(1u << 1)
#define CMD_INTE		(1u << 2)
#define STS_HCH			(1u << 0)
#define STS_CNR			(1u << 11)
#define CRCR_RCS		(1u << 0)

/* Port status and control */
#define PORTSC_CCS		(1u << 0)
#define PORTSC_PED		(1u << 1)
#define PORTSC_PR		(1u << 4)
#define PORTSC_PP		(1u << 9)
#define PORTSC_SPEED(p)		(((p) >> 10) & 0xF)
#define PORTSC_CSC		(1u << 17)
#define PORTSC_PRC		(1u << 21)
/*
 * Bits that keep their value when written back: port power, indicator
 * control, and wake enables. Every other bit is read-only, write-1-to-clear,
 * or has a side effect when written, such as PED, which disables the port.
 */
#define PORTSC_PRESERVE		(PORTSC_PP | (3u << 14) | (7u << 25))

/* Port speed IDs (default protocol speed ID mapping) */
#define SPEED_FS		1
#define SPEED_LS		2
#define SPEED_HS		3
#define SPEED_SS		4

/* Interrupter 0, offset by RTSOFF */
#define RT_IR0			0x20
#define IR_IMAN			0x00
#define IR_ERSTSZ		0x08
#define IR_ERSTBA		0x10
#define IR_ERDP			0x18
#define IMAN_IP			(1u << 0)
#define ERDP_EHB		(1u << 3)

/* Extended capability: USB legacy support */
#define XCAP_ID(v)		((v) & 0xFF)
#define XCAP_NEXT(v)		(((v) >> 8) & 0xFF)
#define XCAP_LEGACY		1
#define LEGACY_BIOS_OWNED	(1u << 16)
#define LEGACY_OS_OWNED		(1u << 24)
#define LEGACY_CTLSTS		0x04
#define LEGACY_SMI_KEEP		((0x7u << 1) | (0xFFu << 5) | (0x7u << 17))
#define LEGACY_SMI_EVENTS	(0x7u << 29)

/* Transfer request block */
struct xhci_trb {
	uint64_t param;
	uint32_t status;
	uint32_t control;
} __attribute__((packed));

#define TRB_CYCLE		(1u << 0)
#define TRB_TC			(1u << 1)
#define TRB_ISP			(1u << 2)
#define TRB_IOC			(1u << 5)
#define TRB_IDT			(1u << 6)
#define TRB_DIR_IN		(1u << 16)
#define TRB_TRT_OUT		(2u << 16)
#define TRB_TRT_IN		(3u << 16)
#define TRB_TYPE(t)		((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)		(((c) >> 10) & 0x3F)
#define TRB_SLOT(s)		((uint32_t)(s) << 24)
#define TRB_EP(e)		((uint32_t)(e) << 16)
#define TRB_GET_SLOT(c)		(((c) >> 24) & 0xFF)
#define TRB_GET_EP(c)		(((c) >> 16) & 0x1F)
#define TRB_GET_CODE(s)		(((s) >> 24) & 0xFF)
#define TRB_GET_LEN(s)		((s) & 0xFFFFFF)

#define T_NORMAL		1
#define T_SETUP			2
#define T_DATA			3
#define T_STATUS		4
#define T_LINK			6
#define T_ENABLE_SLOT		9
#define T_ADDRESS_DEVICE	11
#define T_CONFIGURE_EP		12
#define T_EVALUATE_CTX		13
#define T_RESET_EP		14
#define T_SET_TR_DEQ		16
#define T_EV_TRANSFER		32
#define T_EV_CMD_DONE		33

#define CC_SUCCESS		1
#define CC_STALL		6
#define CC_SHORT		13

/* Endpoint context types */
#define EP_TYPE_CTRL		4
#define EP_TYPE_INT_IN		7

/* USB requests */
#define REQ_GET_STATUS		0x00
#define REQ_CLEAR_FEATURE	0x01
#define REQ_SET_FEATURE		0x03
#define REQ_GET_DESCRIPTOR	0x06
#define REQ_SET_CONFIGURATION	0x09
#define HID_SET_IDLE		0x0A
#define HID_SET_PROTOCOL	0x0B
#define DESC_DEVICE		1
#define DESC_CONFIG		2
#define DESC_INTERFACE		4
#define DESC_ENDPOINT		5
#define DESC_HUB		0x29
#define HUB_PORT_POWER		8
#define HUB_PORT_RESET		4
#define HUB_C_PORT_CONNECTION	16
#define HUB_C_PORT_RESET	20
#define HUB_PS_CONNECTION	(1u << 0)
#define HUB_PS_ENABLE		(1u << 1)
#define HUB_PS_LOW_SPEED	(1u << 9)
#define HUB_PS_HIGH_SPEED	(1u << 10)
#define HUB_PC_RESET		(1u << 4)

#define CLASS_HID		3
#define CLASS_HUB		9
#define HID_SUBCLASS_BOOT	1
#define HID_PROTO_KEYBOARD	1
#define HID_PROTO_MOUSE		2

/* Limits */
#define RING_TRBS		(ANX_PAGE_SIZE / sizeof(struct xhci_trb))
#define XHCI_MAX_CTRL		8
#define XHCI_MAX_SLOTS		32
#define XHCI_MAX_DEVS		16
#define XHCI_MAX_SCRATCH	128
#define USB_MAX_DEPTH		5
#define CFG_COPY_MAX		512
#define CMD_TIMEOUT_MS		2000
#define XFER_TIMEOUT_MS		1000

struct xhci_ring {
	struct xhci_trb *trbs;
	uint64_t phys;
	uint32_t enq;
	uint32_t cycle;
};

struct xhci_ctrl;

enum hid_kind {
	HID_NONE = 0,
	HID_KEYBOARD,
	HID_MOUSE,
};

struct usb_dev {
	struct xhci_ctrl *hc;
	bool     used;
	uint8_t  slot;
	uint8_t  speed;
	uint8_t  root_port;
	uint8_t  depth;
	uint32_t route;
	uint8_t  tt_slot;		/* hub providing the transaction translator */
	uint8_t  tt_port;
	uint16_t mps0;
	uint8_t *in_ctx;
	uint64_t in_ctx_phys;
	struct xhci_ring ep0;

	/* Last control transfer completion */
	bool     ctrl_done;
	uint8_t  ctrl_code;

	/* HID boot interrupt IN pipe */
	enum hid_kind kind;
	uint8_t  int_dci;
	uint16_t int_mps;
	struct xhci_ring int_ring;
	uint8_t *int_buf;
	uint64_t int_buf_phys;
	bool     int_failed;
	bool     int_reported;		/* first report logged */
	struct anx_hid_kbd_state kbd;
};

struct xhci_ctrl {
	volatile uint8_t *base;
	uint32_t op;
	uint32_t rt;
	uint32_t db;
	uint32_t max_slots;
	uint32_t max_ports;
	uint32_t ctx_size;
	uint64_t *dcbaa;
	struct xhci_ring cmd;
	struct xhci_ring ev;		/* event ring: no link TRB, no cycle toggle */
	uint8_t *xfer;			/* bounce page for control data stages */
	uint64_t xfer_phys;
	bool     cmd_done;
	uint8_t  cmd_code;
	uint8_t  cmd_slot;
	bool     running;
	struct usb_dev devs[XHCI_MAX_DEVS];
	struct usb_dev *by_slot[XHCI_MAX_SLOTS + 1];
};

static struct xhci_ctrl g_ctrls[XHCI_MAX_CTRL];
static uint32_t g_ctrl_count;
static uint32_t g_hid_count;
static bool     g_probed;
static uint32_t g_scr_w = 1024;
static uint32_t g_scr_h = 768;

static int usb_attach(struct xhci_ctrl *hc, uint8_t root_port, uint32_t route,
		      uint8_t depth, uint8_t speed, struct usb_dev *parent,
		      uint8_t parent_port);

/* --- MMIO ------------------------------------------------------------ */

static uint32_t rd32(struct xhci_ctrl *hc, uint32_t off)
{
	return *(volatile uint32_t *)(hc->base + off);
}

static void wr32(struct xhci_ctrl *hc, uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(hc->base + off) = val;
}

/* 64-bit registers written as two 32-bit halves, low first. */
static void wr64(struct xhci_ctrl *hc, uint32_t off, uint64_t val)
{
	wr32(hc, off, (uint32_t)val);
	wr32(hc, off + 4, (uint32_t)(val >> 32));
}

static void ring_doorbell(struct xhci_ctrl *hc, uint8_t slot, uint8_t target)
{
	wr32(hc, hc->db + (uint32_t)slot * 4, target);
}

/* Wait until (register & mask) == want. */
static int wait_reg(struct xhci_ctrl *hc, uint32_t off, uint32_t mask,
		    uint32_t want, uint32_t ms)
{
	uint64_t start = anx_rdtsc();
	uint64_t budget = anx_tsc_budget_us((uint64_t)ms * 1000);

	for (;;) {
		if ((rd32(hc, off) & mask) == want)
			return ANX_OK;
		if (anx_rdtsc() - start > budget)
			return ANX_ETIMEDOUT;
		anx_delay_us(100);
	}
}

/* --- Memory ---------------------------------------------------------- */

/* One zeroed page. Pages are identity-mapped below 4 GiB, so the
 * address is also the bus address the controller uses. */
static void *dma_page(uint64_t *phys)
{
	uintptr_t p = anx_page_alloc(0);

	if (!p)
		return NULL;
	anx_memset((void *)p, 0, ANX_PAGE_SIZE);
	if (phys)
		*phys = (uint64_t)p;
	return (void *)p;
}

static int ring_alloc(struct xhci_ring *r, bool link)
{
	r->trbs = dma_page(&r->phys);
	if (!r->trbs)
		return ANX_ENOMEM;
	r->enq = 0;
	r->cycle = 1;
	if (link) {
		struct xhci_trb *l = &r->trbs[RING_TRBS - 1];

		l->param   = r->phys;
		l->control = TRB_TYPE(T_LINK) | TRB_TC;
	}
	return ANX_OK;
}

/*
 * Append a TRB to a command or transfer ring. The last slot holds a link
 * TRB back to the start with toggle-cycle set, so crossing it flips the
 * cycle bit the controller expects.
 */
static void ring_push(struct xhci_ring *r, uint64_t param, uint32_t status,
		      uint32_t control)
{
	struct xhci_trb *t = &r->trbs[r->enq];

	t->param   = param;
	t->status  = status;
	t->control = (control & ~TRB_CYCLE) | (r->cycle ? TRB_CYCLE : 0);

	if (++r->enq == RING_TRBS - 1) {
		struct xhci_trb *l = &r->trbs[RING_TRBS - 1];

		l->control = TRB_TYPE(T_LINK) | TRB_TC |
			     (r->cycle ? TRB_CYCLE : 0);
		r->enq = 0;
		r->cycle ^= 1;
	}
}

static uint64_t ring_enq_addr(const struct xhci_ring *r)
{
	return r->phys + (uint64_t)r->enq * sizeof(struct xhci_trb);
}

/* --- Events ---------------------------------------------------------- */

static void hid_complete(struct usb_dev *d, uint8_t code, uint32_t residual);

static void handle_event(struct xhci_ctrl *hc, const struct xhci_trb *t)
{
	uint32_t type = TRB_GET_TYPE(t->control);
	uint8_t code  = (uint8_t)TRB_GET_CODE(t->status);
	uint8_t slot  = (uint8_t)TRB_GET_SLOT(t->control);
	struct usb_dev *d;

	switch (type) {
	case T_EV_CMD_DONE:
		hc->cmd_code = code;
		hc->cmd_slot = slot;
		hc->cmd_done = true;
		break;
	case T_EV_TRANSFER:
		if (slot == 0 || slot > XHCI_MAX_SLOTS)
			break;
		d = hc->by_slot[slot];
		if (!d)
			break;
		if (TRB_GET_EP(t->control) == 1) {
			d->ctrl_code = code;
			d->ctrl_done = true;
		} else if (TRB_GET_EP(t->control) == d->int_dci) {
			hid_complete(d, code, TRB_GET_LEN(t->status));
		}
		break;
	default:
		/* Port status changes: enumeration is static, nothing to do. */
		break;
	}
}

/* Consume every event the controller has posted. */
static void hc_poll(struct xhci_ctrl *hc)
{
	uint32_t handled = 0;

	for (;;) {
		struct xhci_trb *t = &hc->ev.trbs[hc->ev.enq];

		if ((t->control & TRB_CYCLE) != hc->ev.cycle)
			break;
		handle_event(hc, t);
		handled++;
		if (++hc->ev.enq == RING_TRBS)
			hc->ev.enq = 0;
	}
	if (handled)
		wr64(hc, hc->rt + RT_IR0 + IR_ERDP,
		     ring_enq_addr(&hc->ev) | ERDP_EHB);
}

static int wait_flag(struct xhci_ctrl *hc, const bool *flag, uint32_t ms)
{
	uint64_t start = anx_rdtsc();
	uint64_t budget = anx_tsc_budget_us((uint64_t)ms * 1000);

	for (;;) {
		hc_poll(hc);
		if (*flag)
			return ANX_OK;
		if (anx_rdtsc() - start > budget)
			return ANX_ETIMEDOUT;
		anx_delay_us(50);
	}
}

/* --- Commands -------------------------------------------------------- */

static int hc_cmd(struct xhci_ctrl *hc, uint64_t param, uint32_t control,
		  uint8_t *slot_out)
{
	hc->cmd_done = false;
	ring_push(&hc->cmd, param, 0, control);
	ring_doorbell(hc, 0, 0);

	if (wait_flag(hc, &hc->cmd_done, CMD_TIMEOUT_MS) != ANX_OK) {
		kprintf("xhci: command %u timed out\n",
			(uint32_t)TRB_GET_TYPE(control));
		return ANX_ETIMEDOUT;
	}
	if (slot_out)
		*slot_out = hc->cmd_slot;
	if (hc->cmd_code != CC_SUCCESS) {
		kprintf("xhci: command %u failed (code %u)\n",
			(uint32_t)TRB_GET_TYPE(control),
			(uint32_t)hc->cmd_code);
		return ANX_EIO;
	}
	return ANX_OK;
}

/* --- Contexts -------------------------------------------------------- */

/* Input context entry: 0 control, 1 slot, 1 + DCI endpoint. */
static uint32_t *in_ctx(struct usb_dev *d, uint32_t idx)
{
	return (uint32_t *)(d->in_ctx + idx * d->hc->ctx_size);
}

static void in_ctx_begin(struct usb_dev *d, uint32_t add_flags)
{
	uint32_t *ctrl = in_ctx(d, 0);

	ctrl[0] = 0;		/* drop flags */
	ctrl[1] = add_flags;
}

static void ep_ctx_set(struct usb_dev *d, uint8_t dci, uint32_t type,
		       uint16_t mps, uint8_t interval, uint64_t deq,
		       uint16_t avg_len)
{
	uint32_t *ep = in_ctx(d, 1u + dci);

	ep[0] = (uint32_t)interval << 16;
	ep[1] = (3u << 1) | (type << 3) | ((uint32_t)mps << 16);
	ep[2] = (uint32_t)(deq | 1);		/* dequeue cycle state 1 */
	ep[3] = (uint32_t)(deq >> 32);
	ep[4] = avg_len;
}

/*
 * Interval exponent in 125 us units. High and SuperSpeed descriptors
 * already give an exponent; full and low speed give frames (ms).
 */
static uint8_t ep_interval(uint8_t speed, uint8_t binterval)
{
	uint32_t frames8;
	uint8_t exp = 3;

	if (speed == SPEED_HS || speed >= SPEED_SS) {
		exp = binterval ? (uint8_t)(binterval - 1) : 0;
		return exp > 15 ? 15 : exp;
	}
	frames8 = (binterval ? binterval : 1) * 8u;
	while (exp < 10 && (1u << (exp + 1)) <= frames8)
		exp++;
	return exp;
}

/* --- Control transfers ----------------------------------------------- */

/* Clear a stalled control endpoint so the next request can run. */
static void ep0_recover(struct usb_dev *d)
{
	struct xhci_ctrl *hc = d->hc;

	hc_cmd(hc, 0, TRB_TYPE(T_RESET_EP) | TRB_SLOT(d->slot) | TRB_EP(1),
	       NULL);
	hc_cmd(hc, ring_enq_addr(&d->ep0) | d->ep0.cycle,
	       TRB_TYPE(T_SET_TR_DEQ) | TRB_SLOT(d->slot) | TRB_EP(1), NULL);
}

static int usb_control(struct usb_dev *d, uint8_t rtype, uint8_t req,
		       uint16_t value, uint16_t index, uint16_t len,
		       uint8_t *buf)
{
	struct xhci_ctrl *hc = d->hc;
	bool in = (rtype & 0x80) != 0;
	uint64_t setup;
	uint32_t trt = 0;

	if (len > ANX_PAGE_SIZE)
		return ANX_EINVAL;

	setup = (uint64_t)rtype | ((uint64_t)req << 8) |
		((uint64_t)value << 16) | ((uint64_t)index << 32) |
		((uint64_t)len << 48);
	if (len)
		trt = in ? TRB_TRT_IN : TRB_TRT_OUT;
	if (len && in)
		anx_memset(hc->xfer, 0, len);
	if (len && !in)
		anx_memcpy(hc->xfer, buf, len);

	d->ctrl_done = false;
	ring_push(&d->ep0, setup, 8, TRB_TYPE(T_SETUP) | TRB_IDT | trt);
	if (len)
		ring_push(&d->ep0, hc->xfer_phys, len,
			  TRB_TYPE(T_DATA) | (in ? TRB_DIR_IN : 0));
	/* The status stage runs opposite to the data stage. */
	ring_push(&d->ep0, 0, 0, TRB_TYPE(T_STATUS) | TRB_IOC |
		  ((len && in) ? 0 : TRB_DIR_IN));
	ring_doorbell(hc, d->slot, 1);

	if (wait_flag(hc, &d->ctrl_done, XFER_TIMEOUT_MS) != ANX_OK)
		return ANX_ETIMEDOUT;
	if (d->ctrl_code == CC_STALL) {
		ep0_recover(d);
		return ANX_EIO;
	}
	if (d->ctrl_code != CC_SUCCESS && d->ctrl_code != CC_SHORT)
		return ANX_EIO;
	if (len && in && buf)
		anx_memcpy(buf, hc->xfer, len);
	return ANX_OK;
}

static int get_descriptor(struct usb_dev *d, uint8_t type, uint16_t len,
			  uint8_t *buf)
{
	return usb_control(d, 0x80, REQ_GET_DESCRIPTOR,
			   (uint16_t)((uint16_t)type << 8), 0, len, buf);
}

/* --- HID boot devices ------------------------------------------------ */

static void hid_queue(struct usb_dev *d)
{
	ring_push(&d->int_ring, d->int_buf_phys, d->int_mps,
		  TRB_TYPE(T_NORMAL) | TRB_IOC | TRB_ISP);
	ring_doorbell(d->hc, d->slot, d->int_dci);
}

static void hid_complete(struct usb_dev *d, uint8_t code, uint32_t residual)
{
	uint32_t got;

	if (code != CC_SUCCESS && code != CC_SHORT) {
		if (!d->int_failed)
			kprintf("xhci: slot %u input endpoint error %u\n",
				(uint32_t)d->slot, (uint32_t)code);
		d->int_failed = true;
		return;
	}

	got = residual < d->int_mps ? d->int_mps - residual : 0;

	/* One line per device proves reports arrive, on screen and in the
	 * saved boot log, without logging every keystroke. */
	if (!d->int_reported && got >= 3) {
		kprintf("xhci: first report from slot %u: %02x %02x %02x (%u bytes)\n",
			(uint32_t)d->slot, (uint32_t)d->int_buf[0],
			(uint32_t)d->int_buf[1], (uint32_t)d->int_buf[2], got);
		d->int_reported = true;
	}

	if (d->kind == HID_KEYBOARD) {
		anx_hid_kbd_report(&d->kbd, d->int_buf, got,
				   anx_hid_key_to_input, NULL);
	} else if (d->kind == HID_MOUSE) {
		struct anx_hid_mouse_delta m;
		struct anx_hid_mouse_report r;

		if (anx_hid_mouse_report(d->int_buf, got, &m) == ANX_OK) {
			r.buttons = m.buttons;
			r.x       = m.dx;
			r.y       = m.dy;
			r.wheel   = m.wheel;
			anx_usb_mouse_report(&r, g_scr_w, g_scr_h);
		}
	}
	hid_queue(d);
}

static int hid_setup(struct usb_dev *d, uint8_t iface, uint8_t proto,
		     uint8_t ep_addr, uint16_t mps, uint8_t binterval)
{
	uint32_t *slot;
	int ret;

	/* Both requests may stall on a device that has only one mode or
	 * no idle rate. Neither failure prevents boot reports. */
	(void)usb_control(d, 0x21, HID_SET_PROTOCOL, 0, iface, 0, NULL);
	(void)usb_control(d, 0x21, HID_SET_IDLE, 0, iface, 0, NULL);

	d->int_dci = (uint8_t)((ep_addr & 0x0F) * 2 + 1);
	d->int_mps = mps ? mps : 8;
	if (d->int_mps > 64)
		d->int_mps = 64;
	if (ring_alloc(&d->int_ring, true) != ANX_OK)
		return ANX_ENOMEM;
	d->int_buf = dma_page(&d->int_buf_phys);
	if (!d->int_buf)
		return ANX_ENOMEM;

	in_ctx_begin(d, (1u << 0) | (1u << d->int_dci));
	slot = in_ctx(d, 1);
	slot[0] = (slot[0] & ~(0x1Fu << 27)) | ((uint32_t)d->int_dci << 27);
	ep_ctx_set(d, d->int_dci, EP_TYPE_INT_IN, d->int_mps,
		   ep_interval(d->speed, binterval), d->int_ring.phys,
		   d->int_mps);

	ret = hc_cmd(d->hc, d->in_ctx_phys,
		     TRB_TYPE(T_CONFIGURE_EP) | TRB_SLOT(d->slot), NULL);
	if (ret != ANX_OK)
		return ret;

	d->kind = proto == HID_PROTO_KEYBOARD ? HID_KEYBOARD : HID_MOUSE;
	anx_hid_kbd_reset(&d->kbd);
	g_hid_count++;
	kprintf("xhci: %s on slot %u (endpoint 0x%02x, %u-byte reports)\n",
		d->kind == HID_KEYBOARD ? "keyboard" : "mouse",
		(uint32_t)d->slot, (uint32_t)ep_addr, (uint32_t)d->int_mps);
	hid_queue(d);
	return ANX_OK;
}

/* --- Hubs ------------------------------------------------------------ */

static int hub_port_status(struct usb_dev *hub, uint8_t port,
			   uint16_t *status, uint16_t *change)
{
	uint8_t st[4];
	int ret = usb_control(hub, 0xA3, REQ_GET_STATUS, 0, port, 4, st);

	if (ret != ANX_OK)
		return ret;
	*status = (uint16_t)(st[0] | (st[1] << 8));
	*change = (uint16_t)(st[2] | (st[3] << 8));
	return ANX_OK;
}

static int hub_setup(struct usb_dev *hub)
{
	struct xhci_ctrl *hc = hub->hc;
	uint8_t hd[9];
	uint32_t *slot;
	uint8_t nports, port;
	uint32_t power_ms;

	if (hub->speed >= SPEED_SS) {
		kprintf("xhci: USB 3 hub on slot %u not supported\n",
			(uint32_t)hub->slot);
		return ANX_ENOTSUP;
	}
	if (hub->depth + 1 >= USB_MAX_DEPTH)
		return ANX_ENOTSUP;

	if (usb_control(hub, 0xA0, REQ_GET_DESCRIPTOR,
			(uint16_t)(DESC_HUB << 8), 0, sizeof(hd), hd) != ANX_OK)
		return ANX_EIO;
	nports = hd[2] > 15 ? 15 : hd[2];
	power_ms = (uint32_t)hd[5] * 2;

	/* Tell the controller the slot is a hub before any child is
	 * addressed; it needs the port count and the TT think time. */
	in_ctx_begin(hub, 1u << 0);
	slot = in_ctx(hub, 1);
	slot[0] |= 1u << 26;
	slot[1] = (slot[1] & ~(0xFFu << 24)) | ((uint32_t)nports << 24);
	if (hub->speed == SPEED_HS)
		slot[2] = (slot[2] & ~(3u << 16)) |
			  ((uint32_t)((hd[3] >> 5) & 3) << 16);
	if (hc_cmd(hc, hub->in_ctx_phys,
		   TRB_TYPE(T_CONFIGURE_EP) | TRB_SLOT(hub->slot), NULL) != ANX_OK)
		return ANX_EIO;

	kprintf("xhci: hub on slot %u, %u ports\n", (uint32_t)hub->slot,
		(uint32_t)nports);

	for (port = 1; port <= nports; port++)
		(void)usb_control(hub, 0x23, REQ_SET_FEATURE, HUB_PORT_POWER,
				  port, 0, NULL);
	anx_delay_ms(power_ms + 100);

	for (port = 1; port <= nports; port++) {
		uint16_t status, change;
		uint32_t tries;
		uint8_t speed;

		if (hub_port_status(hub, port, &status, &change) != ANX_OK ||
		    !(status & HUB_PS_CONNECTION))
			continue;

		(void)usb_control(hub, 0x23, REQ_SET_FEATURE, HUB_PORT_RESET,
				  port, 0, NULL);
		for (tries = 0; tries < 50; tries++) {
			anx_delay_ms(10);
			if (hub_port_status(hub, port, &status, &change) == ANX_OK &&
			    (change & HUB_PC_RESET))
				break;
		}
		(void)usb_control(hub, 0x23, REQ_CLEAR_FEATURE,
				  HUB_C_PORT_RESET, port, 0, NULL);
		(void)usb_control(hub, 0x23, REQ_CLEAR_FEATURE,
				  HUB_C_PORT_CONNECTION, port, 0, NULL);
		if (!(status & HUB_PS_ENABLE))
			continue;

		if (status & HUB_PS_LOW_SPEED)
			speed = SPEED_LS;
		else if (status & HUB_PS_HIGH_SPEED)
			speed = SPEED_HS;
		else
			speed = SPEED_FS;

		anx_delay_ms(10);	/* reset recovery */
		(void)usb_attach(hc, hub->root_port,
				 hub->route | ((uint32_t)port << (4 * hub->depth)),
				 (uint8_t)(hub->depth + 1), speed, hub, port);
	}
	return ANX_OK;
}

/* --- Enumeration ----------------------------------------------------- */

static const char *speed_name(uint8_t speed)
{
	switch (speed) {
	case SPEED_FS: return "full";
	case SPEED_LS: return "low";
	case SPEED_HS: return "high";
	default:       return "super";
	}
}

static struct usb_dev *dev_alloc(struct xhci_ctrl *hc)
{
	uint32_t i;

	for (i = 0; i < XHCI_MAX_DEVS; i++) {
		if (!hc->devs[i].used) {
			anx_memset(&hc->devs[i], 0, sizeof(hc->devs[i]));
			hc->devs[i].used = true;
			hc->devs[i].hc = hc;
			return &hc->devs[i];
		}
	}
	return NULL;
}

/* Address a device and read its descriptors. */
static int usb_address(struct usb_dev *d, struct usb_dev *parent,
		       uint8_t parent_port, uint8_t dev_desc[18])
{
	struct xhci_ctrl *hc = d->hc;
	uint64_t out_phys;
	uint32_t *slot;

	if (hc_cmd(hc, 0, TRB_TYPE(T_ENABLE_SLOT), &d->slot) != ANX_OK)
		return ANX_EIO;
	if (d->slot == 0 || d->slot > XHCI_MAX_SLOTS)
		return ANX_ENOTSUP;
	hc->by_slot[d->slot] = d;

	if (!dma_page(&out_phys))
		return ANX_ENOMEM;
	hc->dcbaa[d->slot] = out_phys;
	d->in_ctx = dma_page(&d->in_ctx_phys);
	if (!d->in_ctx || ring_alloc(&d->ep0, true) != ANX_OK)
		return ANX_ENOMEM;

	/* Full- and low-speed devices behind a high-speed hub talk through
	 * that hub's transaction translator; deeper hubs pass it on. */
	if (parent && (d->speed == SPEED_FS || d->speed == SPEED_LS)) {
		if (parent->speed == SPEED_HS) {
			d->tt_slot = parent->slot;
			d->tt_port = parent_port;
		} else {
			d->tt_slot = parent->tt_slot;
			d->tt_port = parent->tt_port;
		}
	}

	switch (d->speed) {
	case SPEED_HS: d->mps0 = 64;  break;
	case SPEED_SS: d->mps0 = 512; break;
	default:       d->mps0 = 8;   break;
	}

	in_ctx_begin(d, (1u << 0) | (1u << 1));
	slot = in_ctx(d, 1);
	slot[0] = d->route | ((uint32_t)d->speed << 20) | (1u << 27);
	slot[1] = (uint32_t)d->root_port << 16;
	slot[2] = (uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8);
	ep_ctx_set(d, 1, EP_TYPE_CTRL, d->mps0, 0, d->ep0.phys, 8);

	if (hc_cmd(hc, d->in_ctx_phys,
		   TRB_TYPE(T_ADDRESS_DEVICE) | TRB_SLOT(d->slot), NULL) != ANX_OK)
		return ANX_EIO;
	anx_delay_ms(2);	/* SET_ADDRESS recovery */

	if (get_descriptor(d, DESC_DEVICE, 8, dev_desc) != ANX_OK)
		return ANX_EIO;
	if (dev_desc[7] && dev_desc[7] != d->mps0 && d->speed < SPEED_SS) {
		d->mps0 = dev_desc[7];
		in_ctx_begin(d, 1u << 1);
		ep_ctx_set(d, 1, EP_TYPE_CTRL, d->mps0, 0, d->ep0.phys, 8);
		/* Evaluate Context reads only the max packet size here; the
		 * dequeue pointer written above is ignored. */
		if (hc_cmd(hc, d->in_ctx_phys,
			   TRB_TYPE(T_EVALUATE_CTX) | TRB_SLOT(d->slot),
			   NULL) != ANX_OK)
			return ANX_EIO;
	}
	return get_descriptor(d, DESC_DEVICE, 18, dev_desc);
}

static int usb_attach(struct xhci_ctrl *hc, uint8_t root_port, uint32_t route,
		      uint8_t depth, uint8_t speed, struct usb_dev *parent,
		      uint8_t parent_port)
{
	struct usb_dev *d = dev_alloc(hc);
	uint8_t dd[18];
	uint8_t cfg[CFG_COPY_MAX];
	uint16_t total, off;
	uint8_t cfg_value, cur_class = 0, cur_sub = 0, cur_proto = 0;
	uint8_t cur_iface = 0;
	bool is_hub;

	if (!d) {
		kprintf("xhci: device table full\n");
		return ANX_ENOMEM;
	}
	d->root_port = root_port;
	d->route = route;
	d->depth = depth;
	d->speed = speed;

	if (usb_address(d, parent, parent_port, dd) != ANX_OK) {
		kprintf("xhci: port %u route %x: address failed\n",
			(uint32_t)root_port, route);
		goto fail;
	}

	kprintf("xhci: port %u route %x %s speed: %04x:%04x class %02x\n",
		(uint32_t)root_port, route, speed_name(speed),
		(uint32_t)(dd[8] | (dd[9] << 8)),
		(uint32_t)(dd[10] | (dd[11] << 8)), (uint32_t)dd[4]);

	if (get_descriptor(d, DESC_CONFIG, 9, cfg) != ANX_OK)
		goto fail;
	total = (uint16_t)(cfg[2] | (cfg[3] << 8));
	if (total > CFG_COPY_MAX)
		total = CFG_COPY_MAX;
	if (total < 9 || get_descriptor(d, DESC_CONFIG, total, cfg) != ANX_OK)
		goto fail;
	cfg_value = cfg[5];

	if (usb_control(d, 0x00, REQ_SET_CONFIGURATION, cfg_value, 0, 0,
			NULL) != ANX_OK)
		goto fail;

	is_hub = dd[4] == CLASS_HUB;
	for (off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
		const uint8_t *desc = cfg + off;

		if (off + desc[0] > total)
			break;
		if (desc[1] == DESC_INTERFACE && desc[0] >= 9) {
			cur_iface = desc[2];
			cur_class = desc[5];
			cur_sub   = desc[6];
			cur_proto = desc[7];
			if (cur_class == CLASS_HUB)
				is_hub = true;
		} else if (desc[1] == DESC_ENDPOINT && desc[0] >= 7 &&
			   !is_hub && d->kind == HID_NONE &&
			   cur_class == CLASS_HID &&
			   cur_sub == HID_SUBCLASS_BOOT &&
			   (cur_proto == HID_PROTO_KEYBOARD ||
			    cur_proto == HID_PROTO_MOUSE) &&
			   (desc[2] & 0x80) && (desc[3] & 0x03) == 3) {
			(void)hid_setup(d, cur_iface, cur_proto, desc[2],
					(uint16_t)(desc[4] | (desc[5] << 8)),
					desc[6]);
		}
	}

	if (is_hub)
		(void)hub_setup(d);
	return ANX_OK;

fail:
	if (d->slot && d->slot <= XHCI_MAX_SLOTS)
		hc->by_slot[d->slot] = NULL;
	d->used = false;
	return ANX_EIO;
}

/* --- Controller bring-up --------------------------------------------- */

static void hc_take_ownership(struct xhci_ctrl *hc, uint32_t hccparams1)
{
	uint32_t off = HCC1_XECP(hccparams1) * 4;
	uint32_t guard;

	for (guard = 0; off && guard < 64; guard++) {
		uint32_t v = rd32(hc, off);

		if (XCAP_ID(v) == XCAP_LEGACY) {
			if (v & LEGACY_BIOS_OWNED) {
				wr32(hc, off, v | LEGACY_OS_OWNED);
				if (wait_reg(hc, off, LEGACY_BIOS_OWNED, 0,
					     1000) != ANX_OK) {
					kprintf("xhci: firmware kept ownership; taking it\n");
					wr32(hc, off, (rd32(hc, off) | LEGACY_OS_OWNED) &
						      ~LEGACY_BIOS_OWNED);
				}
			}
			v = rd32(hc, off + LEGACY_CTLSTS);
			wr32(hc, off + LEGACY_CTLSTS,
			     (v & LEGACY_SMI_KEEP) | LEGACY_SMI_EVENTS);
			return;
		}
		if (!XCAP_NEXT(v))
			return;
		off += XCAP_NEXT(v) * 4;
	}
}

static int root_port_enable(struct xhci_ctrl *hc, uint32_t port,
			    uint8_t *speed)
{
	uint32_t reg = hc->op + OP_PORTSC(port);
	uint32_t sc = rd32(hc, reg);

	if (!(sc & PORTSC_PP)) {
		wr32(hc, reg, (sc & PORTSC_PRESERVE) | PORTSC_PP);
		anx_delay_ms(20);
		sc = rd32(hc, reg);
	}
	if (!(sc & PORTSC_CCS))
		return ANX_ENODEV;

	/* USB 2 ports need a reset to enable; USB 3 ports enable
	 * themselves when the link trains. */
	if (!(sc & PORTSC_PED)) {
		wr32(hc, reg, (sc & PORTSC_PRESERVE) | PORTSC_PR);
		if (wait_reg(hc, reg, PORTSC_PRC, PORTSC_PRC, 500) != ANX_OK)
			return ANX_ETIMEDOUT;
		sc = rd32(hc, reg);
		wr32(hc, reg, (sc & PORTSC_PRESERVE) | PORTSC_PRC | PORTSC_CSC);
		anx_delay_ms(10);	/* reset recovery */
		sc = rd32(hc, reg);
		if (!(sc & PORTSC_PED))
			return ANX_EIO;
	}
	*speed = (uint8_t)PORTSC_SPEED(sc);
	return ANX_OK;
}

static int hc_start(struct xhci_ctrl *hc, struct anx_pci_device *pci)
{
	uint64_t bar, phys;
	uint32_t hcs1, hcs2, hcc1, cmd, i, nscratch;
	uint64_t *scratch;
	struct {
		uint64_t base;
		uint32_t size;
		uint32_t rsvd;
	} __attribute__((packed)) *erst;

	bar = pci->bar[0] & ~0xFULL;
	if ((pci->bar[0] & 0x6) == 0x4)
		bar |= (uint64_t)pci->bar[1] << 32;
	if (!bar)
		return ANX_ENODEV;

	cmd = anx_pci_config_read(pci->bus, pci->slot, pci->func, PCI_COMMAND);
	anx_pci_config_write(pci->bus, pci->slot, pci->func, PCI_COMMAND,
			     cmd | PCI_CMD_MEM | PCI_CMD_BUS_MASTER);

	hc->base = anx_mmio_map(bar, 0x100000);
	if (!hc->base)
		return ANX_ENOMEM;

	hc->op = *(volatile uint8_t *)hc->base;	/* CAPLENGTH */
	hcs1 = rd32(hc, CAP_HCSPARAMS1);
	hcs2 = rd32(hc, CAP_HCSPARAMS2);
	hcc1 = rd32(hc, CAP_HCCPARAMS1);
	hc->db = rd32(hc, CAP_DBOFF) & ~3u;
	hc->rt = rd32(hc, CAP_RTSOFF) & ~0x1Fu;
	hc->ctx_size = (hcc1 & HCC1_CSZ) ? 64 : 32;
	hc->max_ports = HCS1_MAX_PORTS(hcs1);
	hc->max_slots = HCS1_MAX_SLOTS(hcs1);
	if (hc->max_slots > XHCI_MAX_SLOTS)
		hc->max_slots = XHCI_MAX_SLOTS;

	hc_take_ownership(hc, hcc1);

	if (wait_reg(hc, hc->op + OP_USBSTS, STS_CNR, 0, 5000) != ANX_OK)
		kprintf("xhci: controller not ready after 5 s; continuing\n");

	wr32(hc, hc->op + OP_USBCMD,
	     rd32(hc, hc->op + OP_USBCMD) & ~(CMD_RUN | CMD_INTE));
	if (wait_reg(hc, hc->op + OP_USBSTS, STS_HCH, STS_HCH, 100) != ANX_OK) {
		kprintf("xhci: controller did not halt\n");
		return ANX_EIO;
	}

	wr32(hc, hc->op + OP_USBCMD, CMD_HCRST);
	anx_delay_ms(10);
	if (wait_reg(hc, hc->op + OP_USBCMD, CMD_HCRST, 0, 1000) != ANX_OK ||
	    wait_reg(hc, hc->op + OP_USBSTS, STS_CNR, 0, 1000) != ANX_OK) {
		kprintf("xhci: reset did not complete\n");
		return ANX_EIO;
	}
	if (!(rd32(hc, hc->op + OP_PAGESIZE) & 1)) {
		kprintf("xhci: 4 KiB pages not supported\n");
		return ANX_ENOTSUP;
	}

	wr32(hc, hc->op + OP_CONFIG, hc->max_slots);

	hc->dcbaa = dma_page(&phys);
	if (!hc->dcbaa)
		return ANX_ENOMEM;
	nscratch = HCS2_MAX_SCRATCH(hcs2);
	if (nscratch > XHCI_MAX_SCRATCH) {
		kprintf("xhci: %u scratchpad pages requested\n", nscratch);
		return ANX_ENOTSUP;
	}
	if (nscratch) {
		scratch = dma_page(&hc->dcbaa[0]);
		if (!scratch)
			return ANX_ENOMEM;
		for (i = 0; i < nscratch; i++)
			if (!dma_page(&scratch[i]))
				return ANX_ENOMEM;
	}
	wr64(hc, hc->op + OP_DCBAAP, phys);

	if (ring_alloc(&hc->cmd, true) != ANX_OK)
		return ANX_ENOMEM;
	wr64(hc, hc->op + OP_CRCR, hc->cmd.phys | CRCR_RCS);

	if (ring_alloc(&hc->ev, false) != ANX_OK)
		return ANX_ENOMEM;
	erst = dma_page(&phys);
	if (!erst)
		return ANX_ENOMEM;
	erst->base = hc->ev.phys;
	erst->size = (uint32_t)RING_TRBS;
	wr32(hc, hc->rt + RT_IR0 + IR_ERSTSZ, 1);
	wr64(hc, hc->rt + RT_IR0 + IR_ERDP, hc->ev.phys);
	wr64(hc, hc->rt + RT_IR0 + IR_ERSTBA, phys);
	wr32(hc, hc->rt + RT_IR0 + IR_IMAN, IMAN_IP);	/* polled: IE stays 0 */

	hc->xfer = dma_page(&hc->xfer_phys);
	if (!hc->xfer)
		return ANX_ENOMEM;

	wr32(hc, hc->op + OP_USBCMD, CMD_RUN);
	if (wait_reg(hc, hc->op + OP_USBSTS, STS_HCH, 0, 1000) != ANX_OK) {
		kprintf("xhci: controller did not start\n");
		return ANX_EIO;
	}
	hc->running = true;

	/* Let connected devices debounce before reading port status. */
	anx_delay_ms(200);

	for (i = 1; i <= hc->max_ports; i++) {
		uint8_t speed;

		if (root_port_enable(hc, i, &speed) != ANX_OK)
			continue;
		(void)usb_attach(hc, (uint8_t)i, 0, 0, speed, NULL, 0);
	}
	return ANX_OK;
}

/* --- Public API ------------------------------------------------------ */

int anx_xhci_init(void)
{
	struct anx_list_head *pos;
	const struct anx_fb_info *fb;

	/* The driver table calls init once per matching PCI function; the
	 * first call binds every controller and later calls are no-ops. */
	if (g_probed)
		return g_ctrl_count > 0 ? ANX_OK : ANX_ENOENT;
	g_probed = true;

	fb = anx_fb_get_info();
	if (fb && fb->available) {
		g_scr_w = fb->width;
		g_scr_h = fb->height;
	}

	ANX_LIST_FOR_EACH(pos, anx_pci_device_list()) {
		struct anx_pci_device *pci =
			ANX_LIST_ENTRY(pos, struct anx_pci_device, link);
		struct xhci_ctrl *hc;

		if (pci->class_code != XHCI_PCI_CLASS ||
		    pci->subclass != XHCI_PCI_SUBCLASS ||
		    pci->prog_if != XHCI_PCI_PROGIF)
			continue;
		if (g_ctrl_count >= XHCI_MAX_CTRL)
			break;

		kprintf("xhci: controller %04x:%04x at %02x:%02x.%x\n",
			(uint32_t)pci->vendor_id, (uint32_t)pci->device_id,
			(uint32_t)pci->bus, (uint32_t)pci->slot,
			(uint32_t)pci->func);

		hc = &g_ctrls[g_ctrl_count];
		anx_memset(hc, 0, sizeof(*hc));
		if (hc_start(hc, pci) != ANX_OK)
			continue;
		g_ctrl_count++;
	}

	kprintf("xhci: %u controller(s), %u HID boot device(s)\n",
		g_ctrl_count, g_hid_count);
	return g_ctrl_count > 0 ? ANX_OK : ANX_ENOENT;
}

void anx_xhci_poll(void)
{
	uint32_t i;

	for (i = 0; i < g_ctrl_count; i++)
		if (g_ctrls[i].running)
			hc_poll(&g_ctrls[i]);
}

uint32_t anx_xhci_hid_count(void)
{
	return g_hid_count;
}
