/*
 * anx/icm.h — Information Context Management over State Objects (RFC-0025).
 *
 * ICM is a convention, not a new primitive: an artifact's identity is the
 * State Object oid (RFC-0002), its classification lives in anno.icm.* user
 * metadata, and its dependencies are the provenance graph. This module reads
 * those existing facts and renders ICM views (catalog, dependency graph). It
 * adds no kernel object type.
 */

#ifndef ANX_ICM_H
#define ANX_ICM_H

#include <anx/types.h>
#include <anx/state_object.h>

/* Reserved anno.icm.* annotation keys (RFC-0025 Section 4.1). */
#define ANX_ICM_KEY_DOMAIN	"anno.icm.domain"
#define ANX_ICM_KEY_KIND	"anno.icm.kind"
#define ANX_ICM_KEY_AUTHORITY	"anno.icm.authority"
#define ANX_ICM_KEY_STATUS	"anno.icm.status"
#define ANX_ICM_KEY_STACK	"anno.icm.stack"
#define ANX_ICM_KEY_ENTRY	"anno.icm.entry"
#define ANX_ICM_KEY_PUBLISHED	"anno.icm.published"

/* The ICM annotations read off one State Object. Empty strings mean "unset";
 * role is deliberately absent — role is local to the consumer (RFC-0025 4.2).
 * `published` carries a release URI on published artifacts only (RFC-0025 4.1,
 * 9.3); an empty string means the artifact is a working (unpublished) one. */
struct anx_icm_view {
	anx_oid_t oid;
	enum anx_object_type object_type;
	char domain[256];
	char kind[64];
	char authority[32];
	char status[32];
	char stack[128];
	char published[128];
};

/*
 * Tag an existing object with ICM annotations. Writes only user_meta; cannot
 * touch identity, provenance, or policy. Any field passed as NULL is left
 * unchanged. Returns ANX_OK, or a negative error.
 */
int anx_icm_tag(const anx_oid_t *oid, const char *domain, const char *kind,
		const char *authority, const char *status,
		const char *stack, const char *entry);

/*
 * Mark an artifact as a published release by setting anno.icm.published to the
 * given release URI (e.g. "anx:pkg/foo@1.2.0"). Per ICM "published things are
 * versioned" (RFC-0025 9.3), this is intended for sealed objects; the kernel
 * does not require sealing, but a release marker on a mutable object is a
 * caller error in spirit. Writes user_meta only. Returns ANX_OK, ANX_EINVAL on
 * a NULL/empty uri, or ANX_ENOENT if the object does not exist.
 */
int anx_icm_publish(const anx_oid_t *oid, const char *release_uri);

/*
 * Populate `out` from one object's anno.icm.* metadata. Unset keys yield empty
 * strings. Returns ANX_OK, or ANX_ENOENT if the object does not exist.
 */
int anx_icm_read_view(const anx_oid_t *oid, struct anx_icm_view *out);

/*
 * Visitor invoked once per cataloged artifact during anx_icm_catalog().
 * Returning non-zero stops iteration and is propagated as the return value.
 */
typedef int (*anx_icm_visit_fn)(const struct anx_icm_view *view, void *arg);

/*
 * Iterate every artifact in the object store, building an anx_icm_view for
 * each and passing it to `cb`. If `domain` is non-NULL, only artifacts whose
 * anno.icm.domain contains that tag are visited. Returns ANX_OK once all
 * artifacts are visited, or the first non-zero value returned by `cb`.
 */
int anx_icm_catalog(const char *domain, anx_icm_visit_fn cb, void *arg);

/*
 * Count cataloged artifacts (optionally filtered by domain). Convenience over
 * anx_icm_catalog(); returns a count >= 0, or a negative error.
 */
int anx_icm_count(const char *domain);

/*
 * Iterate only artifacts carrying an anno.icm.published marker (the "icm
 * published" view, RFC-0025 9.3). Same return contract as anx_icm_catalog().
 */
int anx_icm_published(anx_icm_visit_fn cb, void *arg);

#endif /* ANX_ICM_H */
