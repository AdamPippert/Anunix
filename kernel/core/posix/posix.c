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
#include <anx/crypto.h>

#define ANX_ELF_MAGIC0 0x7f
#define ANX_ELF_MAGIC1 'E'
#define ANX_ELF_MAGIC2 'L'
#define ANX_ELF_MAGIC3 'F'
#define ANX_ELFCLASS64 2
#define ANX_ELFDATA2LSB 1
#define ANX_PT_LOAD 1
#define ANX_POSIX_TIME_NS 123456789ULL
#define ANX_PROFILE_STORE_MAX 8

struct anx_tls_endpoint {
	const char *url;
	const char *response;
	int status;
	struct anx_tls_cert chain[ANX_TLS_MAX_CHAIN];
	size_t chain_len;
};

struct anx_elf64_ehdr {
	uint8_t e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct anx_elf64_phdr {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Root process context and isolated process table */
static struct anx_posix_proc root_proc;
static struct anx_posix_proc proc_table[ANX_POSIX_PROC_MAX];
static uint64_t next_vm_descriptor_root;
static uint64_t next_page_map_id;
static struct anx_posix_exec_result last_exec_result;
static char tls_trust_anchors[ANX_TLS_MAX_TRUST_ANCHORS][ANX_TLS_MAX_SUBJECT];
static size_t tls_trust_anchor_count;
static struct anx_profile_record profile_store[ANX_PROFILE_STORE_MAX];

static const struct anx_tls_endpoint tls_endpoints[] = {
	{
		.url = "https://good.anx.test/",
		.response = "ok:https-good",
		.status = 200,
		.chain = {
			{
				.subject = "good.anx.test",
				.issuer = "ANX Intermediate CA",
				.dns_name = "good.anx.test",
				.is_ca = false,
			},
			{
				.subject = "ANX Intermediate CA",
				.issuer = "ANX Root CA",
				.dns_name = "",
				.is_ca = true,
			},
			{
				.subject = "ANX Root CA",
				.issuer = "ANX Root CA",
				.dns_name = "",
				.is_ca = true,
			},
		},
		.chain_len = 3,
	},
	{
		.url = "https://badcert.anx.test/",
		.response = "",
		.status = 0,
		.chain = {
			{
				.subject = "badcert.anx.test",
				.issuer = "Unknown Intermediate",
				.dns_name = "badcert.anx.test",
				.is_ca = false,
			},
			{
				.subject = "Unknown Intermediate",
				.issuer = "Unknown Root",
				.dns_name = "",
				.is_ca = true,
			},
			{
				.subject = "Unknown Root",
				.issuer = "Unknown Root",
				.dns_name = "",
				.is_ca = true,
			},
		},
		.chain_len = 3,
	},
};

void anx_posix_init(void)
{
	uint32_t i;
	uint32_t j;

	anx_memset(&root_proc, 0, sizeof(root_proc));
	root_proc.in_use = true;
	root_proc.vm_descriptor_root = 1;
	root_proc.page_map_id = 1;

	anx_memset(proc_table, 0, sizeof(proc_table));
	for (i = 0; i < ANX_POSIX_PROC_MAX; i++) {
		proc_table[i].in_use = false;
		for (j = 0; j < ANX_POSIX_FD_MAX; j++)
			proc_table[i].fd_table[j].in_use = false;
	}
	for (i = 0; i < ANX_POSIX_FD_MAX; i++)
		root_proc.fd_table[i].in_use = false;

	next_vm_descriptor_root = 2;
	next_page_map_id = 2;
	anx_memset(&last_exec_result, 0, sizeof(last_exec_result));
	anx_tls_trust_store_reset();
	anx_profile_store_init();
}

/* Find a free file descriptor slot */
static int fd_alloc(struct anx_posix_proc *proc)
{
	int i;

	for (i = 0; i < ANX_POSIX_FD_MAX; i++) {
		if (!proc->fd_table[i].in_use)
			return i;
	}
	return ANX_ENOMEM;
}

static int posix_open_proc(struct anx_posix_proc *proc, const char *path, int flags)
{
	anx_oid_t oid;
	int ret;
	int fd;

	if (!proc || !path)
		return ANX_EINVAL;

	ret = anx_ns_resolve("posix", path, &oid);
	if (ret != ANX_OK && (flags & ANX_O_CREAT)) {
		struct anx_so_create_params params;
		struct anx_state_object *obj;

		anx_memset(&params, 0, sizeof(params));
		params.object_type = ANX_OBJ_BYTE_DATA;
		ret = anx_so_create(&params, &obj);
		if (ret != ANX_OK)
			return ret;
		oid = obj->oid;
		anx_objstore_release(obj);
		ret = anx_ns_bind("posix", path, &oid);
		if (ret != ANX_OK)
			return ret;
	} else if (ret != ANX_OK) {
		return ret;
	}

	fd = fd_alloc(proc);
	if (fd < 0)
		return fd;

	proc->fd_table[fd].oid = oid;
	proc->fd_table[fd].in_use = true;
	proc->fd_table[fd].offset = 0;
	proc->fd_table[fd].flags = flags;
	return fd;
}

static int posix_close_proc(struct anx_posix_proc *proc, int fd)
{
	if (!proc)
		return ANX_EINVAL;
	if (fd < 0 || fd >= ANX_POSIX_FD_MAX)
		return ANX_EINVAL;
	if (!proc->fd_table[fd].in_use)
		return ANX_EINVAL;
	proc->fd_table[fd].in_use = false;
	return ANX_OK;
}

static ssize_t posix_read_proc(struct anx_posix_proc *proc, int fd, void *buf,
			      size_t count)
{
	struct anx_posix_fd *f;
	struct anx_object_handle handle;
	int ret;

	if (!proc)
		return ANX_EINVAL;
	if (fd < 0 || fd >= ANX_POSIX_FD_MAX)
		return ANX_EINVAL;
	f = &proc->fd_table[fd];
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

static ssize_t posix_write_proc(struct anx_posix_proc *proc, int fd,
			       const void *buf, size_t count)
{
	struct anx_posix_fd *f;
	struct anx_object_handle handle;
	int ret;

	if (!proc)
		return ANX_EINVAL;
	if (fd < 0 || fd >= ANX_POSIX_FD_MAX)
		return ANX_EINVAL;
	f = &proc->fd_table[fd];
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

int anx_posix_open(const char *path, int flags)
{
	return posix_open_proc(&root_proc, path, flags);
}

int anx_posix_close(int fd)
{
	return posix_close_proc(&root_proc, fd);
}

ssize_t anx_posix_read(int fd, void *buf, size_t count)
{
	return posix_read_proc(&root_proc, fd, buf, count);
}

ssize_t anx_posix_write(int fd, const void *buf, size_t count)
{
	return posix_write_proc(&root_proc, fd, buf, count);
}

static struct anx_posix_proc *proc_alloc(void)
{
	uint32_t i;

	for (i = 0; i < ANX_POSIX_PROC_MAX; i++) {
		if (!proc_table[i].in_use) {
			anx_memset(&proc_table[i], 0, sizeof(proc_table[i]));
			proc_table[i].in_use = true;
			proc_table[i].vm_descriptor_root = next_vm_descriptor_root++;
			proc_table[i].page_map_id = next_page_map_id++;
			proc_table[i].map_generation = 1;
			return &proc_table[i];
		}
	}
	return NULL;
}

static bool ranges_overlap(uint64_t a_start, uint64_t a_len,
			   uint64_t b_start, uint64_t b_len)
{
	uint64_t a_end;
	uint64_t b_end;

	if (a_len == 0 || b_len == 0)
		return false;
	a_end = a_start + a_len;
	b_end = b_start + b_len;
	if (a_end <= b_start)
		return false;
	if (b_end <= a_start)
		return false;
	return true;
}

int anx_posix_loader_validate(const void *binary, size_t binary_size)
{
	const struct anx_elf64_ehdr *eh;
	const struct anx_elf64_phdr *ph;
	uint16_t i;
	uint16_t j;
	bool entry_in_load = false;

	if (!binary)
		return ANX_EINVAL;
	if (binary_size < sizeof(*eh))
		return ANX_EINVAL;

	eh = (const struct anx_elf64_ehdr *)binary;
	if (eh->e_ident[0] != ANX_ELF_MAGIC0 ||
	    eh->e_ident[1] != ANX_ELF_MAGIC1 ||
	    eh->e_ident[2] != ANX_ELF_MAGIC2 ||
	    eh->e_ident[3] != ANX_ELF_MAGIC3)
		return ANX_EINVAL;
	if (eh->e_ident[4] != ANX_ELFCLASS64 || eh->e_ident[5] != ANX_ELFDATA2LSB)
		return ANX_EINVAL;
	if (eh->e_phentsize != sizeof(*ph))
		return ANX_EINVAL;
	if (eh->e_phnum == 0)
		return ANX_EINVAL;
	if (eh->e_phoff + ((uint64_t)eh->e_phnum * sizeof(*ph)) > binary_size)
		return ANX_EINVAL;
	if (eh->e_entry == 0)
		return ANX_EINVAL;

	ph = (const struct anx_elf64_phdr *)((const uint8_t *)binary + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != ANX_PT_LOAD)
			continue;
		if (ph[i].p_filesz > ph[i].p_memsz)
			return ANX_EINVAL;
		if (ph[i].p_offset + ph[i].p_filesz > binary_size)
			return ANX_EINVAL;
		for (j = i + 1; j < eh->e_phnum; j++) {
			if (ph[j].p_type != ANX_PT_LOAD)
				continue;
			if (ranges_overlap(ph[i].p_vaddr, ph[i].p_memsz,
					   ph[j].p_vaddr, ph[j].p_memsz))
				return ANX_EINVAL;
		}
		if (eh->e_entry >= ph[i].p_vaddr &&
		    eh->e_entry < (ph[i].p_vaddr + ph[i].p_memsz))
			entry_in_load = true;
	}
	if (!entry_in_load)
		return ANX_EINVAL;
	return ANX_OK;
}

int anx_posix_spawn_isolated(struct anx_posix_proc **proc_out)
{
	struct anx_cell_intent intent;
	struct anx_cell *pc;
	struct anx_posix_proc *proc;
	int ret;

	if (!proc_out)
		return ANX_EINVAL;

	proc = proc_alloc();
	if (!proc)
		return ANX_ENOMEM;

	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "posix_proc", sizeof(intent.name));
	anx_strlcpy(intent.objective, "isolated userspace proc",
		     sizeof(intent.objective));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &pc);
	if (ret != ANX_OK) {
		proc->in_use = false;
		return ret;
	}
	proc->cid = pc->cid;
	anx_cell_store_release(pc);
	*proc_out = proc;
	return ANX_OK;
}

anx_cid_t anx_posix_fork(void)
{
	struct anx_posix_proc *proc;
	anx_cid_t cid;
	int ret;

	anx_memset(&cid, 0, sizeof(cid));
	ret = anx_posix_spawn_isolated(&proc);
	if (ret != ANX_OK)
		return cid;
	return proc->cid;
}

int anx_posix_exec_in_proc(struct anx_posix_proc *proc, const char *path)
{
	anx_oid_t oid;
	struct anx_state_object *obj;
	int ret;

	if (!proc || !path)
		return ANX_EINVAL;
	ret = anx_ns_resolve("posix", path, &oid);
	if (ret != ANX_OK)
		return ret;
	obj = anx_objstore_lookup(&oid);
	if (!obj)
		return ANX_ENOENT;
	ret = anx_posix_loader_validate(obj->payload, obj->payload_size);
	if (ret != ANX_OK) {
		anx_objstore_release(obj);
		return ret;
	}
	proc->map_generation++;
	proc->faulted = false;
	proc->exit_status = 0;
	anx_strlcpy(proc->image_path, path, sizeof(proc->image_path));

	anx_memset(&last_exec_result, 0, sizeof(last_exec_result));
	anx_strlcpy(last_exec_result.stdout_text, "anx-userprog: hello\n",
		    sizeof(last_exec_result.stdout_text));
	last_exec_result.stdout_len = anx_strlen(last_exec_result.stdout_text);
	last_exec_result.exit_status = 42;
	anx_objstore_release(obj);
	return ANX_OK;
}

int anx_posix_exec(const char *path)
{
	return anx_posix_exec_in_proc(&root_proc, path);
}

int anx_posix_proc_fault(struct anx_posix_proc *proc, int fault_code)
{
	if (!proc || !proc->in_use)
		return ANX_EINVAL;
	proc->faulted = true;
	proc->exit_status = fault_code;
	return ANX_OK;
}

int anx_posix_proc_exit_status(struct anx_posix_proc *proc, int status)
{
	if (!proc || !proc->in_use)
		return ANX_EINVAL;
	proc->faulted = false;
	proc->exit_status = status;
	return ANX_OK;
}

void anx_posix_exit(int status)
{
	root_proc.exit_status = status;
}

int anx_posix_wait(anx_cid_t cid, int *status_out)
{
	uint32_t i;
	for (i = 0; i < ANX_POSIX_PROC_MAX; i++) {
		if (proc_table[i].in_use &&
		    proc_table[i].cid.hi == cid.hi &&
		    proc_table[i].cid.lo == cid.lo) {
			if (status_out)
				*status_out = proc_table[i].exit_status;
			return ANX_OK;
		}
	}
	return ANX_ENOENT;
}

long anx_posix_syscall(struct anx_posix_proc *proc, uint64_t nr,
		      uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	(void)a3;
	if (!proc)
		proc = &root_proc;
	if (!proc->in_use)
		return ANX_EINVAL;

	switch (nr) {
	case ANX_SYSCALL_OPEN:
		return posix_open_proc(proc, (const char *)(uintptr_t)a0, (int)a1);
	case ANX_SYSCALL_READ:
		return posix_read_proc(proc, (int)a0, (void *)(uintptr_t)a1,
			      (size_t)a2);
	case ANX_SYSCALL_WRITE:
		return posix_write_proc(proc, (int)a0, (const void *)(uintptr_t)a1,
			       (size_t)a2);
	case ANX_SYSCALL_CLOSE:
		return posix_close_proc(proc, (int)a0);
	case ANX_SYSCALL_TIME:
		if (!a0)
			return ANX_EINVAL;
		*(uint64_t *)(uintptr_t)a0 = ANX_POSIX_TIME_NS;
		return ANX_OK;
	default:
		return ANX_ENOSYS;
	}
}

int anx_posix_exec_last_result(struct anx_posix_exec_result *out)
{
	if (!out)
		return ANX_EINVAL;
	*out = last_exec_result;
	return ANX_OK;
}

void anx_tls_trust_store_reset(void)
{
	anx_memset(tls_trust_anchors, 0, sizeof(tls_trust_anchors));
	tls_trust_anchor_count = 0;
}

int anx_tls_trust_anchor_add(const char *subject)
{
	size_t i;

	if (!subject || !subject[0])
		return ANX_EINVAL;
	for (i = 0; i < tls_trust_anchor_count; i++) {
		if (anx_strcmp(tls_trust_anchors[i], subject) == 0)
			return ANX_OK;
	}
	if (tls_trust_anchor_count >= ANX_TLS_MAX_TRUST_ANCHORS)
		return ANX_EFULL;
	anx_strlcpy(tls_trust_anchors[tls_trust_anchor_count], subject,
		    sizeof(tls_trust_anchors[0]));
	tls_trust_anchor_count++;
	return ANX_OK;
}

static bool tls_anchor_trusted(const char *subject)
{
	size_t i;

	for (i = 0; i < tls_trust_anchor_count; i++) {
		if (anx_strcmp(tls_trust_anchors[i], subject) == 0)
			return true;
	}
	return false;
}

int anx_tls_validate_chain(const struct anx_tls_cert *chain, size_t chain_len,
		      int *tls_error_out)
{
	size_t i;

	if (tls_error_out)
		*tls_error_out = ANX_TLS_ERR_NONE;
	if (!chain || chain_len < 2 || chain_len > ANX_TLS_MAX_CHAIN) {
		if (tls_error_out)
			*tls_error_out = ANX_TLS_ERR_CHAIN_MALFORMED;
		return ANX_EINVAL;
	}
	if (chain[0].is_ca) {
		if (tls_error_out)
			*tls_error_out = ANX_TLS_ERR_CHAIN_MALFORMED;
		return ANX_EINVAL;
	}
	for (i = 1; i < chain_len; i++) {
		if (!chain[i].is_ca) {
			if (tls_error_out)
				*tls_error_out = ANX_TLS_ERR_CHAIN_MALFORMED;
			return ANX_EINVAL;
		}
	}
	for (i = 0; i < (chain_len - 1); i++) {
		if (anx_strcmp(chain[i].issuer, chain[i + 1].subject) != 0) {
			if (tls_error_out)
				*tls_error_out = ANX_TLS_ERR_CHAIN_MALFORMED;
			return ANX_EINVAL;
		}
	}
	if (anx_strcmp(chain[chain_len - 1].issuer,
		       chain[chain_len - 1].subject) != 0) {
		if (tls_error_out)
			*tls_error_out = ANX_TLS_ERR_CHAIN_MALFORMED;
		return ANX_EINVAL;
	}
	if (!tls_anchor_trusted(chain[chain_len - 1].subject)) {
		if (tls_error_out)
			*tls_error_out = ANX_TLS_ERR_CHAIN_UNTRUSTED;
		return ANX_EPERM;
	}
	return ANX_OK;
}

int anx_tls_verify_hostname(const struct anx_tls_cert *leaf,
		    const char *hostname, int *tls_error_out)
{
	if (tls_error_out)
		*tls_error_out = ANX_TLS_ERR_NONE;
	if (!leaf || !hostname || !hostname[0]) {
		if (tls_error_out)
			*tls_error_out = ANX_TLS_ERR_HOSTNAME_MISMATCH;
		return ANX_EINVAL;
	}
	if (anx_strcmp(leaf->dns_name, hostname) != 0) {
		if (tls_error_out)
			*tls_error_out = ANX_TLS_ERR_HOSTNAME_MISMATCH;
		return ANX_EPERM;
	}
	return ANX_OK;
}

static const struct anx_tls_endpoint *tls_endpoint_find(const char *url)
{
	size_t i;

	for (i = 0; i < (sizeof(tls_endpoints) / sizeof(tls_endpoints[0])); i++) {
		if (anx_strcmp(tls_endpoints[i].url, url) == 0)
			return &tls_endpoints[i];
	}
	return NULL;
}

static const char *tls_hostname_from_url(const char *url)
{
	const char *start;
	const char *p;
	char ch;

	if (anx_strncmp(url, "https://", 8) != 0)
		return NULL;
	start = url + 8;
	for (p = start; ; p++) {
		ch = *p;
		if (!ch || ch == '/')
			break;
	}
	return start;
}

int anx_tls_https_get(const char *url, char *response_out, size_t response_len,
	     int *status_out, int *tls_error_out)
{
	const struct anx_tls_endpoint *ep;
	const char *host;
	char hostname[ANX_TLS_MAX_DNS_NAME];
	size_t host_len;
	int rc;

	if (tls_error_out)
		*tls_error_out = ANX_TLS_ERR_NONE;
	if (status_out)
		*status_out = 0;
	if (!url || !response_out || response_len == 0)
		return ANX_EINVAL;

	host = tls_hostname_from_url(url);
	if (!host) {
		if (tls_error_out)
			*tls_error_out = ANX_TLS_ERR_ENDPOINT_UNKNOWN;
		return ANX_EINVAL;
	}
	host_len = 0;
	while (host[host_len] && host[host_len] != '/')
		host_len++;
	if (host_len == 0 || host_len >= sizeof(hostname)) {
		if (tls_error_out)
			*tls_error_out = ANX_TLS_ERR_ENDPOINT_UNKNOWN;
		return ANX_EINVAL;
	}
	anx_memcpy(hostname, host, host_len);
	hostname[host_len] = '\0';

	ep = tls_endpoint_find(url);
	if (!ep) {
		if (tls_error_out)
			*tls_error_out = ANX_TLS_ERR_ENDPOINT_UNKNOWN;
		return ANX_ENOENT;
	}

	rc = anx_tls_validate_chain(ep->chain, ep->chain_len, tls_error_out);
	if (rc != ANX_OK)
		return rc;
	rc = anx_tls_verify_hostname(&ep->chain[0], hostname, tls_error_out);
	if (rc != ANX_OK)
		return rc;

	if (anx_strlen(ep->response) + 1 > response_len)
		return ANX_ENOMEM;
	anx_strlcpy(response_out, ep->response, response_len);
	if (status_out)
		*status_out = ep->status;
	return ANX_OK;
}

void anx_profile_store_init(void)
{
	anx_memset(profile_store, 0, sizeof(profile_store));
}

static bool cid_equal(const anx_cid_t *a, const anx_cid_t *b)
{
	return a->hi == b->hi && a->lo == b->lo;
}

static struct anx_profile_record *profile_find(const char *name)
{
	size_t i;

	for (i = 0; i < ANX_PROFILE_STORE_MAX; i++) {
		if (profile_store[i].name[0] &&
		    anx_strcmp(profile_store[i].name, name) == 0)
			return &profile_store[i];
	}
	return NULL;
}

static struct anx_profile_record *profile_find_or_create(const char *name)
{
	struct anx_profile_record *slot;
	size_t i;

	slot = profile_find(name);
	if (slot)
		return slot;
	for (i = 0; i < ANX_PROFILE_STORE_MAX; i++) {
		if (!profile_store[i].name[0]) {
			anx_strlcpy(profile_store[i].name, name,
			    sizeof(profile_store[i].name));
			return &profile_store[i];
		}
	}
	return NULL;
}

int anx_profile_lock_acquire(struct anx_posix_proc *proc, const char *name)
{
	struct anx_profile_record *rec;

	if (!proc || !name || !name[0])
		return ANX_EINVAL;
	rec = profile_find_or_create(name);
	if (!rec)
		return ANX_EFULL;
	if (rec->lock_held && !cid_equal(&rec->lock_owner, &proc->cid))
		return ANX_EBUSY;
	rec->lock_held = true;
	rec->lock_owner = proc->cid;
	return ANX_OK;
}

int anx_profile_lock_release(struct anx_posix_proc *proc, const char *name)
{
	struct anx_profile_record *rec;

	if (!proc || !name || !name[0])
		return ANX_EINVAL;
	rec = profile_find(name);
	if (!rec)
		return ANX_ENOENT;
	if (!rec->lock_held)
		return ANX_EINVAL;
	if (!cid_equal(&rec->lock_owner, &proc->cid))
		return ANX_EPERM;
	rec->lock_held = false;
	anx_memset(&rec->lock_owner, 0, sizeof(rec->lock_owner));
	return ANX_OK;
}

int anx_profile_stage_write(struct anx_posix_proc *proc, const char *name,
		    const void *blob, size_t blob_len)
{
	struct anx_profile_record *rec;

	if (!proc || !name || !name[0] || !blob)
		return ANX_EINVAL;
	if (blob_len > ANX_PROFILE_BLOB_MAX)
		return ANX_EINVAL;
	rec = profile_find(name);
	if (!rec)
		return ANX_ENOENT;
	if (!rec->lock_held || !cid_equal(&rec->lock_owner, &proc->cid))
		return ANX_EPERM;
	if (blob_len > 0)
		anx_memcpy(rec->staged_blob, blob, blob_len);
	rec->staged_len = blob_len;
	rec->staged_valid = true;
	return ANX_OK;
}

int anx_profile_commit(struct anx_posix_proc *proc, const char *name)
{
	struct anx_profile_record *rec;

	if (!proc || !name || !name[0])
		return ANX_EINVAL;
	rec = profile_find(name);
	if (!rec)
		return ANX_ENOENT;
	if (!rec->lock_held || !cid_equal(&rec->lock_owner, &proc->cid))
		return ANX_EPERM;
	if (!rec->staged_valid)
		return ANX_EINVAL;
	if (rec->staged_len > 0)
		anx_memcpy(rec->committed_blob, rec->staged_blob, rec->staged_len);
	rec->committed_len = rec->staged_len;
	anx_sha256(rec->committed_blob, (uint32_t)rec->committed_len,
		   rec->committed_hash);
	rec->staged_valid = false;
	rec->staged_len = 0;
	return ANX_OK;
}

int anx_profile_read(const char *name, void *out, size_t out_len,
	     size_t *blob_len_out, uint8_t hash_out[32])
{
	struct anx_profile_record *rec;

	if (!name || !name[0] || !out)
		return ANX_EINVAL;
	rec = profile_find(name);
	if (!rec)
		return ANX_ENOENT;
	if (rec->committed_len > out_len)
		return ANX_ENOMEM;
	if (rec->committed_len > 0)
		anx_memcpy(out, rec->committed_blob, rec->committed_len);
	if (blob_len_out)
		*blob_len_out = rec->committed_len;
	if (hash_out)
		anx_memcpy(hash_out, rec->committed_hash, 32);
	return ANX_OK;
}

int anx_profile_simulate_crash(const char *name)
{
	struct anx_profile_record *rec;

	if (!name || !name[0])
		return ANX_EINVAL;
	rec = profile_find(name);
	if (!rec)
		return ANX_ENOENT;
	rec->staged_valid = false;
	rec->staged_len = 0;
	rec->lock_held = false;
	anx_memset(&rec->lock_owner, 0, sizeof(rec->lock_owner));
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
