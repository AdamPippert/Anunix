/*
 * anx/a11y.h — Accessibility object model (P2-001).
 *
 * Provides the accessibility tree, assistive action injection, and
 * focus-narration event stream for assistive technology integration.
 */

#ifndef ANX_A11Y_H
#define ANX_A11Y_H

#include <anx/types.h>
#include <anx/interface_plane.h>

/* ------------------------------------------------------------------ */
/* Accessibility tree                                                   */
/* ------------------------------------------------------------------ */

#define ANX_A11Y_NAME_MAX   128u
#define ANX_A11Y_TREE_MAX    64u

enum anx_a11y_role {
	ANX_A11Y_ROLE_WINDOW,
	ANX_A11Y_ROLE_BUTTON,
	ANX_A11Y_ROLE_TEXT,
	ANX_A11Y_ROLE_INPUT,
	ANX_A11Y_ROLE_LIST,
	ANX_A11Y_ROLE_LISTITEM,
	ANX_A11Y_ROLE_GENERIC,
	ANX_A11Y_ROLE_COUNT,
};

struct anx_a11y_node {
	uint32_t         id;           /* unique within the tree (1-based) */
	uint32_t         parent_id;    /* 0 = root                         */
	enum anx_a11y_role role;
	char             name[ANX_A11Y_NAME_MAX];
	anx_oid_t        surf_oid;     /* associated surface (may be NIL)  */
	bool             focusable;
	bool             active;
};

/* ------------------------------------------------------------------ */
/* Assistive actions                                                    */
/* ------------------------------------------------------------------ */

enum anx_a11y_action {
	ANX_A11Y_ACTION_CLICK,
	ANX_A11Y_ACTION_FOCUS,
	ANX_A11Y_ACTION_SCROLL_UP,
	ANX_A11Y_ACTION_SCROLL_DOWN,
};

/* ------------------------------------------------------------------ */
/* Focus-narration event stream                                         */
/* ------------------------------------------------------------------ */

#define ANX_A11Y_EVENT_STREAM_MAX  32u

enum anx_a11y_event_type {
	ANX_A11Y_EVENT_FOCUS_CHANGED,
	ANX_A11Y_EVENT_NODE_ACTIVATED,
	ANX_A11Y_EVENT_NODE_ADDED,
	ANX_A11Y_EVENT_NODE_REMOVED,
};

struct anx_a11y_event {
	enum anx_a11y_event_type type;
	uint32_t                 node_id;
	uint64_t                 timestamp_ns;
	bool                     active;
};

/* ------------------------------------------------------------------ */
/* Accessibility API                                                    */
/* ------------------------------------------------------------------ */

/* Initialise accessibility subsystem. */
void anx_a11y_init(void);

/* Add a node to the accessibility tree.
 * id must be unique and > 0; parent_id = 0 for root. */
int anx_a11y_node_add(const struct anx_a11y_node *node);

/* Remove a node by id. */
int anx_a11y_node_remove(uint32_t id);

/* Look up a node by id; fills *out if found. */
int anx_a11y_node_get(uint32_t id, struct anx_a11y_node *out);

/* Perform an assistive action on a node.
 * For CLICK: synthesises ANX_EVENT_POINTER_BUTTON on the node's surface.
 * For FOCUS: sets input focus to the node's surface. */
int anx_a11y_action(uint32_t node_id, enum anx_a11y_action action);

/* Notify the accessibility stream of a focus change. */
void anx_a11y_notify_focus(uint32_t node_id);

/* Poll the next a11y event from the stream.
 * Returns ANX_OK (fills *out) or ANX_ENOENT if empty. */
int anx_a11y_event_poll(struct anx_a11y_event *out);

/* Return number of events in the stream. */
uint32_t anx_a11y_event_depth(void);

#endif /* ANX_A11Y_H */
