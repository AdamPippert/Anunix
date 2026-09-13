/*
 * a11y.c — Accessibility object model (P2-001).
 */

#include <anx/a11y.h>
#include <anx/input.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/arch.h>
#include <anx/types.h>
#include <anx/cell.h>
#include <anx/identity.h>
#include <anx/uuid.h>

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
static uint64_t              observation_generation = 1;

static struct anx_a11y_node *find_node(uint32_t id);
static void push_event(enum anx_a11y_event_type type, uint32_t node_id);

static bool valid_node(const struct anx_a11y_node *node)
{
	if (!node || !node->id || (int)node->role < 0 || node->role >= ANX_A11Y_ROLE_COUNT)
		return false;
	for (uint32_t i = 0; i < sizeof(node->name); i++)
		if (!node->name[i])
			return true;
	return false;
}

int anx_a11y_node_update(const struct anx_a11y_node *node)
{
	struct anx_a11y_node *current;
	bool flags;
	int ret = ANX_OK;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!valid_node(node))
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&a11y_lock, &flags);
	current = find_node(node->id);
	if (!current)
		ret = ANX_ENOENT;
	else if (observation_generation == ~(uint64_t)0)
		ret = ANX_EFULL;
	else {
		*current = *node;
		current->active = true;
		observation_generation++;
	}
	anx_spin_unlock_irqrestore(&a11y_lock, flags);
	return ret;
}

int anx_a11y_observe(uint32_t node_id, struct anx_a11y_observation *out)
{
	struct anx_a11y_node *current;
	bool flags;
	if (!out || !node_id)
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&a11y_lock, &flags);
	current = find_node(node_id);
	if (current) {
		anx_memset(out, 0, sizeof(*out));
		out->generation = observation_generation;
		out->node = *current;
	}
	anx_spin_unlock_irqrestore(&a11y_lock, flags);
	return current ? ANX_OK : ANX_ENOENT;
}

int anx_a11y_action_checked(uint32_t node_id, uint64_t generation,
			    enum anx_a11y_action action, struct anx_a11y_receipt *out)
{
	const anx_cid_t *active = anx_cell_current_id();
	struct anx_cell *caller;
	struct anx_a11y_node *node;
	struct anx_surface *surface;
	struct anx_a11y_receipt receipt = {0};
	anx_oid_t focus;
	bool flags;
	int ret = ANX_EPERM;
	if (!out || !node_id || !generation || (int)action < 0 || action > ANX_A11Y_ACTION_SCROLL_DOWN)
		return ANX_EINVAL;
	/* Activation events alone do not prove a click or scroll reached an application. */
	if (action != ANX_A11Y_ACTION_FOCUS)
		return ANX_ENOTSUP;
	if (!active)
		return ANX_EPERM;
	caller = anx_cell_store_lookup(active);
	if (!caller)
		return ANX_EPERM;
	if (!caller->execution.allow_side_effects || anx_cell_status_terminal(caller->status) ||
	    anx_identity_admit(caller, NULL) != ANX_OK)
		goto release;
	anx_spin_lock_irqsave(&a11y_lock, &flags);
	if (generation != observation_generation) {
		ret = ANX_EBUSY;
		goto unlock;
	}
	if (observation_generation == ~(uint64_t)0) {
		ret = ANX_EFULL;
		goto unlock;
	}
	node = find_node(node_id);
	if (!node) {
		ret = ANX_ENOENT;
		goto unlock;
	}
	if (anx_uuid_is_nil(&node->action_principal) ||
	    anx_uuid_compare(&node->action_principal, active) ||
	    !node->visible || !node->enabled || !node->focusable)
		goto unlock;
	ret = anx_iface_surface_lookup(node->surf_oid, &surface);
	if (ret != ANX_OK)
		goto unlock;
	if (surface->state != ANX_SURF_VISIBLE) {
		ret = ANX_EPERM;
		goto unlock;
	}
	anx_input_focus_set(node->surf_oid);
	focus = anx_input_focus_get();
	observation_generation++;
	if (anx_uuid_compare(&focus, &node->surf_oid)) {
		ret = ANX_EIO;
		goto unlock;
	}
	push_event(ANX_A11Y_EVENT_FOCUS_CHANGED, node_id);
	receipt.observation_generation = generation;
	receipt.resulting_generation = observation_generation;
	receipt.focused_surface = focus;
	receipt.verified_focus = true;
	*out = receipt;
	ret = ANX_OK;
unlock:
	anx_spin_unlock_irqrestore(&a11y_lock, flags);
release:
	anx_cell_store_release(caller);
	return ret;
}

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
	if (anx_cell_current_id())
		return;
	anx_spin_init(&a11y_lock);
	anx_memset(tree,         0, sizeof(tree));
	anx_memset(event_stream, 0, sizeof(event_stream));
	tree_count  = 0;
	event_head  = 0;
	event_count = 0;
	/* A reset cannot make an earlier observation current again. */
	if (observation_generation != ~(uint64_t)0)
		observation_generation++;
}

int
anx_a11y_node_add(const struct anx_a11y_node *node)
{
	uint32_t i;
	bool flags;

	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!valid_node(node))
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&a11y_lock, &flags);

	/* duplicate check */
	if (find_node(node->id)) {
		anx_spin_unlock_irqrestore(&a11y_lock, flags);
		return ANX_EBUSY;
	}
	if (observation_generation == ~(uint64_t)0) {
		anx_spin_unlock_irqrestore(&a11y_lock, flags);
		return ANX_EFULL;
	}

	for (i = 0; i < ANX_A11Y_TREE_MAX; i++) {
		if (!tree[i].active) {
			tree[i] = *node;
			tree[i].active = true;
			tree_count++;
			observation_generation++;
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
	if (anx_cell_current_id())
		return ANX_EPERM;

	anx_spin_lock_irqsave(&a11y_lock, &flags);

	n = find_node(id);
	if (!n) {
		anx_spin_unlock_irqrestore(&a11y_lock, flags);
		return ANX_ENOENT;
	}

	n->active = false;
	tree_count--;
	if (observation_generation != ~(uint64_t)0)
		observation_generation++;
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
	if (anx_cell_current_id())
		return ANX_EPERM;
	if ((int)action < 0 || action > ANX_A11Y_ACTION_SCROLL_DOWN)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&a11y_lock, &flags);

	n = find_node(node_id);
	if (!n) {
		anx_spin_unlock_irqrestore(&a11y_lock, flags);
		return ANX_ENOENT;
	}
	if (observation_generation == ~(uint64_t)0) {
		anx_spin_unlock_irqrestore(&a11y_lock, flags);
		return ANX_EFULL;
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
	observation_generation++;

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
	if (anx_cell_current_id())
		return;

	anx_spin_lock_irqsave(&a11y_lock, &flags);
	push_event(ANX_A11Y_EVENT_FOCUS_CHANGED, node_id);
	if (observation_generation != ~(uint64_t)0)
		observation_generation++;
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
