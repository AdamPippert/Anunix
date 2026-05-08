/*
 * anx/uor.h — Universal Object Reference: topological coordinate layer.
 *
 * RFC-0002 §7 (UOR Projection and Topological Identity).
 *
 * UOR is a deterministic, content-derived coordinate system that sits
 * alongside — not replacing — the State Object identity model:
 *
 *   oid           which object is this?
 *   content_hash  is the payload intact?
 *   uor_address   where does this object live in topology?
 *
 * A UOR projection is computed from a canonical manifest of the State
 * Object's identity, version, content hash, type, parents, and schema.
 * Projection is deterministic: identical manifests project to identical
 * coordinates. Mutating an object (new version, new content_hash)
 * produces a new projection while the OID stays stable.
 *
 * The boundary_key derived from the projection feeds the disk store's
 * sorted index, giving locality-ordered range scans without changing
 * the object authority model.
 */

#ifndef ANX_UOR_H
#define ANX_UOR_H

#include <anx/types.h>

struct anx_state_object;	/* forward */

/* Address width in bytes. SHA-256 over canonical manifest. */
#define ANX_UOR_ADDR_BYTES		32

/*
 * Region width in bk space. Objects with the same (object_type,
 * content-hash prefix) cluster into 16-bit regions, so a range scan
 * over [region_lo, region_hi] returns one locality group.
 */
#define ANX_UOR_REGION_BITS		16
#define ANX_UOR_REGION_WIDTH		(1ULL << ANX_UOR_REGION_BITS)
#define ANX_UOR_REGION_MASK		(ANX_UOR_REGION_WIDTH - 1)

/* Canonical manifest is bounded; oversized objects are rejected. */
#define ANX_UOR_MANIFEST_MAX		1024
#define ANX_UOR_MANIFEST_MAX_PARENTS	16

/* Wire format version for the canonical manifest. Bumped when the
 * serialization changes — projections are not stable across versions. */
#define ANX_UOR_MANIFEST_VERSION	1

/* Metadata keys attached to a sealed object's system_meta. */
#define ANX_META_UOR_ADDRESS		"uor.address"
#define ANX_META_UOR_REGION_LO		"uor.region.lo"
#define ANX_META_UOR_REGION_HI		"uor.region.hi"
#define ANX_META_UOR_BOUNDARY_KEY	"uor.boundary_key"
#define ANX_META_UOR_LOCALITY_METRIC	"uor.locality_metric"
#define ANX_META_UOR_TOPOLOGY_EPOCH	"uor.topology_epoch"
#define ANX_META_UOR_MANIFEST_VERSION	"uor.manifest_version"

/*
 * A UOR reference. Bound to (oid, version, content_hash) of a State
 * Object via the canonical manifest. Mutating any of those produces a
 * new projection.
 */
struct anx_uor_ref {
	uint8_t  address[ANX_UOR_ADDR_BYTES];
	uint64_t boundary_key;
	uint64_t region_lo;
	uint64_t region_hi;
	uint16_t locality_metric;	/* reserved; 0 in this projection */
	uint16_t manifest_version;	/* equals ANX_UOR_MANIFEST_VERSION */
	uint32_t flags;
};

/* Canonical manifest byte buffer. */
struct anx_uor_manifest {
	uint8_t  bytes[ANX_UOR_MANIFEST_MAX];
	uint32_t len;
};

/*
 * Build a canonical manifest for an object. Layout (all little-endian):
 *
 *   [0..3]    magic 'A','U','O','R'
 *   [4..5]    manifest_version (uint16)
 *   [6..7]    object_type      (uint16)
 *   [8..23]   oid              (hi[8] || lo[8])
 *   [24..31]  obj.version      (uint64)
 *   [32..63]  content_hash     (32 bytes)
 *   [64..67]  parent_count     (uint32, bounded)
 *   [68..]    parents          (16 bytes each)
 *   [..]      schema_uri_len   (uint16)
 *   [..]      schema_uri       (bytes, no NUL)
 *   [..]      schema_ver_len   (uint16)
 *   [..]      schema_version   (bytes, no NUL)
 *
 * Returns ANX_OK on success, ANX_ENOMEM if the manifest would overflow.
 */
int anx_uor_build_manifest(const struct anx_state_object *obj,
			   struct anx_uor_manifest *out);

/*
 * Project a manifest into a UOR reference. The address is SHA-256 over
 * the manifest bytes; the boundary_key packs object_type into the high
 * byte and the next seven bytes of the address (big-endian) into the
 * low 56 bits, which gives type-bucket clustering while preserving
 * content-derived spread within each type.
 */
int anx_uor_project_manifest(const struct anx_uor_manifest *manifest,
			     uint16_t object_type,
			     struct anx_uor_ref *out);

/* Convenience: build manifest then project. */
int anx_uor_project_object(const struct anx_state_object *obj,
			   struct anx_uor_ref *out);

/* Boundary key extracted from a projection. */
uint64_t anx_uor_boundary_key(const struct anx_uor_ref *ref);

/*
 * Write the projection's coordinates into the object's system_meta.
 * Existing uor.* keys are overwritten. The object's content is not
 * modified — only metadata is touched.
 */
int anx_uor_attach_metadata(struct anx_state_object *obj,
			    const struct anx_uor_ref *ref);

/*
 * Format an address as 64 lowercase hex chars + NUL into out (must be
 * at least 65 bytes). Used for diagnostics and the uor.address meta
 * value.
 */
void anx_uor_format_address(const uint8_t addr[ANX_UOR_ADDR_BYTES],
			    char *out, uint32_t out_len);

/*
 * Rebuild the topology index by recomputing projections for every
 * object in the in-memory store and reattaching uor.* metadata.
 * Non-destructive: payloads, OIDs, and provenance are untouched.
 *
 * `count_out` (optional) receives the number of objects projected.
 * Returns ANX_OK on success.
 */
int anx_uor_rebuild_topology_index(uint32_t *count_out);

/* Increment and read the global topology epoch. The epoch is bumped on
 * every rebuild so callers can invalidate region caches deterministically. */
uint64_t anx_uor_topology_epoch(void);
uint64_t anx_uor_topology_epoch_bump(void);

#endif /* ANX_UOR_H */
