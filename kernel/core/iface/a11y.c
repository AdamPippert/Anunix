/*
 * a11y.c — Accessibility object model (P2-001).
 */

#include <anx/a11y.h>
#include <anx/input.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/arch.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Accessibility tree                                                   */
/* ------------------------------------------------------------------ */

static struct anx_a11y_node tree[ANX_A11Y_TREE_MAX];
static uint32_t             tree_count;
static struct anx_spinlock  a11y_lock;

/* ------------------------------------------------------------------ */
/* Event stream (ring buffer)                                           */
/* ------------------------------------------------------------------ */

static struct anx_a11y_event event_stream[ANX_A11Y_EVENT_STREAM_MAX];
static uint32_t              event_head;   /* next write slot */
static uint32_t              event_count;  /* events available to read */

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static struct anx_a11y_node *
find_node(uint32_t id)
{
	uint32_t i;

	for (i = 0; i < ANX_A11Y_TREE_MAX; i++) {
		if (tree[i].active && tree[i].id == id)
			return &tree[i];
	}
	return NULL;
}

static void
push_event(enum anx_a11y_event_type type, uint32_t node_id)
{
	struct anx_a11y_event *ev;
	uint32_t slot;

	slot = event_head % ANX_A11Y_EVENT_STREAM_MAX;
	ev   = &event_stream[slot];

	ev->type         = type;
	ev->node_id      = node_id;
	ev->timestamp_ns = arch_time_now();
	ev->active       = true;

	event_head++;
	if (event_count < ANX_A11Y_EVENT_STREAM_MAX)
		event_count++;
}

/* ------------------------------------------------------------------ */
/* Tree operations                                                      */
/* ------------------------------------------------------------------ */

void
anx_a11y_init(void)
{
	anx_spin_init(&a11y_lock);
	anx_memset(tree,         0, sizeof(tree));
	anx_memset(event_stream, 0, sizeof(event_stream));
	tree_count  = 0;
	event_head  = 0;
	event_count = 0;
}

int
anx_a11y_node_add(const struct anx_a11y_node *node)
{
	uint32_t i;
	bool flags;

	if (!node || node->id == 0)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&a11y_lock, &flags);

	/* duplicate check */
	if (find_node(node->id)) {
		anx_spin_unlock_irqrestore(&a11y_lock, flags);
		return ANX_EBUSY;
	}

	for (i = 0; i < ANX_A11Y_TREE_MAX; i++) {
		if (!tree[i].active) {
			tree[i] = *node;
			tree[i].active = true;
			tree_count++;
			push_event(ANX_A11Y_EVENT_NODE_ADDED, node->id);
			anx_spin_unlock_irqrestore(&a11y_lock, flags);
			return ANX_OK;
		}
	}

	anx_spin_unlock_irqrestore(&a11y_lock, flags);
	return ANX_EFULL;
}

int
anx_a11y_node_remove(uint32_t id)
{
	struct anx_a11y_node *n;
	bool flags;

	anx_spin_lock_irqsave(&a11y_lock, &flags);

	n = find_node(id);
	if (!n) {
		anx_spin_unlock_irqrestore(&a11y_lock, flags);
		return ANX_ENOENT;
	}

	n->active = false;
	tree_count--;
	push_event(ANX_A11Y_EVENT_NODE_REMOVED, id);

	anx_spin_unlock_irqrestore(&a11y_lock, flags);
	return ANX_OK;
}

int
anx_a11y_node_get(uint32_t id, struct anx_a11y_node *out)
{
	struct anx_a11y_node *n;
	bool flags;

	if (!out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&a11y_lock, &flags);

	n = find_node(id);
	if (!n) {
		anx_spin_unlock_irqrestore(&a11y_lock, flags);
		return ANX_ENOENT;
	}

	*out = *n;
	anx_spin_unlock_irqrestore(&a11y_lock, flags);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Assistive actions                                                    */
/* ------------------------------------------------------------------ */

int
anx_a11y_action(uint32_t node_id, enum anx_a11y_action action)
{
	struct anx_a11y_node *n;
	bool flags;

	anx_spin_lock_irqsave(&a11y_lock, &flags);

	n = find_node(node_id);
	if (!n) {
		anx_spin_unlock_irqrestore(&a11y_lock, flags);
		return ANX_ENOENT;
	}

	switch (action) {
	case ANX_A11Y_ACTION_CLICK:
		push_event(ANX_A11Y_EVENT_NODE_ACTIVATED, node_id);
		break;
	case ANX_A11Y_ACTION_FOCUS:
		if (n->focusable) {
			push_event(ANX_A11Y_EVENT_FOCUS_CHANGED, node_id);
			anx_input_focus_set(n->surf_oid);
		}
		break;
	case ANX_A11Y_ACTION_SCROLL_UP:
	case ANX_A11Y_ACTION_SCROLL_DOWN:
		/* synthesise scroll — recorded as node activation for now */
		push_event(ANX_A11Y_EVENT_NODE_ACTIVATED, node_id);
		break;
	}

	anx_spin_unlock_irqrestore(&a11y_lock, flags);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Focus narration stream                                               */
/* ------------------------------------------------------------------ */

void
anx_a11y_notify_focus(uint32_t node_id)
{
	bool flags;

	anx_spin_lock_irqsave(&a11y_lock, &flags);
	push_event(ANX_A11Y_EVENT_FOCUS_CHANGED, node_id);
	anx_spin_unlock_irqrestore(&a11y_lock, flags);
}

int
anx_a11y_event_poll(struct anx_a11y_event *out)
{
	uint32_t read_slot;
	bool flags;

	if (!out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&a11y_lock, &flags);

	if (event_count == 0) {
		anx_spin_unlock_irqrestore(&a11y_lock, flags);
		return ANX_ENOENT;
	}

	/* oldest event is at (event_head - event_count) mod STREAM_MAX */
	read_slot = (event_head - event_count) % ANX_A11Y_EVENT_STREAM_MAX;
	*out = event_stream[read_slot];
	event_stream[read_slot].active = false;
	event_count--;

	anx_spin_unlock_irqrestore(&a11y_lock, flags);
	return ANX_OK;
}

uint32_t
anx_a11y_event_depth(void)
{
	bool flags;
	uint32_t n;

	anx_spin_lock_irqsave(&a11y_lock, &flags);
	n = event_count;
	anx_spin_unlock_irqrestore(&a11y_lock, flags);
	return n;
}
