/*
 * anx/uuid.h — UUIDv7 generation and utilities.
 *
 * UUIDv7 is time-ordered (RFC 9562): the high 48 bits are a Unix
 * timestamp in milliseconds, giving globally unique, temporally
 * sortable identifiers.
 */

#ifndef ANX_UUID_H
#define ANX_UUID_H

#include <anx/types.h>

/* Nil UUID (all zeros) */
#define ANX_UUID_NIL ((struct anx_uuid){ .hi = 0, .lo = 0 })

/* Generate a new UUIDv7 */
void anx_uuid_generate(struct anx_uuid *out);

/* Compare two UUIDs. Returns <0, 0, or >0. */
int anx_uuid_compare(const struct anx_uuid *a, const struct anx_uuid *b);

/* Check if a UUID is nil */
static inline bool anx_uuid_is_nil(const struct anx_uuid *u)
{
	return u->hi == 0 && u->lo == 0;
}

/* Format UUID as string: "01234567-89ab-7cde-8f01-234567890abc" + NUL (37 bytes) */
void anx_uuid_to_string(const struct anx_uuid *u, char *buf, size_t len);

/* Hash a UUID for use in hash tables */
uint64_t anx_uuid_hash(const struct anx_uuid *u);

#endif /* ANX_UUID_H */
