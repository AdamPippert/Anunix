/*
 * anx/base64.h — Base64 encode / decode (RFC 4648 §4).
 */

#ifndef ANX_BASE64_H
#define ANX_BASE64_H

#include <anx/types.h>

/* Minimum dst buffer size for encoding src_len bytes. */
#define ANX_BASE64_ENC_LEN(n) (((n) + 2) / 3 * 4)

/* Encode src_len bytes into base64 at dst.
 * dst must be at least ANX_BASE64_ENC_LEN(src_len) bytes.
 * Returns bytes written (not NUL-terminated). */
size_t anx_base64_encode(const uint8_t *src, size_t src_len,
			  char *dst, size_t dst_cap);

/* Decode base64 string (may contain '=' padding) into dst.
 * Returns decoded byte count, or 0 on error. */
size_t anx_base64_decode(const char *src, size_t src_len,
			  uint8_t *dst, size_t dst_cap);

#endif /* ANX_BASE64_H */
