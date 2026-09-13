#include <anx/identity.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>

static void put64(uint8_t *out, uint64_t value)
{
	for (uint32_t i = 0; i < 8; i++)
		out[i] = (uint8_t)(value >> (8 * i));
}

static int encode(const struct anx_identity_commitment *c, uint8_t out[ANX_IDENTITY_WIRE_BYTES])
{
	if (!c || c->schema != 1 || anx_uuid_is_nil(&c->identity_id) || !c->generation ||
	    (c->authority & ~ANX_CAP_AUTH_ALL))
		return ANX_EINVAL;
	anx_memcpy(out, "ANX-IDENTITY-v1", 16);
	put64(out + 16, c->identity_id.hi);
	put64(out + 24, c->identity_id.lo);
	put64(out + 32, c->generation);
	for (uint32_t i = 0; i < 4; i++)
		out[40 + i] = (uint8_t)(c->authority >> (8 * i));
	anx_memcpy(out + 44, c->operator_key, 32);
	anx_memcpy(out + 76, c->previous_digest, 32);
	return ANX_OK;
}

int anx_identity_digest(const struct anx_identity_commitment *c, uint8_t digest[32])
{
	uint8_t bytes[ANX_IDENTITY_WIRE_BYTES];
	if (!digest || encode(c, bytes) != ANX_OK)
		return ANX_EINVAL;
	anx_sha256(bytes, sizeof(bytes), digest);
	return ANX_OK;
}

int anx_identity_create(const struct anx_identity_commitment *root, const uint8_t signature[64])
{
	(void)root; (void)signature;
	return ANX_ENOSYS;
}

int anx_identity_transition(const struct anx_identity_commitment *next, const uint8_t signature[64])
{
	(void)next; (void)signature;
	return ANX_ENOSYS;
}

int anx_identity_get(const anx_oid_t *identity_id, struct anx_identity_view *out)
{
	(void)identity_id; (void)out;
	return ANX_ENOSYS;
}

int anx_identity_bind(struct anx_cell *cell, const anx_oid_t *identity_id)
{
	(void)cell; (void)identity_id;
	return ANX_ENOSYS;
}

int anx_identity_admit(const struct anx_cell *cell, anx_oid_t *record_out)
{
	(void)cell; (void)record_out;
	return ANX_ENOSYS;
}
