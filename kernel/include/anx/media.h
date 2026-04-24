/*
 * anx/media.h — Media path baseline (P2-002).
 *
 * Audio output pipeline with timing guarantees, video decode
 * integration strategy, and A/V sync control.
 */

#ifndef ANX_MEDIA_H
#define ANX_MEDIA_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Audio                                                                */
/* ------------------------------------------------------------------ */

#define ANX_AUDIO_SAMPLE_RATE    44100u
#define ANX_AUDIO_BUFFER_SAMPLES  4096u
#define ANX_AUDIO_QUEUE_MAX          8u

struct anx_audio_buffer {
	int16_t  samples[ANX_AUDIO_BUFFER_SAMPLES];
	uint32_t count;          /* valid samples in this buffer */
	uint64_t timestamp_ns;   /* presentation timestamp       */
	bool     underrun;       /* filled by driver if underrun */
};

/* ------------------------------------------------------------------ */
/* Media session                                                        */
/* ------------------------------------------------------------------ */

enum anx_media_state {
	ANX_MEDIA_STOPPED,
	ANX_MEDIA_PLAYING,
	ANX_MEDIA_PAUSED,
	ANX_MEDIA_SEEKING,
};

#define ANX_MEDIA_URI_MAX  128u

struct anx_media_session {
	enum anx_media_state state;
	char                 uri[ANX_MEDIA_URI_MAX];
	uint64_t             position_ns;      /* current playback position */
	uint64_t             duration_ns;      /* 0 if unknown              */
	uint32_t             buffers_played;
	uint32_t             underrun_count;
	bool                 active;
};

/* ------------------------------------------------------------------ */
/* Media API                                                            */
/* ------------------------------------------------------------------ */

/* Initialise media subsystem. */
void anx_media_init(void);

/* Open a media session for the given URI. */
int anx_media_open(const char *uri, struct anx_media_session *sess);

/* State transitions. */
int anx_media_play(struct anx_media_session *sess);
int anx_media_pause(struct anx_media_session *sess);
int anx_media_stop(struct anx_media_session *sess);
int anx_media_seek(struct anx_media_session *sess, uint64_t position_ns);

/* Enqueue an audio buffer for playback. Returns ANX_EFULL if queue full. */
int anx_media_audio_enqueue(struct anx_media_session *sess,
                              const struct anx_audio_buffer *buf);

/*
 * Simulate one playback tick: dequeue next buffer, update position,
 * set underrun flag if queue was empty.
 * Returns ANX_OK (buffer played), ANX_ENOENT (underrun — no buffer).
 */
int anx_media_tick(struct anx_media_session *sess,
                    struct anx_audio_buffer *played_out);

/* Close a session. */
void anx_media_close(struct anx_media_session *sess);

#endif /* ANX_MEDIA_H */
