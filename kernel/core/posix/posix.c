/*
 * posix.c — POSIX compatibility shim.
 *
 * Maps file and process operations to Anunix primitives.
 * Uses the "posix" namespace for path resolution and State
 * Objects for file storage.
 */

#include <anx/types.h>
#include <anx/posix.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/cell.h>
#include <anx/string.h>
#include <anx/uuid.h>

/* Current process context (single-process kernel for now) */
static struct anx_posix_proc root_proc;

void anx_posix_init(void)
{
	uint32_t i;

	anx_memset(&root_proc, 0, sizeof(root_proc));
	for (i = 0; i < ANX_POSIX_FD_MAX; i++)
		root_proc.fd_table[i].in_use = false;
}

/* Find a free file descriptor slot */
static int fd_alloc(void)
{
	int i;

	for (i = 0; i < ANX_POSIX_FD_MAX; i++) {
		if (!root_proc.fd_table[i].in_use)
			return i;
	}
	return ANX_ENOMEM;
}

int anx_posix_open(const char *path, int flags)
{
	anx_oid_t oid;
	int ret;
	int fd;

	if (!path)
		return ANX_EINVAL;

	/* Try to resolve the path in the posix namespace */
	ret = anx_ns_resolve("posix", path, &oid);

	if (ret != ANX_OK && (flags & ANX_O_CREAT)) {
		/* Create a new state object */
		struct anx_so_create_params params;
		struct anx_state_object *obj;

		anx_memset(&params, 0, sizeof(params));
		params.object_type = ANX_OBJ_BYTE_DATA;

		ret = anx_so_create(&params, &obj);
		if (ret != ANX_OK)
			return ret;

		oid = obj->oid;
		anx_objstore_release(obj);

		/* Bind to the namespace */
		ret = anx_ns_bind("posix", path, &oid);
		if (ret != ANX_OK)
			return ret;
	} else if (ret != ANX_OK) {
		return ret;
	}

	/* Allocate a file descriptor */
	fd = fd_alloc();
	if (fd < 0)
		return fd;

	root_proc.fd_table[fd].oid = oid;
	root_proc.fd_table[fd].in_use = true;
	root_proc.fd_table[fd].offset = 0;
	root_proc.fd_table[fd].flags = flags;

	return fd;
}

int anx_posix_close(int fd)
{
	if (fd < 0 || fd >= ANX_POSIX_FD_MAX)
		return ANX_EINVAL;
	if (!root_proc.fd_table[fd].in_use)
		return ANX_EINVAL;

	root_proc.fd_table[fd].in_use = false;
	return ANX_OK;
}

ssize_t anx_posix_read(int fd, void *buf, size_t count)
{
	struct anx_posix_fd *f;
	struct anx_object_handle handle;
	int ret;

	if (fd < 0 || fd >= ANX_POSIX_FD_MAX)
		return ANX_EINVAL;
	f = &root_proc.fd_table[fd];
	if (!f->in_use)
		return ANX_EINVAL;

	ret = anx_so_open(&f->oid, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK)
		return ret;

	ret = anx_so_read_payload(&handle, f->offset, buf, count);
	anx_so_close(&handle);

	if (ret == ANX_OK) {
		f->offset += count;
		return (ssize_t)count;
	}
	return ret;
}

ssize_t anx_posix_write(int fd, const void *buf, size_t count)
{
	struct anx_posix_fd *f;
	struct anx_object_handle handle;
	int ret;

	if (fd < 0 || fd >= ANX_POSIX_FD_MAX)
		return ANX_EINVAL;
	f = &root_proc.fd_table[fd];
	if (!f->in_use)
		return ANX_EINVAL;

	ret = anx_so_open(&f->oid, ANX_OPEN_WRITE, &handle);
	if (ret != ANX_OK)
		return ret;

	ret = anx_so_write_payload(&handle, f->offset, buf, count);
	anx_so_close(&handle);

	if (ret == ANX_OK) {
		f->offset += count;
		return (ssize_t)count;
	}
	return ret;
}

anx_cid_t anx_posix_fork(void)
{
	struct anx_cell *parent_cell;
	struct anx_cell *child;
	struct anx_cell_intent intent;
	anx_cid_t child_cid;
	int ret;

	/* Look up or create a parent cell */
	if (anx_uuid_is_nil(&root_proc.cid)) {
		struct anx_cell *pc;

		anx_memset(&intent, 0, sizeof(intent));
		anx_strlcpy(intent.name, "posix_root", sizeof(intent.name));
		anx_strlcpy(intent.objective, "root process",
			     sizeof(intent.objective));

		ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &pc);
		if (ret != ANX_OK) {
			anx_memset(&child_cid, 0, sizeof(child_cid));
			return child_cid;
		}
		root_proc.cid = pc->cid;
		anx_cell_store_release(pc);
	}

	parent_cell = anx_cell_store_lookup(&root_proc.cid);
	if (!parent_cell) {
		anx_memset(&child_cid, 0, sizeof(child_cid));
		return child_cid;
	}

	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "posix_child", sizeof(intent.name));
	anx_strlcpy(intent.objective, "forked process",
		     sizeof(intent.objective));

	ret = anx_cell_derive_child(parent_cell, ANX_CELL_TASK_EXECUTION,
				    &intent, &child);
	anx_cell_store_release(parent_cell);

	if (ret != ANX_OK) {
		anx_memset(&child_cid, 0, sizeof(child_cid));
		return child_cid;
	}

	child_cid = child->cid;
	anx_cell_store_release(child);
	return child_cid;
}

int anx_posix_exec(const char *path)
{
	(void)path;
	/* Stub: full exec requires loading a program into a cell */
	return ANX_ENOSYS;
}

void anx_posix_exit(int status)
{
	(void)status;
	/* Stub: would complete the current cell */
}

int anx_posix_wait(anx_cid_t cid, int *status_out)
{
	struct anx_cell *child;

	child = anx_cell_store_lookup(&cid);
	if (!child)
		return ANX_ENOENT;

	/* In a real implementation, we'd block until the child completes.
	 * For now, just check the current status. */
	if (status_out)
		*status_out = child->error_code;

	anx_cell_store_release(child);
	return ANX_OK;
}

int anx_posix_stat(const char *path, struct anx_posix_stat *buf)
{
	anx_oid_t oid;
	struct anx_state_object *obj;
	int ret;

	if (!path || !buf)
		return ANX_EINVAL;

	ret = anx_ns_resolve("posix", path, &oid);
	if (ret != ANX_OK)
		return ret;

	obj = anx_objstore_lookup(&oid);
	if (!obj)
		return ANX_ENOENT;

	buf->oid = obj->oid;
	buf->size = obj->payload_size;
	buf->created_at = 0;	/* TODO: store creation time */
	buf->type = (int)obj->object_type;

	anx_objstore_release(obj);
	return ANX_OK;
}
