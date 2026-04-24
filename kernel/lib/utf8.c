/*
 * utf8.c — Strict UTF-8 decoder.
 */

#include <anx/utf8.h>
#include <anx/types.h>

int
anx_utf8_decode(const uint8_t *buf, uint32_t len,
                 uint32_t *cp, uint32_t *consumed)
{
	uint32_t b0, b1, b2, b3, n;

	if (!buf || !cp || !consumed || len == 0)
		return ANX_EINVAL;

	b0 = buf[0];

	if (b0 < 0x80) {
		*cp       = b0;
		*consumed = 1;
		return ANX_OK;
	}

	if (b0 < 0xC0) {
		/* Bare continuation byte — invalid as a lead. */
		return ANX_EINVAL;
	}

	if (b0 < 0xE0) {
		/* 2-byte sequence: 110xxxxx 10xxxxxx */
		if (len < 2)
			return ANX_EINVAL;
		b1 = buf[1];
		if ((b1 & 0xC0) != 0x80)
			return ANX_EINVAL;
		n = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
		if (n < 0x80)
			return ANX_EINVAL;  /* overlong */
		*cp       = n;
		*consumed = 2;
		return ANX_OK;
	}

	if (b0 < 0xF0) {
		/* 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx */
		if (len < 3)
			return ANX_EINVAL;
		b1 = buf[1];
		b2 = buf[2];
		if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80)
			return ANX_EINVAL;
		n = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
		if (n < 0x800)
			return ANX_EINVAL;  /* overlong */
		if (n >= 0xD800 && n <= 0xDFFF)
			return ANX_EINVAL;  /* UTF-16 surrogate halves */
		*cp       = n;
		*consumed = 3;
		return ANX_OK;
	}

	if (b0 < 0xF8) {
		/* 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
		if (len < 4)
			return ANX_EINVAL;
		b1 = buf[1];
		b2 = buf[2];
		b3 = buf[3];
		if ((b1 & 0xC0) != 0x80 ||
		    (b2 & 0xC0) != 0x80 ||
		    (b3 & 0xC0) != 0x80)
			return ANX_EINVAL;
		n = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) |
		    ((b2 & 0x3F) << 6)  |  (b3 & 0x3F);
		if (n < 0x10000)
			return ANX_EINVAL;  /* overlong */
		if (n > 0x10FFFF)
			return ANX_EINVAL;  /* above Unicode range */
		*cp       = n;
		*consumed = 4;
		return ANX_OK;
	}

	/* Bytes 0xF8-0xFF are never valid UTF-8 lead bytes. */
	return ANX_EINVAL;
}
