/*
 * anx/object_group.h — Object Groups for sandbox access control.
 *
 * An object group is a sealable bundle of grants used to give a sandboxed
 * cell access to specific State Objects (or families of objects) without
 * dropping the kernel-wide default-deny stance applied to sandboxes.
 *
 * Each grant either points at a specific OID (when ns_prefix is empty) or
 * at every object whose name lives under a namespace prefix.  Once
 * sealed, a group is immutable; revoking access means dropping the
 * group reference from the sandbox lens, not editing the group.
 */

#ifndef ANX_OBJECT_GROUP_H
#define ANX_OBJECT_GROUP_H

#include <anx/types.h>

/* Operation bits for a grant or an objstore lookup. */
#define ANX_SBX_READ		(1u << 0)
#define ANX_SBX_WRITE		(1u << 1)
#define ANX_SBX_CREATE		(1u << 2)	/* mint a new child object */
#define ANX_SBX_EXEC		(1u << 3)	/* invoke as workflow/cell */

#define ANX_GROUP_NAME_MAX	64
#define ANX_GROUP_PREFIX_MAX	64
#define ANX_GROUP_MAX_GRANTS	32

/*
 * One grant entry inside a group.
 *
 *   oid != 0      → grant applies to that exact OID.
 *   ns_prefix[0]  → grant applies to every object whose namespace path
 *                   starts with this prefix.  (Namespaces are an
 *                   RFC-0002 concept; for now we store the prefix and
 *                   the lens does string comparison against the object's
 *                   namespace metadata, falling back to literal OID
 *                   match when the metadata is unset.)
 */
struct anx_sbx_grant {
	anx_oid_t	oid;
	char		ns_prefix[ANX_GROUP_PREFIX_MAX];
	uint32_t	op_mask;
};

struct anx_object_group {
	bool		in_use;
	anx_oid_t	oid;
	char		name[ANX_GROUP_NAME_MAX];
	bool		sealed;
	uint32_t	grant_count;
	struct anx_sbx_grant grants[ANX_GROUP_MAX_GRANTS];
};

/* Initialize the group registry — call once at boot. */
void anx_object_group_init(void);

/* Create a new (unsealed) group; returns its OID. */
int anx_object_group_create(const char *name, anx_oid_t *oid_out);

/* Append a grant to an unsealed group. */
int anx_object_group_grant(const anx_oid_t *group_oid,
			   const struct anx_sbx_grant *grant);

/* Seal a group; further grants are rejected. */
int anx_object_group_seal(const anx_oid_t *group_oid);

/* Look up a group by OID (returns internal pointer, valid until destroy). */
struct anx_object_group *anx_object_group_get(const anx_oid_t *oid);

/* Destroy a group — for tests / shutdown. */
int anx_object_group_destroy(const anx_oid_t *oid);

/*
 * True if the group permits `op` on `oid`.  Used by the sandbox lens to
 * resolve a check against a group reference.
 */
bool anx_object_group_check(const struct anx_object_group *g,
			    const anx_oid_t *oid, uint32_t op);

#endif /* ANX_OBJECT_GROUP_H */
