/*
 * player.c — Video clip player + sink registry (RFC-0024).
 *
 * The player walks a clip's frames at a target output frame rate.
 * Source frames are selected by nearest-neighbour using a Q32.32
 * source-frame cursor — no inter-frame interpolation, since blending
 * adjacent video frames produces objectionable motion blur and would
 * cost width*height*4 bytes of multiplication per output frame.  At
 * matched rates (in_fps == out_fps) the cursor advances exactly 1.0
 * per output frame and the player degenerates to a 1:1 walk.
 *
 * The sink registry mirrors the audio sink design.  Sinks receive
 * a stable RGBA pointer that lives only for the duration of the
 * write_frame() call, so the surface sink memcpys into its target
 * canvas synchronously and the capture sink CRCs the buffer in place.
 */

#include <anx/video.h>
#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/spinlock.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Sink registry                                                       */
/* ------------------------------------------------------------------ */

#define ANX_VIDEO_SINK_MAX	8

struct sink_slot {
	char                              name[ANX_VIDEO_SINK_NAME_MAX];
	const struct anx_video_sink_ops  *ops;
	bool                              in_use;
};

static struct sink_slot     g_sinks[ANX_VIDEO_SINK_MAX];
static struct sink_slot    *g_active;
static struct anx_spinlock  g_lock;
static bool                 g_lock_ready;
static bool                 g_sinks_ready;

/* Bootstrap the spinlock without registering sinks (used by sink
 * registration itself to avoid recursion). */
static void
ensure_lock(void)
{
	if (!g_lock_ready) {
		anx_spin_init(&g_lock);
		g_lock_ready = true;
	}
}

void
anx_video_init(void)
{
	if (g_sinks_ready)
		return;
	ensure_lock();
	anx_video_sink_null_register();
	anx_video_sink_capture_register();
	anx_video_sink_surface_register();
	anx_video_sink_select("null");
	g_sinks_ready = true;
}

int
anx_video_sink_register(const char *name,
			const struct anx_video_sink_ops *ops)
{
	uint32_t i;
	int      ret = ANX_EFULL;

	if (!name || !ops)
		return ANX_EINVAL;
	ensure_lock();

	anx_spin_lock(&g_lock);
	for (i = 0; i < ANX_VIDEO_SINK_MAX; i++) {
		if (g_sinks[i].in_use &&
		    anx_strcmp(g_sinks[i].name, name) == 0) {
			g_sinks[i].ops = ops;
			ret = ANX_OK;
			goto out;
		}
	}
	for (i = 0; i < ANX_VIDEO_SINK_MAX; i++) {
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

int
anx_video_sink_select(const char *name)
{
	uint32_t i;
	int      ret = ANX_ENOENT;

	if (!name)
		return ANX_EINVAL;
	ensure_lock();

	anx_spin_lock(&g_lock);
	for (i = 0; i < ANX_VIDEO_SINK_MAX; i++) {
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

const char *
anx_video_sink_active(void)
{
	const char *name = NULL;

	ensure_lock();
	anx_spin_lock(&g_lock);
	if (g_active)
		name = g_active->name;
	anx_spin_unlock(&g_lock);
	return name;
}

/* ------------------------------------------------------------------ */
/* Player loop                                                         */
/* ------------------------------------------------------------------ */

/*
 * Compute step_q32: per-output-frame increment of the source cursor,
 * given input fps and output fps (both 24.8 fixed-point).  A 24-fps
 * clip on a 60-fps output ticks at step = 24/60 = 0.4 source frames
 * per output frame.  Equal rates → step = 1.0 (cursor walks 1:1).
 */
static uint64_t
compute_step_q32(uint32_t in_fps_x256, uint32_t out_fps_x256)
{
	if (out_fps_x256 == 0 || in_fps_x256 == 0 ||
	    in_fps_x256 == out_fps_x256)
		return ((uint64_t)1) << 32;
	/* (in / out) << 32 == (in << 32) / out, in 64-bit math.  in_fps_x256
	 * is at most a few-hundred-thousand → no overflow. */
	return ((uint64_t)in_fps_x256 << 32) / (uint64_t)out_fps_x256;
}

int
anx_video_play_clip(const anx_oid_t *clip_oid,
		    uint32_t out_fps_x256,
		    uint64_t max_out_frames,
		    uint64_t *frames_out)
{
	struct anx_state_object         *obj;
	const struct anx_video_clip_hdr *hdr;
	const struct anx_video_sink_ops *ops;
	struct anx_video_format          fmt;
	uint64_t                         pos_q32 = 0;
	uint64_t                         step_q32;
	uint64_t                         out_idx = 0;
	int                              rc;

	if (!clip_oid)
		return ANX_EINVAL;
	if (!g_sinks_ready)
		anx_video_init();

	obj = anx_objstore_lookup(clip_oid);
	if (!obj)
		return ANX_ENOENT;
	if (obj->object_type != ANX_OBJ_VIDEO_CLIP || !obj->payload) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}
	rc = anx_video_clip_validate(obj->payload,
				     (uint32_t)obj->payload_size);
	if (rc != ANX_OK) {
		anx_objstore_release(obj);
		return rc;
	}
	hdr = (const struct anx_video_clip_hdr *)obj->payload;

	if (out_fps_x256 == 0)
		out_fps_x256 = hdr->fps_x256;
	step_q32 = compute_step_q32(hdr->fps_x256, out_fps_x256);

	/* Snapshot active sink. */
	anx_spin_lock(&g_lock);
	ops = g_active ? g_active->ops : NULL;
	anx_spin_unlock(&g_lock);
	if (!ops) {
		anx_objstore_release(obj);
		return ANX_ENODEV;
	}

	fmt.width        = hdr->width;
	fmt.height       = hdr->height;
	fmt.fps_x256     = out_fps_x256;
	fmt.pixel_format = 0;	/* RGBA8888 */
	if (ops->open) {
		rc = ops->open(&fmt);
		if (rc != ANX_OK) {
			anx_objstore_release(obj);
			return rc;
		}
	}

	/* Hot loop: pick a source frame, hand it to the sink, advance. */
	while (max_out_frames == 0 || out_idx < max_out_frames) {
		uint64_t       src_idx = pos_q32 >> 32;
		const uint8_t *frame;
		uint32_t       fbytes;

		if (src_idx >= hdr->frame_count)
			break;	/* clip ended */

		rc = anx_video_clip_frame_get(obj->payload,
					      (uint32_t)obj->payload_size,
					      src_idx, &frame, &fbytes);
		if (rc != ANX_OK)
			break;

		if (ops->write_frame) {
			int wrc = ops->write_frame(out_idx, src_idx, frame,
						   hdr->width, hdr->height);
			if (wrc != ANX_OK) {
				rc = wrc;
				break;
			}
		}
		out_idx++;
		pos_q32 += step_q32;
	}

	if (ops->close)
		ops->close();

	if (frames_out)
		*frames_out = out_idx;
	anx_objstore_release(obj);
	return rc == ANX_OK || rc == ANX_ENOENT ? ANX_OK : rc;
}
