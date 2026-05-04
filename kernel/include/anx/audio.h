/*
 * anx/audio.h — Audio engine and player (RFC-0024).
 */

#ifndef ANX_AUDIO_H
#define ANX_AUDIO_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Format                                                              */
/* ------------------------------------------------------------------ */

#define ANX_AUDIO_DEFAULT_RATE		48000U
#define ANX_AUDIO_DEFAULT_CHANNELS	2U
#define ANX_AUDIO_INTERNAL_BPS		16U	/* signed int16 */
#define ANX_AUDIO_MAX_STREAMS		16U

struct anx_audio_format {
	uint32_t sample_rate;
	uint16_t channels;
	uint16_t bits_per_sample;	/* 8, 16, 24, 32 */
	uint8_t  is_float;		/* 0=int, 1=float (input only) */
	uint8_t  reserved[3];
};

/* ------------------------------------------------------------------ */
/* Clip object payload header (ANX_OBJ_AUDIO_CLIP)                     */
/* ------------------------------------------------------------------ */

#define ANX_AUDIO_CLIP_MAGIC	0x41434C50U	/* "ACLP" */
#define ANX_AUDIO_CLIP_VERSION	1U

struct anx_audio_clip_hdr {
	uint32_t magic;
	uint32_t version;
	struct anx_audio_format fmt;
	uint64_t frame_count;	/* one frame == channels samples */
	uint64_t payload_offset;	/* always sizeof(hdr) */
};

/* ------------------------------------------------------------------ */
/* Stream                                                              */
/* ------------------------------------------------------------------ */

struct anx_state_object;	/* forward — pinned source pointer */

struct anx_audio_stream {
	anx_oid_t                source_oid;
	struct anx_audio_format  src_fmt;
	uint64_t                 src_offset;	/* output frames produced */
	uint64_t                 src_total;	/* total source frames */
	int16_t                  volume_q15;	/* 0..32767, 32767 = unity */
	bool                     loop;
	bool                     paused;
	bool                     in_use;
	uint16_t                 id;

	/*
	 * Pinned source state (mixer-private; do not touch from API
	 * callers).  src_obj is held with an objstore refcount for the
	 * lifetime of the stream slot, so src_base stays valid without
	 * any lookup on the audio hot path.  src_pos_q32 is the current
	 * source position in Q32.32 frames; src_step_q32 is the per-
	 * output-frame increment after sample-rate negotiation.
	 */
	struct anx_state_object *src_obj;
	const uint8_t           *src_base;
	uint64_t                 src_pos_q32;
	uint64_t                 src_step_q32;
};

/* ------------------------------------------------------------------ */
/* Mixer API                                                           */
/* ------------------------------------------------------------------ */

void anx_audio_init(void);
int  anx_audio_stream_add(const anx_oid_t *src,
			  struct anx_audio_stream **out);
int  anx_audio_stream_remove(uint16_t id);
int  anx_audio_stream_pause(uint16_t id, bool paused);
int  anx_audio_stream_set_volume(uint16_t id, int16_t volume_q15);

/*
 * Pull `frames` stereo samples (frames * 2 int16) into `dst`.
 * Returns number of frames produced (can be less than requested if
 * all sources have ended).
 */
int  anx_audio_mix_pull(int16_t *dst, uint32_t frames);

/* ------------------------------------------------------------------ */
/* Sink driver registry                                                */
/* ------------------------------------------------------------------ */

#define ANX_AUDIO_SINK_NAME_MAX	32

struct anx_audio_sink_ops {
	int  (*open)(struct anx_audio_format *neg_fmt);
	int  (*write)(const int16_t *frames, uint32_t frame_count);
	void (*close)(void);
};

int  anx_audio_sink_register(const char *name,
			     const struct anx_audio_sink_ops *ops);
int  anx_audio_sink_select(const char *name);
int  anx_audio_sink_run(uint32_t total_frames);
const char *anx_audio_sink_active(void);

/* Built-in sinks (registered by anx_audio_init) */
void anx_audio_sink_null_register(void);
void anx_audio_sink_capture_register(void);
const int16_t *anx_audio_sink_capture_buffer(uint32_t *frames_out);
void anx_audio_sink_capture_reset(void);

/*
 * Wav-file sink: when selected, the next anx_audio_sink_run() captures
 * the mix internally; on close() it serializes the captured frames as
 * a clip payload and creates a sealed ANX_OBJ_AUDIO_CLIP whose OID is
 * retrievable via anx_audio_sink_wav_last_oid().  This lets tests and
 * non-realtime cells render a mix to a State Object without any
 * external file I/O.  The sink also emits a RIFF/WAV byte image for
 * downstream tooling that wants the canonical container layout.
 */
void anx_audio_sink_wav_register(void);
int  anx_audio_sink_wav_last_oid(anx_oid_t *out);
const uint8_t *anx_audio_sink_wav_last_riff(uint32_t *size_out);

/*
 * Encode a buffer of int16 stereo frames at ANX_AUDIO_DEFAULT_RATE as
 * a RIFF/WAV byte image.  Caller supplies the destination buffer.
 * Returns the number of bytes written (>= 44, the WAV header size) or
 * a negative error.  Pass dst=NULL to query the required size.
 */
int  anx_audio_wav_encode(const int16_t *frames, uint32_t frame_count,
			  uint32_t sample_rate, uint16_t channels,
			  uint8_t *dst, uint32_t dst_size);

/*
 * Probe hardware-backed sinks (HDA, Apple Silicon audio, ...).  Called
 * by anx_audio_init().  Each backend self-registers as a named sink
 * if the matching hardware is present.  Always returns ANX_OK; the
 * absence of any backend leaves only the null/capture/wav sinks.
 */
int  anx_audio_probe_hw_sinks(void);

/* ------------------------------------------------------------------ */
/* Clip helpers (validation + creation)                                */
/* ------------------------------------------------------------------ */

int  anx_audio_clip_validate(const void *payload, uint32_t size);

/*
 * Create a sealed ANX_OBJ_AUDIO_CLIP State Object from a raw PCM
 * payload (must include the anx_audio_clip_hdr at offset 0).
 */
int  anx_audio_clip_create(const void *payload, uint32_t size,
			   anx_oid_t *out_oid);

/* ------------------------------------------------------------------ */
/* Cell dispatch                                                       */
/* ------------------------------------------------------------------ */

/*
 * Intents (v1):
 *   audio-play    in[0] = clip OID                 → trace OID
 *   audio-mix     in[*] = clip OIDs (up to 4)      → mixed clip OID
 */
int anx_audio_cell_dispatch(const char *intent,
			    const anx_oid_t *in_oids, uint32_t in_count,
			    anx_oid_t *out_oid_out);

#endif /* ANX_AUDIO_H */
