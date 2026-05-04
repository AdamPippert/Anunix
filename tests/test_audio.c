/*
 * test_audio.c — Tests for the audio engine + clip + cell (RFC-0024).
 *
 * Coverage:
 *   1.  Clip header validation (magic, version, sizes).
 *   2.  Mixer baseline: single-stream DC pass-through at unity volume.
 *   3.  Two-stream additive mix at unity volume.
 *   4.  Sub-unity Q15 volume attenuation.
 *   5.  Cell dispatch ("audio-play") emits a trace OID.
 *   6.  Pause flag silences a stream mid-pull.
 *   7.  Loop flag wraps the source instead of ending.
 *   8.  Mono → stereo upmix.
 *   9.  8-bit and 24-bit decode parity.
 *  10.  Linear-interpolation upsample (24 kHz → 48 kHz).
 *  11.  Saturation clamp on heavy mix sums.
 *  12.  Concurrent stream cap (ANX_AUDIO_MAX_STREAMS) is enforced.
 *  13.  Wav sink seals a clip OID and a valid RIFF byte image.
 *  14.  Wav encoder header round-trip.
 */

#include <anx/types.h>
#include <anx/audio.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/alloc.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Build a 16-bit stereo clip of N frames of a constant DC sample. */
static int
make_dc_clip(uint32_t frames, int16_t left, int16_t right,
	     anx_oid_t *oid_out)
{
	uint32_t hdr_sz = sizeof(struct anx_audio_clip_hdr);
	uint32_t pcm_sz = frames * 2 * sizeof(int16_t);
	uint32_t total  = hdr_sz + pcm_sz;
	uint8_t *blob   = (uint8_t *)anx_alloc(total);
	uint32_t i;

	if (!blob) return ANX_ENOMEM;
	struct anx_audio_clip_hdr *h = (struct anx_audio_clip_hdr *)blob;
	anx_memset(h, 0, sizeof(*h));
	h->magic               = ANX_AUDIO_CLIP_MAGIC;
	h->version             = ANX_AUDIO_CLIP_VERSION;
	h->fmt.sample_rate     = ANX_AUDIO_DEFAULT_RATE;
	h->fmt.channels        = 2;
	h->fmt.bits_per_sample = 16;
	h->frame_count         = frames;
	h->payload_offset      = hdr_sz;
	int16_t *p = (int16_t *)(blob + hdr_sz);
	for (i = 0; i < frames; i++) {
		p[2 * i + 0] = left;
		p[2 * i + 1] = right;
	}
	int rc = anx_audio_clip_create(blob, total, oid_out);
	anx_free(blob);
	return rc;
}

/* Build a clip at any (rate, bps, channels) of constant DC samples.
 * sample_int is interpreted as int16-domain and rescaled for bps. */
static int
make_clip_format(uint32_t frames, uint32_t rate, uint16_t channels,
		 uint16_t bps, int16_t sample_int, anx_oid_t *oid_out)
{
	uint32_t hdr_sz = sizeof(struct anx_audio_clip_hdr);
	uint32_t bps_bytes = bps / 8;
	uint32_t pcm_sz = frames * channels * bps_bytes;
	uint32_t total  = hdr_sz + pcm_sz;
	uint8_t *blob   = (uint8_t *)anx_alloc(total);
	uint32_t i, c;

	if (!blob) return ANX_ENOMEM;
	struct anx_audio_clip_hdr *h = (struct anx_audio_clip_hdr *)blob;
	anx_memset(h, 0, sizeof(*h));
	h->magic               = ANX_AUDIO_CLIP_MAGIC;
	h->version             = ANX_AUDIO_CLIP_VERSION;
	h->fmt.sample_rate     = rate;
	h->fmt.channels        = channels;
	h->fmt.bits_per_sample = bps;
	h->frame_count         = frames;
	h->payload_offset      = hdr_sz;

	uint8_t *p = blob + hdr_sz;
	for (i = 0; i < frames; i++) {
		for (c = 0; c < channels; c++) {
			int32_t v = sample_int;
			switch (bps) {
			case 8: {
				/* int16-domain → int8 (truncate hi byte) */
				int8_t s8 = (int8_t)(v >> 8);
				*p++ = (uint8_t)s8;
				break;
			}
			case 16:
				*p++ = (uint8_t)(v & 0xFF);
				*p++ = (uint8_t)((v >> 8) & 0xFF);
				break;
			case 24: {
				int32_t s24 = v << 8;	/* int16 → int24 */
				*p++ = (uint8_t)(s24 & 0xFF);
				*p++ = (uint8_t)((s24 >> 8) & 0xFF);
				*p++ = (uint8_t)((s24 >> 16) & 0xFF);
				break;
			}
			case 32: {
				int32_t s32 = v << 16;	/* int16 → int32 */
				*p++ = (uint8_t)(s32 & 0xFF);
				*p++ = (uint8_t)((s32 >> 8) & 0xFF);
				*p++ = (uint8_t)((s32 >> 16) & 0xFF);
				*p++ = (uint8_t)((s32 >> 24) & 0xFF);
				break;
			}
			default:
				*p++ = 0; break;
			}
		}
	}
	int rc = anx_audio_clip_create(blob, total, oid_out);
	anx_free(blob);
	return rc;
}

/* Build a sweep clip: 16-bit stereo, source samples follow s[i] = i. */
static int
make_ramp_clip(uint32_t frames, uint32_t rate, anx_oid_t *oid_out)
{
	uint32_t hdr_sz = sizeof(struct anx_audio_clip_hdr);
	uint32_t pcm_sz = frames * 2 * sizeof(int16_t);
	uint32_t total  = hdr_sz + pcm_sz;
	uint8_t *blob   = (uint8_t *)anx_alloc(total);
	uint32_t i;

	if (!blob) return ANX_ENOMEM;
	struct anx_audio_clip_hdr *h = (struct anx_audio_clip_hdr *)blob;
	anx_memset(h, 0, sizeof(*h));
	h->magic               = ANX_AUDIO_CLIP_MAGIC;
	h->version             = ANX_AUDIO_CLIP_VERSION;
	h->fmt.sample_rate     = rate;
	h->fmt.channels        = 2;
	h->fmt.bits_per_sample = 16;
	h->frame_count         = frames;
	h->payload_offset      = hdr_sz;
	int16_t *p = (int16_t *)(blob + hdr_sz);
	for (i = 0; i < frames; i++) {
		p[2 * i + 0] = (int16_t)(i * 100);	/* 0, 100, 200, ... */
		p[2 * i + 1] = (int16_t)(i * 100);
	}
	int rc = anx_audio_clip_create(blob, total, oid_out);
	anx_free(blob);
	return rc;
}

/* ------------------------------------------------------------------ */

int test_audio(void)
{
	int rc;

	anx_objstore_init();
	anx_audio_init();

	/* === Test 1: validate rejects bad magic === */
	{
		uint8_t buf[64];
		anx_memset(buf, 0, sizeof(buf));
		struct anx_audio_clip_hdr *h = (struct anx_audio_clip_hdr *)buf;
		h->magic = 0xDEADBEEF;
		if (anx_audio_clip_validate(buf, sizeof(buf)) == ANX_OK)
			return -1;
	}

	/* === Test 2: select capture sink === */
	rc = anx_audio_sink_select("capture");
	if (rc != ANX_OK) return -2;

	/* === Test 3: single-stream DC pass-through, unity volume === */
	{
		anx_oid_t                clip;
		struct anx_audio_stream *st;
		const int16_t           *cap;
		uint32_t                 cap_frames;
		uint32_t                 i;
		const uint32_t           N = 256;

		anx_audio_sink_capture_reset();

		rc = make_dc_clip(N, 1000, -1000, &clip);
		if (rc != ANX_OK) return -3;
		rc = anx_audio_stream_add(&clip, &st);
		if (rc != ANX_OK) return -4;
		rc = anx_audio_sink_run(N);
		if (rc != ANX_OK) return -5;

		cap = anx_audio_sink_capture_buffer(&cap_frames);
		if (cap_frames == 0) return -6;
		for (i = 0; i < cap_frames && i < 16; i++) {
			if (cap[2 * i + 0] != 1000 ||
			    cap[2 * i + 1] != -1000)
				return -7;
		}
		anx_audio_stream_remove(st->id);
	}

	/* === Test 4: two-stream additive mix === */
	{
		anx_oid_t                a, b;
		struct anx_audio_stream *sa, *sb;
		const int16_t           *cap;
		uint32_t                 cf;
		const uint32_t           N = 64;

		anx_audio_sink_capture_reset();
		rc = make_dc_clip(N, 500, 500, &a); if (rc != ANX_OK) return -8;
		rc = make_dc_clip(N, 200, 200, &b); if (rc != ANX_OK) return -9;
		rc = anx_audio_stream_add(&a, &sa); if (rc != ANX_OK) return -10;
		rc = anx_audio_stream_add(&b, &sb); if (rc != ANX_OK) return -11;
		rc = anx_audio_sink_run(N);          if (rc != ANX_OK) return -12;
		cap = anx_audio_sink_capture_buffer(&cf);
		if (cf == 0) return -13;
		if (cap[0] != 700 || cap[1] != 700) return -14;
		anx_audio_stream_remove(sa->id);
		anx_audio_stream_remove(sb->id);
	}

	/* === Test 5: Q15 sub-unity volume === */
	{
		anx_oid_t                clip;
		struct anx_audio_stream *st;
		const int16_t           *cap;
		uint32_t                 cf;
		const uint32_t           N = 32;

		anx_audio_sink_capture_reset();
		rc = make_dc_clip(N, 32000, 32000, &clip);
		if (rc != ANX_OK) return -15;
		rc = anx_audio_stream_add(&clip, &st); if (rc != ANX_OK) return -16;
		anx_audio_stream_set_volume(st->id, 16384); /* 0.5 */
		rc = anx_audio_sink_run(N); if (rc != ANX_OK) return -17;
		cap = anx_audio_sink_capture_buffer(&cf);
		/* (32000 * 16384 + (1<<14)) >> 15 == 16000 */
		if (cap[0] != 16000) return -18;
		anx_audio_stream_remove(st->id);
	}

	/* === Test 6: cell dispatch audio-play === */
	{
		anx_oid_t clip, out;
		const uint32_t N = 32;
		anx_audio_sink_capture_reset();
		rc = make_dc_clip(N, 100, 100, &clip);
		if (rc != ANX_OK) return -19;
		anx_memset(&out, 0, sizeof(out));
		rc = anx_audio_cell_dispatch("audio-play", &clip, 1, &out);
		if (rc != ANX_OK) return -20;
		struct anx_state_object *trace = anx_objstore_lookup(&out);
		if (!trace) return -21;
		if (trace->object_type != ANX_OBJ_EXECUTION_TRACE) {
			anx_objstore_release(trace);
			return -22;
		}
		anx_objstore_release(trace);
	}

	/* === Test 7: pause silences mid-pull === */
	{
		anx_oid_t                clip;
		struct anx_audio_stream *st;
		const int16_t           *cap;
		uint32_t                 cf;
		const uint32_t           N = 16;

		anx_audio_sink_capture_reset();
		rc = make_dc_clip(N, 5000, 5000, &clip);
		if (rc != ANX_OK) return -23;
		rc = anx_audio_stream_add(&clip, &st); if (rc != ANX_OK) return -24;
		anx_audio_stream_pause(st->id, true);
		rc = anx_audio_sink_run(N);
		(void)rc; /* paused stream → no contribution; sink_run reports
		           * ANX_OK but produced=0; that's expected. */
		cap = anx_audio_sink_capture_buffer(&cf);
		/* No frames should have been written because produced=0
		 * caused the sink loop to break before the first write.
		 * cf may be 0; if non-zero, samples must be silence. */
		if (cf > 0 && (cap[0] != 0 || cap[1] != 0))
			return -25;
		anx_audio_stream_remove(st->id);
	}

	/* === Test 8: loop wraps source === */
	{
		anx_oid_t                clip;
		struct anx_audio_stream *st;
		const int16_t           *cap;
		uint32_t                 cf;
		const uint32_t           N_SRC = 4;	/* tiny source */
		const uint32_t           N_OUT = 16;	/* request 4× the source */

		anx_audio_sink_capture_reset();
		rc = make_dc_clip(N_SRC, 1234, 1234, &clip);
		if (rc != ANX_OK) return -26;
		rc = anx_audio_stream_add(&clip, &st); if (rc != ANX_OK) return -27;
		st->loop = true;
		rc = anx_audio_sink_run(N_OUT); if (rc != ANX_OK) return -28;
		cap = anx_audio_sink_capture_buffer(&cf);
		if (cf < N_OUT) return -29;	/* loop must produce all */
		for (uint32_t i = 0; i < N_OUT; i++) {
			if (cap[2 * i + 0] != 1234 ||
			    cap[2 * i + 1] != 1234)
				return -30;
		}
		anx_audio_stream_remove(st->id);
	}

	/* === Test 9: mono → stereo upmix === */
	{
		anx_oid_t                clip;
		struct anx_audio_stream *st;
		const int16_t           *cap;
		uint32_t                 cf;
		const uint32_t           N = 16;

		anx_audio_sink_capture_reset();
		rc = make_clip_format(N, ANX_AUDIO_DEFAULT_RATE, 1, 16,
				      4242, &clip);
		if (rc != ANX_OK) return -31;
		rc = anx_audio_stream_add(&clip, &st); if (rc != ANX_OK) return -32;
		rc = anx_audio_sink_run(N); if (rc != ANX_OK) return -33;
		cap = anx_audio_sink_capture_buffer(&cf);
		if (cf == 0) return -34;
		/* Mono samples must be duplicated to both output channels. */
		if (cap[0] != 4242 || cap[1] != 4242) return -35;
		anx_audio_stream_remove(st->id);
	}

	/* === Test 10: 8-bit and 24-bit decode parity === */
	{
		/* 8-bit DC of (15000>>8)<<8 = 14848 (after re-scale) */
		anx_oid_t                c8, c24;
		struct anx_audio_stream *st;
		const int16_t           *cap;
		uint32_t                 cf;
		const uint32_t           N = 8;

		/* 8-bit at 15000 → stored as int8 = 58 → decoded back as 58<<8 = 14848 */
		anx_audio_sink_capture_reset();
		rc = make_clip_format(N, ANX_AUDIO_DEFAULT_RATE, 2, 8,
				      15000, &c8);
		if (rc != ANX_OK) return -36;
		rc = anx_audio_stream_add(&c8, &st); if (rc != ANX_OK) return -37;
		rc = anx_audio_sink_run(N); if (rc != ANX_OK) return -38;
		cap = anx_audio_sink_capture_buffer(&cf);
		if (cf == 0) return -39;
		if (cap[0] != 14848) return -40;	/* 58 << 8 */
		anx_audio_stream_remove(st->id);

		/* 24-bit at 15000 → stored as int24 (15000<<8) → decoded
		 * as int24 >> 8 = 15000 (lossless round-trip). */
		anx_audio_sink_capture_reset();
		rc = make_clip_format(N, ANX_AUDIO_DEFAULT_RATE, 2, 24,
				      15000, &c24);
		if (rc != ANX_OK) return -41;
		rc = anx_audio_stream_add(&c24, &st); if (rc != ANX_OK) return -42;
		rc = anx_audio_sink_run(N); if (rc != ANX_OK) return -43;
		cap = anx_audio_sink_capture_buffer(&cf);
		if (cf == 0) return -44;
		if (cap[0] != 15000) return -45;
		anx_audio_stream_remove(st->id);
	}

	/* === Test 11: linear-interp upsample 24 kHz → 48 kHz ===
	 *
	 * Source ramp s[i] = i*100 at 24 kHz produces, at 48 kHz output,
	 * a sequence with mid-points interpolated.  Output frame k maps
	 * to source position k * 24000 / 48000 = k/2 (Q32.32 step = 0.5).
	 *
	 *   k=0  → src[0]               = 0
	 *   k=1  → src[0] + 0.5*(s[1]-s[0]) = 50
	 *   k=2  → src[1]               = 100
	 *   k=3  → src[1] + 0.5*(s[2]-s[1]) = 150
	 *
	 * Linear interp is exact-by-construction for this ramp.
	 */
	{
		anx_oid_t                clip;
		struct anx_audio_stream *st;
		const int16_t           *cap;
		uint32_t                 cf;
		const uint32_t           N_SRC = 100;
		const uint32_t           N_OUT = 8;

		anx_audio_sink_capture_reset();
		rc = make_ramp_clip(N_SRC, 24000, &clip);
		if (rc != ANX_OK) return -46;
		rc = anx_audio_stream_add(&clip, &st); if (rc != ANX_OK) return -47;
		rc = anx_audio_sink_run(N_OUT); if (rc != ANX_OK) return -48;
		cap = anx_audio_sink_capture_buffer(&cf);
		if (cf < N_OUT) return -49;
		/* Allow ±2 LSB tolerance for the fixed-point step rounding;
		 * step = (24000<<32)/48000 is exactly 0x80000000, so error
		 * should be 0 for this case. */
		const int16_t expected[] = { 0, 50, 100, 150, 200, 250, 300, 350 };
		for (uint32_t i = 0; i < N_OUT; i++) {
			int16_t got = cap[2 * i + 0];
			int16_t exp = expected[i];
			int16_t diff = got > exp ? got - exp : exp - got;
			if (diff > 2) return -50;
		}
		anx_audio_stream_remove(st->id);
	}

	/* === Test 12: heavy mix saturates without overflow ===
	 *
	 * Five DC streams at +20000 each sum to +100000, which clamps to
	 * +32767.  The accumulator must not wrap.
	 */
	{
		anx_oid_t                clip[5];
		struct anx_audio_stream *st[5];
		const int16_t           *cap;
		uint32_t                 cf;
		const uint32_t           N = 8;

		anx_audio_sink_capture_reset();
		for (uint32_t i = 0; i < 5; i++) {
			rc = make_dc_clip(N, 20000, 20000, &clip[i]);
			if (rc != ANX_OK) return -51;
			rc = anx_audio_stream_add(&clip[i], &st[i]);
			if (rc != ANX_OK) return -52;
		}
		rc = anx_audio_sink_run(N); if (rc != ANX_OK) return -53;
		cap = anx_audio_sink_capture_buffer(&cf);
		if (cf == 0) return -54;
		if (cap[0] != 32767 || cap[1] != 32767) return -55;
		for (uint32_t i = 0; i < 5; i++)
			anx_audio_stream_remove(st[i]->id);
	}

	/* === Test 13: stream cap enforced === */
	{
		anx_oid_t                clip;
		struct anx_audio_stream *st[ANX_AUDIO_MAX_STREAMS];
		uint16_t                 ids[ANX_AUDIO_MAX_STREAMS];
		uint32_t                 i;
		struct anx_audio_stream *over;

		rc = make_dc_clip(8, 1, 1, &clip);
		if (rc != ANX_OK) return -56;
		for (i = 0; i < ANX_AUDIO_MAX_STREAMS; i++) {
			rc = anx_audio_stream_add(&clip, &st[i]);
			if (rc != ANX_OK) return -57;
			ids[i] = st[i]->id;
		}
		/* (ANX_AUDIO_MAX_STREAMS + 1)th add must fail with EFULL. */
		rc = anx_audio_stream_add(&clip, &over);
		if (rc != ANX_EFULL) return -58;
		for (i = 0; i < ANX_AUDIO_MAX_STREAMS; i++)
			anx_audio_stream_remove(ids[i]);
	}

	/* === Test 14: wav sink produces a clip OID === */
	{
		anx_oid_t                clip, wav_oid;
		struct anx_audio_stream *st;
		const uint32_t           N = 64;

		rc = anx_audio_sink_select("wav");
		if (rc != ANX_OK) return -59;

		rc = make_dc_clip(N, 7777, -7777, &clip);
		if (rc != ANX_OK) return -60;
		rc = anx_audio_stream_add(&clip, &st);
		if (rc != ANX_OK) return -61;
		rc = anx_audio_sink_run(N);
		if (rc != ANX_OK) return -62;
		anx_audio_stream_remove(st->id);

		rc = anx_audio_sink_wav_last_oid(&wav_oid);
		if (rc != ANX_OK) return -63;

		struct anx_state_object *clip_obj = anx_objstore_lookup(&wav_oid);
		if (!clip_obj) return -64;
		if (clip_obj->object_type != ANX_OBJ_AUDIO_CLIP) {
			anx_objstore_release(clip_obj);
			return -65;
		}
		const struct anx_audio_clip_hdr *h =
			(const struct anx_audio_clip_hdr *)clip_obj->payload;
		if (h->frame_count != N ||
		    h->fmt.sample_rate != ANX_AUDIO_DEFAULT_RATE ||
		    h->fmt.channels != 2 ||
		    h->fmt.bits_per_sample != 16) {
			anx_objstore_release(clip_obj);
			return -66;
		}
		const int16_t *pcm = (const int16_t *)
			((const uint8_t *)clip_obj->payload + h->payload_offset);
		if (pcm[0] != 7777 || pcm[1] != -7777) {
			anx_objstore_release(clip_obj);
			return -67;
		}
		anx_objstore_release(clip_obj);

		/* RIFF byte image must also be available and well-formed. */
		uint32_t rsize;
		const uint8_t *riff = anx_audio_sink_wav_last_riff(&rsize);
		if (!riff || rsize < 44) return -68;
		if (riff[0] != 'R' || riff[1] != 'I' || riff[2] != 'F' || riff[3] != 'F')
			return -69;
		if (riff[8] != 'W' || riff[9] != 'A' || riff[10] != 'V' || riff[11] != 'E')
			return -70;
		if (riff[12] != 'f' || riff[13] != 'm' || riff[14] != 't' || riff[15] != ' ')
			return -71;
		if (riff[36] != 'd' || riff[37] != 'a' || riff[38] != 't' || riff[39] != 'a')
			return -72;
		/* sample rate at offset 24 (LE32) */
		uint32_t srate = (uint32_t)riff[24] |
				 ((uint32_t)riff[25] << 8) |
				 ((uint32_t)riff[26] << 16) |
				 ((uint32_t)riff[27] << 24);
		if (srate != ANX_AUDIO_DEFAULT_RATE) return -73;

		/* Restore null sink for downstream tests. */
		anx_audio_sink_select("null");
	}

	/* === Test 15: wav encoder size query and round-trip === */
	{
		int16_t  pcm[8] = { 0, 0, 100, 100, 200, 200, 300, 300 };
		uint8_t  buf[256];
		int      need, wrote;

		need = anx_audio_wav_encode(NULL, 4, 48000, 2, NULL, 0);
		if (need != 44 + 4 * 2 * 2) return -74;	/* hdr + 16 PCM bytes */

		wrote = anx_audio_wav_encode(pcm, 4, 48000, 2,
					     buf, sizeof(buf));
		if (wrote != need) return -75;
		/* RIFF size in header (offset 4) = total - 8 */
		uint32_t riff_sz = (uint32_t)buf[4] |
				   ((uint32_t)buf[5] << 8) |
				   ((uint32_t)buf[6] << 16) |
				   ((uint32_t)buf[7] << 24);
		if (riff_sz != (uint32_t)wrote - 8) return -76;
		/* data size at offset 40 = pcm bytes */
		uint32_t data_sz = (uint32_t)buf[40] |
				   ((uint32_t)buf[41] << 8) |
				   ((uint32_t)buf[42] << 16) |
				   ((uint32_t)buf[43] << 24);
		if (data_sz != 16) return -77;
		/* PCM verbatim at offset 44 */
		const int16_t *out_pcm = (const int16_t *)&buf[44];
		for (uint32_t i = 0; i < 4 * 2; i++) {
			if (out_pcm[i] != pcm[i]) return -78;
		}
	}

	return 0;
}
