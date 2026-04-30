/*
 * test_media.c — Tests for P2-002: media path baseline.
 *
 * Tests:
 * - P2-002-U01: audio underrun recovery
 * - P2-002-U02: playback position advances correctly
 * - P2-002-U03: state machine transitions are enforced
 */

#include <anx/types.h>
#include <anx/media.h>
#include <anx/string.h>

#define ASSERT(cond, code)    do { if (!(cond)) return (code); } while (0)
#define ASSERT_EQ(a, b, code) do { if ((a) != (b)) return (code); } while (0)

/* ------------------------------------------------------------------ */
/* P2-002-U01: audio underrun recovery                                 */
/* ------------------------------------------------------------------ */

static int test_underrun_recovery(void)
{
	struct anx_media_session sess;
	struct anx_audio_buffer buf, played;
	int rc;

	anx_media_init();

	rc = anx_media_open("test://audio.ogg", &sess);
	ASSERT_EQ(rc, ANX_OK, -100);
	ASSERT_EQ(sess.state, ANX_MEDIA_STOPPED, -101);

	/* Start playing. */
	rc = anx_media_play(&sess);
	ASSERT_EQ(rc, ANX_OK, -102);
	ASSERT_EQ(sess.state, ANX_MEDIA_PLAYING, -103);

	/* Tick with empty queue → underrun. */
	rc = anx_media_tick(&sess, &played);
	ASSERT_EQ(rc, ANX_ENOENT, -104);
	ASSERT(played.underrun == true, -105);

	ASSERT_EQ(sess.underrun_count, 1u, -106);

	/* Enqueue a buffer. */
	anx_memset(&buf, 0, sizeof(buf));
	buf.count        = ANX_AUDIO_BUFFER_SAMPLES;
	buf.timestamp_ns = 1000000ULL;
	rc = anx_media_audio_enqueue(&sess, &buf);
	ASSERT_EQ(rc, ANX_OK, -107);

	/* Tick now plays the buffer. */
	rc = anx_media_tick(&sess, &played);
	ASSERT_EQ(rc, ANX_OK, -108);
	ASSERT(played.underrun == false, -109);
	ASSERT_EQ(played.count, ANX_AUDIO_BUFFER_SAMPLES, -110);

	/* underrun_count stays at 1 (no new underrun). */
	ASSERT_EQ(sess.underrun_count, 1u, -111);
	ASSERT_EQ(sess.buffers_played, 1u, -112);

	/* Next tick underruns again. */
	rc = anx_media_tick(&sess, &played);
	ASSERT_EQ(rc, ANX_ENOENT, -113);
	ASSERT_EQ(sess.underrun_count, 2u, -114);

	anx_media_close(&sess);
	return 0;
}

/* ------------------------------------------------------------------ */
/* P2-002-U02: playback position advances with buffer count/rate       */
/* ------------------------------------------------------------------ */

static int test_position_advance(void)
{
	struct anx_media_session sess;
	struct anx_audio_buffer buf, played;
	uint64_t expected_ns;
	int rc;

	anx_media_init();

	rc = anx_media_open("test://music.flac", &sess);
	ASSERT_EQ(rc, ANX_OK, -200);
	anx_media_play(&sess);

	/* Enqueue two full buffers. */
	anx_memset(&buf, 0, sizeof(buf));
	buf.count = ANX_AUDIO_BUFFER_SAMPLES;
	anx_media_audio_enqueue(&sess, &buf);
	anx_media_audio_enqueue(&sess, &buf);

	/* Each buffer advances position by samples/rate nanoseconds.
	 * 4096 / 44100 * 1e9 ≈ 92879818 ns */
	expected_ns = ((uint64_t)ANX_AUDIO_BUFFER_SAMPLES * 1000000000ULL)
	              / ANX_AUDIO_SAMPLE_RATE;

	rc = anx_media_tick(&sess, &played);
	ASSERT_EQ(rc, ANX_OK, -201);
	ASSERT_EQ(sess.position_ns, expected_ns, -202);

	rc = anx_media_tick(&sess, &played);
	ASSERT_EQ(rc, ANX_OK, -203);
	ASSERT_EQ(sess.position_ns, expected_ns * 2, -204);

	anx_media_close(&sess);
	return 0;
}

/* ------------------------------------------------------------------ */
/* P2-002-U03: state machine transitions enforced                      */
/* ------------------------------------------------------------------ */

static int test_state_transitions(void)
{
	struct anx_media_session sess;
	int rc;

	anx_media_init();

	rc = anx_media_open("test://video.mkv", &sess);
	ASSERT_EQ(rc, ANX_OK, -300);
	ASSERT_EQ(sess.state, ANX_MEDIA_STOPPED, -301);

	/* Only one session open at a time. */
	struct anx_media_session sess2;
	rc = anx_media_open("test://other.mkv", &sess2);
	ASSERT_EQ(rc, ANX_EBUSY, -302);

	/* STOPPED → PLAYING */
	rc = anx_media_play(&sess);
	ASSERT_EQ(rc, ANX_OK, -303);
	ASSERT_EQ(sess.state, ANX_MEDIA_PLAYING, -304);

	/* PLAYING → PLAYING (idempotent) */
	rc = anx_media_play(&sess);
	ASSERT_EQ(rc, ANX_OK, -305);
	ASSERT_EQ(sess.state, ANX_MEDIA_PLAYING, -306);

	/* PLAYING → PAUSED */
	rc = anx_media_pause(&sess);
	ASSERT_EQ(rc, ANX_OK, -307);
	ASSERT_EQ(sess.state, ANX_MEDIA_PAUSED, -308);

	/* PAUSED → SEEKING */
	rc = anx_media_seek(&sess, 5000000000ULL);
	ASSERT_EQ(rc, ANX_OK, -309);
	ASSERT_EQ(sess.state, ANX_MEDIA_SEEKING,    -310);
	ASSERT_EQ(sess.position_ns, 5000000000ULL,  -311);

	/* → STOPPED */
	rc = anx_media_stop(&sess);
	ASSERT_EQ(rc, ANX_OK, -312);
	ASSERT_EQ(sess.state,       ANX_MEDIA_STOPPED, -313);
	ASSERT_EQ(sess.position_ns, 0ULL,              -314);

	/* Tick while stopped: EINVAL (not playing). */
	struct anx_audio_buffer played;
	rc = anx_media_tick(&sess, &played);
	ASSERT_EQ(rc, ANX_EINVAL, -315);

	/* Close. */
	anx_media_close(&sess);

	/* After close, operations fail. */
	rc = anx_media_play(&sess);
	ASSERT_EQ(rc, ANX_ENOENT, -316);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int test_media(void)
{
	int rc;

	rc = test_underrun_recovery();
	if (rc) return rc;

	rc = test_position_advance();
	if (rc) return rc;

	rc = test_state_transitions();
	if (rc) return rc;

	return 0;
}
