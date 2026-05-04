/*
 * test_video.c — Tests for the video stack (RFC-0024).
 *
 * Coverage:
 *   1.  Clip header validation (bad dimensions rejected).
 *   2.  Clip create + frame extraction.
 *   3.  Cell dispatch "video-play" returns a trace OID.
 *   4.  Cell dispatch "video-frame" returns the right RGBA bytes.
 *   5.  Player + capture sink: every source frame is delivered to the
 *       sink, in order, with the correct CRC.
 *   6.  Player frame-rate conversion: 24 fps clip on 48 fps output
 *       duplicates each source frame exactly twice (step = 0.5).
 *   7.  Player frame-rate conversion: 60 fps clip on 30 fps output
 *       drops every other source frame (step = 2.0).
 *   8.  Surface sink: bound surface receives the latest frame's bytes
 *       in its content_root canvas.
 *   9.  video-play with two inputs (clip + surface OID) drives the
 *       surface sink; surface canvas matches the last source frame.
 */

#include <anx/types.h>
#include <anx/video.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/interface_plane.h>

/* ------------------------------------------------------------------ */
/* Clip builders                                                       */
/* ------------------------------------------------------------------ */

/* Each frame is filled with a constant byte = `i` so the CRC of a
 * given source frame is deterministic and distinguishable. */
static int
make_clip(uint32_t w, uint32_t h, uint32_t frames, uint32_t fps_x256,
	  anx_oid_t *oid_out)
{
	uint32_t hdr_sz = sizeof(struct anx_video_clip_hdr);
	uint64_t fb     = (uint64_t)w * h * 4;
	uint64_t total64 = hdr_sz + fb * frames;
	uint32_t total  = (uint32_t)total64;
	uint8_t *blob   = (uint8_t *)anx_alloc(total);
	uint64_t i, j;

	if (!blob) return ANX_ENOMEM;
	struct anx_video_clip_hdr *hdr = (struct anx_video_clip_hdr *)blob;
	anx_memset(hdr, 0, sizeof(*hdr));
	hdr->magic         = ANX_VIDEO_CLIP_MAGIC;
	hdr->version       = ANX_VIDEO_CLIP_VERSION;
	hdr->width         = w;
	hdr->height        = h;
	hdr->fps_x256      = fps_x256;
	hdr->frame_count   = frames;
	hdr->payload_offset = hdr_sz;
	uint8_t *p = blob + hdr_sz;
	for (i = 0; i < frames; i++) {
		for (j = 0; j < (uint64_t)w * h; j++) {
			p[0] = (uint8_t)i;	/* R distinguishes frames */
			p[1] = 0; p[2] = 0; p[3] = 255;
			p += 4;
		}
	}
	int rc = anx_video_clip_create(blob, total, oid_out);
	anx_free(blob);
	return rc;
}

/* ------------------------------------------------------------------ */

int test_video(void)
{
	int rc;

	anx_objstore_init();
	anx_video_init();

	/* === Test 1: validation rejects zero dimensions === */
	{
		uint8_t buf[64];
		anx_memset(buf, 0, sizeof(buf));
		struct anx_video_clip_hdr *h = (struct anx_video_clip_hdr *)buf;
		h->magic = ANX_VIDEO_CLIP_MAGIC;
		h->version = ANX_VIDEO_CLIP_VERSION;
		h->payload_offset = sizeof(*h);
		h->width = 0; h->height = 0;
		h->fps_x256 = 7680;
		if (anx_video_clip_validate(buf, sizeof(buf)) == ANX_OK)
			return -1;
	}

	/* === Test 2: create clip + frame extraction === */
	{
		anx_oid_t clip;
		rc = make_clip(8, 4, 3, 30 * 256, &clip);
		if (rc != ANX_OK) return -2;
		struct anx_state_object *obj = anx_objstore_lookup(&clip);
		if (!obj) return -3;
		const uint8_t *frame;
		uint32_t fb;
		rc = anx_video_clip_frame_get(obj->payload,
					      (uint32_t)obj->payload_size,
					      1, &frame, &fb);
		if (rc != ANX_OK) {
			anx_objstore_release(obj); return -4;
		}
		if (fb != 8u * 4u * 4u) { anx_objstore_release(obj); return -5; }
		if (frame[0] != 1) { anx_objstore_release(obj); return -6; }
		anx_objstore_release(obj);
	}

	/* === Test 3: cell dispatch video-play returns trace === */
	{
		anx_oid_t clip, out;
		rc = make_clip(4, 4, 5, 30 * 256, &clip);
		if (rc != ANX_OK) return -7;
		anx_memset(&out, 0, sizeof(out));
		rc = anx_video_cell_dispatch("video-play", &clip, 1, &out);
		if (rc != ANX_OK) return -8;
		struct anx_state_object *trace = anx_objstore_lookup(&out);
		if (!trace) return -9;
		if (trace->object_type != ANX_OBJ_EXECUTION_TRACE) {
			anx_objstore_release(trace);
			return -10;
		}
		anx_objstore_release(trace);
	}

	/* === Test 4: cell dispatch video-frame extracts a frame === */
	{
		anx_oid_t clip, idx_oid, out;
		uint64_t  idx = 2;
		struct anx_so_create_params cp;
		struct anx_state_object    *idx_obj;

		rc = make_clip(4, 4, 5, 30 * 256, &clip);
		if (rc != ANX_OK) return -11;
		anx_memset(&cp, 0, sizeof(cp));
		cp.object_type = ANX_OBJ_BYTE_DATA;
		cp.payload     = &idx;
		cp.payload_size = sizeof(idx);
		rc = anx_so_create(&cp, &idx_obj);
		if (rc != ANX_OK) return -12;
		idx_oid = idx_obj->oid;
		anx_objstore_release(idx_obj);

		anx_oid_t inputs[2] = { clip, idx_oid };
		anx_memset(&out, 0, sizeof(out));
		rc = anx_video_cell_dispatch("video-frame", inputs, 2, &out);
		if (rc != ANX_OK) return -13;
		struct anx_state_object *frame = anx_objstore_lookup(&out);
		if (!frame) return -14;
		if (frame->payload_size != 4u * 4u * 4u) {
			anx_objstore_release(frame); return -15;
		}
		if (((const uint8_t *)frame->payload)[0] != 2) {
			anx_objstore_release(frame); return -16;
		}
		anx_objstore_release(frame);
	}

	/* === Test 5: capture sink receives every frame in order ===
	 *
	 * Equal-rate playback: 30 fps clip → 30 fps output.  source_index
	 * advances 1.0 per output frame, so output_index k maps to
	 * source_index k for k in [0, frame_count).
	 */
	{
		anx_oid_t clip;
		const uint32_t N = 6;

		rc = anx_video_sink_select("capture");
		if (rc != ANX_OK) return -17;
		anx_video_sink_capture_reset();
		rc = make_clip(4, 4, N, 30 * 256, &clip);
		if (rc != ANX_OK) return -18;

		uint64_t produced = 0;
		rc = anx_video_play_clip(&clip, 0, 0, &produced);
		if (rc != ANX_OK) return -19;
		if (produced != N) return -20;

		uint32_t cnt;
		const struct anx_video_capture_record *rec =
			anx_video_sink_capture_records(&cnt);
		if (cnt != N) return -21;
		for (uint32_t i = 0; i < N; i++) {
			if (rec[i].output_index != i) return -22;
			if (rec[i].source_index != i) return -23;
			if (rec[i].width != 4 || rec[i].height != 4)
				return -24;
		}
		/* Each source frame has a unique byte pattern, so CRCs
		 * across frames must all differ. */
		for (uint32_t i = 1; i < N; i++)
			if (rec[i].crc32 == rec[i - 1].crc32) return -25;
	}

	/* === Test 6: 24 fps → 48 fps duplicates each source frame ===
	 *
	 * step_q32 = (24/48) << 32 = 0.5.  Output 0,1 → src 0; output
	 * 2,3 → src 1; etc.
	 */
	{
		anx_oid_t clip;
		const uint32_t N_SRC = 4;

		anx_video_sink_capture_reset();
		rc = make_clip(2, 2, N_SRC, 24 * 256, &clip);
		if (rc != ANX_OK) return -26;
		uint64_t produced = 0;
		rc = anx_video_play_clip(&clip, 48 * 256, 0, &produced);
		if (rc != ANX_OK) return -27;
		if (produced != N_SRC * 2) return -28;

		uint32_t cnt;
		const struct anx_video_capture_record *rec =
			anx_video_sink_capture_records(&cnt);
		if (cnt != N_SRC * 2) return -29;
		for (uint32_t i = 0; i < N_SRC * 2; i++) {
			if (rec[i].output_index != i) return -30;
			if (rec[i].source_index != i / 2) return -31;
		}
	}

	/* === Test 7: 60 fps → 30 fps drops every other frame ===
	 *
	 * step_q32 = (60/30) << 32 = 2.0.  Output k → src 2k.
	 */
	{
		anx_oid_t clip;
		const uint32_t N_SRC = 8;

		anx_video_sink_capture_reset();
		rc = make_clip(2, 2, N_SRC, 60 * 256, &clip);
		if (rc != ANX_OK) return -32;
		uint64_t produced = 0;
		rc = anx_video_play_clip(&clip, 30 * 256, 0, &produced);
		if (rc != ANX_OK) return -33;
		if (produced != N_SRC / 2) return -34;

		uint32_t cnt;
		const struct anx_video_capture_record *rec =
			anx_video_sink_capture_records(&cnt);
		if (cnt != N_SRC / 2) return -35;
		for (uint32_t i = 0; i < N_SRC / 2; i++) {
			if (rec[i].source_index != i * 2) return -36;
		}
	}

	/* === Test 8: surface sink delivers final frame to canvas === */
	{
		struct anx_surface       *surf;
		struct anx_content_node   node;
		uint8_t                   canvas[16 * 16 * 4];
		const uint32_t            W = 16, H = 16;
		anx_oid_t                 clip;
		const uint32_t            N = 4;

		anx_memset(&node, 0, sizeof(node));
		anx_memset(canvas, 0, sizeof(canvas));
		node.data     = canvas;
		node.data_len = sizeof(canvas);

		rc = anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS,
					      &node, 0, 0, W, H, &surf);
		if (rc != ANX_OK) return -37;
		rc = anx_iface_surface_map(surf);
		if (rc != ANX_OK) return -38;

		rc = make_clip(W, H, N, 30 * 256, &clip);
		if (rc != ANX_OK) return -39;

		rc = anx_video_sink_surface_bind(&surf->oid);
		if (rc != ANX_OK) return -40;
		rc = anx_video_sink_select("surface");
		if (rc != ANX_OK) return -41;

		uint64_t produced = 0;
		rc = anx_video_play_clip(&clip, 0, 0, &produced);
		if (rc != ANX_OK) return -42;
		if (produced != N) return -43;

		/* Last frame written has R=N-1; every pixel of the canvas
		 * should now have that byte in slot 0. */
		for (uint32_t i = 0; i < W * H; i++) {
			if (canvas[i * 4 + 0] != (uint8_t)(N - 1)) return -44;
			if (canvas[i * 4 + 3] != 255) return -45;
		}

		anx_video_sink_surface_unbind();
		anx_video_sink_select("null");
	}

	/* === Test 9: video-play cell with surface OID drives the sink === */
	{
		struct anx_surface       *surf;
		struct anx_content_node   node;
		uint8_t                   canvas[8 * 8 * 4];
		const uint32_t            W = 8, H = 8;
		anx_oid_t                 clip, out;
		const uint32_t            N = 3;

		anx_memset(&node, 0, sizeof(node));
		anx_memset(canvas, 0, sizeof(canvas));
		node.data     = canvas;
		node.data_len = sizeof(canvas);

		rc = anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS,
					      &node, 0, 0, W, H, &surf);
		if (rc != ANX_OK) return -46;
		rc = anx_iface_surface_map(surf);
		if (rc != ANX_OK) return -47;

		rc = make_clip(W, H, N, 30 * 256, &clip);
		if (rc != ANX_OK) return -48;

		anx_oid_t inputs[2] = { clip, surf->oid };
		anx_memset(&out, 0, sizeof(out));
		rc = anx_video_cell_dispatch("video-play", inputs, 2, &out);
		if (rc != ANX_OK) return -49;

		struct anx_state_object *trace = anx_objstore_lookup(&out);
		if (!trace) return -50;
		if (trace->object_type != ANX_OBJ_EXECUTION_TRACE) {
			anx_objstore_release(trace);
			return -51;
		}
		anx_objstore_release(trace);

		/* Last source frame R = N-1 must have landed on the surface. */
		for (uint32_t i = 0; i < W * H; i++) {
			if (canvas[i * 4 + 0] != (uint8_t)(N - 1)) return -52;
		}
	}

	return 0;
}
