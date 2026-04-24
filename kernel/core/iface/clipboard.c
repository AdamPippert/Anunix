/*
 * clipboard.c — Clipboard, drag-and-drop, and file-picker (P1-003).
 *
 * All three subsystems live here: they share the same permission table
 * and are always initialised together via anx_clipboard_init().
 */

#include <anx/clipboard.h>
#include <anx/interface_plane.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Permission table                                                     */
/* ------------------------------------------------------------------ */

#define CLIP_PERM_MAX  32

struct clip_perm {
	anx_cid_t cid;
	uint32_t  flags;
	bool      active;
};

static struct clip_perm  clip_perms[CLIP_PERM_MAX];
static struct anx_spinlock clip_lock;

static uint32_t
perm_flags_for(anx_cid_t cid)
{
	uint32_t i;

	for (i = 0; i < CLIP_PERM_MAX; i++) {
		if (clip_perms[i].active &&
		    anx_uuid_compare(&clip_perms[i].cid, &cid) == 0)
			return clip_perms[i].flags;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Clipboard storage                                                    */
/* ------------------------------------------------------------------ */

static char    clip_mime[ANX_CLIPBOARD_MIME_MAX];
static uint8_t clip_data[ANX_CLIPBOARD_MAX_SIZE];
static uint32_t clip_len;
static bool    clip_valid;

/* ------------------------------------------------------------------ */
/* Drag state                                                           */
/* ------------------------------------------------------------------ */

static struct anx_drag_payload drag_state;
static bool                    drag_active;

/* ------------------------------------------------------------------ */
/* File-picker state                                                    */
/* ------------------------------------------------------------------ */

struct filepick_slot {
	uint32_t  id;
	anx_cid_t requester;
	char      filter[64];
	bool      active;
	bool      fulfilled;
	char      result[ANX_FILEPICK_PATH_MAX];
};

static struct filepick_slot filepick_slot;
static uint32_t             filepick_next_id;

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

void
anx_clipboard_init(void)
{
	anx_spin_init(&clip_lock);
	anx_memset(clip_perms, 0, sizeof(clip_perms));
	anx_memset(clip_mime,  0, sizeof(clip_mime));
	anx_memset(clip_data,  0, sizeof(clip_data));
	clip_len   = 0;
	clip_valid = false;

	anx_memset(&drag_state, 0, sizeof(drag_state));
	drag_active = false;

	anx_memset(&filepick_slot, 0, sizeof(filepick_slot));
	filepick_next_id = 1;
}

/* ------------------------------------------------------------------ */
/* Clipboard permissions                                                */
/* ------------------------------------------------------------------ */

int
anx_clipboard_grant(anx_cid_t cid, uint32_t flags)
{
	uint32_t i;
	bool f;

	anx_spin_lock_irqsave(&clip_lock, &f);

	/* Update existing entry if present. */
	for (i = 0; i < CLIP_PERM_MAX; i++) {
		if (clip_perms[i].active &&
		    anx_uuid_compare(&clip_perms[i].cid, &cid) == 0) {
			clip_perms[i].flags |= flags;
			anx_spin_unlock_irqrestore(&clip_lock, f);
			return ANX_OK;
		}
	}

	/* Find a free slot. */
	for (i = 0; i < CLIP_PERM_MAX; i++) {
		if (!clip_perms[i].active) {
			clip_perms[i].cid    = cid;
			clip_perms[i].flags  = flags;
			clip_perms[i].active = true;
			anx_spin_unlock_irqrestore(&clip_lock, f);
			return ANX_OK;
		}
	}

	anx_spin_unlock_irqrestore(&clip_lock, f);
	return ANX_EFULL;
}

int
anx_clipboard_revoke(anx_cid_t cid, uint32_t flags)
{
	uint32_t i;
	bool f;

	anx_spin_lock_irqsave(&clip_lock, &f);
	for (i = 0; i < CLIP_PERM_MAX; i++) {
		if (clip_perms[i].active &&
		    anx_uuid_compare(&clip_perms[i].cid, &cid) == 0) {
			clip_perms[i].flags &= ~flags;
			if (clip_perms[i].flags == 0)
				clip_perms[i].active = false;
			anx_spin_unlock_irqrestore(&clip_lock, f);
			return ANX_OK;
		}
	}
	anx_spin_unlock_irqrestore(&clip_lock, f);
	return ANX_ENOENT;
}

/* ------------------------------------------------------------------ */
/* Clipboard read/write                                                 */
/* ------------------------------------------------------------------ */

int
anx_clipboard_write(anx_cid_t cid, const char *mime_type,
                     const void *data, uint32_t len)
{
	bool f;

	if (!mime_type || !data)
		return ANX_EINVAL;
	if (len > ANX_CLIPBOARD_MAX_SIZE)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&clip_lock, &f);
	if (!(perm_flags_for(cid) & ANX_CLIPBOARD_FLAG_WRITE)) {
		anx_spin_unlock_irqrestore(&clip_lock, f);
		return ANX_EPERM;
	}
	anx_strlcpy(clip_mime, mime_type, sizeof(clip_mime));
	anx_memcpy(clip_data, data, len);
	clip_len   = len;
	clip_valid = true;
	anx_spin_unlock_irqrestore(&clip_lock, f);
	return ANX_OK;
}

int
anx_clipboard_read(anx_cid_t cid,
                    char *mime_type_out, uint32_t mime_max,
                    void *buf, uint32_t buf_max,
                    uint32_t *len_out)
{
	bool f;

	if (!buf || !len_out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&clip_lock, &f);
	if (!(perm_flags_for(cid) & ANX_CLIPBOARD_FLAG_READ)) {
		anx_spin_unlock_irqrestore(&clip_lock, f);
		return ANX_EPERM;
	}
	if (!clip_valid) {
		anx_spin_unlock_irqrestore(&clip_lock, f);
		return ANX_ENOENT;
	}
	if (clip_len > buf_max) {
		anx_spin_unlock_irqrestore(&clip_lock, f);
		return ANX_EINVAL;
	}
	anx_memcpy(buf, clip_data, clip_len);
	*len_out = clip_len;
	if (mime_type_out && mime_max > 0)
		anx_strlcpy(mime_type_out, clip_mime, mime_max);
	anx_spin_unlock_irqrestore(&clip_lock, f);
	return ANX_OK;
}

void
anx_clipboard_clear(void)
{
	bool f;

	anx_spin_lock_irqsave(&clip_lock, &f);
	anx_memset(clip_mime, 0, sizeof(clip_mime));
	anx_memset(clip_data, 0, sizeof(clip_data));
	clip_len   = 0;
	clip_valid = false;
	anx_spin_unlock_irqrestore(&clip_lock, f);
}

/* ------------------------------------------------------------------ */
/* Drag-and-drop                                                        */
/* ------------------------------------------------------------------ */

int
anx_iface_drag_begin(struct anx_surface *source,
                      const char *mime_type,
                      const void *data, uint32_t len)
{
	bool f;

	if (!source || !mime_type || !data)
		return ANX_EINVAL;
	if (len > ANX_DRAG_DATA_MAX)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&clip_lock, &f);
	if (drag_active) {
		anx_spin_unlock_irqrestore(&clip_lock, f);
		return ANX_EBUSY;
	}
	anx_memset(&drag_state, 0, sizeof(drag_state));
	anx_strlcpy(drag_state.mime_type, mime_type,
	            sizeof(drag_state.mime_type));
	anx_memcpy(drag_state.data, data, len);
	drag_state.data_len    = len;
	drag_state.source_surf = source->oid;
	drag_active = true;
	anx_spin_unlock_irqrestore(&clip_lock, f);
	return ANX_OK;
}

int
anx_iface_drag_deliver(struct anx_surface *target,
                        struct anx_drag_payload *out)
{
	bool f;

	if (!target || !out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&clip_lock, &f);
	if (!drag_active) {
		anx_spin_unlock_irqrestore(&clip_lock, f);
		return ANX_ENOENT;
	}
	*out = drag_state;
	anx_memset(&drag_state, 0, sizeof(drag_state));
	drag_active = false;
	anx_spin_unlock_irqrestore(&clip_lock, f);
	return ANX_OK;
}

void
anx_iface_drag_cancel(void)
{
	bool f;

	anx_spin_lock_irqsave(&clip_lock, &f);
	anx_memset(&drag_state, 0, sizeof(drag_state));
	drag_active = false;
	anx_spin_unlock_irqrestore(&clip_lock, f);
}

/* ------------------------------------------------------------------ */
/* File-picker                                                          */
/* ------------------------------------------------------------------ */

int
anx_filepick_request(anx_cid_t requester, const char *filter, uint32_t *id_out)
{
	bool f;

	if (!id_out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&clip_lock, &f);
	if (filepick_slot.active) {
		anx_spin_unlock_irqrestore(&clip_lock, f);
		return ANX_EBUSY;
	}
	filepick_slot.id        = filepick_next_id++;
	filepick_slot.requester = requester;
	filepick_slot.active    = true;
	filepick_slot.fulfilled = false;
	anx_memset(filepick_slot.result, 0, sizeof(filepick_slot.result));
	if (filter)
		anx_strlcpy(filepick_slot.filter, filter,
		            sizeof(filepick_slot.filter));
	else
		filepick_slot.filter[0] = '\0';
	*id_out = filepick_slot.id;
	anx_spin_unlock_irqrestore(&clip_lock, f);
	return ANX_OK;
}

int
anx_filepick_respond(uint32_t request_id, const char *path)
{
	bool f;

	if (!path)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&clip_lock, &f);
	if (!filepick_slot.active || filepick_slot.id != request_id) {
		anx_spin_unlock_irqrestore(&clip_lock, f);
		return ANX_ENOENT;
	}
	anx_strlcpy(filepick_slot.result, path,
	            sizeof(filepick_slot.result));
	filepick_slot.fulfilled = true;
	anx_spin_unlock_irqrestore(&clip_lock, f);
	return ANX_OK;
}

int
anx_filepick_result(uint32_t request_id, char *path_out, uint32_t max)
{
	bool f;

	if (!path_out || max == 0)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&clip_lock, &f);
	if (!filepick_slot.active || filepick_slot.id != request_id) {
		anx_spin_unlock_irqrestore(&clip_lock, f);
		return ANX_ENOENT;
	}
	if (!filepick_slot.fulfilled) {
		anx_spin_unlock_irqrestore(&clip_lock, f);
		return ANX_EBUSY;
	}
	anx_strlcpy(path_out, filepick_slot.result, max);
	filepick_slot.active = false;
	anx_spin_unlock_irqrestore(&clip_lock, f);
	return ANX_OK;
}
