/*
 * anx/input.h — Kernel input abstraction layer.
 *
 * Bridges raw arch-level input (PS/2, USB HID, touch) to the Interface
 * Plane event queue. The focus surface receives ANX_EVENT_KEY_* and
 * ANX_EVENT_POINTER_* events via anx_iface_event_post().
 *
 * Keycode set is USB HID Usage Table page 0x07 (Keyboard/Keypad).
 * Low values (1-127) match the HID usage IDs. High values (0x100+)
 * are Anunix extensions for non-HID devices.
 */

#ifndef ANX_INPUT_H
#define ANX_INPUT_H

#include <anx/types.h>
#include <anx/interface_plane.h>

/* ------------------------------------------------------------------ */
/* Modifier flags                                                       */
/* ------------------------------------------------------------------ */

#define ANX_MOD_SHIFT    (1u << 0)
#define ANX_MOD_CTRL     (1u << 1)
#define ANX_MOD_ALT      (1u << 2)
#define ANX_MOD_META     (1u << 3)  /* Super / Win / Cmd */
#define ANX_MOD_CAPSLOCK (1u << 4)
#define ANX_MOD_NUMLOCK  (1u << 5)

/* ------------------------------------------------------------------ */
/* USB HID keycodes (page 0x07 subset)                                  */
/* ------------------------------------------------------------------ */

#define ANX_KEY_NONE        0x00
#define ANX_KEY_A           0x04
#define ANX_KEY_B           0x05
#define ANX_KEY_C           0x06
#define ANX_KEY_D           0x07
#define ANX_KEY_E           0x08
#define ANX_KEY_F           0x09
#define ANX_KEY_G           0x0A
#define ANX_KEY_H           0x0B
#define ANX_KEY_I           0x0C
#define ANX_KEY_J           0x0D
#define ANX_KEY_K           0x0E
#define ANX_KEY_L           0x0F
#define ANX_KEY_M           0x10
#define ANX_KEY_N           0x11
#define ANX_KEY_O           0x12
#define ANX_KEY_P           0x13
#define ANX_KEY_Q           0x14
#define ANX_KEY_R           0x15
#define ANX_KEY_S           0x16
#define ANX_KEY_T           0x17
#define ANX_KEY_U           0x18
#define ANX_KEY_V           0x19
#define ANX_KEY_W           0x1A
#define ANX_KEY_X           0x1B
#define ANX_KEY_Y           0x1C
#define ANX_KEY_Z           0x1D

#define ANX_KEY_1           0x1E
#define ANX_KEY_2           0x1F
#define ANX_KEY_3           0x20
#define ANX_KEY_4           0x21
#define ANX_KEY_5           0x22
#define ANX_KEY_6           0x23
#define ANX_KEY_7           0x24
#define ANX_KEY_8           0x25
#define ANX_KEY_9           0x26
#define ANX_KEY_0           0x27

#define ANX_KEY_ENTER       0x28
#define ANX_KEY_ESC         0x29
#define ANX_KEY_BACKSPACE   0x2A
#define ANX_KEY_TAB         0x2B
#define ANX_KEY_SPACE       0x2C
#define ANX_KEY_MINUS       0x2D
#define ANX_KEY_EQUAL       0x2E
#define ANX_KEY_LBRACKET    0x2F
#define ANX_KEY_RBRACKET    0x30
#define ANX_KEY_BACKSLASH   0x31
#define ANX_KEY_SEMICOLON   0x33
#define ANX_KEY_APOSTROPHE  0x34
#define ANX_KEY_GRAVE       0x35
#define ANX_KEY_COMMA       0x36
#define ANX_KEY_DOT         0x37
#define ANX_KEY_SLASH       0x38
#define ANX_KEY_CAPSLOCK    0x39

#define ANX_KEY_F1          0x3A
#define ANX_KEY_F2          0x3B
#define ANX_KEY_F3          0x3C
#define ANX_KEY_F4          0x3D
#define ANX_KEY_F5          0x3E
#define ANX_KEY_F6          0x3F
#define ANX_KEY_F7          0x40
#define ANX_KEY_F8          0x41
#define ANX_KEY_F9          0x42
#define ANX_KEY_F10         0x43
#define ANX_KEY_F11         0x44
#define ANX_KEY_F12         0x45

#define ANX_KEY_PRINTSCREEN 0x46
#define ANX_KEY_SCROLLLOCK  0x47
#define ANX_KEY_PAUSE       0x48
#define ANX_KEY_INSERT      0x49
#define ANX_KEY_HOME        0x4A
#define ANX_KEY_PAGEUP      0x4B
#define ANX_KEY_DELETE      0x4C
#define ANX_KEY_END         0x4D
#define ANX_KEY_PAGEDOWN    0x4E
#define ANX_KEY_RIGHT       0x4F
#define ANX_KEY_LEFT        0x50
#define ANX_KEY_DOWN        0x51
#define ANX_KEY_UP          0x52

#define ANX_KEY_LCTRL       0xE0
#define ANX_KEY_LSHIFT      0xE1
#define ANX_KEY_LALT        0xE2
#define ANX_KEY_LMETA       0xE3
#define ANX_KEY_RCTRL       0xE4
#define ANX_KEY_RSHIFT      0xE5
#define ANX_KEY_RALT        0xE6
#define ANX_KEY_RMETA       0xE7

/* ------------------------------------------------------------------ */
/* PS/2 scancode set 1 → HID keycode table (exported for arch use)    */
/* ------------------------------------------------------------------ */

/* Returns ANX_KEY_NONE (0) for unknown scancodes */
uint32_t anx_input_scancode_to_hid(uint8_t scancode);

/* ------------------------------------------------------------------ */
/* Input subsystem API                                                  */
/* ------------------------------------------------------------------ */

/* Initialize input subsystem (called from anx_iface_init) */
int  anx_input_init(void);

/* Set which surface receives keyboard events.
 * Pass a zero OID to clear focus (events discarded). */
void anx_input_focus_set(anx_oid_t surf_oid);
anx_oid_t anx_input_focus_get(void);

/* Called by arch keyboard IRQ handler on every key press/release.
 *   scancode — PS/2 set-1 scancode (raw, including 0x80 release bit)
 *   unicode  — resolved character (0 if non-printable) */
void anx_input_ps2_key(uint8_t scancode, uint32_t unicode);

/* Input telemetry for deterministic routing tests and diagnostics. */
struct anx_input_stats {
	uint64_t delivered;
	uint64_t dropped_no_focus;
	uint64_t last_timestamp_ns;
};

void anx_input_stats_get(struct anx_input_stats *out);
void anx_input_stats_reset(void);

/* Generic key event injection (USB HID, synthetic) */
void anx_input_key_down(uint32_t hid_key, uint32_t modifiers, uint32_t unicode);
void anx_input_key_up(uint32_t hid_key, uint32_t modifiers);

/* Pointer event injection */
void anx_input_pointer_move(int32_t x, int32_t y, uint32_t buttons);
void anx_input_pointer_button(int32_t x, int32_t y,
                               uint32_t buttons, uint32_t modifiers);

#endif /* ANX_INPUT_H */
