/*
 * audio.c — Software mixer (RFC-0024).
 *
 * Internal format: signed-int16 stereo at ANX_AUDIO_DEFAULT_RATE.
 *
 * Hot-path design:
 *   - Each stream pins its source State Object on add (refcount up) and
 *     caches the payload base pointer.  No object-store calls happen
 *     during anx_audio_mix_pull().
 *   - mix_pull() snapshots the live stream array under a single
 *     spinlock acquire, mixes unlocked into an int32 accumulator, then
 *     re-acquires the lock briefly to commit updated source positions.
 *   - The common case (int16 stereo, src_rate == out_rate) takes a
 *     specialized inner loop with two int16 loads per frame and an
 *     optional Q15 volume multiply.  The general path handles 8/16/24/
 *     32-bit integer PCM, mono->stereo upmix, and linear-interpolation
 *     resampling using a Q32.32 source position cursor.
 *   - Volume is Q15 with 32767 ≡ unity (the multiply is skipped).
 *     Sub-unity attenuation uses a rounded shift to halve quantization
 *     error.  Mid-mix never saturates (int32 accumulator); the final
 *     int32→int16 clamp happens once per pull.
 *
 * Thread/IRQ model: the spinlock guards stream-table mutations and the
 * snapshot/commit phases of mix_pull only.  The decode + mix inner
 * loop runs without any locking and never touches shared state.
 */

#include <anx/audio.h>
#include <anx/types.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/state_object.h>
#include <anx/spinlock.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Compile-time tunables                                               */
/* ------------------------------------------------------------------ */

#define ANX_AUDIO_VOL_UNITY	32767		/* Q15 unity sentinel */
#define ANX_AUDIO_PULL_CHUNK	1024U		/* internal mix chunk in frames */

/* ------------------------------------------------------------------ */
/* Stream pool                                                         */
/* ------------------------------------------------------------------ */

static struct anx_audio_stream	g_streams[ANX_AUDIO_MAX_STREAMS];
static uint16_t			g_next_id = 1;
static struct anx_spinlock	g_lock;
static bool			g_initialized;

/* int32 accumulator for one chunk; static to keep the stack clean. */
static int32_t			g_accum[ANX_AUDIO_PULL_CHUNK * 2];

/* ------------------------------------------------------------------ */
/* Small inline helpers                                                */
/* ------------------------------------------------------------------ */

static inline int16_t
sat_i32(int32_t v)
{
	if (v >  32767) return  32767;
	if (v < -32768) return -32768;
	return (int16_t)v;
}

/*
 * Sign-extending little-endian sample loaders.  Each returns a value
 * already scaled into the int16 range so the mixer can sum without
 * per-format bookkeeping.
 */
static inline int32_t
decode_i8(const uint8_t *p)
{
	return ((int32_t)(int8_t)p[0]) << 8;
}

static inline int32_t
decode_le16(const uint8_t *p)
{
	uint16_t u = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
	return (int32_t)(int16_t)u;
}

static inline int32_t
decode_le24(const uint8_t *p)
{
	int32_t v = (int32_t)((uint32_t)p[0] |
			      ((uint32_t)p[1] << 8) |
			      ((uint32_t)p[2] << 16));
	if (v & 0x00800000)
		v |= (int32_t)0xFF000000U;
	return v >> 8;	/* drop 8 LSBs to reach int16 dynamic range */
}

static inline int32_t
decode_le32(const uint8_t *p)
{
	int32_t v = (int32_t)((uint32_t)p[0] |
			      ((uint32_t)p[1] << 8) |
			      ((uint32_t)p[2] << 16) |
			      ((uint32_t)p[3] << 24));
	return v >> 16;	/* drop 16 LSBs to reach int16 dynamic range */
}

/* ------------------------------------------------------------------ */
/* Source resolution + clip header validation                          */
/* ------------------------------------------------------------------ */

static int
pin_clip(const anx_oid_t *oid, struct anx_state_object **obj_out,
	 const struct anx_audio_clip_hdr **hdr_out,
	 const uint8_t **base_out)
{
	struct anx_state_object         *obj;
	const struct anx_audio_clip_hdr *hdr;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_ENOENT;
	if (obj->object_type != ANX_OBJ_AUDIO_CLIP || !obj->payload ||
	    obj->payload_size < sizeof(*hdr)) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}
	hdr = (const struct anx_audio_clip_hdr *)obj->payload;
	if (hdr->magic != ANX_AUDIO_CLIP_MAGIC ||
	    hdr->version != ANX_AUDIO_CLIP_VERSION ||
	    hdr->payload_offset != sizeof(*hdr)) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}
	*obj_out  = obj;
	*hdr_out  = hdr;
	*base_out = (const uint8_t *)obj->payload + hdr->payload_offset;
	return ANX_OK;
}

/*
 * Compute the per-output-frame step in Q32.32 from a source rate.
 * step = (in_rate / out_rate) << 32, in pure 64-bit integer math.
 */
static uint64_t
compute_step_q32(uint32_t in_rate)
{
	uint32_t out_rate = ANX_AUDIO_DEFAULT_RATE;

	if (in_rate == 0 || in_rate == out_rate)
		return ((uint64_t)1) << 32;
	return ((uint64_t)in_rate << 32) / (uint64_t)out_rate;
}

/* ------------------------------------------------------------------ */
/* Inner mixers                                                        */
/* ------------------------------------------------------------------ */

struct stream_snap {
	uint16_t	slot;
	uint16_t	bps;
	uint16_t	channels;
	int16_t		volume_q15;
	bool		loop;
	bool		eqrate;		/* src_rate == out_rate */
	bool		is_int16_stereo;
	bool		ended;		/* set true if source ran out mid-pull */
	const uint8_t  *base;
	uint64_t	src_total;	/* in source frames */
	uint64_t	pos_q32;	/* updated by inner mixers */
	uint64_t	step_q32;
};

/*
 * Read a single source frame at integer index 'idx' into (l, r).
 * Mono is duplicated to both channels.
 */
static inline void
read_frame_i(const uint8_t *base, uint64_t idx,
	     uint16_t bps, uint16_t channels,
	     int32_t *out_l, int32_t *out_r)
{
	uint32_t bps_bytes = bps >> 3;
	uint64_t off       = idx * (uint64_t)channels * bps_bytes;
	const uint8_t *p   = base + off;
	int32_t l, r;

	switch (bps) {
	case  8: l = decode_i8(p);   break;
	case 16: l = decode_le16(p); break;
	case 24: l = decode_le24(p); break;
	case 32: l = decode_le32(p); break;
	default: l = 0; break;
	}
	if (channels >= 2) {
		const uint8_t *pr = p + bps_bytes;
		switch (bps) {
		case  8: r = decode_i8(pr);   break;
		case 16: r = decode_le16(pr); break;
		case 24: r = decode_le24(pr); break;
		case 32: r = decode_le32(pr); break;
		default: r = 0; break;
		}
	} else {
		r = l;
	}
	*out_l = l;
	*out_r = r;
}

/*
 * Fast path: int16 stereo, source rate == output rate.  pos_q32 is
 * always frame-aligned for this caller; we step by integer source
 * frames.  Returns the number of output frames produced.
 */
static uint32_t
mix_eqrate_int16_stereo(int32_t *acc, uint32_t frames,
			struct stream_snap *s)
{
	const int16_t *src   = (const int16_t *)s->base;
	uint64_t       pos   = s->pos_q32 >> 32;
	uint64_t       total = s->src_total;
	int16_t        vol   = s->volume_q15;
	bool           loop  = s->loop;
	bool           unity = (vol == ANX_AUDIO_VOL_UNITY);
	uint32_t       produced = 0;
	uint32_t       f;

	for (f = 0; f < frames; f++) {
		if (pos >= total) {
			if (loop) {
				pos = 0;
			} else {
				s->ended = true;
				break;
			}
		}
		int32_t l = src[2 * pos + 0];
		int32_t r = src[2 * pos + 1];
		if (!unity) {
			l = (l * (int32_t)vol + (1 << 14)) >> 15;
			r = (r * (int32_t)vol + (1 << 14)) >> 15;
		}
		acc[2 * f + 0] += l;
		acc[2 * f + 1] += r;
		pos++;
		produced++;
	}
	s->pos_q32 = pos << 32;
	return produced;
}

/*
 * General path: any bps, any channel count, any rate.  Linear
 * interpolation across the source.  pos_q32 is the source-frame
 * cursor in Q32.32; step_q32 is the per-output-frame increment.
 *
 * Linear interp: out = src[i] + ((src[i+1] - src[i]) * frac) >> 32,
 * computed in 64-bit signed arithmetic.  At the final source frame
 * we fall back to zero-order hold to avoid reading past the buffer.
 */
static uint32_t
mix_general(int32_t *acc, uint32_t frames, struct stream_snap *s)
{
	uint64_t pos       = s->pos_q32;
	uint64_t step      = s->step_q32;
	uint64_t total     = s->src_total;
	uint64_t total_q32 = total << 32;
	int16_t  vol       = s->volume_q15;
	bool     unity     = (vol == ANX_AUDIO_VOL_UNITY);
	bool     loop      = s->loop;
	uint32_t produced  = 0;
	uint32_t f;

	for (f = 0; f < frames; f++) {
		if (pos >= total_q32) {
			if (loop) {
				pos = 0;
			} else {
				s->ended = true;
				break;
			}
		}

		uint64_t idx0 = pos >> 32;
		uint32_t frac = (uint32_t)(pos & 0xFFFFFFFFu);
		int32_t  l, r;

		read_frame_i(s->base, idx0, s->bps, s->channels, &l, &r);

		if (frac != 0 && (idx0 + 1) < total) {
			int32_t l1, r1;
			read_frame_i(s->base, idx0 + 1, s->bps,
				     s->channels, &l1, &r1);
			int64_t dl = (int64_t)(l1 - l) * (int64_t)frac;
			int64_t dr = (int64_t)(r1 - r) * (int64_t)frac;
			l += (int32_t)(dl >> 32);
			r += (int32_t)(dr >> 32);
		}

		if (!unity) {
			l = (l * (int32_t)vol + (1 << 14)) >> 15;
			r = (r * (int32_t)vol + (1 << 14)) >> 15;
		}
		acc[2 * f + 0] += l;
		acc[2 * f + 1] += r;

		pos += step;
		produced++;
	}
	s->pos_q32 = pos;
	return produced;
}

static uint32_t
mix_one(int32_t *acc, uint32_t frames, struct stream_snap *s)
{
	if (s->is_int16_stereo && s->eqrate)
		return mix_eqrate_int16_stereo(acc, frames, s);
	return mix_general(acc, frames, s);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void
anx_audio_init(void)
{
	uint32_t i;

	if (g_initialized)
		return;

	for (i = 0; i < ANX_AUDIO_MAX_STREAMS; i++) {
		anx_memset(&g_streams[i], 0, sizeof(g_streams[i]));
		g_streams[i].volume_q15 = ANX_AUDIO_VOL_UNITY;
	}
	g_next_id = 1;
	anx_spin_init(&g_lock);
	g_initialized = true;

	anx_audio_sink_null_register();
	anx_audio_sink_capture_register();
	anx_audio_sink_wav_register();
	anx_audio_sink_select("null");

	/*
	 * Probe hardware sinks.  Each backend self-registers if it
	 * finds matching hardware; if not, it returns ANX_ENODEV and
	 * the sink simply isn't selectable.  We never fail audio init
	 * because of a missing backend — the null sink is always ready.
	 */
	(void)anx_audio_probe_hw_sinks();
}

int
anx_audio_stream_add(const anx_oid_t *src, struct anx_audio_stream **out)
{
	struct anx_state_object         *obj  = NULL;
	const struct anx_audio_clip_hdr *hdr  = NULL;
	const uint8_t                   *base = NULL;
	uint32_t                         i, slot = ANX_AUDIO_MAX_STREAMS;
	int                              rc;

	if (!src || !out)
		return ANX_EINVAL;
	if (!g_initialized)
		anx_audio_init();

	rc = pin_clip(src, &obj, &hdr, &base);
	if (rc != ANX_OK)
		return rc;

	anx_spin_lock(&g_lock);
	for (i = 0; i < ANX_AUDIO_MAX_STREAMS; i++) {
		if (!g_streams[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot == ANX_AUDIO_MAX_STREAMS) {
		anx_spin_unlock(&g_lock);
		anx_objstore_release(obj);
		return ANX_EFULL;
	}

	struct anx_audio_stream *st = &g_streams[slot];
	anx_memset(st, 0, sizeof(*st));
	st->source_oid    = *src;
	st->src_fmt       = hdr->fmt;
	st->src_total     = hdr->frame_count;
	st->volume_q15    = ANX_AUDIO_VOL_UNITY;
	st->id            = g_next_id++;
	st->in_use        = true;
	st->src_obj       = obj;	/* refcount held for stream lifetime */
	st->src_base      = base;
	st->src_pos_q32   = 0;
	st->src_step_q32  = compute_step_q32(hdr->fmt.sample_rate);
	*out = st;
	anx_spin_unlock(&g_lock);

	return ANX_OK;
}

/*
 * Drop a slot's pinned reference and clear it.  Must be called with
 * g_lock held; releases the lock briefly across anx_objstore_release
 * (which may take its own internal lock and then re-acquires ours).
 * Idempotent: a slot that is not in_use is left alone.
 */
static void
release_slot_locked(struct anx_audio_stream *st)
{
	struct anx_state_object *obj;

	if (!st->in_use)
		return;
	obj = st->src_obj;
	st->in_use      = false;
	st->src_obj     = NULL;
	st->src_base    = NULL;
	st->src_pos_q32 = 0;
	if (obj) {
		anx_spin_unlock(&g_lock);
		anx_objstore_release(obj);
		anx_spin_lock(&g_lock);
	}
}

static struct anx_audio_stream *
find_stream_locked(uint16_t id)
{
	uint32_t i;
	for (i = 0; i < ANX_AUDIO_MAX_STREAMS; i++) {
		if (g_streams[i].in_use && g_streams[i].id == id)
			return &g_streams[i];
	}
	return NULL;
}

int
anx_audio_stream_remove(uint16_t id)
{
	struct anx_audio_stream *st;

	anx_spin_lock(&g_lock);
	st = find_stream_locked(id);
	if (!st) {
		anx_spin_unlock(&g_lock);
		return ANX_ENOENT;
	}
	release_slot_locked(st);
	anx_spin_unlock(&g_lock);
	return ANX_OK;
}

int
anx_audio_stream_pause(uint16_t id, bool paused)
{
	struct anx_audio_stream *st;

	anx_spin_lock(&g_lock);
	st = find_stream_locked(id);
	if (!st) {
		anx_spin_unlock(&g_lock);
		return ANX_ENOENT;
	}
	st->paused = paused;
	anx_spin_unlock(&g_lock);
	return ANX_OK;
}

int
anx_audio_stream_set_volume(uint16_t id, int16_t volume_q15)
{
	struct anx_audio_stream *st;

	if (volume_q15 < 0)
		return ANX_EINVAL;
	anx_spin_lock(&g_lock);
	st = find_stream_locked(id);
	if (!st) {
		anx_spin_unlock(&g_lock);
		return ANX_ENOENT;
	}
	st->volume_q15 = volume_q15;
	anx_spin_unlock(&g_lock);
	return ANX_OK;
}

/*
 * One pull chunk: snapshot live streams, mix unlocked, commit results.
 * 'frames' must be <= ANX_AUDIO_PULL_CHUNK.  dst is fully written —
 * positions where no stream contributed are zero.  Returns the largest
 * frame count produced by any single stream (i.e. how far the active
 * region of dst extends), or 0 if no stream produced anything.
 */
static uint32_t
pull_chunk(int16_t *dst, uint32_t frames)
{
	struct stream_snap snap[ANX_AUDIO_MAX_STREAMS];
	uint32_t           nsnap = 0;
	uint32_t           i, f;
	uint32_t           max_produced = 0;

	/* Zero the int32 accumulator (the saturation pass writes every
	 * dst slot, so no need to zero dst). */
	anx_memset(g_accum, 0, frames * 2 * sizeof(int32_t));

	/* Snapshot phase. */
	anx_spin_lock(&g_lock);
	for (i = 0; i < ANX_AUDIO_MAX_STREAMS; i++) {
		struct anx_audio_stream *st = &g_streams[i];

		if (!st->in_use || st->paused || !st->src_base)
			continue;

		struct stream_snap *sn = &snap[nsnap++];
		sn->slot       = (uint16_t)i;
		sn->base       = st->src_base;
		sn->src_total  = st->src_total;
		sn->pos_q32    = st->src_pos_q32;
		sn->step_q32   = st->src_step_q32;
		sn->bps        = st->src_fmt.bits_per_sample;
		sn->channels   = st->src_fmt.channels;
		sn->volume_q15 = st->volume_q15;
		sn->loop       = st->loop;
		sn->ended      = false;
		sn->eqrate     = (sn->step_q32 == ((uint64_t)1 << 32));
		sn->is_int16_stereo = (sn->bps == 16 && sn->channels == 2);
	}
	anx_spin_unlock(&g_lock);

	/* Mix phase (lock-free, hot loop). */
	uint32_t produced[ANX_AUDIO_MAX_STREAMS];
	for (i = 0; i < nsnap; i++) {
		produced[i] = mix_one(g_accum, frames, &snap[i]);
		if (produced[i] > max_produced)
			max_produced = produced[i];
	}

	/* Saturate-clamp accumulator into output. */
	for (f = 0; f < frames; f++) {
		dst[2 * f + 0] = sat_i32(g_accum[2 * f + 0]);
		dst[2 * f + 1] = sat_i32(g_accum[2 * f + 1]);
	}

	/* Commit phase: write back source positions and reap ended streams. */
	anx_spin_lock(&g_lock);
	for (i = 0; i < nsnap; i++) {
		struct anx_audio_stream *st = &g_streams[snap[i].slot];

		if (!st->in_use)
			continue;	/* concurrently removed */
		st->src_pos_q32 = snap[i].pos_q32;
		st->src_offset += produced[i];
		if (snap[i].ended)
			release_slot_locked(st);
	}
	anx_spin_unlock(&g_lock);

	return max_produced;
}

int
anx_audio_mix_pull(int16_t *dst, uint32_t frames)
{
	uint32_t produced_total = 0;

	if (!dst || frames == 0)
		return 0;
	if (!g_initialized)
		anx_audio_init();

	while (frames > 0) {
		uint32_t want = frames < ANX_AUDIO_PULL_CHUNK
				? frames : ANX_AUDIO_PULL_CHUNK;
		uint32_t got  = pull_chunk(dst, want);

		produced_total += got;
		dst    += want * 2;
		frames -= want;

		if (got < want)
			break;	/* all sources ended; remaining is silence */
	}
	return (int)produced_total;
}

/* ------------------------------------------------------------------ */
/* Clip helpers                                                        */
/* ------------------------------------------------------------------ */

int
anx_audio_clip_validate(const void *payload, uint32_t size)
{
	const struct anx_audio_clip_hdr *hdr;
	uint64_t needed;

	if (!payload || size < sizeof(*hdr))
		return ANX_EINVAL;
	hdr = (const struct anx_audio_clip_hdr *)payload;
	if (hdr->magic != ANX_AUDIO_CLIP_MAGIC)
		return ANX_EINVAL;
	if (hdr->version != ANX_AUDIO_CLIP_VERSION)
		return ANX_EINVAL;
	if (hdr->payload_offset != sizeof(*hdr))
		return ANX_EINVAL;
	if (hdr->fmt.channels == 0 || hdr->fmt.sample_rate == 0 ||
	    hdr->fmt.bits_per_sample == 0)
		return ANX_EINVAL;
	if ((hdr->fmt.bits_per_sample & 7) != 0)
		return ANX_EINVAL;	/* only byte-aligned widths */

	needed = hdr->payload_offset +
		 hdr->frame_count *
		 (uint64_t)hdr->fmt.channels *
		 (uint64_t)(hdr->fmt.bits_per_sample / 8);
	if ((uint64_t)size < needed)
		return ANX_EINVAL;
	return ANX_OK;
}

int
anx_audio_clip_create(const void *payload, uint32_t size,
		      anx_oid_t *out_oid)
{
	struct anx_so_create_params cp;
	struct anx_state_object    *obj;
	int rc;

	if (!out_oid)
		return ANX_EINVAL;
	rc = anx_audio_clip_validate(payload, size);
	if (rc != ANX_OK)
		return rc;

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_AUDIO_CLIP;
	cp.schema_uri     = "anx:schema/media/audio-clip/v1";
	cp.schema_version = "1";
	cp.payload        = payload;
	cp.payload_size   = size;

	rc = anx_so_create(&cp, &obj);
	if (rc != ANX_OK)
		return rc;
	*out_oid = obj->oid;
	anx_objstore_release(obj);
	return ANX_OK;
}
