/*
 * forms.h — HTML form element state and editing operations.
 *
 * The forms subsystem tracks every focusable form element on the current
 * page, its on-screen bounding box, and its current value.  Two UTF-8 byte
 * offsets implement cursor and selection without multi-byte encoding issues.
 *
 * Lifecycle:
 *   form_state_reset()   — call on every navigate (clears all fields)
 *   form_field_register()— called by layout walker for each form element
 *   form_click()         — dispatch a pointer click to the forms subsystem
 *   form_key()           — dispatch a key event to the focused field
 *   form_collect()       — build a URL-encoded query string for submission
 */

#ifndef ANX_BROWSER_FORMS_H
#define ANX_BROWSER_FORMS_H

#include <anx/types.h>

/* ── Field types ─────────────────────────────────────────────────── */

#define FORM_TYPE_TEXT      0   /* <input type="text|email|search"> */
#define FORM_TYPE_PASSWORD  1   /* <input type="password">          */
#define FORM_TYPE_SUBMIT    2   /* <input type="submit"> / <button> */
#define FORM_TYPE_RESET     3   /* <input type="reset">             */
#define FORM_TYPE_CHECKBOX  4   /* <input type="checkbox">          */
#define FORM_TYPE_RADIO     5   /* <input type="radio">             */
#define FORM_TYPE_TEXTAREA  6   /* <textarea>                       */
#define FORM_TYPE_SELECT    7   /* <select>                         */
#define FORM_TYPE_HIDDEN    8   /* <input type="hidden">            */

#define FORM_FIELDS_MAX  32
#define FORM_VALUE_MAX  512
#define FORM_NAME_MAX    64

struct form_field {
	uint8_t  type;               /* FORM_TYPE_* */
	bool     active;             /* true if this slot is in use */
	bool     focused;
	bool     checked;            /* checkbox / radio state */

	/* Field identity */
	char     name[FORM_NAME_MAX];
	char     value[FORM_VALUE_MAX];  /* editable text or selected option */
	char     placeholder[FORM_NAME_MAX];
	char     label[FORM_VALUE_MAX];  /* display text for buttons */

	/* Text-editing state (UTF-8 byte offsets) */
	uint32_t cursor_pos;         /* byte offset of the insert point */
	uint32_t sel_anchor;         /* other end of selection (=cursor if no sel) */

	/* Screen bounding box (set during layout) */
	int32_t  bbox_x, bbox_y;
	uint32_t bbox_w, bbox_h;

	/* Form association */
	uint32_t form_idx;           /* index of enclosing <form> (0 = none) */
};

struct form_state {
	struct form_field fields[FORM_FIELDS_MAX];
	uint32_t          n_fields;
	int32_t           focused_idx;   /* -1 = none */
};

/* ── Lifecycle ────────────────────────────────────────────────────── */

void form_state_reset(struct form_state *fs);

/*
 * Register a form element discovered during layout.
 * Returns index of the registered field, or -1 if capacity reached.
 */
int32_t form_field_register(struct form_state *fs,
			      uint8_t type,
			      const char *name,
			      const char *value,
			      const char *placeholder,
			      const char *label,
			      bool checked,
			      int32_t bbox_x, int32_t bbox_y,
			      uint32_t bbox_w, uint32_t bbox_h);

/* ── Interaction ─────────────────────────────────────────────────── */

/*
 * Hit-test (x, y) against all fields.
 * Returns the field index that was hit (may have changed focus),
 * or -1 if no field was hit.
 */
int32_t form_click(struct form_state *fs, int32_t x, int32_t y);

/*
 * Deliver a key event to the currently focused field.
 * key:  UTF-8 string of the key value (e.g. "a", "A", "Backspace",
 *       "ArrowLeft", "Enter", "Tab").
 * Returns true if the form field consumed the key.
 */
bool form_key(struct form_state *fs, const char *key);

/*
 * Advance focus to the next (or previous) field.
 */
void form_focus_next(struct form_state *fs, bool reverse);

/* ── Data collection ─────────────────────────────────────────────── */

/*
 * Write URL-encoded form data into buf (e.g. "name=John&email=j%40x.com").
 * Only fields with non-empty names are included; checkboxes are included
 * only when checked.
 * Returns number of bytes written (not including NUL).
 */
size_t form_collect(const struct form_state *fs,
		     char *buf, size_t cap);

/*
 * If the focused field is inside a <form>, return the form's action URL
 * and method.  Writes into action_buf / method_buf.
 * Returns true if a submit action should be triggered.
 */
bool form_submit_action(const struct form_state *fs,
			  char *action_buf, size_t action_cap,
			  char *method_buf, size_t method_cap);

#endif /* ANX_BROWSER_FORMS_H */
