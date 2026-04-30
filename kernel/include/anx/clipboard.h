/*
 * anx/clipboard.h — Clipboard, drag-and-drop, and file-picker contracts.
 *
 * All data transfer operations are capability-gated per cell. Clipboard
 * access requires explicit grants; drag-drop is surface-scoped. The
 * file-picker provides a minimal request/respond contract for WM integration.
 */

#ifndef ANX_CLIPBOARD_H
#define ANX_CLIPBOARD_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Clipboard permission flags                                           */
/* ------------------------------------------------------------------ */

#define ANX_CLIPBOARD_FLAG_READ   (1u << 0)
#define ANX_CLIPBOARD_FLAG_WRITE  (1u << 1)

/* ------------------------------------------------------------------ */
/* Clipboard API                                                        */
/* ------------------------------------------------------------------ */

#define ANX_CLIPBOARD_MAX_SIZE  4096u  /* max payload bytes */
#define ANX_CLIPBOARD_MIME_MAX    64u  /* max MIME type string length */

/* Initialise clipboard subsystem (called by anx_iface_init). */
void anx_clipboard_init(void);

/* Grant permission flags to a cell. Returns ANX_EFULL if table is full. */
int anx_clipboard_grant(anx_cid_t cid, uint32_t flags);

/* Revoke permission flags from a cell. */
int anx_clipboard_revoke(anx_cid_t cid, uint32_t flags);

/* Write clipboard content. Requires ANX_CLIPBOARD_FLAG_WRITE. */
int anx_clipboard_write(anx_cid_t cid, const char *mime_type,
                         const void *data, uint32_t len);

/* Read clipboard content into buf. Requires ANX_CLIPBOARD_FLAG_READ. */
int anx_clipboard_read(anx_cid_t cid,
                        char *mime_type_out, uint32_t mime_max,
                        void *buf, uint32_t buf_max,
                        uint32_t *len_out);

/* Clear clipboard content and state. */
void anx_clipboard_clear(void);

/* ------------------------------------------------------------------ */
/* Drag-and-drop                                                        */
/* ------------------------------------------------------------------ */

#define ANX_DRAG_DATA_MAX  512u

struct anx_drag_payload {
	char      mime_type[ANX_CLIPBOARD_MIME_MAX];
	uint8_t   data[ANX_DRAG_DATA_MAX];
	uint32_t  data_len;
	anx_oid_t source_surf;
};

struct anx_surface;  /* forward */

/* Begin a drag from source surface. Stores state for delivery. */
int anx_iface_drag_begin(struct anx_surface *source,
                          const char *mime_type,
                          const void *data, uint32_t len);

/* Deliver current drag payload to target; fills out and clears drag state. */
int anx_iface_drag_deliver(struct anx_surface *target,
                             struct anx_drag_payload *out);

/* Cancel any in-progress drag. */
void anx_iface_drag_cancel(void);

/* ------------------------------------------------------------------ */
/* File-picker contract                                                 */
/* ------------------------------------------------------------------ */

#define ANX_FILEPICK_PATH_MAX  256u

/* Request a file-picker dialog. Returns a unique request ID. */
int anx_filepick_request(anx_cid_t requester, const char *filter,
                          uint32_t *id_out);

/* WM/compositor responds to a pending request with the chosen path. */
int anx_filepick_respond(uint32_t request_id, const char *path);

/* Retrieve the result path for a fulfilled request. */
int anx_filepick_result(uint32_t request_id,
                         char *path_out, uint32_t max);

#endif /* ANX_CLIPBOARD_H */
