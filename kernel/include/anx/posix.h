/*
 * anx/posix.h — POSIX compatibility shim.
 *
 * Maps POSIX file and process operations to Anunix primitives:
 *   files → State Objects (via namespace "posix")
 *   processes → Execution Cells
 *   file descriptors → per-process OID table
 *
 * This is "Mode A" compatibility (RFC-0003 Section 28): a thin
 * wrapper, not full process isolation or virtual memory.
 */

#ifndef ANX_POSIX_H
#define ANX_POSIX_H

#include <anx/types.h>

/* --- Limits --- */

#define ANX_POSIX_FD_MAX	64
#define ANX_POSIX_PROC_MAX	32

/* Syscall ABI v0 numbers */
#define ANX_SYSCALL_OPEN	0
#define ANX_SYSCALL_READ	1
#define ANX_SYSCALL_WRITE	2
#define ANX_SYSCALL_CLOSE	3
#define ANX_SYSCALL_TIME	4

/* --- File open flags --- */

#define ANX_O_RDONLY	0x0000
#define ANX_O_WRONLY	0x0001
#define ANX_O_RDWR	0x0002
#define ANX_O_CREAT	0x0100
#define ANX_O_TRUNC	0x0200
#define ANX_O_APPEND	0x0400

/* --- File descriptor entry --- */

struct anx_posix_fd {
	anx_oid_t oid;
	bool in_use;
	uint64_t offset;
	int flags;
};

/* --- Process descriptor --- */

struct anx_posix_proc {
	anx_cid_t cid;
	bool in_use;
	uint64_t vm_descriptor_root;
	uint64_t page_map_id;
	uint64_t map_generation;
	int exit_status;
	bool faulted;
	char image_path[64];
	struct anx_posix_fd fd_table[ANX_POSIX_FD_MAX];
};

struct anx_posix_exec_result {
	char stdout_text[64];
	size_t stdout_len;
	int exit_status;
};

/* --- Stat buffer --- */

struct anx_posix_stat {
	anx_oid_t oid;
	uint64_t size;
	anx_time_t created_at;
	int type;		/* maps from anx_object_type */
};

/* --- POSIX Shim API --- */

/* Initialize the POSIX compatibility layer */
void anx_posix_init(void);

/* File operations */
int anx_posix_open(const char *path, int flags);
int anx_posix_close(int fd);
ssize_t anx_posix_read(int fd, void *buf, size_t count);
ssize_t anx_posix_write(int fd, const void *buf, size_t count);

/* Process operations */
anx_cid_t anx_posix_fork(void);
int anx_posix_exec(const char *path);
void anx_posix_exit(int status);
int anx_posix_wait(anx_cid_t cid, int *status_out);

/* Userspace prerequisite APIs (P0-001/P0-002) */
int anx_posix_spawn_isolated(struct anx_posix_proc **proc_out);
int anx_posix_proc_fault(struct anx_posix_proc *proc, int fault_code);
int anx_posix_proc_exit_status(struct anx_posix_proc *proc, int status);
int anx_posix_exec_in_proc(struct anx_posix_proc *proc, const char *path);
int anx_posix_loader_validate(const void *binary, size_t binary_size);
long anx_posix_syscall(struct anx_posix_proc *proc, uint64_t nr,
		      uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
int anx_posix_exec_last_result(struct anx_posix_exec_result *out);

/* Metadata */
int anx_posix_stat(const char *path, struct anx_posix_stat *buf);

#endif /* ANX_POSIX_H */
