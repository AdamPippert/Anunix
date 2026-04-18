/*
 * base64.c — Standard base64 decode (RFC 4648 §4).
 *
 * Minimal implementation: no line-length enforcement, no streaming.
 * Padding ('=') is accepted but not required.
 */

#include <anx/base64.h>
#include <anx/types.h>

static int8_t b64_char_to_val(char c)
{
	if (c >= 'A' && c <= 'Z') return (int8_t)(c - 'A');
	if (c >= 'a' && c <= 'z') return (int8_t)(c - 'a' + 26);
	if (c >= '0' && c <= '9') return (int8_t)(c - '0' + 52);
	if (c == '+')              return 62;
	if (c == '/')              return 63;
	return -1;
}

int anx_base64_decode(const char *src, uint32_t src_len,
		      uint8_t *dst, uint32_t dst_cap, uint32_t *dst_len)
{
	uint32_t out = 0;
	uint32_t i = 0;

	while (i < src_len) {
		int8_t v0, v1, v2, v3;
		char c0, c1, c2, c3;

		/* Skip whitespace */
		while (i < src_len && (src[i] == ' ' || src[i] == '\n' ||
		                        src[i] == '\r' || src[i] == '\t'))
			i++;
		if (i >= src_len)
			break;

		c0 = src[i++];
		if (c0 == '=') break;

		/* Need at least 2 chars for one output byte */
		while (i < src_len && (src[i] == ' ' || src[i] == '\n' ||
		                        src[i] == '\r' || src[i] == '\t'))
			i++;
		if (i >= src_len)
			return ANX_EINVAL;

		c1 = src[i++];
		if (c1 == '=') return ANX_EINVAL;

		while (i < src_len && (src[i] == ' ' || src[i] == '\n' ||
		                        src[i] == '\r' || src[i] == '\t'))
			i++;
		c2 = (i < src_len) ? src[i++] : '=';

		while (i < src_len && (src[i] == ' ' || src[i] == '\n' ||
		                        src[i] == '\r' || src[i] == '\t'))
			i++;
		c3 = (i < src_len) ? src[i++] : '=';

		v0 = b64_char_to_val(c0);
		v1 = b64_char_to_val(c1);
		if (v0 < 0 || v1 < 0)
			return ANX_EINVAL;

		if (out >= dst_cap) return ANX_ENOMEM;
		dst[out++] = (uint8_t)((v0 << 2) | (v1 >> 4));

		if (c2 != '=') {
			v2 = b64_char_to_val(c2);
			if (v2 < 0) return ANX_EINVAL;
			if (out >= dst_cap) return ANX_ENOMEM;
			dst[out++] = (uint8_t)((v1 << 4) | (v2 >> 2));

			if (c3 != '=') {
				v3 = b64_char_to_val(c3);
				if (v3 < 0) return ANX_EINVAL;
				if (out >= dst_cap) return ANX_ENOMEM;
				dst[out++] = (uint8_t)((v2 << 6) | v3);
			}
		}
	}

	if (dst_len)
		*dst_len = out;
	return ANX_OK;
}
