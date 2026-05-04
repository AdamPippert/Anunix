/*
 * test_hda.c — Host tests for the in-memory pieces of the HDA driver.
 *
 * The driver itself (PCI probe, MMIO, codec verbs) needs hardware to
 * exercise.  These tests cover the bits that are pure data-shaping:
 *   - 32-bit verb word construction
 *   - SD format register encoding for the rates / bit-depths the
 *     mixer can produce
 *   - BDL entry layout and size
 *   - Stream-descriptor offset math (input vs output streams)
 */

#include <anx/types.h>
#include <anx/hda.h>
#include <anx/string.h>

int test_hda(void)
{
	/* === Test 1: short-form verb encoding === */
	{
		/* GET_PARAM (verb 0xF00) on cad=0, nid=0, payload=0x04
		 * (NODE_COUNT) → 0x000F0004 */
		uint32_t v = anx_hda_verb(0, 0, 0xF00, 0x04);
		if (v != 0x000F0004u) return -1;

		/* SET_PIN_WIDGET_CTRL (verb 0x707) on cad=1, nid=0x14,
		 * payload=0x40 (OUT_EN) → cad=1,nid=0x14,verb=0x707,pl=0x40
		 *  = 0x10000000 | 0x01400000 | 0x00070700 | 0x40
		 *  = 0x11470740 */
		v = anx_hda_verb(1, 0x14, 0x707, 0x40);
		if (v != 0x11470740u) return -2;

		/* Verb truncation: only 12 bits of verb survive. */
		v = anx_hda_verb(0, 0, 0x1F00, 0);
		if (v != 0x000F0000u) return -3;
	}

	/* === Test 2: long-form verb encoding === */
	{
		/* SET_CONVERTER_FORMAT is verb 0x2 (after the >> 8) with a
		 * 16-bit payload.  cad=2, nid=0x03, verb=0x2, payload=0x4011
		 * → 0x20020 << 16... let me compute manually:
		 *   cad=2  → 0x20000000
		 *   nid=3  → 0x00300000
		 *   verb=2 → 0x00020000
		 *   payload= 0x4011
		 *   total = 0x20324011 */
		uint32_t v = anx_hda_verb_long(2, 3, 0x2, 0x4011);
		if (v != 0x20324011u) return -4;
	}

	/* === Test 3: format encoding for the mixer's native rate === */
	{
		/* 48 kHz, 16-bit, 2 channels:
		 *   base=0, mult=0, div=0, bits=1, ch=1
		 *   fmt = (0<<14)|(0<<11)|(0<<8)|(1<<4)|1 = 0x11 */
		uint16_t f = anx_hda_format_encode(48000, 16, 2);
		if (f != 0x0011) return -5;

		/* 44.1 kHz, 16-bit, 2 channels:
		 *   base=1 → 0x4000
		 *   bits=1 → 0x10
		 *   ch=1   → 0x01
		 *   total  = 0x4011 */
		f = anx_hda_format_encode(44100, 16, 2);
		if (f != 0x4011) return -6;

		/* 96 kHz, 24-bit, 2 channels:
		 *   base=0, mult=1 (×2), div=0
		 *   bits=3 → bits<<4 = 0x30
		 *   ch=1
		 *   fmt = (1<<11)|0x30|1 = 0x0831 */
		f = anx_hda_format_encode(96000, 24, 2);
		if (f != 0x0831) return -7;

		/* 24 kHz, 16-bit, 2 channels:
		 *   base=0, div=1, bits=1, ch=1
		 *   fmt = (1<<8)|0x10|1 = 0x0111 */
		f = anx_hda_format_encode(24000, 16, 2);
		if (f != 0x0111) return -8;

		/* Mono 8-bit at 48 kHz:
		 *   bits=0, ch=0 → 0x0000 */
		f = anx_hda_format_encode(48000, 8, 1);
		if (f != 0x0000) return -9;
	}

	/* === Test 4: BDL entry layout matches the HDA spec === */
	{
		struct anx_hda_bdl_entry e;
		anx_memset(&e, 0, sizeof(e));
		e.addr   = 0x1000200030004000ULL;
		e.length = 0x1000;
		e.ioc    = 1;

		/* Entry must be exactly 16 bytes (HDA spec). */
		if (sizeof(e) != 16) return -10;

		/* The first 8 bytes must be the address as a single LE u64. */
		const uint8_t *p = (const uint8_t *)&e;
		uint64_t back = 0;
		for (uint32_t i = 0; i < 8; i++)
			back |= ((uint64_t)p[i]) << (i * 8);
		if (back != e.addr) return -11;

		/* Length follows at offset 8. */
		uint32_t lback = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
				 ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
		if (lback != e.length) return -12;

		/* IOC at offset 12. */
		uint32_t iback = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
				 ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
		if (iback != e.ioc) return -13;
	}

	/* === Test 5: stream-descriptor offset math ===
	 *
	 * Input streams come first, then output streams, each at 0x20.
	 * Controller with 4 input streams: SD_OUT[0] should land at
	 * 0x80 + 4*0x20 = 0x80 + 0x80 = 0x100.
	 */
	{
		if (anx_hda_stream_off(0, 0) != 0x80) return -14;
		if (anx_hda_stream_off(0, 1) != 0xA0) return -15;
		if (anx_hda_stream_off(4, 0) != 0x100) return -16;
		if (anx_hda_stream_off(4, 1) != 0x120) return -17;
	}

	return 0;
}
