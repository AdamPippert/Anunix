/*
 * sha1.h — SHA-1 digest (RFC 3174).
 * Used exclusively for WebSocket Sec-WebSocket-Accept handshake.
 */

#ifndef ANX_SHA1_H
#define ANX_SHA1_H

#include <anx/types.h>

#define SHA1_DIGEST_LEN 20

void anx_sha1(const uint8_t *data, size_t len, uint8_t out[SHA1_DIGEST_LEN]);

#endif /* ANX_SHA1_H */
