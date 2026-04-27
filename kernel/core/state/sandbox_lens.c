/*
 * sandbox_lens.c — Active access filter for sandboxed cells.
 *
 * The active lens is held in a single static pointer.  Sandboxed code
 * runs inside install/restore brackets that snapshot the prior lens to
 * a caller-owned save buffer; nested sandboxes restore correctly so
 * long as restore order matches install order.
 *
 * This works under the cooperative single-threaded execution model used
 * during Phase 1.  When per-CPU/per-cell lens storage is needed it
 * replaces the static here without changing call sites.
 */

#include <anx/types.h>
#include <anx/sandbox_lens.h>
#include <anx/object_group.h>
#include <anx/string.h>
#include <anx/kprintf.h>

static struct anx_sandbox_lens *active_lens;

int anx_sandbox_lens_install(struct anx_sandbox_lens_save *save,
			     struct anx_sandbox_lens *lens)
{
	if (!save)
		return ANX_EINVAL;

	save->prev = active_lens;
	save->active = true;
	active_lens = lens;
	return ANX_OK;
}

void anx_sandbox_lens_restore(const struct anx_sandbox_lens_save *save)
{
	if (!save || !save->active)
		return;
	active_lens = save->prev;
}

struct anx_sandbox_lens *anx_sandbox_lens_active(void)
{
	return active_lens;
}

static bool oid_eq(const anx_oid_t *a, const anx_oid_t *b)
{
	return a->hi == b->hi && a->lo == b->lo;
}

bool anx_sandbox_lens_check(const anx_oid_t *oid, uint32_t op)
{
	struct anx_sandbox_lens *l = active_lens;
	uint32_t i;

	if (!l)
		return true;	/* no sandbox active → no enforcement */
	if (!oid)
		return false;

	/*
	 * Always allow the sandbox to look up its own VM object — the
	 * cell needs that to read its config and write outputs.
	 */
	if (oid_eq(oid, &l->vm_oid))
		return true;

	/* Explicit per-OID inputs grant READ. */
	if (op & ANX_SBX_READ) {
		for (i = 0; i < l->input_count; i++) {
			if (oid_eq(&l->inputs[i], oid))
				return true;
		}
	}

	/*
	 * Group grants — union of every group attached to the lens.
	 */
	for (i = 0; i < l->group_count; i++) {
		struct anx_object_group *g =
			anx_object_group_get(&l->groups[i]);
		if (!g)
			continue;
		if (anx_object_group_check(g, oid, op))
			return true;
	}

	/* Output namespace permits CREATE/WRITE for objects whose
	 * caller declared they belong under that prefix.  Phase 1 does
	 * not yet know the caller's intent here so the prefix is
	 * recorded for downstream tooling rather than enforced.
	 */
	(void)l->output_ns;

	return false;
}
