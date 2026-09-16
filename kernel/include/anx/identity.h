#ifndef ANX_IDENTITY_H
#define ANX_IDENTITY_H

#include <anx/cell.h>
#include <anx/capability.h>

#define ANX_IDENTITY_WIRE_BYTES 108U
#define ANX_IDENTITY_RECORD_BYTES (ANX_IDENTITY_WIRE_BYTES + 64U)
#define ANX_IDENTITY_MAX 256U
#define ANX_IDENTITY_SCHEMA "anx:identity/commitment"

struct anx_identity_commitment {
	uint32_t schema;
	anx_oid_t identity_id;
	uint64_t generation;
	uint32_t authority;
	uint8_t operator_key[32];
	uint8_t previous_digest[32];
};

struct anx_identity_view {
	struct anx_identity_commitment commitment;
	uint8_t digest[32];
	anx_oid_t record_oid;
};

/* Sign the returned SHA-256 digest with the operator's Ed25519 key. */
int anx_identity_digest(const struct anx_identity_commitment *commitment, uint8_t digest[32]);
/* Trusted bootstrap: signed generation 1, zero predecessor, outside a cell. */
int anx_identity_create(const struct anx_identity_commitment *root, const uint8_t signature[64]);
/* Every update needs the pinned operator key and the current predecessor. */
int anx_identity_transition(const struct anx_identity_commitment *next, const uint8_t signature[64]);
int anx_identity_get(const anx_oid_t *identity_id, struct anx_identity_view *out);
/* Only trusted control code can bind a CREATED cell. Derived children inherit. */
int anx_identity_bind(struct anx_cell *cell, const anx_oid_t *identity_id);
/* An optional output names the decision's commitment, including policy denials. */
int anx_identity_admit(const struct anx_cell *cell, anx_oid_t *record_out);

#endif
