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

#define ANX_TLS_MAX_DNS_NAME	64
#define ANX_TLS_MAX_SUBJECT	64
#define ANX_TLS_MAX_CHAIN	8
#define ANX_TLS_MAX_TRUST_ANCHORS	8

#define ANX_TLS_ERR_NONE		0
#define ANX_TLS_ERR_CHAIN_MALFORMED	1
#define ANX_TLS_ERR_CHAIN_UNTRUSTED	2
#define ANX_TLS_ERR_HOSTNAME_MISMATCH	3
#define ANX_TLS_ERR_ENDPOINT_UNKNOWN	4

struct anx_tls_cert {
	char subject[ANX_TLS_MAX_SUBJECT];
	char issuer[ANX_TLS_MAX_SUBJECT];
	char dns_name[ANX_TLS_MAX_DNS_NAME];
	bool is_ca;
};

#define ANX_PROFILE_NAME_MAX	32
#define ANX_PROFILE_BLOB_MAX	256

struct anx_profile_record {
	char name[ANX_PROFILE_NAME_MAX];
	uint8_t committed_blob[ANX_PROFILE_BLOB_MAX];
	size_t committed_len;
	uint8_t committed_hash[32];
	uint8_t staged_blob[ANX_PROFILE_BLOB_MAX];
	size_t staged_len;
	bool staged_valid;
	bool lock_held;
	anx_cid_t lock_owner;
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

/* TLS trust baseline APIs (P0-005) */
void anx_tls_trust_store_reset(void);
int anx_tls_trust_anchor_add(const char *subject);
int anx_tls_validate_chain(const struct anx_tls_cert *chain, size_t chain_len,
		      int *tls_error_out);
int anx_tls_verify_hostname(const struct anx_tls_cert *leaf,
		    const char *hostname, int *tls_error_out);
int anx_tls_https_get(const char *url, char *response_out, size_t response_len,
	     int *status_out, int *tls_error_out);

/* Profile/cache storage APIs (P0-006) */
void anx_profile_store_init(void);
int anx_profile_lock_acquire(struct anx_posix_proc *proc, const char *name);
int anx_profile_lock_release(struct anx_posix_proc *proc, const char *name);
int anx_profile_stage_write(struct anx_posix_proc *proc, const char *name,
		    const void *blob, size_t blob_len);
int anx_profile_commit(struct anx_posix_proc *proc, const char *name);
int anx_profile_read(const char *name, void *out, size_t out_len,
	     size_t *blob_len_out, uint8_t hash_out[32]);
int anx_profile_simulate_crash(const char *name);

/* Metadata */
int anx_posix_stat(const char *path, struct anx_posix_stat *buf);

#endif /* ANX_POSIX_H */
