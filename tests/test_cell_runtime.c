/*
 * test_cell_runtime.c — Tests for the cell runtime pipeline.
 */

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/uuid.h>
#include <anx/string.h>

int test_cell_runtime(void)
{
	struct anx_cell *cell;
	struct anx_cell *child;
	struct anx_cell_intent intent;
	int ret;

	anx_objstore_init();
	anx_cell_store_init();

	/* Create and run a simple cell through the full pipeline */
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "test_run", sizeof(intent.name));
	anx_strlcpy(intent.objective, "test full pipeline",
		     sizeof(intent.objective));

	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &cell);
	if (ret != ANX_OK)
		return -1;

	ret = anx_cell_run(cell);
	if (ret != ANX_OK)
		return -2;

	if (cell->status != ANX_CELL_COMPLETED)
		return -3;

	if (cell->attempt_count != 1)
		return -4;

	/* Trace should have been written */
	if (anx_uuid_is_nil(&cell->trace_id))
		return -5;

	/* Test child cell derivation */
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "parent", sizeof(intent.name));
	{
		struct anx_cell *parent;
		struct anx_cell_intent child_intent;

		ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &parent);
		if (ret != ANX_OK)
			return -6;

		parent->execution.allow_recursive_cells = true;

		anx_memset(&child_intent, 0, sizeof(child_intent));
		anx_strlcpy(child_intent.name, "child",
			     sizeof(child_intent.name));

		ret = anx_cell_derive_child(parent, ANX_CELL_TASK_EXECUTION,
					    &child_intent, &child);
		if (ret != ANX_OK)
			return -7;

		if (child->recursion_depth != 1)
			return -8;

		if (parent->child_count != 1)
			return -9;

		anx_cell_destroy(child);
		anx_cell_destroy(parent);
	}

	anx_cell_destroy(cell);

	/* --- DAG: fan-in dependency gating --- */
	{
		struct anx_cell *a, *b, *c, *d;
		struct anx_cell_intent i0;
		int deps;

		anx_memset(&i0, 0, sizeof(i0));
		anx_strlcpy(i0.name, "dag_a", sizeof(i0.name));
		if (anx_cell_create(ANX_CELL_TASK_EXECUTION, &i0, &a) != ANX_OK)
			return -100;
		anx_strlcpy(i0.name, "dag_b", sizeof(i0.name));
		if (anx_cell_create(ANX_CELL_TASK_EXECUTION, &i0, &b) != ANX_OK)
			return -101;
		anx_strlcpy(i0.name, "dag_c", sizeof(i0.name));
		if (anx_cell_create(ANX_CELL_TASK_EXECUTION, &i0, &c) != ANX_OK)
			return -102;
		anx_strlcpy(i0.name, "dag_d", sizeof(i0.name));
		if (anx_cell_create(ANX_CELL_TASK_EXECUTION, &i0, &d) != ANX_OK)
			return -103;

		/* D depends on A, B, C (fan-in). */
		if (anx_cell_add_dependency(d, &a->cid) != ANX_OK)
			return -104;
		if (anx_cell_add_dependency(d, &b->cid) != ANX_OK)
			return -105;
		if (anx_cell_add_dependency(d, &c->cid) != ANX_OK)
			return -106;
		if (d->dep_count != 3)
			return -107;

		/* Duplicate edge rejected. */
		if (anx_cell_add_dependency(d, &a->cid) != ANX_EINVAL)
			return -108;

		/* Self-dependency rejected. */
		if (anx_cell_add_dependency(d, &d->cid) != ANX_EINVAL)
			return -109;

		/* Cycle rejected: A depends on D, D depends on A. */
		if (anx_cell_add_dependency(a, &d->cid) != ANX_EINVAL)
			return -110;

		/* D not ready — run should return ANX_EBUSY. */
		deps = anx_cell_deps_satisfied(d);
		if (deps != 0)
			return -111;
		if (anx_cell_run(d) != ANX_EBUSY)
			return -112;
		if (d->status != ANX_CELL_CREATED)
			return -113;

		/* Complete A and B — still not ready. */
		if (anx_cell_run(a) != ANX_OK)
			return -114;
		if (anx_cell_run(b) != ANX_OK)
			return -115;
		if (anx_cell_deps_satisfied(d) != 0)
			return -116;
		if (anx_cell_run(d) != ANX_EBUSY)
			return -117;

		/* Complete C — now all satisfied. */
		if (anx_cell_run(c) != ANX_OK)
			return -118;
		if (anx_cell_deps_satisfied(d) != 1)
			return -119;
		if (anx_cell_run(d) != ANX_OK)
			return -120;
		if (d->status != ANX_CELL_COMPLETED)
			return -121;

		anx_cell_destroy(d);
		anx_cell_destroy(c);
		anx_cell_destroy(b);
		anx_cell_destroy(a);
	}

	/* --- DAG: failed predecessor propagates --- */
	{
		struct anx_cell *p, *s;
		struct anx_cell_intent i0;

		anx_memset(&i0, 0, sizeof(i0));
		anx_strlcpy(i0.name, "fail_p", sizeof(i0.name));
		if (anx_cell_create(ANX_CELL_TASK_EXECUTION, &i0, &p) != ANX_OK)
			return -130;
		anx_strlcpy(i0.name, "fail_s", sizeof(i0.name));
		if (anx_cell_create(ANX_CELL_TASK_EXECUTION, &i0, &s) != ANX_OK)
			return -131;

		if (anx_cell_add_dependency(s, &p->cid) != ANX_OK)
			return -132;

		/* Force p into FAILED without running. */
		if (anx_cell_transition(p, ANX_CELL_ADMITTED) != ANX_OK)
			return -133;
		if (anx_cell_transition(p, ANX_CELL_PLANNING) != ANX_OK)
			return -134;
		if (anx_cell_transition(p, ANX_CELL_FAILED) != ANX_OK)
			return -135;

		/* s must fail because p failed. */
		if (anx_cell_deps_satisfied(s) != ANX_EPERM)
			return -136;
		if (anx_cell_run(s) != ANX_EPERM)
			return -137;
		if (s->status != ANX_CELL_FAILED)
			return -138;

		anx_cell_destroy(s);
		anx_cell_destroy(p);
	}

	return 0;
}
