/*
 * cell.c — Audio player cell dispatch (RFC-0024).
 *
 * Workflow CELL_CALL nodes route here when the intent string starts
 * with "audio-".  The cell consumes a clip OID, plays it through the
 * currently selected sink, and returns a trace OID describing what
 * was played.
 */

#include <anx/audio.h>
#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>

/* Trace payload (BYTE_DATA): "audio-play oid=<...> frames=<N> sink=<name>\n" */
static int emit_trace(const char *intent, const anx_oid_t *src,
		      uint32_t frames, const char *sink_name,
		      anx_oid_t *out_oid)
{
	struct anx_so_create_params cp;
	struct anx_state_object    *obj;
	char                        buf[160];
	int                         len;

	(void)src;	/* OID not formatted here for simplicity */
	len = anx_snprintf(buf, sizeof(buf),
			   "%s frames=%u sink=%s\n",
			   intent, frames,
			   sink_name ? sink_name : "(none)");
	if (len <= 0)
		return ANX_EIO;

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_EXECUTION_TRACE;
	cp.schema_uri     = "anx:schema/audio/trace/v1";
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

static int do_play(const anx_oid_t *src, anx_oid_t *out_oid)
{
	struct anx_audio_stream *st;
	int                      rc;
	uint32_t                 frames_to_play;
	int                      played;

	rc = anx_audio_stream_add(src, &st);
	if (rc != ANX_OK)
		return rc;

	/* Compute total frames at the internal sample rate for this stream. */
	{
		uint32_t in_rate  = st->src_fmt.sample_rate;
		uint32_t out_rate = ANX_AUDIO_DEFAULT_RATE;
		uint64_t out_frames;

		if (in_rate == 0)
			in_rate = ANX_AUDIO_DEFAULT_RATE;
		out_frames = (st->src_total * (uint64_t)out_rate +
			      (uint64_t)in_rate - 1) / in_rate;
		if (out_frames > 0xFFFFFFFFULL)
			out_frames = 0xFFFFFFFFULL;
		frames_to_play = (uint32_t)out_frames;
	}

	played = anx_audio_sink_run(frames_to_play);
	(void)played;	/* sink may stop early */

	{
		uint16_t id = st->id;
		anx_audio_stream_remove(id);
	}

	return emit_trace("audio-play", src, frames_to_play,
			  anx_audio_sink_active(), out_oid);
}

static int do_mix(const anx_oid_t *clips, uint32_t count, anx_oid_t *out_oid)
{
	struct anx_audio_stream *streams[ANX_AUDIO_MAX_STREAMS];
	uint32_t                 added = 0;
	uint32_t                 i;
	int                      rc = ANX_OK;
	uint32_t                 max_frames = 0;
	int16_t                 *out;
	int                      produced;

	if (count == 0 || count > ANX_AUDIO_MAX_STREAMS)
		return ANX_EINVAL;

	/* Add each input as a stream and track max output length. */
	for (i = 0; i < count; i++) {
		struct anx_state_object *obj = anx_objstore_lookup(&clips[i]);
		const struct anx_audio_clip_hdr *hdr;
		uint64_t out_frames;

		if (!obj || obj->object_type != ANX_OBJ_AUDIO_CLIP) {
			if (obj) anx_objstore_release(obj);
			rc = ANX_EINVAL;
			goto cleanup_added;
		}
		hdr = (const struct anx_audio_clip_hdr *)obj->payload;
		out_frames = (hdr->frame_count * (uint64_t)ANX_AUDIO_DEFAULT_RATE +
			      (uint64_t)hdr->fmt.sample_rate - 1) /
			     (hdr->fmt.sample_rate ? hdr->fmt.sample_rate
						   : ANX_AUDIO_DEFAULT_RATE);
		if (out_frames > max_frames)
			max_frames = (uint32_t)out_frames;
		anx_objstore_release(obj);

		rc = anx_audio_stream_add(&clips[i], &streams[i]);
		if (rc != ANX_OK)
			goto cleanup_added;
		added++;
	}

	if (max_frames == 0) {
		rc = ANX_EINVAL;
		goto cleanup_added;
	}

	/* Pull the entire mix into a fresh PCM buffer. */
	out = (int16_t *)anx_alloc(max_frames * 2 * sizeof(int16_t));
	if (!out) {
		rc = ANX_ENOMEM;
		goto cleanup_added;
	}
	produced = anx_audio_mix_pull(out, max_frames);
	if (produced <= 0) {
		anx_free(out);
		rc = ANX_EIO;
		goto cleanup_added;
	}

	/* Build a clip payload around the mix. */
	{
		uint32_t hdr_sz = (uint32_t)sizeof(struct anx_audio_clip_hdr);
		uint32_t pcm_sz = (uint32_t)produced * 2u * sizeof(int16_t);
		uint32_t total  = hdr_sz + pcm_sz;
		uint8_t *blob   = (uint8_t *)anx_alloc(total);

		if (!blob) {
			anx_free(out);
			rc = ANX_ENOMEM;
			goto cleanup_added;
		}
		struct anx_audio_clip_hdr *h = (struct anx_audio_clip_hdr *)blob;
		anx_memset(h, 0, sizeof(*h));
		h->magic            = ANX_AUDIO_CLIP_MAGIC;
		h->version          = ANX_AUDIO_CLIP_VERSION;
		h->fmt.sample_rate  = ANX_AUDIO_DEFAULT_RATE;
		h->fmt.channels     = ANX_AUDIO_DEFAULT_CHANNELS;
		h->fmt.bits_per_sample = ANX_AUDIO_INTERNAL_BPS;
		h->frame_count      = (uint64_t)produced;
		h->payload_offset   = sizeof(*h);
		anx_memcpy(blob + hdr_sz, out, pcm_sz);
		anx_free(out);

		rc = anx_audio_clip_create(blob, total, out_oid);
		anx_free(blob);
	}

cleanup_added:
	for (i = 0; i < added; i++)
		anx_audio_stream_remove(streams[i]->id);
	return rc;
}

int anx_audio_cell_dispatch(const char *intent,
			    const anx_oid_t *in_oids, uint32_t in_count,
			    anx_oid_t *out_oid_out)
{
	if (!intent || !out_oid_out)
		return ANX_EINVAL;

	anx_memset(out_oid_out, 0, sizeof(*out_oid_out));

	if (anx_strcmp(intent, "audio-play") == 0) {
		if (in_count < 1)
			return ANX_EINVAL;
		return do_play(&in_oids[0], out_oid_out);
	}
	if (anx_strcmp(intent, "audio-mix") == 0) {
		return do_mix(in_oids, in_count, out_oid_out);
	}
	return ANX_ENOSYS;
}
