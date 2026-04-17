/*
 * input.c — Kernel input subsystem.
 *
 * Translates raw arch-level key scancodes into Interface Plane events
 * and delivers them to the focused surface via anx_iface_event_post().
 * The arch keyboard IRQ handler calls anx_input_ps2_key() after (or
 * instead of) buffering the ASCII character for arch_console_getc().
 *
 * Focus is a single surface OID. The compositor sets it via
 * anx_input_focus_set(); events are dropped if no surface has focus.
 */

#include <anx/input.h>
#include <anx/interface_plane.h>
#include <anx/spinlock.h>
#include <anx/uuid.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static anx_oid_t         focused_surf;
static uint32_t          current_modifiers;
static struct anx_spinlock input_lock;
static bool              initialized;

/* ------------------------------------------------------------------ */
/* PS/2 scancode set 1 → USB HID keycode                               */
/* ------------------------------------------------------------------ */

/*
 * Indexed by scancode (0x00–0x7F). 0 = unknown.
 * Only the keys that exist on a standard US 104-key layout are mapped;
 * extended scancodes (E0 prefix) are handled separately.
 */
static const uint8_t sc1_to_hid[128] = {
	/* 0x00 */ ANX_KEY_NONE,
	/* 0x01 */ ANX_KEY_ESC,
	/* 0x02 */ ANX_KEY_1,
	/* 0x03 */ ANX_KEY_2,
	/* 0x04 */ ANX_KEY_3,
	/* 0x05 */ ANX_KEY_4,
	/* 0x06 */ ANX_KEY_5,
	/* 0x07 */ ANX_KEY_6,
	/* 0x08 */ ANX_KEY_7,
	/* 0x09 */ ANX_KEY_8,
	/* 0x0A */ ANX_KEY_9,
	/* 0x0B */ ANX_KEY_0,
	/* 0x0C */ ANX_KEY_MINUS,
	/* 0x0D */ ANX_KEY_EQUAL,
	/* 0x0E */ ANX_KEY_BACKSPACE,
	/* 0x0F */ ANX_KEY_TAB,
	/* 0x10 */ ANX_KEY_Q,
	/* 0x11 */ ANX_KEY_W,
	/* 0x12 */ ANX_KEY_E,
	/* 0x13 */ ANX_KEY_R,
	/* 0x14 */ ANX_KEY_T,
	/* 0x15 */ ANX_KEY_Y,
	/* 0x16 */ ANX_KEY_U,
	/* 0x17 */ ANX_KEY_I,
	/* 0x18 */ ANX_KEY_O,
	/* 0x19 */ ANX_KEY_P,
	/* 0x1A */ ANX_KEY_LBRACKET,
	/* 0x1B */ ANX_KEY_RBRACKET,
	/* 0x1C */ ANX_KEY_ENTER,
	/* 0x1D */ ANX_KEY_LCTRL,
	/* 0x1E */ ANX_KEY_A,
	/* 0x1F */ ANX_KEY_S,
	/* 0x20 */ ANX_KEY_D,
	/* 0x21 */ ANX_KEY_F,
	/* 0x22 */ ANX_KEY_G,
	/* 0x23 */ ANX_KEY_H,
	/* 0x24 */ ANX_KEY_J,
	/* 0x25 */ ANX_KEY_K,
	/* 0x26 */ ANX_KEY_L,
	/* 0x27 */ ANX_KEY_SEMICOLON,
	/* 0x28 */ ANX_KEY_APOSTROPHE,
	/* 0x29 */ ANX_KEY_GRAVE,
	/* 0x2A */ ANX_KEY_LSHIFT,
	/* 0x2B */ ANX_KEY_BACKSLASH,
	/* 0x2C */ ANX_KEY_Z,
	/* 0x2D */ ANX_KEY_X,
	/* 0x2E */ ANX_KEY_C,
	/* 0x2F */ ANX_KEY_V,
	/* 0x30 */ ANX_KEY_B,
	/* 0x31 */ ANX_KEY_N,
	/* 0x32 */ ANX_KEY_M,
	/* 0x33 */ ANX_KEY_COMMA,
	/* 0x34 */ ANX_KEY_DOT,
	/* 0x35 */ ANX_KEY_SLASH,
	/* 0x36 */ ANX_KEY_RSHIFT,
	/* 0x37 */ ANX_KEY_NONE,  /* KP * */
	/* 0x38 */ ANX_KEY_LALT,
	/* 0x39 */ ANX_KEY_SPACE,
	/* 0x3A */ ANX_KEY_CAPSLOCK,
	/* 0x3B */ ANX_KEY_F1,
	/* 0x3C */ ANX_KEY_F2,
	/* 0x3D */ ANX_KEY_F3,
	/* 0x3E */ ANX_KEY_F4,
	/* 0x3F */ ANX_KEY_F5,
	/* 0x40 */ ANX_KEY_F6,
	/* 0x41 */ ANX_KEY_F7,
	/* 0x42 */ ANX_KEY_F8,
	/* 0x43 */ ANX_KEY_F9,
	/* 0x44 */ ANX_KEY_F10,
	/* 0x45 */ ANX_KEY_SCROLLLOCK,
	/* 0x46 */ ANX_KEY_SCROLLLOCK,
	/* 0x47–0x53: keypad, ignored for now */
};

uint32_t
anx_input_scancode_to_hid(uint8_t scancode)
{
	if (scancode >= 128)
		return ANX_KEY_NONE;
	return sc1_to_hid[scancode];
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

int
anx_input_init(void)
{
	anx_spin_init(&input_lock);
	/* zero-init of focused_surf is a null OID — no focus */
	current_modifiers = 0;
	initialized = true;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Focus management                                                     */
/* ------------------------------------------------------------------ */

void
anx_input_focus_set(anx_oid_t surf_oid)
{
	bool flags;

	anx_spin_lock_irqsave(&input_lock, &flags);
	focused_surf = surf_oid;
	anx_spin_unlock_irqrestore(&input_lock, flags);
}

anx_oid_t
anx_input_focus_get(void)
{
	bool flags;
	anx_oid_t oid;

	anx_spin_lock_irqsave(&input_lock, &flags);
	oid = focused_surf;
	anx_spin_unlock_irqrestore(&input_lock, flags);
	return oid;
}

/* ------------------------------------------------------------------ */
/* Modifier tracking helpers                                            */
/* ------------------------------------------------------------------ */

static void
update_mods_down(uint32_t hid_key)
{
	switch (hid_key) {
	case ANX_KEY_LSHIFT: case ANX_KEY_RSHIFT:
		current_modifiers |= ANX_MOD_SHIFT; break;
	case ANX_KEY_LCTRL:  case ANX_KEY_RCTRL:
		current_modifiers |= ANX_MOD_CTRL;  break;
	case ANX_KEY_LALT:   case ANX_KEY_RALT:
		current_modifiers |= ANX_MOD_ALT;   break;
	case ANX_KEY_LMETA:  case ANX_KEY_RMETA:
		current_modifiers |= ANX_MOD_META;  break;
	case ANX_KEY_CAPSLOCK:
		current_modifiers ^= ANX_MOD_CAPSLOCK; break;
	}
}

static void
update_mods_up(uint32_t hid_key)
{
	switch (hid_key) {
	case ANX_KEY_LSHIFT: case ANX_KEY_RSHIFT:
		current_modifiers &= ~ANX_MOD_SHIFT; break;
	case ANX_KEY_LCTRL:  case ANX_KEY_RCTRL:
		current_modifiers &= ~ANX_MOD_CTRL;  break;
	case ANX_KEY_LALT:   case ANX_KEY_RALT:
		current_modifiers &= ~ANX_MOD_ALT;   break;
	case ANX_KEY_LMETA:  case ANX_KEY_RMETA:
		current_modifiers &= ~ANX_MOD_META;  break;
	}
}

/* ------------------------------------------------------------------ */
/* Event posting                                                        */
/* ------------------------------------------------------------------ */

static void
post_key_event(enum anx_event_type type,
               uint32_t hid_key, uint32_t mods, uint32_t unicode)
{
	struct anx_event ev;
	anx_oid_t target;
	bool flags;

	if (!initialized)
		return;

	anx_spin_lock_irqsave(&input_lock, &flags);
	target = focused_surf;
	anx_spin_unlock_irqrestore(&input_lock, flags);

	/* Drop if no surface has focus */
	if (target.hi == 0 && target.lo == 0)
		return;

	ev.type            = type;
	ev.timestamp_ns    = 0;  /* arch_time_now() not safe from IRQ here */
	ev.target_surf     = target;
	ev.device_id       = 0;  /* keyboard device 0 */
	/* source_cell is zero (kernel-generated) */
	ev.source_cell.hi  = 0;
	ev.source_cell.lo  = 0;

	ev.data.key.keycode   = hid_key;
	ev.data.key.modifiers = mods;
	ev.data.key.unicode   = unicode;

	/* OID assigned by anx_iface_event_post */
	ev.oid.hi = 0;
	ev.oid.lo = 0;

	anx_iface_event_post(&ev);
}

/* ------------------------------------------------------------------ */
/* PS/2 entry point (called from arch kbd_irq_handler)                 */
/* ------------------------------------------------------------------ */

void
anx_input_ps2_key(uint8_t scancode, uint32_t unicode)
{
	bool is_release = (scancode & 0x80) != 0;
	uint8_t raw = scancode & 0x7F;
	uint32_t hid_key = anx_input_scancode_to_hid(raw);

	if (hid_key == ANX_KEY_NONE)
		return;

	if (is_release) {
		update_mods_up(hid_key);
		post_key_event(ANX_EVENT_KEY_UP, hid_key,
		               current_modifiers, 0);
	} else {
		update_mods_down(hid_key);
		post_key_event(ANX_EVENT_KEY_DOWN, hid_key,
		               current_modifiers, unicode);
	}
}

/* ------------------------------------------------------------------ */
/* Generic injection (USB HID, synthetic test events)                  */
/* ------------------------------------------------------------------ */

void
anx_input_key_down(uint32_t hid_key, uint32_t modifiers, uint32_t unicode)
{
	update_mods_down(hid_key);
	post_key_event(ANX_EVENT_KEY_DOWN, hid_key, modifiers, unicode);
}

void
anx_input_key_up(uint32_t hid_key, uint32_t modifiers)
{
	update_mods_up(hid_key);
	post_key_event(ANX_EVENT_KEY_UP, hid_key, modifiers, 0);
}

/* ------------------------------------------------------------------ */
/* Pointer injection                                                    */
/* ------------------------------------------------------------------ */

void
anx_input_pointer_move(int32_t x, int32_t y, uint32_t buttons)
{
	struct anx_event ev;
	anx_oid_t target;
	bool flags;

	if (!initialized)
		return;

	anx_spin_lock_irqsave(&input_lock, &flags);
	target = focused_surf;
	anx_spin_unlock_irqrestore(&input_lock, flags);

	if (target.hi == 0 && target.lo == 0)
		return;

	ev.type                  = ANX_EVENT_POINTER_MOVE;
	ev.timestamp_ns          = 0;
	ev.target_surf           = target;
	ev.source_cell.hi        = 0;
	ev.source_cell.lo        = 0;
	ev.device_id             = 0;
	ev.oid.hi                = 0;
	ev.oid.lo                = 0;
	ev.data.pointer.x        = x;
	ev.data.pointer.y        = y;
	ev.data.pointer.buttons  = buttons;
	ev.data.pointer.modifiers = current_modifiers;

	anx_iface_event_post(&ev);
}

void
anx_input_pointer_button(int32_t x, int32_t y,
                          uint32_t buttons, uint32_t modifiers)
{
	struct anx_event ev;
	anx_oid_t target;
	bool flags;

	if (!initialized)
		return;

	anx_spin_lock_irqsave(&input_lock, &flags);
	target = focused_surf;
	anx_spin_unlock_irqrestore(&input_lock, flags);

	if (target.hi == 0 && target.lo == 0)
		return;

	ev.type                   = ANX_EVENT_POINTER_BUTTON;
	ev.timestamp_ns           = 0;
	ev.target_surf            = target;
	ev.source_cell.hi         = 0;
	ev.source_cell.lo         = 0;
	ev.device_id              = 0;
	ev.oid.hi                 = 0;
	ev.oid.lo                 = 0;
	ev.data.pointer.x         = x;
	ev.data.pointer.y         = y;
	ev.data.pointer.buttons   = buttons;
	ev.data.pointer.modifiers = modifiers;

	anx_iface_event_post(&ev);
}
