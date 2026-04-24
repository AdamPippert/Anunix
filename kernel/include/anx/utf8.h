/*
 * anx/utf8.h — UTF-8 decoder.
 *
 * Provides strict decoding with explicit rejection of overlong encodings,
 * surrogates, truncated sequences, and codepoints above U+10FFFF.
 */

#ifndef ANX_UTF8_H
#define ANX_UTF8_H

#include <anx/types.h>

/*
 * Decode one UTF-8 codepoint from buf[0..len-1].
 * On success: fills *cp and *consumed, returns ANX_OK.
 * On error:   returns ANX_EINVAL (overlong, surrogate, truncated, invalid byte).
 */
int anx_utf8_decode(const uint8_t *buf, uint32_t len,
                     uint32_t *cp, uint32_t *consumed);

#endif /* ANX_UTF8_H */
