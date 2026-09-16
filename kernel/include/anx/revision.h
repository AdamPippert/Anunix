#ifndef ANX_REVISION_H
#define ANX_REVISION_H

#include <anx/cell.h>

#define ANX_REVISION_LEASE_MAX 256U
enum anx_revision_class {
	ANX_REVISION_PARAMETERS = 1,
	ANX_REVISION_IMPLEMENTATION = 2,
};
struct anx_revision_lease_view {
	anx_oid_t id;
	enum anx_revision_class ceiling;
	bool revoked;
};

/* Immutable ceiling; controller-only issuance, binding, and revocation. */
int anx_revision_lease_create(enum anx_revision_class ceiling, struct anx_revision_lease_view *out);
int anx_revision_lease_get(const anx_oid_t *id, struct anx_revision_lease_view *out);
int anx_revision_lease_bind(struct anx_cell *cell, const anx_oid_t *id);
int anx_revision_lease_revoke(const anx_oid_t *id);
/* Internal operation classification; no caller-supplied class on mutation APIs. */
int anx_revision_check(enum anx_revision_class required, bool require_lease);

#endif
