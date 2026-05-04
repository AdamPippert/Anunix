/*
 * anx/video.h — Video clip object and player (RFC-0024).
 */

#ifndef ANX_VIDEO_H
#define ANX_VIDEO_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Clip payload header (ANX_OBJ_VIDEO_CLIP)                            */
/* ------------------------------------------------------------------ */

#define ANX_VIDEO_CLIP_MAGIC	0x56434C50U	/* "VCLP" */
#define ANX_VIDEO_CLIP_VERSION	1U

/* Limits to bound dispatch buffers. */
#define ANX_VIDEO_MAX_W		3840U
#define ANX_VIDEO_MAX_H		2160U

struct anx_video_clip_hdr {
	uint32_t magic;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t fps_x256;	/* fps in 24.8 fixed-point */
	uint32_t reserved;
	uint64_t frame_count;
	uint64_t payload_offset;	/* always sizeof(hdr) */
};

/* ------------------------------------------------------------------ */
/* Validation / creation                                               */
/* ------------------------------------------------------------------ */

int anx_video_clip_validate(const void *payload, uint32_t size);

int anx_video_clip_create(const void *payload, uint32_t size,
			  anx_oid_t *out_oid);

int anx_video_clip_frame_get(const void *payload, uint32_t size,
			     uint64_t frame_idx,
			     const uint8_t **frame_out,
			     uint32_t *frame_bytes_out);

/* ------------------------------------------------------------------ */
/* Player + sink registry                                              */
/* ------------------------------------------------------------------ */

#define ANX_VIDEO_SINK_NAME_MAX	32

struct anx_video_format {
	uint32_t width;
	uint32_t height;
	uint32_t fps_x256;	/* fps in 24.8 fixed-point */
	uint32_t pixel_format;	/* 0 = RGBA8888 (only one supported in v1) */
};

struct anx_video_sink_ops {
	int  (*open)(const struct anx_video_format *fmt);
	int  (*write_frame)(uint64_t output_index, uint64_t source_index,
			    const uint8_t *rgba, uint32_t width,
			    uint32_t height);
	void (*close)(void);
};

void anx_video_init(void);
int  anx_video_sink_register(const char *name,
			     const struct anx_video_sink_ops *ops);
int  anx_video_sink_select(const char *name);
const char *anx_video_sink_active(void);

/*
 * Play a clip through the active sink at the requested output frame
 * rate.  When out_fps_x256 is 0, the clip's own fps is preserved.
 * 'max_out_frames' caps the playback length (use 0 for the natural
 * end of the clip).  Returns the count of output frames produced via
 * 'frames_out' (may be NULL).
 */
int  anx_video_play_clip(const anx_oid_t *clip_oid,
			 uint32_t out_fps_x256,
			 uint64_t max_out_frames,
			 uint64_t *frames_out);

/* Built-in sinks (registered by anx_video_init). */
void anx_video_sink_null_register(void);
void anx_video_sink_capture_register(void);

/*
 * The capture sink keeps a ring of recent frame records so tests can
 * verify pacing and frame-selection behaviour.  CRC32 is computed
 * over the full RGBA frame buffer and lets a test prove that the
 * exact source frame ended up at a given output position.
 */
struct anx_video_capture_record {
	uint64_t output_index;
	uint64_t source_index;
	uint32_t width;
	uint32_t height;
	uint32_t crc32;
};
void anx_video_sink_capture_reset(void);
const struct anx_video_capture_record *
	anx_video_sink_capture_records(uint32_t *count_out);

/*
 * Surface sink writes RGBA frames into an iface surface's canvas.
 * Bind it before playback and unbind after; selection of the "surface"
 * sink without a binding is a no-op (write_frame returns ANX_ENODEV).
 */
void anx_video_sink_surface_register(void);
int  anx_video_sink_surface_bind(const anx_oid_t *surface_oid);
void anx_video_sink_surface_unbind(void);

/* ------------------------------------------------------------------ */
/* Cell dispatch                                                       */
/* ------------------------------------------------------------------ */

/*
 * Intents (v1):
 *   video-play    in[0] = clip OID
 *                 in[1] = optional surface OID (binds surface sink)
 *                                                  → trace OID
 *   video-frame   in[0] = clip OID, in[1] = idx-int → RGBA byte-data OID
 */
int anx_video_cell_dispatch(const char *intent,
			    const anx_oid_t *in_oids, uint32_t in_count,
			    anx_oid_t *out_oid_out);

#endif /* ANX_VIDEO_H */
