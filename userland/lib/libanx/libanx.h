#ifndef ANX_LIBANX_H
#define ANX_LIBANX_H

/*
 * libanx — userland adapter library for Anunix kernel APIs
 *
 * Encodes the five recurring adaptation patterns from RFC-0010:
 *   POR  - Path-to-Object Resolution   (path.c)
 *   CLW  - Cell Lifecycle Wrap         (exec.c)
 *   CCO  - Capability-Checked Open     (access.c)
 *   SOT  - Streaming Output as Trace   (stream.c)
 *   MFO  - Metadata-Aware Format Out   (fmt.c)
 */

#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/memplane.h>
#include <anx/capability.h>
#include <anx/errno.h>

/* --- POR: Path-to-Object Resolution --- */

enum anx_open_mode {
	ANX_OPEN_READ  = 0,
	ANX_OPEN_WRITE = 1,
	ANX_OPEN_RW    = 2,
};

struct anx_object_handle {
	anx_oid_t               oid;
	struct anx_state_object *obj;
	int                      fd;   /* posix shim fd, -1 if unused */
};

int anx_path_open(const char *path, enum anx_open_mode mode,
                  struct anx_object_handle *out);
void anx_path_close(struct anx_object_handle *h);

/* --- CLW: Cell Lifecycle Wrap --- */

int anx_exec_child(struct anx_cell *parent, const char *binary_path,
                   char *const argv[], int *exit_code_out);

/* --- CCO: Capability-Checked Open --- */

int anx_access_check(const anx_oid_t *oid, uint32_t required_caps,
                     const anx_cid_t *caller_cid);

/* --- SOT: Streaming Output as Trace Object --- */

int anx_stream_write(int fd, const void *buf, size_t len,
                     const anx_oid_t *source_oid, int commit_as_object);

/* --- MFO: Metadata-Aware Format Output --- */

void        anx_fmt_oid(char *buf, size_t len, const anx_oid_t *oid);
void        anx_fmt_tier(char *buf, size_t len, uint8_t tier_mask);
const char *anx_fmt_obj_type(enum anx_object_type t);
const char *anx_fmt_cell_status(enum anx_cell_status s);
const char *anx_fmt_engine_class(enum anx_engine_class c);
const char *anx_fmt_mem_validation(enum anx_mem_validation_state v);

#endif /* ANX_LIBANX_H */
