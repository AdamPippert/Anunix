/*
 * video_player.c — Video clip object + player cell (RFC-0024).
 *
 * Clips are RGBA frame containers.  This file owns clip validation,
 * a single-frame extractor (the "video-frame" pure transform) and the
 * cell dispatch shim that routes "video-play" / "video-frame" intents.
 *
 * The actual playback loop and sink registry live in
 * kernel/core/video/{player,sink}.c — those handle frame-rate
 * conversion and pump frames into a pluggable sink (null/capture/
 * surface).  This file calls into anx_video_play_clip() and emits the
 * trace OID that the workflow layer consumes.
 */

#include <anx/video.h>
#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

int anx_video_clip_validate(const void *payload, uint32_t size)
{
	const struct anx_video_clip_hdr *hdr;
	uint64_t bytes_per_frame;
	uint64_t needed;

	if (!payload || size < sizeof(*hdr))
		return ANX_EINVAL;
	hdr = (const struct anx_video_clip_hdr *)payload;
	if (hdr->magic != ANX_VIDEO_CLIP_MAGIC)
		return ANX_EINVAL;
	if (hdr->version != ANX_VIDEO_CLIP_VERSION)
		return ANX_EINVAL;
	if (hdr->payload_offset != sizeof(*hdr))
		return ANX_EINVAL;
	if (hdr->width == 0 || hdr->width > ANX_VIDEO_MAX_W ||
	    hdr->height == 0 || hdr->height > ANX_VIDEO_MAX_H)
		return ANX_EINVAL;
	if (hdr->fps_x256 == 0)
		return ANX_EINVAL;

	bytes_per_frame = (uint64_t)hdr->width * hdr->height * 4ull;
	needed = hdr->payload_offset + bytes_per_frame * hdr->frame_count;
	if ((uint64_t)size < needed)
		return ANX_EINVAL;
	return ANX_OK;
}

int anx_video_clip_create(const void *payload, uint32_t size,
			  anx_oid_t *out_oid)
{
	struct anx_so_create_params cp;
	struct anx_state_object    *obj;
	int rc;

	if (!out_oid)
		return ANX_EINVAL;
	rc = anx_video_clip_validate(payload, size);
	if (rc != ANX_OK)
		return rc;

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_VIDEO_CLIP;
	cp.schema_uri     = "anx:schema/media/video-clip/v1";
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

int anx_video_clip_frame_get(const void *payload, uint32_t size,
			     uint64_t frame_idx,
			     const uint8_t **frame_out,
			     uint32_t *frame_bytes_out)
{
	const struct anx_video_clip_hdr *hdr;
	uint64_t bytes_per_frame;
	uint64_t off;
	int rc;

	if (!frame_out || !frame_bytes_out)
		return ANX_EINVAL;
	rc = anx_video_clip_validate(payload, size);
	if (rc != ANX_OK)
		return rc;
	hdr = (const struct anx_video_clip_hdr *)payload;
	if (frame_idx >= hdr->frame_count)
		return ANX_ENOENT;
	bytes_per_frame = (uint64_t)hdr->width * hdr->height * 4ull;
	off = hdr->payload_offset + bytes_per_frame * frame_idx;
	*frame_out       = (const uint8_t *)payload + off;
	*frame_bytes_out = (uint32_t)bytes_per_frame;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Cell dispatch                                                       */
/* ------------------------------------------------------------------ */

static int emit_trace(uint64_t frames, uint32_t width, uint32_t height,
		      uint32_t fps_x256, const char *sink_name,
		      anx_oid_t *out_oid)
{
	struct anx_so_create_params cp;
	struct anx_state_object    *obj;
	char                        buf[192];
	int                         len;

	len = anx_snprintf(buf, sizeof(buf),
			   "video-play frames=%llu w=%u h=%u fps_x256=%u sink=%s\n",
			   (unsigned long long)frames, width, height,
			   fps_x256, sink_name ? sink_name : "(none)");
	if (len <= 0)
		return ANX_EIO;

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_EXECUTION_TRACE;
	cp.schema_uri     = "anx:schema/video/trace/v1";
	cp.schema_version = "1";
	cp.payload        = buf;
	cp.payload_size   = (uint32_t)len;
	int rc = anx_so_create(&cp, &obj);
	if (rc != ANX_OK)
		return rc;
	*out_oid = obj->oid;
	anx_objstore_release(obj);
	return ANX_OK;
}

/*
 * Play 'src' through the active video sink (or, if a surface OID is
 * supplied, by binding the surface sink for the duration of the run).
 * Emits a trace OID describing the playback.
 */
static int do_play(const anx_oid_t *src, const anx_oid_t *surf_oid,
		   anx_oid_t *out_oid)
{
	struct anx_state_object         *obj;
	const struct anx_video_clip_hdr *hdr;
	const char                      *prev_sink = NULL;
	uint64_t                         frames    = 0;
	int                              rc;
	bool                             surf_bound = false;

	obj = anx_objstore_lookup(src);
	if (!obj)
		return ANX_ENOENT;
	if (obj->object_type != ANX_OBJ_VIDEO_CLIP || !obj->payload) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}
	rc = anx_video_clip_validate(obj->payload, (uint32_t)obj->payload_size);
	if (rc != ANX_OK) {
		anx_objstore_release(obj);
		return rc;
	}
	hdr = (const struct anx_video_clip_hdr *)obj->payload;
	anx_objstore_release(obj);

	if (surf_oid && (surf_oid->hi || surf_oid->lo)) {
		rc = anx_video_sink_surface_bind(surf_oid);
		if (rc != ANX_OK)
			return rc;
		surf_bound = true;
		prev_sink  = anx_video_sink_active();
		(void)anx_video_sink_select("surface");
	}

	rc = anx_video_play_clip(src, 0 /* preserve clip fps */,
				 0 /* play to end */, &frames);

	if (surf_bound) {
		if (prev_sink && anx_strcmp(prev_sink, "surface") != 0)
			anx_video_sink_select(prev_sink);
		anx_video_sink_surface_unbind();
	}

	if (rc != ANX_OK)
		return rc;

	return emit_trace(frames, hdr->width, hdr->height, hdr->fps_x256,
			  anx_video_sink_active(), out_oid);
}

static int do_frame(const anx_oid_t *src, uint64_t idx, anx_oid_t *out_oid)
{
	struct anx_state_object         *obj;
	const uint8_t                   *frame;
	uint32_t                         fbytes;
	struct anx_so_create_params      cp;
	struct anx_state_object         *frame_obj;
	int                              rc;

	obj = anx_objstore_lookup(src);
	if (!obj)
		return ANX_ENOENT;
	if (obj->object_type != ANX_OBJ_VIDEO_CLIP) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}
	rc = anx_video_clip_frame_get(obj->payload,
				      (uint32_t)obj->payload_size,
				      idx, &frame, &fbytes);
	if (rc != ANX_OK) {
		anx_objstore_release(obj);
		return rc;
	}

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_BYTE_DATA;
	cp.schema_uri     = "anx:schema/video/frame/rgba8/v1";
	cp.schema_version = "1";
	cp.payload        = frame;
	cp.payload_size   = fbytes;
	rc = anx_so_create(&cp, &frame_obj);
	anx_objstore_release(obj);
	if (rc != ANX_OK)
		return rc;
	*out_oid = frame_obj->oid;
	anx_objstore_release(frame_obj);
	return ANX_OK;
}

int anx_video_cell_dispatch(const char *intent,
			    const anx_oid_t *in_oids, uint32_t in_count,
			    anx_oid_t *out_oid_out)
{
	if (!intent || !out_oid_out)
		return ANX_EINVAL;
	anx_memset(out_oid_out, 0, sizeof(*out_oid_out));

	if (anx_strcmp(intent, "video-play") == 0) {
		const anx_oid_t *surf = NULL;
		if (in_count < 1)
			return ANX_EINVAL;
		if (in_count >= 2)
			surf = &in_oids[1];
		return do_play(&in_oids[0], surf, out_oid_out);
	}
	if (anx_strcmp(intent, "video-frame") == 0) {
		struct anx_state_object *idx_obj;
		uint64_t idx = 0;

		if (in_count < 2)
			return ANX_EINVAL;
		idx_obj = anx_objstore_lookup(&in_oids[1]);
		if (!idx_obj)
			return ANX_ENOENT;
		if (idx_obj->payload && idx_obj->payload_size >= sizeof(uint64_t))
			idx = *(const uint64_t *)idx_obj->payload;
		else if (idx_obj->payload && idx_obj->payload_size >= sizeof(uint32_t))
			idx = *(const uint32_t *)idx_obj->payload;
		anx_objstore_release(idx_obj);
		return do_frame(&in_oids[0], idx, out_oid_out);
	}
	return ANX_ENOSYS;
}
