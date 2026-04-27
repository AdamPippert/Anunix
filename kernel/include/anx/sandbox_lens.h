/*
 * anx/sandbox_lens.h — Active access filter for sandboxed cells.
 *
 * When a sandboxed VM Cell runs, it installs a lens that scopes every
 * subsequent anx_objstore_lookup() to a default-deny set of OIDs.  The
 * lens combines three grant kinds:
 *
 *   1. inputs[]   — explicit per-OID READ grants pulled from the
 *                   sandbox spec.
 *   2. output_ns  — a namespace prefix where the sandbox may CREATE
 *                   and WRITE objects.
 *   3. groups[]   — sealed object groups (RFC-0021) referenced by OID;
 *                   their grants are unioned into the lens.
 *
 * The lens uses push/restore semantics, not push/pop — install() takes a
 * caller-owned "previous-lens snapshot" buffer that must be passed back
 * to restore() in LIFO order.  This works under the cooperative single-
 * threaded execution model in use today; switching to per-CPU storage is
 * a Phase-2 concern that does not change the call sites.
 *
 * When no lens is active anx_sandbox_lens_check() returns true (the
 * unsandboxed kernel path is unaffected).
 */

#ifndef ANX_SANDBOX_LENS_H
#define ANX_SANDBOX_LENS_H

#include <anx/types.h>
#include <anx/object_group.h>

#define ANX_LENS_MAX_INPUTS	8
#define ANX_LENS_MAX_GROUPS	8
#define ANX_LENS_NS_MAX		64

struct anx_sandbox_lens {
	anx_oid_t	vm_oid;			/* sandbox owning this lens */
	anx_oid_t	inputs[ANX_LENS_MAX_INPUTS];
	uint32_t	input_count;
	char		output_ns[ANX_LENS_NS_MAX];
	anx_oid_t	groups[ANX_LENS_MAX_GROUPS];
	uint32_t	group_count;
};

/*
 * Snapshot used to nest lenses.  Callers allocate this on their stack;
 * its contents are opaque and only valid for matched install/restore
 * pairs.
 */
struct anx_sandbox_lens_save {
	struct anx_sandbox_lens *prev;
	bool			 active;
};

/*
 * Install `lens` as the active lens, saving the prior state into `save`.
 * `lens` must remain valid until anx_sandbox_lens_restore() is called.
 * Passing a NULL `lens` clears the current lens (useful for kernel
 * internals that need to step out of a sandbox briefly).
 */
int anx_sandbox_lens_install(struct anx_sandbox_lens_save *save,
			     struct anx_sandbox_lens *lens);

/* Restore the prior lens recorded in `save`. */
void anx_sandbox_lens_restore(const struct anx_sandbox_lens_save *save);

/* The currently active lens, or NULL. */
struct anx_sandbox_lens *anx_sandbox_lens_active(void);

/*
 * True if the active lens permits `op` on `oid`.  When no lens is
 * active, returns true (no enforcement).  The objstore lookup path
 * calls this with op=ANX_SBX_READ.
 */
bool anx_sandbox_lens_check(const anx_oid_t *oid, uint32_t op);

#endif /* ANX_SANDBOX_LENS_H */
