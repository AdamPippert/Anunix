/*
 * anx/hda.h — Intel High Definition Audio controller driver.
 *
 * This is the v1 ALSA-equivalent: a custom HDA driver written from
 * scratch against the Intel HD Audio Specification (Rev 1.0a, 2010).
 * Replaces what `snd_hda_intel.ko` does on Linux — discovers the HDA
 * PCIe controller, sets up the CORB/RIRB verb pipeline, finds an
 * output codec, configures one output stream, and pumps PCM samples
 * out via a DMA ring buffer.
 *
 * Polled-mode only in v1; hooks for IRQ-driven mode are sketched but
 * not used.  Targets:
 *   - QEMU `intel-hda` device (driverless HDA controller emulation)
 *   - Framework Desktop / Laptop 16 (real Realtek ALC HDA codec)
 *   - Any modern x86_64 board with an AMD/Intel HDA controller
 */

#ifndef ANX_HDA_H
#define ANX_HDA_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Codec verb encoding (public for tests)                              */
/* ------------------------------------------------------------------ */

/*
 * A "verb" is a 32-bit command word sent to a codec via CORB.  The
 * format depends on the verb type — most use 8-bit verb + 8-bit
 * payload; a few "long form" verbs use 4-bit verb + 16-bit payload.
 *
 * Layout:
 *   bits 31:28  Codec address (4 bits)
 *   bits 27:20  Node ID (NID, 8 bits)
 *   bits 19:0   Verb + payload (20 bits)
 */
static inline uint32_t
anx_hda_verb(uint8_t cad, uint8_t nid, uint16_t verb, uint16_t payload)
{
	return ((uint32_t)(cad & 0xF) << 28) |
	       ((uint32_t)nid << 20) |
	       ((uint32_t)(verb & 0xFFF) << 8) |
	       ((uint32_t)payload & 0xFF);
}

/* "Long form" verbs (verb has 4 bits, payload has 16). */
static inline uint32_t
anx_hda_verb_long(uint8_t cad, uint8_t nid, uint8_t verb, uint16_t payload)
{
	return ((uint32_t)(cad & 0xF) << 28) |
	       ((uint32_t)nid << 20) |
	       ((uint32_t)(verb & 0xF) << 16) |
	       ((uint32_t)payload);
}

/* ------------------------------------------------------------------ */
/* HDA controller registers (public for tests)                         */
/* ------------------------------------------------------------------ */

/* Global registers (offsets from BAR0). */
#define ANX_HDA_GCAP		0x00	/* 16: capabilities */
#define ANX_HDA_VMIN		0x02	/* 8: minor version */
#define ANX_HDA_VMAJ		0x03	/* 8: major version */
#define ANX_HDA_OUTPAY		0x04	/* 16: output payload */
#define ANX_HDA_INPAY		0x06	/* 16: input payload */
#define ANX_HDA_GCTL		0x08	/* 32: global control */
#define ANX_HDA_WAKEEN		0x0C	/* 16: wake enable */
#define ANX_HDA_STATESTS	0x0E	/* 16: codec state status (per codec bit) */
#define ANX_HDA_INTCTL		0x20	/* 32: interrupt control */
#define ANX_HDA_INTSTS		0x24	/* 32: interrupt status */
#define ANX_HDA_WALCLK		0x30	/* 32: wall clock */

/* CORB (Command Output Ring Buffer) registers */
#define ANX_HDA_CORBLBASE	0x40
#define ANX_HDA_CORBUBASE	0x44
#define ANX_HDA_CORBWP		0x48	/* 16: write pointer */
#define ANX_HDA_CORBRP		0x4A	/* 16: read pointer (HW updates) */
#define ANX_HDA_CORBCTL		0x4C	/* 8:  control */
#define ANX_HDA_CORBSTS		0x4D
#define ANX_HDA_CORBSIZE	0x4E	/* 8: size code */

/* RIRB (Response Input Ring Buffer) registers */
#define ANX_HDA_RIRBLBASE	0x50
#define ANX_HDA_RIRBUBASE	0x54
#define ANX_HDA_RIRBWP		0x58	/* 16: write pointer (HW updates) */
#define ANX_HDA_RINTCNT		0x5A	/* 16: interrupt count */
#define ANX_HDA_RIRBCTL		0x5C	/* 8 */
#define ANX_HDA_RIRBSTS		0x5D
#define ANX_HDA_RIRBSIZE	0x5E

/* Immediate Command Interface (HDA spec §3.4.3) — alternative to CORB/RIRB
 * that some controllers (notably QEMU's intel-hda) implement reliably. */
#define ANX_HDA_ICOI		0x60	/* 32: immediate command output */
#define ANX_HDA_IRR		0x64	/* 32: immediate response */
#define ANX_HDA_ICS		0x68	/* 16: immediate command status */
#define ANX_HDA_ICS_ICB		(1u << 0)	/* immediate command busy */
#define ANX_HDA_ICS_IRV		(1u << 1)	/* immediate response valid */

/* DMA position buffer base */
#define ANX_HDA_DPLBASE		0x70
#define ANX_HDA_DPUBASE		0x74

/* Stream descriptor base.  Each SD is 0x20 bytes, output streams start
 * after the input streams in the descriptor table.  Use stream_off(). */
static inline uint32_t
anx_hda_stream_off(uint32_t input_streams, uint32_t output_idx)
{
	return 0x80 + (input_streams + output_idx) * 0x20;
}

/* Stream descriptor offsets (from start of SD) */
#define ANX_HDA_SD_CTL		0x00	/* 24-bit control */
#define ANX_HDA_SD_STS		0x03	/*  8-bit status */
#define ANX_HDA_SD_LPIB		0x04	/* 32-bit link position in buffer */
#define ANX_HDA_SD_CBL		0x08	/* 32-bit cyclic buffer length */
#define ANX_HDA_SD_LVI		0x0C	/* 16-bit last valid BDL index */
#define ANX_HDA_SD_FIFOS	0x10	/* 16-bit FIFO size */
#define ANX_HDA_SD_FMT		0x12	/* 16-bit format */
#define ANX_HDA_SD_BDLPL	0x18	/* 32-bit BDL pointer low */
#define ANX_HDA_SD_BDLPU	0x1C	/* 32-bit BDL pointer high */

/* GCTL bits */
#define ANX_HDA_GCTL_CRST	(1u << 0)	/* controller reset (1 = leave reset) */

/* CORB/RIRB control bits */
#define ANX_HDA_CORBCTL_RUN	(1u << 1)
#define ANX_HDA_RIRBCTL_RUN	(1u << 1)

/* SD CTL bits */
#define ANX_HDA_SD_CTL_RUN	(1u << 1)
#define ANX_HDA_SD_CTL_IOCE	(1u << 2)	/* IRQ on completion enable */
#define ANX_HDA_SD_CTL_FEIE	(1u << 3)	/* FIFO error IRQ enable */
#define ANX_HDA_SD_CTL_DEIE	(1u << 4)	/* descriptor error IRQ enable */

/*
 * Encode the SD format register (16 bits, HDA spec §3.7.1 Table 23):
 *   bit 14    : 0 = PCM, 1 = non-PCM
 *   bit 14    : base rate (0 = 48 kHz family, 1 = 44.1 kHz family)
 *   bits 13:11: multiplier  (0=×1, 1=×2, 2=×3, 3=×4)
 *   bits 10:8 : divisor     (0=/1, 1=/2, ..., 7=/8)
 *   bits 6:4  : bits/sample (0=8, 1=16, 2=20, 3=24, 4=32)
 *   bits 3:0  : channels - 1
 *
 * Inline so host tests can exercise it without linking the driver.
 */
static inline uint16_t
anx_hda_format_encode(uint32_t sample_rate, uint16_t bps, uint16_t channels)
{
	uint16_t fmt = 0;
	uint16_t base, mult, div, bits;

	if (sample_rate == 44100 || sample_rate == 88200 ||
	    sample_rate == 22050) {
		base = 1;
		if (sample_rate == 88200)      { mult = 1; div = 0; }
		else if (sample_rate == 22050) { mult = 0; div = 1; }
		else                           { mult = 0; div = 0; }
	} else {
		base = 0;	/* 48 kHz family */
		if      (sample_rate == 96000) { mult = 1; div = 0; }
		else if (sample_rate == 192000){ mult = 3; div = 0; }
		else if (sample_rate == 24000) { mult = 0; div = 1; }
		else if (sample_rate == 16000) { mult = 0; div = 2; }
		else                           { mult = 0; div = 0; } /* 48k */
	}

	switch (bps) {
	case  8: bits = 0; break;
	case 16: bits = 1; break;
	case 20: bits = 2; break;
	case 24: bits = 3; break;
	case 32: bits = 4; break;
	default: bits = 1; break;
	}

	fmt |= ((uint16_t)(base & 1))  << 14;
	fmt |= ((uint16_t)(mult & 7))  << 11;
	fmt |= ((uint16_t)(div  & 7))  << 8;
	fmt |= ((uint16_t)(bits & 7))  << 4;
	fmt |= ((uint16_t)(channels ? channels - 1 : 0) & 0xF);
	return fmt;
}

/* ------------------------------------------------------------------ */
/* BDL (Buffer Descriptor List) entry                                  */
/* ------------------------------------------------------------------ */

struct anx_hda_bdl_entry {
	uint64_t addr;		/* 64-bit physical address */
	uint32_t length;	/* in bytes; multiple of 0x80 (128) */
	uint32_t ioc;		/* bit 0 = interrupt-on-completion */
};

/* ------------------------------------------------------------------ */
/* Public driver API                                                   */
/* ------------------------------------------------------------------ */

/*
 * Probe for an HDA controller, initialize it, register an audio sink
 * named "hda".  Returns ANX_OK if a sink was registered, ANX_ENODEV
 * if no controller was found, or another error if init failed.
 *
 * Idempotent: calling more than once is harmless.
 */
int anx_hda_init(void);

/* Test-time: returns true if the driver successfully bound to hardware. */
bool anx_hda_present(void);

#endif /* ANX_HDA_H */
