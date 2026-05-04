/*
 * sink.c — Audio sink registry and built-in null/capture sinks.
 */

#include <anx/audio.h>
#include <anx/types.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/spinlock.h>
#include <anx/kprintf.h>

#define ANX_AUDIO_SINK_MAX	8

struct sink_slot {
	char                            name[ANX_AUDIO_SINK_NAME_MAX];
	const struct anx_audio_sink_ops *ops;
	bool                            in_use;
};

static struct sink_slot   g_sinks[ANX_AUDIO_SINK_MAX];
static struct sink_slot  *g_active;
static struct anx_spinlock g_lock;
static bool                g_inited;

static void ensure_init(void)
{
	if (!g_inited) {
		anx_spin_init(&g_lock);
		g_inited = true;
	}
}

int anx_audio_sink_register(const char *name,
			    const struct anx_audio_sink_ops *ops)
{
	uint32_t i;
	int      ret = ANX_EFULL;

	if (!name || !ops)
		return ANX_EINVAL;
	ensure_init();

	anx_spin_lock(&g_lock);
	for (i = 0; i < ANX_AUDIO_SINK_MAX; i++) {
		if (g_sinks[i].in_use &&
		    anx_strcmp(g_sinks[i].name, name) == 0) {
			g_sinks[i].ops = ops;
			ret = ANX_OK;
			goto out;
		}
	}
	for (i = 0; i < ANX_AUDIO_SINK_MAX; i++) {
		if (!g_sinks[i].in_use) {
			anx_strlcpy(g_sinks[i].name, name,
				    sizeof(g_sinks[i].name));
			g_sinks[i].ops    = ops;
			g_sinks[i].in_use = true;
			ret = ANX_OK;
			break;
		}
	}
out:
	anx_spin_unlock(&g_lock);
	return ret;
}

int anx_audio_sink_select(const char *name)
{
	uint32_t i;
	int      ret = ANX_ENOENT;

	if (!name)
		return ANX_EINVAL;
	ensure_init();

	anx_spin_lock(&g_lock);
	for (i = 0; i < ANX_AUDIO_SINK_MAX; i++) {
		if (g_sinks[i].in_use &&
		    anx_strcmp(g_sinks[i].name, name) == 0) {
			g_active = &g_sinks[i];
			ret = ANX_OK;
			break;
		}
	}
	anx_spin_unlock(&g_lock);
	return ret;
}

const char *anx_audio_sink_active(void)
{
	const char *name = NULL;

	ensure_init();
	anx_spin_lock(&g_lock);
	if (g_active)
		name = g_active->name;
	anx_spin_unlock(&g_lock);
	return name;
}

int anx_audio_sink_run(uint32_t total_frames)
{
	struct sink_slot                *active;
	const struct anx_audio_sink_ops *ops;
	struct anx_audio_format          fmt;
	int16_t                         *buf;
	uint32_t                         chunk = 1024;
	uint32_t                         done  = 0;
	int                              ret   = ANX_OK;

	ensure_init();
	anx_spin_lock(&g_lock);
	active = g_active;
	anx_spin_unlock(&g_lock);
	if (!active || !active->ops)
		return ANX_ENOENT;
	ops = active->ops;

	anx_memset(&fmt, 0, sizeof(fmt));
	fmt.sample_rate     = ANX_AUDIO_DEFAULT_RATE;
	fmt.channels        = ANX_AUDIO_DEFAULT_CHANNELS;
	fmt.bits_per_sample = ANX_AUDIO_INTERNAL_BPS;
	if (ops->open) {
		ret = ops->open(&fmt);
		if (ret != ANX_OK)
			return ret;
	}

	buf = (int16_t *)anx_alloc(chunk * 2 * sizeof(int16_t));
	if (!buf) {
		if (ops->close) ops->close();
		return ANX_ENOMEM;
	}

	while (done < total_frames) {
		uint32_t want = total_frames - done;
		int      got;

		if (want > chunk) want = chunk;
		got = anx_audio_mix_pull(buf, want);
		if (got <= 0)
			break;
		if (ops->write) {
			ret = ops->write(buf, (uint32_t)got);
			if (ret != ANX_OK)
				break;
		}
		done += (uint32_t)got;
	}

	anx_free(buf);
	if (ops->close)
		ops->close();
	return done > 0 ? ANX_OK : ret;
}

/* ------------------------------------------------------------------ */
/* Built-in: null sink (drains the mixer)                              */
/* ------------------------------------------------------------------ */

static int null_open(struct anx_audio_format *fmt) { (void)fmt; return ANX_OK; }
static int null_write(const int16_t *f, uint32_t n) { (void)f; (void)n; return ANX_OK; }
static void null_close(void) {}

static const struct anx_audio_sink_ops null_ops = {
	.open  = null_open,
	.write = null_write,
	.close = null_close,
};

void anx_audio_sink_null_register(void)
{
	anx_audio_sink_register("null", &null_ops);
}

/* ------------------------------------------------------------------ */
/* Built-in: capture sink (writes into a static buffer for tests)      */
/* ------------------------------------------------------------------ */

#define CAPTURE_FRAMES_MAX	(ANX_AUDIO_DEFAULT_RATE * 4)	/* 4 seconds */

static int16_t  g_cap_buf[CAPTURE_FRAMES_MAX * 2];
static uint32_t g_cap_frames;

static int cap_open(struct anx_audio_format *fmt) { (void)fmt; g_cap_frames = 0; return ANX_OK; }
static int cap_write(const int16_t *f, uint32_t n)
{
	uint32_t room = CAPTURE_FRAMES_MAX - g_cap_frames;
	uint32_t copy = n < room ? n : room;

	if (copy) {
		anx_memcpy(&g_cap_buf[g_cap_frames * 2], f,
			   copy * 2 * sizeof(int16_t));
		g_cap_frames += copy;
	}
	return ANX_OK;
}
static void cap_close(void) {}

static const struct anx_audio_sink_ops cap_ops = {
	.open  = cap_open,
	.write = cap_write,
	.close = cap_close,
};

void anx_audio_sink_capture_register(void)
{
	anx_audio_sink_register("capture", &cap_ops);
}

const int16_t *anx_audio_sink_capture_buffer(uint32_t *frames_out)
{
	if (frames_out)
		*frames_out = g_cap_frames;
	return g_cap_buf;
}

void anx_audio_sink_capture_reset(void)
{
	g_cap_frames = 0;
	anx_memset(g_cap_buf, 0, sizeof(g_cap_buf));
}

/* ------------------------------------------------------------------ */
/* RIFF / WAV byte serializer                                          */
/*                                                                     */
/* We hand-roll the RIFF container because it's small, well-defined,   */
/* and pulling in a vendored encoder for ~44 bytes of header would be  */
/* overkill.  Layout (PCM, single fmt + data chunk):                   */
/*                                                                     */
/*   "RIFF" <riff_size:LE32> "WAVE"                                    */
/*   "fmt " <16:LE32> <fmt_chunk: 16 bytes>                            */
/*       audio_format:LE16=1   channels:LE16   sample_rate:LE32        */
/*       byte_rate:LE32        block_align:LE16  bits_per_sample:LE16  */
/*   "data" <data_size:LE32> <pcm bytes>                               */
/* ------------------------------------------------------------------ */

#include <anx/state_object.h>

static void
write_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void
write_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)((v >> 8) & 0xFF);
	p[2] = (uint8_t)((v >> 16) & 0xFF);
	p[3] = (uint8_t)((v >> 24) & 0xFF);
}

#define ANX_WAV_HDR_SIZE	44u

int
anx_audio_wav_encode(const int16_t *frames, uint32_t frame_count,
		     uint32_t sample_rate, uint16_t channels,
		     uint8_t *dst, uint32_t dst_size)
{
	uint32_t bps          = ANX_AUDIO_INTERNAL_BPS;
	uint32_t bytes_per_sample = bps / 8;
	uint32_t block_align  = channels * bytes_per_sample;
	uint32_t byte_rate    = sample_rate * block_align;
	uint64_t data_bytes64 = (uint64_t)frame_count * block_align;
	uint64_t total64      = ANX_WAV_HDR_SIZE + data_bytes64;

	if (channels == 0 || sample_rate == 0)
		return ANX_EINVAL;
	if (data_bytes64 > 0xFFFFFFFFu - ANX_WAV_HDR_SIZE)
		return ANX_EINVAL;	/* RIFF size field is 32 bits */

	if (dst == NULL)
		return (int)total64;	/* size query */

	if ((uint64_t)dst_size < total64)
		return ANX_EINVAL;

	uint32_t data_bytes = (uint32_t)data_bytes64;

	/* RIFF chunk */
	dst[0] = 'R'; dst[1] = 'I'; dst[2] = 'F'; dst[3] = 'F';
	write_le32(&dst[4], (uint32_t)(total64 - 8));	/* riff size */
	dst[8] = 'W'; dst[9] = 'A'; dst[10] = 'V'; dst[11] = 'E';

	/* fmt chunk */
	dst[12] = 'f'; dst[13] = 'm'; dst[14] = 't'; dst[15] = ' ';
	write_le32(&dst[16], 16);		/* fmt chunk size */
	write_le16(&dst[20], 1);		/* PCM */
	write_le16(&dst[22], channels);
	write_le32(&dst[24], sample_rate);
	write_le32(&dst[28], byte_rate);
	write_le16(&dst[32], (uint16_t)block_align);
	write_le16(&dst[34], (uint16_t)bps);

	/* data chunk */
	dst[36] = 'd'; dst[37] = 'a'; dst[38] = 't'; dst[39] = 'a';
	write_le32(&dst[40], data_bytes);

	if (frames && data_bytes > 0)
		anx_memcpy(&dst[ANX_WAV_HDR_SIZE], frames, data_bytes);

	return (int)total64;
}

/* ------------------------------------------------------------------ */
/* Built-in: wav-file sink                                             */
/*                                                                     */
/* On open(), reuses the capture buffer (4 s of stereo @ 48 kHz).      */
/* On write(), accumulates frames just like the capture sink.          */
/* On close(), seals the captured PCM as an ANX_OBJ_AUDIO_CLIP and     */
/* records the OID + RIFF byte image for retrieval.                    */
/* ------------------------------------------------------------------ */

static anx_oid_t        g_wav_last_oid;
static bool             g_wav_have_oid;
static uint8_t         *g_wav_last_riff;
static uint32_t         g_wav_last_riff_size;
static uint32_t         g_wav_frames;

static int
wav_open(struct anx_audio_format *fmt)
{
	(void)fmt;
	g_wav_frames     = 0;
	g_wav_have_oid   = false;
	if (g_wav_last_riff) {
		anx_free(g_wav_last_riff);
		g_wav_last_riff = NULL;
	}
	g_wav_last_riff_size = 0;
	g_cap_frames = 0;	/* reuse the capture buffer for storage */
	return ANX_OK;
}

static int
wav_write(const int16_t *f, uint32_t n)
{
	uint32_t room = CAPTURE_FRAMES_MAX - g_cap_frames;
	uint32_t copy = n < room ? n : room;

	if (copy > 0) {
		anx_memcpy(&g_cap_buf[g_cap_frames * 2], f,
			   copy * 2 * sizeof(int16_t));
		g_cap_frames += copy;
		g_wav_frames += copy;
	}
	return ANX_OK;
}

static void
wav_close(void)
{
	uint32_t hdr_sz = (uint32_t)sizeof(struct anx_audio_clip_hdr);
	uint32_t pcm_sz = g_wav_frames * 2u * sizeof(int16_t);
	uint32_t total  = hdr_sz + pcm_sz;
	uint8_t *blob;
	int rc;

	if (g_wav_frames == 0)
		return;

	/* 1) Seal the captured PCM as a State Object (clip payload). */
	blob = (uint8_t *)anx_alloc(total);
	if (blob) {
		struct anx_audio_clip_hdr *h =
			(struct anx_audio_clip_hdr *)blob;
		anx_memset(h, 0, sizeof(*h));
		h->magic               = ANX_AUDIO_CLIP_MAGIC;
		h->version             = ANX_AUDIO_CLIP_VERSION;
		h->fmt.sample_rate     = ANX_AUDIO_DEFAULT_RATE;
		h->fmt.channels        = ANX_AUDIO_DEFAULT_CHANNELS;
		h->fmt.bits_per_sample = ANX_AUDIO_INTERNAL_BPS;
		h->frame_count         = g_wav_frames;
		h->payload_offset      = hdr_sz;
		anx_memcpy(blob + hdr_sz, g_cap_buf, pcm_sz);

		rc = anx_audio_clip_create(blob, total, &g_wav_last_oid);
		if (rc == ANX_OK)
			g_wav_have_oid = true;
		anx_free(blob);
	}

	/* 2) Also produce the canonical RIFF/WAV byte image. */
	{
		int need = anx_audio_wav_encode(NULL, g_wav_frames,
						ANX_AUDIO_DEFAULT_RATE,
						ANX_AUDIO_DEFAULT_CHANNELS,
						NULL, 0);
		if (need > 0) {
			uint8_t *buf = (uint8_t *)anx_alloc((uint32_t)need);
			if (buf) {
				int wrote = anx_audio_wav_encode(
					g_cap_buf, g_wav_frames,
					ANX_AUDIO_DEFAULT_RATE,
					ANX_AUDIO_DEFAULT_CHANNELS,
					buf, (uint32_t)need);
				if (wrote == need) {
					g_wav_last_riff      = buf;
					g_wav_last_riff_size = (uint32_t)wrote;
				} else {
					anx_free(buf);
				}
			}
		}
	}
}

static const struct anx_audio_sink_ops wav_ops = {
	.open  = wav_open,
	.write = wav_write,
	.close = wav_close,
};

void
anx_audio_sink_wav_register(void)
{
	anx_audio_sink_register("wav", &wav_ops);
}

int
anx_audio_sink_wav_last_oid(anx_oid_t *out)
{
	if (!out)
		return ANX_EINVAL;
	if (!g_wav_have_oid)
		return ANX_ENOENT;
	*out = g_wav_last_oid;
	return ANX_OK;
}

const uint8_t *
anx_audio_sink_wav_last_riff(uint32_t *size_out)
{
	if (size_out)
		*size_out = g_wav_last_riff_size;
	return g_wav_last_riff;
}
