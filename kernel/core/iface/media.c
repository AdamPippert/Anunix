/*
 * media.c — Media path baseline (P2-002).
 */

#include <anx/media.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/arch.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Single active session                                                */
/* ------------------------------------------------------------------ */

static struct anx_media_session session;
static bool                     session_active;

/* ------------------------------------------------------------------ */
/* Audio buffer queue (circular)                                        */
/* ------------------------------------------------------------------ */

static struct anx_audio_buffer audio_queue[ANX_AUDIO_QUEUE_MAX];
static uint32_t                audio_head;   /* next read  */
static uint32_t                audio_tail;   /* next write */
static uint32_t                audio_count;

static struct anx_spinlock     media_lock;

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

void
anx_media_init(void)
{
	anx_spin_init(&media_lock);
	anx_memset(&session,     0, sizeof(session));
	anx_memset(audio_queue,  0, sizeof(audio_queue));
	session_active = false;
	audio_head     = 0;
	audio_tail     = 0;
	audio_count    = 0;
}

/* ------------------------------------------------------------------ */
/* Session management                                                   */
/* ------------------------------------------------------------------ */

int
anx_media_open(const char *uri, struct anx_media_session *sess)
{
	bool flags;

	if (!uri || !sess)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&media_lock, &flags);

	if (session_active) {
		anx_spin_unlock_irqrestore(&media_lock, flags);
		return ANX_EBUSY;
	}

	anx_memset(&session, 0, sizeof(session));
	anx_strlcpy(session.uri, uri, ANX_MEDIA_URI_MAX);
	session.state  = ANX_MEDIA_STOPPED;
	session.active = true;
	session_active = true;

	*sess = session;

	anx_spin_unlock_irqrestore(&media_lock, flags);
	return ANX_OK;
}

int
anx_media_play(struct anx_media_session *sess)
{
	bool flags;

	if (!sess)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&media_lock, &flags);

	if (!session_active) {
		anx_spin_unlock_irqrestore(&media_lock, flags);
		return ANX_ENOENT;
	}

	if (session.state == ANX_MEDIA_PLAYING) {
		anx_spin_unlock_irqrestore(&media_lock, flags);
		return ANX_OK;
	}

	session.state = ANX_MEDIA_PLAYING;
	*sess = session;

	anx_spin_unlock_irqrestore(&media_lock, flags);
	return ANX_OK;
}

int
anx_media_pause(struct anx_media_session *sess)
{
	bool flags;

	if (!sess)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&media_lock, &flags);

	if (!session_active) {
		anx_spin_unlock_irqrestore(&media_lock, flags);
		return ANX_ENOENT;
	}

	session.state = ANX_MEDIA_PAUSED;
	*sess = session;

	anx_spin_unlock_irqrestore(&media_lock, flags);
	return ANX_OK;
}

int
anx_media_stop(struct anx_media_session *sess)
{
	bool flags;

	if (!sess)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&media_lock, &flags);

	if (!session_active) {
		anx_spin_unlock_irqrestore(&media_lock, flags);
		return ANX_ENOENT;
	}

	session.state       = ANX_MEDIA_STOPPED;
	session.position_ns = 0;
	*sess = session;

	anx_spin_unlock_irqrestore(&media_lock, flags);
	return ANX_OK;
}

int
anx_media_seek(struct anx_media_session *sess, uint64_t position_ns)
{
	bool flags;

	if (!sess)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&media_lock, &flags);

	if (!session_active) {
		anx_spin_unlock_irqrestore(&media_lock, flags);
		return ANX_ENOENT;
	}

	session.state       = ANX_MEDIA_SEEKING;
	session.position_ns = position_ns;
	*sess = session;

	anx_spin_unlock_irqrestore(&media_lock, flags);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Audio queue                                                          */
/* ------------------------------------------------------------------ */

int
anx_media_audio_enqueue(struct anx_media_session *sess,
                         const struct anx_audio_buffer *buf)
{
	bool flags;

	if (!sess || !buf)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&media_lock, &flags);

	if (!session_active) {
		anx_spin_unlock_irqrestore(&media_lock, flags);
		return ANX_ENOENT;
	}

	if (audio_count >= ANX_AUDIO_QUEUE_MAX) {
		anx_spin_unlock_irqrestore(&media_lock, flags);
		return ANX_EFULL;
	}

	audio_queue[audio_tail % ANX_AUDIO_QUEUE_MAX] = *buf;
	audio_tail++;
	audio_count++;

	anx_spin_unlock_irqrestore(&media_lock, flags);
	return ANX_OK;
}

int
anx_media_tick(struct anx_media_session *sess,
               struct anx_audio_buffer *played_out)
{
	bool flags;
	uint32_t slot;

	if (!sess || !played_out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&media_lock, &flags);

	if (!session_active || session.state != ANX_MEDIA_PLAYING) {
		anx_spin_unlock_irqrestore(&media_lock, flags);
		return ANX_EINVAL;
	}

	if (audio_count == 0) {
		/* underrun */
		session.underrun_count++;
		anx_memset(played_out, 0, sizeof(*played_out));
		played_out->underrun = true;
		*sess = session;
		anx_spin_unlock_irqrestore(&media_lock, flags);
		return ANX_ENOENT;
	}

	slot = audio_head % ANX_AUDIO_QUEUE_MAX;
	*played_out = audio_queue[slot];
	audio_head++;
	audio_count--;

	/* advance position by samples / rate */
	session.position_ns +=
		((uint64_t)played_out->count * 1000000000ULL) / ANX_AUDIO_SAMPLE_RATE;
	session.buffers_played++;
	*sess = session;

	anx_spin_unlock_irqrestore(&media_lock, flags);
	return ANX_OK;
}

void
anx_media_close(struct anx_media_session *sess)
{
	bool flags;

	anx_spin_lock_irqsave(&media_lock, &flags);

	anx_memset(&session, 0, sizeof(session));
	anx_memset(audio_queue, 0, sizeof(audio_queue));
	session_active = false;
	audio_head     = 0;
	audio_tail     = 0;
	audio_count    = 0;

	if (sess)
		anx_memset(sess, 0, sizeof(*sess));

	anx_spin_unlock_irqrestore(&media_lock, flags);
}
