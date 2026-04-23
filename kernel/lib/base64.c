/*
 * base64.c — Base64 encode / decode (RFC 4648, standard alphabet).
 */

#include "base64.h"
#include <anx/string.h>

static const char B64_ENC[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t anx_base64_encode(const uint8_t *src, size_t src_len,
			  char *dst, size_t dst_cap)
{
	size_t i, out = 0;
	uint32_t triple;

	for (i = 0; i < src_len; i += 3) {
		size_t rem = src_len - i;

		triple  = (uint32_t)src[i] << 16;
		if (rem > 1) triple |= (uint32_t)src[i + 1] << 8;
		if (rem > 2) triple |= (uint32_t)src[i + 2];

		if (out + 4 > dst_cap)
			break;
		dst[out++] = B64_ENC[(triple >> 18) & 0x3F];
		dst[out++] = B64_ENC[(triple >> 12) & 0x3F];
		dst[out++] = (rem > 1) ? B64_ENC[(triple >> 6) & 0x3F] : '=';
		dst[out++] = (rem > 2) ? B64_ENC[(triple     ) & 0x3F] : '=';
	}
	return out;
}

static int b64_val(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

size_t anx_base64_decode(const char *src, size_t src_len,
			  uint8_t *dst, size_t dst_cap)
{
	size_t out = 0;
	size_t i;

	for (i = 0; i + 3 < src_len; i += 4) {
		int a = b64_val(src[i]);
		int b = b64_val(src[i + 1]);
		int c = (src[i + 2] != '=') ? b64_val(src[i + 2]) : 0;
		int d = (src[i + 3] != '=') ? b64_val(src[i + 3]) : 0;

		if (a < 0 || b < 0 || c < 0 || d < 0)
			return 0;

		if (out < dst_cap) dst[out++] = (uint8_t)((a << 2) | (b >> 4));
		if (src[i + 2] != '=' && out < dst_cap)
			dst[out++] = (uint8_t)((b << 4) | (c >> 2));
		if (src[i + 3] != '=' && out < dst_cap)
			dst[out++] = (uint8_t)((c << 6) | d);
	}
	return out;
}
