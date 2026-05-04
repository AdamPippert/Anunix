/*
 * hda.c — Intel High Definition Audio controller driver.
 *
 * From-scratch implementation of the bottom half of what Linux ALSA's
 * snd_hda_intel does: probe a PCI HDA controller, set up the verb
 * pipeline (CORB/RIRB), discover an output codec, configure one
 * stream, and pump PCM out via a DMA Buffer Descriptor List.
 *
 * Constraints in v1:
 *   - Polled mode only.  The audio thread blocks on LPIB advancing
 *     past the current write region before refilling.
 *   - Hardcoded format: 48 kHz, 16-bit signed, 2 channels.  This
 *     matches the mixer's internal format so no resample on output.
 *   - One output stream (the first available output SD).
 *   - Single-codec, single-DAC, single-pin path.  Anything fancier
 *     (multi-stream mix, codec graphs with mixer widgets, headphone
 *     redirection) belongs in v2.
 *
 * Design choices motivated by runtime speed:
 *   - All DMA buffers are 4 KiB-aligned page allocations.  The audio
 *     ring uses 4 BDL entries × 4 KiB each = 16 KiB, which at 48 kHz
 *     stereo int16 is 87 ms of buffering — enough headroom for any
 *     practical pull cadence without inflating worst-case latency.
 *   - The verb path is two-call: write to CORBWP, spin on RIRBWP.  No
 *     allocations, no callbacks; the immediate-response fast path of
 *     ALSA is omitted in favour of a single uniform code path.
 *   - Format register is precomputed once at open() and never changed.
 *
 * References:
 *   - Intel HD Audio Specification Rev 1.0a (2010)
 *   - QEMU hw/audio/intel-hda.c (controller emulation)
 *   - QEMU hw/audio/hda-codec.c (the "hda-output" codec we test against)
 */

#include <anx/hda.h>
#include <anx/audio.h>
#include <anx/pci.h>
#include <anx/page.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/spinlock.h>
#include <anx/list.h>
#include <anx/types.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* PCI class identifiers                                               */
/* ------------------------------------------------------------------ */

#define HDA_PCI_CLASS		0x04	/* multimedia */
#define HDA_PCI_SUBCLASS	0x03	/* HD Audio */

/* Codec verb opcodes (subset; see HDA spec §7.3.3). */
#define HDA_VERB_GET_PARAM			0xF00
#define HDA_VERB_SET_CONNECT_SEL		0x701
#define HDA_VERB_GET_CONNECT_SEL		0xF01
#define HDA_VERB_SET_PROCESSING_STATE		0x703
#define HDA_VERB_GET_AMP_GAIN			0xB00
#define HDA_VERB_SET_AMP_GAIN			0x300
#define HDA_VERB_SET_CONVERTER_FORMAT		0x200	/* "long form": 4-bit verb, 16-bit payload */
#define HDA_VERB_GET_CONVERTER_FORMAT		0xA00
#define HDA_VERB_SET_CONVERTER_STREAM		0x706
#define HDA_VERB_GET_CONVERTER_STREAM		0xF06
#define HDA_VERB_SET_PIN_WIDGET_CTRL		0x707
#define HDA_VERB_GET_PIN_WIDGET_CTRL		0xF07
#define HDA_VERB_SET_POWER_STATE		0x705
#define HDA_VERB_GET_POWER_STATE		0xF05
#define HDA_VERB_GET_CONFIG_DEFAULT		0xF1C
#define HDA_VERB_FUNCTION_RESET			0x7FF

/* GET_PARAM sub-codes */
#define HDA_PARAM_VENDOR_ID			0x00
#define HDA_PARAM_REV_ID			0x02
#define HDA_PARAM_NODE_COUNT			0x04	/* sub-node start/count */
#define HDA_PARAM_FUNCTION_GROUP_TYPE		0x05
#define HDA_PARAM_AUDIO_WIDGET_CAP		0x09
#define HDA_PARAM_PCM_SIZE_RATES		0x0A
#define HDA_PARAM_STREAM_FORMATS		0x0B
#define HDA_PARAM_PIN_CAP			0x0C
#define HDA_PARAM_CONN_LIST_LEN			0x0E

/* Function group types (returned by GET_PARAM 0x05) */
#define HDA_FGT_AUDIO				0x01

/* Widget types (top 4 bits of widget capabilities) */
#define HDA_WTYPE_AUDIO_OUT			0x0
#define HDA_WTYPE_AUDIO_IN			0x1
#define HDA_WTYPE_AUDIO_MIXER			0x2
#define HDA_WTYPE_AUDIO_SELECT			0x3
#define HDA_WTYPE_PIN_COMPLEX			0x4

/* Pin widget control bits (verb 0x707) */
#define HDA_PIN_CTL_OUT_EN			(1u << 6)
#define HDA_PIN_CTL_HP_EN			(1u << 7)

/* ------------------------------------------------------------------ */
/* Compile-time tunables                                               */
/* ------------------------------------------------------------------ */

#define HDA_CORB_ENTRIES	256		/* 256 × 4B = 1 KiB */
#define HDA_RIRB_ENTRIES	256		/* 256 × 8B = 2 KiB */

#define HDA_BDL_ENTRIES		4
#define HDA_PERIOD_BYTES	4096		/* 1 page */
#define HDA_RING_BYTES		(HDA_BDL_ENTRIES * HDA_PERIOD_BYTES)

/* Number of int16 samples in one period. */
#define HDA_PERIOD_SAMPLES	(HDA_PERIOD_BYTES / 2)

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

struct hda_state {
	struct anx_pci_device *pci;
	volatile uint8_t      *bar;	/* MMIO base (BAR0) */

	/* Verb pipeline */
	volatile uint32_t     *corb;
	volatile uint64_t     *rirb;
	uint16_t               corb_wp;
	uint16_t               rirb_rp;

	/* Stream layout (from GCAP) */
	uint8_t                num_input_streams;
	uint8_t                num_output_streams;
	uint8_t                num_bidir_streams;

	/* Stream descriptor we use for output (first OSS slot). */
	uint32_t               sd_off;

	/* BDL + audio ring */
	struct anx_hda_bdl_entry *bdl;
	uint8_t                  *ring;		/* virtual */
	uintptr_t                 ring_phys;
	uintptr_t                 bdl_phys;

	/* Codec address found in STATESTS. */
	uint8_t                codec_addr;

	/* Discovered widget IDs (from codec graph traversal). */
	uint8_t                afg_nid;
	uint8_t                dac_nid;
	uint8_t                pin_nid;

	/* Stream tag (1..15) we negotiated with the codec. */
	uint8_t                stream_tag;

	/* Format precomputed in open(). */
	uint16_t               format;

	/* Ring write cursor (in bytes from start of ring). */
	uint32_t               ring_wp;

	bool                   ready;	/* HW probed, sink usable */
	bool                   running;	/* DMA started */
};

static struct hda_state g_hda;

/* ------------------------------------------------------------------ */
/* MMIO helpers                                                        */
/* ------------------------------------------------------------------ */

static inline uint8_t  reg8 (uint32_t off) { return *(volatile uint8_t  *)(g_hda.bar + off); }
static inline uint16_t reg16(uint32_t off) { return *(volatile uint16_t *)(g_hda.bar + off); }
static inline uint32_t reg32(uint32_t off) { return *(volatile uint32_t *)(g_hda.bar + off); }
static inline void wr8 (uint32_t off, uint8_t  v) { *(volatile uint8_t  *)(g_hda.bar + off) = v; }
static inline void wr16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(g_hda.bar + off) = v; }
static inline void wr32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_hda.bar + off) = v; }

/* ------------------------------------------------------------------ */
/* Reset controller                                                    */
/* ------------------------------------------------------------------ */

static int
hda_reset(void)
{
	uint32_t i;

	/* CRST = 0 → enter reset; spin until it reads back 0. */
	wr32(ANX_HDA_GCTL, reg32(ANX_HDA_GCTL) & ~ANX_HDA_GCTL_CRST);
	for (i = 0; i < 100000; i++) {
		if ((reg32(ANX_HDA_GCTL) & ANX_HDA_GCTL_CRST) == 0)
			break;
	}
	if ((reg32(ANX_HDA_GCTL) & ANX_HDA_GCTL_CRST) != 0)
		return ANX_EIO;

	/* CRST = 1 → leave reset. */
	wr32(ANX_HDA_GCTL, reg32(ANX_HDA_GCTL) | ANX_HDA_GCTL_CRST);
	for (i = 0; i < 100000; i++) {
		if (reg32(ANX_HDA_GCTL) & ANX_HDA_GCTL_CRST)
			break;
	}
	if ((reg32(ANX_HDA_GCTL) & ANX_HDA_GCTL_CRST) == 0)
		return ANX_EIO;

	/* Spec: wait at least 521 µs after leaving reset before codecs
	 * are guaranteed to have responded.  Burn a loop. */
	for (i = 0; i < 200000; i++)
		(void)reg32(ANX_HDA_WALCLK);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* CORB / RIRB setup and verb send                                     */
/* ------------------------------------------------------------------ */

static int
corb_rirb_init(void)
{
	uintptr_t corb_phys, rirb_phys;

	/* Allocate one page for CORB (1 KiB used) and one for RIRB (2 KiB). */
	corb_phys = anx_page_alloc(0);
	rirb_phys = anx_page_alloc(0);
	if (!corb_phys || !rirb_phys)
		return ANX_ENOMEM;

	g_hda.corb = (volatile uint32_t *)corb_phys;
	g_hda.rirb = (volatile uint64_t *)rirb_phys;
	anx_memset((void *)g_hda.corb, 0, ANX_PAGE_SIZE);
	anx_memset((void *)g_hda.rirb, 0, ANX_PAGE_SIZE);

	/* Stop CORB/RIRB engines before reprogramming. */
	wr8(ANX_HDA_CORBCTL, 0);
	wr8(ANX_HDA_RIRBCTL, 0);

	/* Program base addresses (low+high, identity-mapped phys==virt). */
	wr32(ANX_HDA_CORBLBASE, (uint32_t)(corb_phys & 0xFFFFFFFFu));
	wr32(ANX_HDA_CORBUBASE, (uint32_t)(corb_phys >> 32));
	wr32(ANX_HDA_RIRBLBASE, (uint32_t)(rirb_phys & 0xFFFFFFFFu));
	wr32(ANX_HDA_RIRBUBASE, (uint32_t)(rirb_phys >> 32));

	/* Size = 256 entries (encoding 0x2 in low 2 bits of CORBSIZE/RIRBSIZE). */
	wr8(ANX_HDA_CORBSIZE, 0x2);
	wr8(ANX_HDA_RIRBSIZE, 0x2);

	/* Reset CORB read pointer: spec §3.3.21 — write 0x8000 (CORBRPRST),
	 * wait for the bit to be set in readback, then write 0 and wait
	 * for the bit to clear.  The "wait for it to clear" step is what
	 * trips up most first-pass drivers; without it, CORB DMA stays in
	 * the reset state and never advances. */
	wr16(ANX_HDA_CORBRP, 0x8000);
	for (uint32_t i = 0; i < 100000; i++)
		if (reg16(ANX_HDA_CORBRP) & 0x8000) break;
	wr16(ANX_HDA_CORBRP, 0);
	for (uint32_t i = 0; i < 100000; i++)
		if ((reg16(ANX_HDA_CORBRP) & 0x8000) == 0) break;
	g_hda.corb_wp = 0;
	wr16(ANX_HDA_CORBWP, 0);

	/* Reset RIRB write pointer (bit 15 of RIRBWP).  This bit is
	 * self-clearing — no second write needed. */
	wr16(ANX_HDA_RIRBWP, 0x8000);
	for (uint32_t i = 0; i < 100000; i++)
		if ((reg16(ANX_HDA_RIRBWP) & 0x8000) == 0) break;
	g_hda.rirb_rp = 0;

	/* Disable interrupt counts (RINTCNT=0 means "interrupt every response",
	 * but we don't enable IRQs in v1). */
	wr16(ANX_HDA_RINTCNT, 0);

	/* Start engines. */
	wr8(ANX_HDA_CORBCTL, ANX_HDA_CORBCTL_RUN);
	wr8(ANX_HDA_RIRBCTL, ANX_HDA_RIRBCTL_RUN);
	return ANX_OK;
}

/*
 * Immediate Command Interface — simpler alternative to CORB/RIRB.
 * Writes the verb into ICOI, polls ICS for the response-valid bit,
 * reads the response from IRR.  Used when CORB DMA is uncooperative
 * (e.g. on some QEMU versions) or as the unconditional v1 path.
 */
static uint32_t
verb_immediate(uint32_t verb)
{
	uint32_t i;

	/* Wait for the controller to be ready (ICB clear). */
	for (i = 0; i < 1000000; i++) {
		if ((reg16(ANX_HDA_ICS) & ANX_HDA_ICS_ICB) == 0)
			break;
	}
	if (i == 1000000)
		return 0;

	/* Clear stale IRV (write-1-to-clear) and submit the verb. */
	wr16(ANX_HDA_ICS, ANX_HDA_ICS_IRV);
	wr32(ANX_HDA_ICOI, verb);
	wr16(ANX_HDA_ICS, ANX_HDA_ICS_ICB);

	/* Poll for response valid. */
	for (i = 0; i < 1000000; i++) {
		uint16_t s = reg16(ANX_HDA_ICS);
		if (s & ANX_HDA_ICS_IRV)
			break;
	}
	if (i == 1000000)
		return 0;

	uint32_t resp = reg32(ANX_HDA_IRR);
	wr16(ANX_HDA_ICS, ANX_HDA_ICS_IRV);	/* ack */
	return resp;
}

/*
 * Send a verb and return its response.  Tries the CORB/RIRB ring first;
 * if it doesn't advance within the timeout, falls back to the
 * immediate-command interface for the rest of this run.  Returns 0 on
 * total failure.
 */
static bool g_hda_use_immediate;

static uint32_t
verb_xchg(uint32_t verb)
{
	uint32_t i;
	uint16_t next_rp;
	uint64_t resp;

	if (g_hda_use_immediate)
		return verb_immediate(verb);

	g_hda.corb_wp = (g_hda.corb_wp + 1) & (HDA_CORB_ENTRIES - 1);
	g_hda.corb[g_hda.corb_wp] = verb;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	wr16(ANX_HDA_CORBWP, g_hda.corb_wp);

	next_rp = (g_hda.rirb_rp + 1) & (HDA_RIRB_ENTRIES - 1);
	for (i = 0; i < 1000000; i++) {
		uint16_t wp = reg16(ANX_HDA_RIRBWP) & (HDA_RIRB_ENTRIES - 1);
		if (wp == next_rp)
			break;
	}
	if (i == 1000000) {
		/* CORB DMA timed out — fall back to immediate commands.
		 * Roll back our CORB write pointer so the slot is reusable
		 * if a future kick brings DMA online. */
		g_hda.corb_wp = (g_hda.corb_wp + HDA_CORB_ENTRIES - 1) &
				(HDA_CORB_ENTRIES - 1);
		g_hda_use_immediate = true;
		kprintf("[hda] CORB/RIRB timed out; using immediate commands\n");
		return verb_immediate(verb);
	}

	resp = g_hda.rirb[next_rp];
	g_hda.rirb_rp = next_rp;
	return (uint32_t)(resp & 0xFFFFFFFFu);
}

static uint32_t
codec_get_param(uint8_t cad, uint8_t nid, uint8_t param)
{
	return verb_xchg(anx_hda_verb(cad, nid, HDA_VERB_GET_PARAM, param));
}

/* ------------------------------------------------------------------ */
/* Codec graph traversal                                               */
/* ------------------------------------------------------------------ */

/* Find the audio function group inside a codec's root.  Returns NID
 * of the AFG, or 0 if not found. */
static uint8_t
find_afg(uint8_t cad)
{
	uint32_t nc = codec_get_param(cad, 0, HDA_PARAM_NODE_COUNT);
	uint8_t  start = (uint8_t)((nc >> 16) & 0xFF);
	uint8_t  count = (uint8_t)(nc & 0xFF);
	uint32_t i;

	for (i = 0; i < count; i++) {
		uint8_t  nid = start + (uint8_t)i;
		uint32_t fgt = codec_get_param(cad, nid,
					       HDA_PARAM_FUNCTION_GROUP_TYPE);
		if ((fgt & 0xFF) == HDA_FGT_AUDIO)
			return nid;
	}
	return 0;
}

static int
find_dac_and_pin(uint8_t cad, uint8_t afg, uint8_t *dac_out, uint8_t *pin_out)
{
	uint32_t nc = codec_get_param(cad, afg, HDA_PARAM_NODE_COUNT);
	uint8_t  start = (uint8_t)((nc >> 16) & 0xFF);
	uint8_t  count = (uint8_t)(nc & 0xFF);
	uint32_t i;
	uint8_t  dac = 0, pin = 0;

	for (i = 0; i < count; i++) {
		uint8_t  nid  = start + (uint8_t)i;
		uint32_t cap  = codec_get_param(cad, nid,
						HDA_PARAM_AUDIO_WIDGET_CAP);
		uint8_t  type = (uint8_t)((cap >> 20) & 0xF);

		if (type == HDA_WTYPE_AUDIO_OUT && dac == 0)
			dac = nid;
		else if (type == HDA_WTYPE_PIN_COMPLEX && pin == 0)
			pin = nid;

		if (dac && pin)
			break;
	}
	if (!dac || !pin)
		return ANX_ENODEV;
	*dac_out = dac;
	*pin_out = pin;
	return ANX_OK;
}

static uint8_t
find_first_codec(void)
{
	uint16_t st = reg16(ANX_HDA_STATESTS);
	uint32_t i;
	for (i = 0; i < 15; i++)
		if (st & (1u << i))
			return (uint8_t)i;
	return 0xFF;
}

/* ------------------------------------------------------------------ */
/* Stream + BDL setup                                                  */
/* ------------------------------------------------------------------ */

static int
stream_alloc_buffers(void)
{
	uintptr_t bdl_phys;
	uintptr_t ring_phys;
	uint32_t  i;

	bdl_phys  = anx_page_alloc(0);
	if (!bdl_phys)
		return ANX_ENOMEM;
	/* Ring: 16 KiB = 4 pages, contiguous from page allocator. */
	ring_phys = anx_page_alloc(2);
	if (!ring_phys) {
		anx_page_free(bdl_phys, 0);
		return ANX_ENOMEM;
	}

	g_hda.bdl       = (struct anx_hda_bdl_entry *)bdl_phys;
	g_hda.ring      = (uint8_t *)ring_phys;
	g_hda.bdl_phys  = bdl_phys;
	g_hda.ring_phys = ring_phys;

	anx_memset(g_hda.bdl,  0, ANX_PAGE_SIZE);
	anx_memset(g_hda.ring, 0, HDA_RING_BYTES);

	for (i = 0; i < HDA_BDL_ENTRIES; i++) {
		g_hda.bdl[i].addr   = ring_phys + i * HDA_PERIOD_BYTES;
		g_hda.bdl[i].length = HDA_PERIOD_BYTES;
		g_hda.bdl[i].ioc    = 0;	/* IRQs disabled in v1 */
	}
	return ANX_OK;
}

static void
stream_program(uint8_t stream_tag, uint16_t fmt)
{
	uint32_t off = g_hda.sd_off;

	/* Stop the SD before reprogramming. */
	wr8(off + ANX_HDA_SD_CTL, 0);

	/* Stream-reset is bit 0 of CTL.  Pulse it. */
	wr8(off + ANX_HDA_SD_CTL, 1);
	for (uint32_t i = 0; i < 1000; i++)
		if (reg8(off + ANX_HDA_SD_CTL) & 1) break;
	wr8(off + ANX_HDA_SD_CTL, 0);
	for (uint32_t i = 0; i < 1000; i++)
		if ((reg8(off + ANX_HDA_SD_CTL) & 1) == 0) break;

	/* Program BDL pointer and cyclic length. */
	wr32(off + ANX_HDA_SD_BDLPL,
	     (uint32_t)(g_hda.bdl_phys & 0xFFFFFFFFu));
	wr32(off + ANX_HDA_SD_BDLPU,
	     (uint32_t)(g_hda.bdl_phys >> 32));
	wr32(off + ANX_HDA_SD_CBL, HDA_RING_BYTES);
	wr16(off + ANX_HDA_SD_LVI, HDA_BDL_ENTRIES - 1);

	wr16(off + ANX_HDA_SD_FMT, fmt);

	/*
	 * SDxCTL bits 23:20 = stream tag (the codec uses this to know
	 * which DMA stream is sending samples its way).  Bits 1..0 are
	 * RUN and SRST.
	 */
	{
		uint32_t ctl = ((uint32_t)stream_tag & 0xF) << 20;
		wr8(off + ANX_HDA_SD_CTL + 0, (uint8_t)(ctl & 0xFF));
		wr8(off + ANX_HDA_SD_CTL + 1, (uint8_t)((ctl >> 8) & 0xFF));
		wr8(off + ANX_HDA_SD_CTL + 2, (uint8_t)((ctl >> 16) & 0xFF));
	}
}

static void
stream_run(bool run)
{
	uint32_t off = g_hda.sd_off;
	uint8_t  ctl = reg8(off + ANX_HDA_SD_CTL);
	if (run)
		ctl |= ANX_HDA_SD_CTL_RUN;
	else
		ctl &= (uint8_t)~ANX_HDA_SD_CTL_RUN;
	wr8(off + ANX_HDA_SD_CTL, ctl);
}

/* ------------------------------------------------------------------ */
/* Audio sink ops                                                      */
/* ------------------------------------------------------------------ */

static int
hda_sink_open(struct anx_audio_format *fmt)
{
	if (!g_hda.ready)
		return ANX_ENODEV;
	if (!fmt)
		return ANX_EINVAL;

	/* We only do 48 kHz int16 stereo on the wire — the mixer's
	 * native format.  Negotiate by overwriting the request. */
	fmt->sample_rate     = 48000;
	fmt->channels        = 2;
	fmt->bits_per_sample = 16;
	fmt->is_float        = 0;

	g_hda.format     = anx_hda_format_encode(48000, 16, 2);
	g_hda.stream_tag = 1;
	g_hda.ring_wp    = 0;

	/* Tell the codec what stream tag and format to expect. */
	verb_xchg(anx_hda_verb(g_hda.codec_addr, g_hda.dac_nid,
			       HDA_VERB_SET_POWER_STATE, 0));	/* D0 */
	verb_xchg(anx_hda_verb(g_hda.codec_addr, g_hda.afg_nid,
			       HDA_VERB_SET_POWER_STATE, 0));	/* D0 */
	verb_xchg(anx_hda_verb_long(g_hda.codec_addr, g_hda.dac_nid,
				    HDA_VERB_SET_CONVERTER_FORMAT >> 8,
				    g_hda.format));
	verb_xchg(anx_hda_verb(g_hda.codec_addr, g_hda.dac_nid,
			       HDA_VERB_SET_CONVERTER_STREAM,
			       (uint16_t)((g_hda.stream_tag << 4) | 0)));
	verb_xchg(anx_hda_verb(g_hda.codec_addr, g_hda.pin_nid,
			       HDA_VERB_SET_PIN_WIDGET_CTRL,
			       HDA_PIN_CTL_OUT_EN));

	stream_program(g_hda.stream_tag, g_hda.format);
	stream_run(true);
	g_hda.running = true;
	return ANX_OK;
}

/*
 * Wait for the controller's link position (LPIB) to advance past the
 * region we are about to overwrite.  Returns ANX_OK on success.  If
 * the stream isn't running for any reason, falls through immediately.
 */
static int
hda_drain_to(uint32_t target_byte)
{
	uint32_t i;
	for (i = 0; i < 10000000; i++) {
		uint32_t lpib = reg32(g_hda.sd_off + ANX_HDA_SD_LPIB);
		uint32_t cur  = lpib % HDA_RING_BYTES;
		/* If LPIB is "behind" target_byte in the ring, we're safe. */
		uint32_t ahead = (target_byte + HDA_RING_BYTES - cur)
				 % HDA_RING_BYTES;
		if (ahead < HDA_RING_BYTES / 2)
			return ANX_OK;
	}
	return ANX_ETIMEDOUT;
}

static int
hda_sink_write(const int16_t *frames, uint32_t frame_count)
{
	uint32_t bytes = frame_count * 2u * sizeof(int16_t);
	uint32_t copied = 0;

	if (!g_hda.ready || !g_hda.running)
		return ANX_ENODEV;
	if (!frames || bytes == 0)
		return ANX_OK;

	while (copied < bytes) {
		uint32_t want = bytes - copied;
		uint32_t room = HDA_RING_BYTES - g_hda.ring_wp;
		uint32_t n    = want < room ? want : room;

		/* Block until LPIB has moved past the region we'd write. */
		(void)hda_drain_to(g_hda.ring_wp);

		anx_memcpy(g_hda.ring + g_hda.ring_wp,
			   (const uint8_t *)frames + copied, n);
		copied        += n;
		g_hda.ring_wp = (g_hda.ring_wp + n) % HDA_RING_BYTES;
	}
	return ANX_OK;
}

static void
hda_sink_close(void)
{
	if (!g_hda.running)
		return;
	stream_run(false);
	g_hda.running = false;

	/* Release the codec stream tag back to "no stream". */
	verb_xchg(anx_hda_verb(g_hda.codec_addr, g_hda.dac_nid,
			       HDA_VERB_SET_CONVERTER_STREAM, 0));
}

static const struct anx_audio_sink_ops hda_sink_ops = {
	.open  = hda_sink_open,
	.write = hda_sink_write,
	.close = hda_sink_close,
};

/* ------------------------------------------------------------------ */
/* Public init                                                         */
/* ------------------------------------------------------------------ */

bool
anx_hda_present(void)
{
	return g_hda.ready;
}

int
anx_hda_init(void)
{
	struct anx_pci_device *pci = NULL;
	struct anx_list_head  *head;
	struct anx_list_head  *it;
	uint32_t bar0;
	uint16_t gcap;
	int      rc;

	if (g_hda.ready)
		return ANX_OK;

	head = anx_pci_device_list();
	ANX_LIST_FOR_EACH(it, head) {
		struct anx_pci_device *d =
			ANX_LIST_ENTRY(it, struct anx_pci_device, link);
		if (d->class_code == HDA_PCI_CLASS &&
		    d->subclass   == HDA_PCI_SUBCLASS) {
			pci = d;
			break;
		}
	}
	if (!pci)
		return ANX_ENODEV;

	/* BAR0 holds the controller MMIO base.  Mask off the type bits. */
	bar0 = pci->bar[0] & ~0xFu;
	if (bar0 == 0)
		return ANX_ENODEV;

	g_hda.pci = pci;
	g_hda.bar = (volatile uint8_t *)(uintptr_t)bar0;

	anx_pci_enable_bus_master(pci);

	rc = hda_reset();
	if (rc != ANX_OK) {
		kprintf("[hda] controller reset failed (%d)\n", rc);
		return rc;
	}

	gcap = reg16(ANX_HDA_GCAP);
	g_hda.num_input_streams  = (uint8_t)((gcap >> 8) & 0x0F);
	g_hda.num_output_streams = (uint8_t)((gcap >> 12) & 0x0F);
	g_hda.num_bidir_streams  = (uint8_t)((gcap >> 3) & 0x1F);

	if (g_hda.num_output_streams == 0) {
		kprintf("[hda] controller advertises zero output streams\n");
		return ANX_ENODEV;
	}
	g_hda.sd_off = anx_hda_stream_off(g_hda.num_input_streams, 0);

	rc = corb_rirb_init();
	if (rc != ANX_OK)
		return rc;

	/* Find the first codec that responded after reset. */
	{
		uint8_t cad = find_first_codec();
		if (cad == 0xFF) {
			kprintf("[hda] no codec found in STATESTS\n");
			return ANX_ENODEV;
		}
		g_hda.codec_addr = cad;
	}

	/* Locate the audio function group, then a DAC + Pin. */
	g_hda.afg_nid = find_afg(g_hda.codec_addr);
	if (!g_hda.afg_nid) {
		kprintf("[hda] no audio function group\n");
		return ANX_ENODEV;
	}
	rc = find_dac_and_pin(g_hda.codec_addr, g_hda.afg_nid,
			      &g_hda.dac_nid, &g_hda.pin_nid);
	if (rc != ANX_OK) {
		kprintf("[hda] no DAC + Pin output path\n");
		return rc;
	}

	/* Reset the AFG to known defaults. */
	verb_xchg(anx_hda_verb(g_hda.codec_addr, g_hda.afg_nid,
			       HDA_VERB_FUNCTION_RESET, 0));

	rc = stream_alloc_buffers();
	if (rc != ANX_OK)
		return rc;

	g_hda.ready = true;
	anx_audio_sink_register("hda", &hda_sink_ops);

	kprintf("[hda] codec=%u afg=%u dac=%u pin=%u sd@%u sink registered\n",
		g_hda.codec_addr, g_hda.afg_nid, g_hda.dac_nid, g_hda.pin_nid,
		g_hda.sd_off);
	return ANX_OK;
}
