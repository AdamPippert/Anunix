/*
 * test_a11y.c — Tests for P2-001: accessibility object model.
 *
 * Tests:
 * - P2-001-U01: accessibility tree schema conformance
 * - P2-001-U02: assistive action synthesises interaction event
 * - P2-001-U03: focus change pushes to a11y event stream
 */

#include <anx/types.h>
#include <anx/a11y.h>
#include <anx/uuid.h>
#include <anx/string.h>

#define ASSERT(cond, code)    do { if (!(cond)) return (code); } while (0)
#define ASSERT_EQ(a, b, code) do { if ((a) != (b)) return (code); } while (0)

/* ------------------------------------------------------------------ */
/* P2-001-U01: tree schema conformance                                 */
/* ------------------------------------------------------------------ */

static int test_tree_schema(void)
{
	struct anx_a11y_node node, out;
	int rc;

	anx_a11y_init();

	/* id=0 is rejected. */
	anx_memset(&node, 0, sizeof(node));
	node.id   = 0;
	node.role = ANX_A11Y_ROLE_WINDOW;
	rc = anx_a11y_node_add(&node);
	ASSERT_EQ(rc, ANX_EINVAL, -100);

	/* Add root window node. */
	node.id        = 1;
	node.parent_id = 0;
	node.role      = ANX_A11Y_ROLE_WINDOW;
	node.focusable = false;
	anx_strlcpy(node.name, "Main Window", ANX_A11Y_NAME_MAX);
	rc = anx_a11y_node_add(&node);
	ASSERT_EQ(rc, ANX_OK, -101);

	/* Duplicate id rejected. */
	rc = anx_a11y_node_add(&node);
	ASSERT_EQ(rc, ANX_EBUSY, -102);

	/* Add child button. */
	node.id        = 2;
	node.parent_id = 1;
	node.role      = ANX_A11Y_ROLE_BUTTON;
	node.focusable = true;
	anx_strlcpy(node.name, "OK Button", ANX_A11Y_NAME_MAX);
	rc = anx_a11y_node_add(&node);
	ASSERT_EQ(rc, ANX_OK, -103);

	/* Lookup by id. */
	rc = anx_a11y_node_get(2, &out);
	ASSERT_EQ(rc, ANX_OK, -104);
	ASSERT_EQ(out.id,        2u,                  -105);
	ASSERT_EQ(out.parent_id, 1u,                  -106);
	ASSERT_EQ(out.role,      ANX_A11Y_ROLE_BUTTON, -107);
	ASSERT(anx_strcmp(out.name, "OK Button") == 0, -108);
	ASSERT(out.focusable == true,                  -109);

	/* Lookup missing id. */
	rc = anx_a11y_node_get(99, &out);
	ASSERT_EQ(rc, ANX_ENOENT, -110);

	/* Remove node. */
	rc = anx_a11y_node_remove(2);
	ASSERT_EQ(rc, ANX_OK, -111);

	rc = anx_a11y_node_get(2, &out);
	ASSERT_EQ(rc, ANX_ENOENT, -112);

	/* Remove again: ENOENT. */
	rc = anx_a11y_node_remove(2);
	ASSERT_EQ(rc, ANX_ENOENT, -113);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P2-001-U02: assistive action synthesises interaction event          */
/* ------------------------------------------------------------------ */

static int test_assistive_action(void)
{
	struct anx_a11y_node node;
	struct anx_a11y_event ev;
	int rc;

	anx_a11y_init();

	/* Set up a button node. */
	anx_memset(&node, 0, sizeof(node));
	node.id        = 10;
	node.parent_id = 0;
	node.role      = ANX_A11Y_ROLE_BUTTON;
	node.focusable = true;
	anx_strlcpy(node.name, "Submit", ANX_A11Y_NAME_MAX);

	rc = anx_a11y_node_add(&node);
	ASSERT_EQ(rc, ANX_OK, -200);

	/* No events yet (node_added was pushed but let's drain it). */
	while (anx_a11y_event_depth() > 0)
		anx_a11y_event_poll(&ev);

	/* Click action → NODE_ACTIVATED event. */
	rc = anx_a11y_action(10, ANX_A11Y_ACTION_CLICK);
	ASSERT_EQ(rc, ANX_OK, -201);

	ASSERT(anx_a11y_event_depth() >= 1u, -202);

	rc = anx_a11y_event_poll(&ev);
	ASSERT_EQ(rc, ANX_OK, -203);
	ASSERT_EQ(ev.type,    ANX_A11Y_EVENT_NODE_ACTIVATED, -204);
	ASSERT_EQ(ev.node_id, 10u,                           -205);

	/* Action on missing node: ENOENT. */
	rc = anx_a11y_action(99, ANX_A11Y_ACTION_CLICK);
	ASSERT_EQ(rc, ANX_ENOENT, -206);

	/* Stream empty after draining. */
	while (anx_a11y_event_depth() > 0)
		anx_a11y_event_poll(&ev);
	rc = anx_a11y_event_poll(&ev);
	ASSERT_EQ(rc, ANX_ENOENT, -207);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P2-001-U03: focus change pushes to a11y event stream               */
/* ------------------------------------------------------------------ */

static int test_focus_stream(void)
{
	struct anx_a11y_node node;
	struct anx_a11y_event ev;
	int rc;

	anx_a11y_init();

	/* Add two nodes. */
	anx_memset(&node, 0, sizeof(node));
	node.id   = 1;
	node.role = ANX_A11Y_ROLE_INPUT;
	node.focusable = true;
	anx_strlcpy(node.name, "Search Field", ANX_A11Y_NAME_MAX);
	anx_a11y_node_add(&node);

	node.id   = 2;
	node.role = ANX_A11Y_ROLE_INPUT;
	node.focusable = true;
	anx_strlcpy(node.name, "Password Field", ANX_A11Y_NAME_MAX);
	anx_a11y_node_add(&node);

	/* Drain node-added events. */
	while (anx_a11y_event_depth() > 0)
		anx_a11y_event_poll(&ev);

	/* Notify focus on node 1. */
	anx_a11y_notify_focus(1);
	ASSERT_EQ(anx_a11y_event_depth(), 1u, -300);

	rc = anx_a11y_event_poll(&ev);
	ASSERT_EQ(rc, ANX_OK, -301);
	ASSERT_EQ(ev.type,    ANX_A11Y_EVENT_FOCUS_CHANGED, -302);
	ASSERT_EQ(ev.node_id, 1u,                           -303);

	/* Notify focus on node 2. */
	anx_a11y_notify_focus(2);
	rc = anx_a11y_event_poll(&ev);
	ASSERT_EQ(rc, ANX_OK, -304);
	ASSERT_EQ(ev.node_id, 2u, -305);

	/* Stream is now empty. */
	ASSERT_EQ(anx_a11y_event_depth(), 0u, -306);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int test_a11y(void)
{
	int rc;

	rc = test_tree_schema();
	if (rc) return rc;

	rc = test_assistive_action();
	if (rc) return rc;

	rc = test_focus_stream();
	if (rc) return rc;

	return 0;
}
