/*
 * anx/base64.h — Standard base64 decode (RFC 4648 §4).
 */

#ifndef ANX_BASE64_H
#define ANX_BASE64_H

#include <anx/types.h>

/* Decode base64 src into dst.  Returns ANX_OK on success. */
int anx_base64_decode(const char *src, uint32_t src_len,
		      uint8_t *dst, uint32_t dst_cap, uint32_t *dst_len);

#endif /* ANX_BASE64_H */
