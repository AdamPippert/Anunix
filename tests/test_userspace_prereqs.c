/*
 * test_userspace_prereqs.c — P0-001/P0-002 acceptance tests.
 */

#include <anx/types.h>
#include <anx/posix.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/cell.h>
#include <anx/string.h>
#include <anx/crypto.h>

#define ASSERT(cond, code) do { if (!(cond)) return (code); } while (0)

struct test_elf64_ehdr {
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

struct test_elf64_phdr {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

static void build_valid_elf(uint8_t *buf, size_t size)
{
	struct test_elf64_ehdr *eh;
	struct test_elf64_phdr *ph;

	anx_memset(buf, 0, size);
	eh = (struct test_elf64_ehdr *)buf;
	eh->e_ident[0] = 0x7f;
	eh->e_ident[1] = 'E';
	eh->e_ident[2] = 'L';
	eh->e_ident[3] = 'F';
	eh->e_ident[4] = 2;
	eh->e_ident[5] = 1;
	eh->e_phoff = sizeof(*eh);
	eh->e_phentsize = sizeof(*ph);
	eh->e_phnum = 1;
	eh->e_entry = 0x400000;

	ph = (struct test_elf64_phdr *)(buf + eh->e_phoff);
	ph->p_type = 1;
	ph->p_offset = 0x80;
	ph->p_vaddr = 0x400000;
	ph->p_filesz = 16;
	ph->p_memsz = 16;
	buf[0x80] = 0x90;
}

static int write_binary_fixture(const char *path, const uint8_t *bin, size_t len)
{
	int fd;
	ssize_t n;
	int ret;

	fd = anx_posix_open(path, ANX_O_CREAT | ANX_O_RDWR);
	if (fd < 0)
		return fd;
	n = anx_posix_write(fd, bin, len);
	if (n != (ssize_t)len)
		return -900;
	ret = anx_posix_close(fd);
	if (ret != ANX_OK)
		return ret;
	return ANX_OK;
}

static int setup_fixture(void)
{
	anx_objstore_init();
	anx_ns_init();
	anx_cell_store_init();
	anx_posix_init();
	return ANX_OK;
}

/* P0-001-U01 */
static int test_isolate_vm_descriptors(void)
{
	struct anx_posix_proc *proc_a;
	struct anx_posix_proc *proc_b;
	int rc;

	rc = anx_posix_spawn_isolated(&proc_a);
	ASSERT(rc == ANX_OK, -100);
	rc = anx_posix_spawn_isolated(&proc_b);
	ASSERT(rc == ANX_OK, -101);

	ASSERT(proc_a->vm_descriptor_root != 0, -102);
	ASSERT(proc_b->vm_descriptor_root != 0, -103);
	ASSERT(proc_a->page_map_id != 0, -104);
	ASSERT(proc_b->page_map_id != 0, -105);
	ASSERT(proc_a->vm_descriptor_root != proc_b->vm_descriptor_root, -106);
	ASSERT(proc_a->page_map_id != proc_b->page_map_id, -107);
	return 0;
}

/* P0-001-U02 */
static int test_sibling_integrity_after_fault(void)
{
	struct anx_posix_proc *proc_a;
	struct anx_posix_proc *proc_b;
	uint64_t map_generation_b;
	int rc;

	rc = anx_posix_spawn_isolated(&proc_a);
	ASSERT(rc == ANX_OK, -110);
	rc = anx_posix_spawn_isolated(&proc_b);
	ASSERT(rc == ANX_OK, -111);
	map_generation_b = proc_b->map_generation;

	rc = anx_posix_proc_fault(proc_a, -77);
	ASSERT(rc == ANX_OK, -112);
	ASSERT(proc_a->faulted, -113);
	ASSERT(proc_a->exit_status == -77, -114);
	ASSERT(!proc_b->faulted, -115);
	ASSERT(proc_b->map_generation == map_generation_b, -116);
	return 0;
}

/* P0-001-I01 */
static int test_two_process_lifecycle(void)
{
	uint8_t elf_bin[256];
	struct anx_posix_proc *proc_a;
	struct anx_posix_proc *proc_b;
	int status_a;
	int status_b;
	int rc;

	build_valid_elf(elf_bin, sizeof(elf_bin));
	rc = write_binary_fixture("/bin/p0-lifecycle", elf_bin, sizeof(elf_bin));
	ASSERT(rc == ANX_OK, -120);

	rc = anx_posix_spawn_isolated(&proc_a);
	ASSERT(rc == ANX_OK, -121);
	rc = anx_posix_spawn_isolated(&proc_b);
	ASSERT(rc == ANX_OK, -122);

	rc = anx_posix_exec_in_proc(proc_a, "/bin/p0-lifecycle");
	ASSERT(rc == ANX_OK, -123);
	rc = anx_posix_exec_in_proc(proc_b, "/bin/p0-lifecycle");
	ASSERT(rc == ANX_OK, -124);

	rc = anx_posix_proc_exit_status(proc_a, 11);
	ASSERT(rc == ANX_OK, -125);
	rc = anx_posix_proc_exit_status(proc_b, 22);
	ASSERT(rc == ANX_OK, -126);

	rc = anx_posix_wait(proc_a->cid, &status_a);
	ASSERT(rc == ANX_OK, -127);
	rc = anx_posix_wait(proc_b->cid, &status_b);
	ASSERT(rc == ANX_OK, -128);
	ASSERT(status_a == 11, -129);
	ASSERT(status_b == 22, -130);
	return 0;
}

/* P0-002-U01 */
static int test_reject_invalid_binary_matrix(void)
{
	uint8_t elf_bin[256];
	struct test_elf64_ehdr *eh;
	struct test_elf64_phdr *ph;
	int rc;

	build_valid_elf(elf_bin, sizeof(elf_bin));
	rc = anx_posix_loader_validate(elf_bin, sizeof(elf_bin));
	ASSERT(rc == ANX_OK, -200);

	elf_bin[0] = 0x00;
	rc = anx_posix_loader_validate(elf_bin, sizeof(elf_bin));
	ASSERT(rc == ANX_EINVAL, -201);

	build_valid_elf(elf_bin, sizeof(elf_bin));
	eh = (struct test_elf64_ehdr *)elf_bin;
	eh->e_entry = 0x500000;
	rc = anx_posix_loader_validate(elf_bin, sizeof(elf_bin));
	ASSERT(rc == ANX_EINVAL, -202);

	build_valid_elf(elf_bin, sizeof(elf_bin));
	eh = (struct test_elf64_ehdr *)elf_bin;
	eh->e_phnum = 2;
	ph = (struct test_elf64_phdr *)(elf_bin + eh->e_phoff);
	ph[0].p_type = 1;
	ph[0].p_offset = 0x80;
	ph[0].p_vaddr = 0x400000;
	ph[0].p_filesz = 16;
	ph[0].p_memsz = 0x100;
	ph[1].p_type = 1;
	ph[1].p_offset = 0x90;
	ph[1].p_vaddr = 0x400080;
	ph[1].p_filesz = 16;
	ph[1].p_memsz = 0x80;
	rc = anx_posix_loader_validate(elf_bin, sizeof(elf_bin));
	ASSERT(rc == ANX_EINVAL, -203);
	return 0;
}

/* P0-002-U02 */
static int test_syscall_table_bounds(void)
{
	struct anx_posix_proc *proc;
	long rc;
	int ret;

	ret = anx_posix_spawn_isolated(&proc);
	ASSERT(ret == ANX_OK, -210);
	rc = anx_posix_syscall(proc, 999, 0, 0, 0, 0);
	ASSERT(rc == ANX_ENOSYS, -211);
	return 0;
}

/* P0-002-I01 */
static int test_static_userprog_run(void)
{
	uint8_t elf_bin[256];
	struct anx_posix_proc *proc;
	struct anx_posix_exec_result result;
	int rc;

	build_valid_elf(elf_bin, sizeof(elf_bin));
	rc = write_binary_fixture("/bin/p0-userprog", elf_bin, sizeof(elf_bin));
	ASSERT(rc == ANX_OK, -220);
	rc = anx_posix_spawn_isolated(&proc);
	ASSERT(rc == ANX_OK, -221);
	rc = anx_posix_exec_in_proc(proc, "/bin/p0-userprog");
	ASSERT(rc == ANX_OK, -222);
	rc = anx_posix_exec_last_result(&result);
	ASSERT(rc == ANX_OK, -223);
	ASSERT(result.exit_status == 42, -224);
	ASSERT(result.stdout_len == anx_strlen("anx-userprog: hello\n"), -225);
	ASSERT(anx_strcmp(result.stdout_text, "anx-userprog: hello\n") == 0, -226);
	return 0;
}

/* P0-002-I02 */
static int test_abi_smoke(void)
{
	struct anx_posix_proc *proc;
	char write_buf[] = "abi-smoke";
	char read_buf[16];
	long fd;
	long rc;
	uint64_t ts = 0;
	int ret;

	ret = anx_posix_spawn_isolated(&proc);
	ASSERT(ret == ANX_OK, -230);
	fd = anx_posix_syscall(proc, ANX_SYSCALL_OPEN,
			      (uint64_t)(uintptr_t)"/tmp/abi-smoke.txt",
			      ANX_O_CREAT | ANX_O_RDWR, 0, 0);
	ASSERT(fd >= 0, -231);
	rc = anx_posix_syscall(proc, ANX_SYSCALL_WRITE, (uint64_t)fd,
			      (uint64_t)(uintptr_t)write_buf,
			      anx_strlen(write_buf), 0);
	ASSERT(rc == (long)anx_strlen(write_buf), -232);
	rc = anx_posix_syscall(proc, ANX_SYSCALL_CLOSE, (uint64_t)fd, 0, 0, 0);
	ASSERT(rc == ANX_OK, -233);

	fd = anx_posix_syscall(proc, ANX_SYSCALL_OPEN,
			      (uint64_t)(uintptr_t)"/tmp/abi-smoke.txt",
			      ANX_O_RDONLY, 0, 0);
	ASSERT(fd >= 0, -234);
	anx_memset(read_buf, 0, sizeof(read_buf));
	rc = anx_posix_syscall(proc, ANX_SYSCALL_READ, (uint64_t)fd,
			      (uint64_t)(uintptr_t)read_buf,
			      anx_strlen(write_buf), 0);
	ASSERT(rc == (long)anx_strlen(write_buf), -235);
	ASSERT(anx_strcmp(read_buf, write_buf) == 0, -236);
	rc = anx_posix_syscall(proc, ANX_SYSCALL_CLOSE, (uint64_t)fd, 0, 0, 0);
	ASSERT(rc == ANX_OK, -237);

	rc = anx_posix_syscall(proc, ANX_SYSCALL_TIME,
			      (uint64_t)(uintptr_t)&ts, 0, 0, 0);
	ASSERT(rc == ANX_OK, -238);
	ASSERT(ts == 123456789ULL, -239);
	return 0;
}

/* P0-005-U01 */
static int test_cert_chain_validation(void)
{
	struct anx_tls_cert valid_chain[3];
	struct anx_tls_cert malformed_chain[3];
	struct anx_tls_cert untrusted_chain[3];
	int tls_err;
	int rc;

	anx_tls_trust_store_reset();
	rc = anx_tls_trust_anchor_add("ANX Root CA");
	ASSERT(rc == ANX_OK, -300);

	anx_memset(valid_chain, 0, sizeof(valid_chain));
	anx_strlcpy(valid_chain[0].subject, "good.anx.test", sizeof(valid_chain[0].subject));
	anx_strlcpy(valid_chain[0].issuer, "ANX Intermediate CA", sizeof(valid_chain[0].issuer));
	anx_strlcpy(valid_chain[0].dns_name, "good.anx.test", sizeof(valid_chain[0].dns_name));
	valid_chain[1].is_ca = true;
	anx_strlcpy(valid_chain[1].subject, "ANX Intermediate CA", sizeof(valid_chain[1].subject));
	anx_strlcpy(valid_chain[1].issuer, "ANX Root CA", sizeof(valid_chain[1].issuer));
	valid_chain[2].is_ca = true;
	anx_strlcpy(valid_chain[2].subject, "ANX Root CA", sizeof(valid_chain[2].subject));
	anx_strlcpy(valid_chain[2].issuer, "ANX Root CA", sizeof(valid_chain[2].issuer));

	tls_err = -1;
	rc = anx_tls_validate_chain(valid_chain, 3, &tls_err);
	ASSERT(rc == ANX_OK, -301);
	ASSERT(tls_err == ANX_TLS_ERR_NONE, -302);

	malformed_chain[0] = valid_chain[0];
	malformed_chain[1] = valid_chain[1];
	malformed_chain[2] = valid_chain[2];
	anx_strlcpy(malformed_chain[0].issuer, "Wrong Issuer", sizeof(malformed_chain[0].issuer));
	tls_err = -1;
	rc = anx_tls_validate_chain(malformed_chain, 3, &tls_err);
	ASSERT(rc == ANX_EINVAL, -303);
	ASSERT(tls_err == ANX_TLS_ERR_CHAIN_MALFORMED, -304);

	untrusted_chain[0] = valid_chain[0];
	untrusted_chain[1] = valid_chain[1];
	untrusted_chain[2] = valid_chain[2];
	anx_strlcpy(untrusted_chain[2].subject, "Unknown Root", sizeof(untrusted_chain[2].subject));
	anx_strlcpy(untrusted_chain[2].issuer, "Unknown Root", sizeof(untrusted_chain[2].issuer));
	anx_strlcpy(untrusted_chain[1].issuer, "Unknown Root", sizeof(untrusted_chain[1].issuer));
	tls_err = -1;
	rc = anx_tls_validate_chain(untrusted_chain, 3, &tls_err);
	ASSERT(rc == ANX_EPERM, -305);
	ASSERT(tls_err == ANX_TLS_ERR_CHAIN_UNTRUSTED, -306);
	return 0;
}

/* P0-005-U02 */
static int test_hostname_verification(void)
{
	struct anx_tls_cert leaf;
	int tls_err;
	int rc;

	anx_memset(&leaf, 0, sizeof(leaf));
	anx_strlcpy(leaf.dns_name, "good.anx.test", sizeof(leaf.dns_name));

	tls_err = -1;
	rc = anx_tls_verify_hostname(&leaf, "good.anx.test", &tls_err);
	ASSERT(rc == ANX_OK, -310);
	ASSERT(tls_err == ANX_TLS_ERR_NONE, -311);

	tls_err = -1;
	rc = anx_tls_verify_hostname(&leaf, "wrong.anx.test", &tls_err);
	ASSERT(rc == ANX_EPERM, -312);
	ASSERT(tls_err == ANX_TLS_ERR_HOSTNAME_MISMATCH, -313);
	return 0;
}

/* P0-005-I01 */
static int test_https_success_path(void)
{
	char response[64];
	int status;
	int tls_err;
	int rc;

	anx_tls_trust_store_reset();
	rc = anx_tls_trust_anchor_add("ANX Root CA");
	ASSERT(rc == ANX_OK, -320);
	anx_memset(response, 0, sizeof(response));
	status = 0;
	tls_err = -1;
	rc = anx_tls_https_get("https://good.anx.test/", response, sizeof(response),
			       &status, &tls_err);
	ASSERT(rc == ANX_OK, -321);
	ASSERT(status == 200, -322);
	ASSERT(tls_err == ANX_TLS_ERR_NONE, -323);
	ASSERT(anx_strcmp(response, "ok:https-good") == 0, -324);
	return 0;
}

/* P0-005-I02 */
static int test_https_invalid_cert_path(void)
{
	char response[64];
	int status;
	int tls_err;
	int rc;

	anx_tls_trust_store_reset();
	rc = anx_tls_trust_anchor_add("ANX Root CA");
	ASSERT(rc == ANX_OK, -330);
	anx_memset(response, 0, sizeof(response));
	status = 0;
	tls_err = -1;
	rc = anx_tls_https_get("https://badcert.anx.test/", response, sizeof(response),
			       &status, &tls_err);
	ASSERT(rc == ANX_EPERM, -331);
	ASSERT(tls_err == ANX_TLS_ERR_CHAIN_UNTRUSTED, -332);
	ASSERT(status == 0, -333);
	return 0;
}

/* P0-006-U01 */
static int test_atomic_commit(void)
{
	struct anx_posix_proc *proc;
	char out[64];
	size_t out_len;
	int rc;

	rc = anx_posix_spawn_isolated(&proc);
	ASSERT(rc == ANX_OK, -340);
	rc = anx_profile_lock_acquire(proc, "profile.atomic");
	ASSERT(rc == ANX_OK, -341);
	rc = anx_profile_stage_write(proc, "profile.atomic", "v1-profile", 10);
	ASSERT(rc == ANX_OK, -342);
	rc = anx_profile_commit(proc, "profile.atomic");
	ASSERT(rc == ANX_OK, -343);
	rc = anx_profile_lock_release(proc, "profile.atomic");
	ASSERT(rc == ANX_OK, -344);

	rc = anx_profile_lock_acquire(proc, "profile.atomic");
	ASSERT(rc == ANX_OK, -345);
	rc = anx_profile_stage_write(proc, "profile.atomic", "v2-partial", 10);
	ASSERT(rc == ANX_OK, -346);
	rc = anx_profile_simulate_crash("profile.atomic");
	ASSERT(rc == ANX_OK, -347);

	anx_memset(out, 0, sizeof(out));
	out_len = 0;
	rc = anx_profile_read("profile.atomic", out, sizeof(out), &out_len, NULL);
	ASSERT(rc == ANX_OK, -348);
	ASSERT(out_len == 10, -349);
	ASSERT(anx_memcmp(out, "v1-profile", 10) == 0, -350);
	return 0;
}

/* P0-006-U02 */
static int test_lock_contention(void)
{
	struct anx_posix_proc *proc_a;
	struct anx_posix_proc *proc_b;
	int rc;

	rc = anx_posix_spawn_isolated(&proc_a);
	ASSERT(rc == ANX_OK, -360);
	rc = anx_posix_spawn_isolated(&proc_b);
	ASSERT(rc == ANX_OK, -361);

	rc = anx_profile_lock_acquire(proc_a, "profile.lock");
	ASSERT(rc == ANX_OK, -362);
	rc = anx_profile_lock_acquire(proc_b, "profile.lock");
	ASSERT(rc == ANX_EBUSY, -363);
	rc = anx_profile_lock_release(proc_a, "profile.lock");
	ASSERT(rc == ANX_OK, -364);
	rc = anx_profile_lock_acquire(proc_b, "profile.lock");
	ASSERT(rc == ANX_OK, -365);
	rc = anx_profile_lock_release(proc_b, "profile.lock");
	ASSERT(rc == ANX_OK, -366);
	return 0;
}

/* P0-006-I01 */
static int test_crash_recovery_profile(void)
{
	struct anx_posix_proc *proc;
	char out[64];
	size_t out_len;
	uint8_t hash[32];
	uint8_t expected_hash[32];
	int rc;

	rc = anx_posix_spawn_isolated(&proc);
	ASSERT(rc == ANX_OK, -370);
	rc = anx_profile_lock_acquire(proc, "profile.recovery");
	ASSERT(rc == ANX_OK, -371);
	rc = anx_profile_stage_write(proc, "profile.recovery", "state-v1", 8);
	ASSERT(rc == ANX_OK, -372);
	rc = anx_profile_commit(proc, "profile.recovery");
	ASSERT(rc == ANX_OK, -373);
	rc = anx_profile_lock_release(proc, "profile.recovery");
	ASSERT(rc == ANX_OK, -374);

	rc = anx_profile_lock_acquire(proc, "profile.recovery");
	ASSERT(rc == ANX_OK, -375);
	rc = anx_profile_stage_write(proc, "profile.recovery", "state-v2-partial", 16);
	ASSERT(rc == ANX_OK, -376);
	rc = anx_profile_simulate_crash("profile.recovery");
	ASSERT(rc == ANX_OK, -377);

	anx_memset(out, 0, sizeof(out));
	anx_memset(hash, 0, sizeof(hash));
	out_len = 0;
	rc = anx_profile_read("profile.recovery", out, sizeof(out), &out_len, hash);
	ASSERT(rc == ANX_OK, -378);
	ASSERT(out_len == 8, -379);
	ASSERT(anx_memcmp(out, "state-v1", 8) == 0, -380);
	anx_sha256("state-v1", 8, expected_hash);
	ASSERT(anx_memcmp(hash, expected_hash, 32) == 0, -381);
	return 0;
}

int test_userspace_prereqs(void)
{
	int rc;

	rc = setup_fixture();
	if (rc != ANX_OK)
		return -1;
	rc = test_isolate_vm_descriptors();
	if (rc != 0)
		return rc;
	rc = test_sibling_integrity_after_fault();
	if (rc != 0)
		return rc;
	rc = test_two_process_lifecycle();
	if (rc != 0)
		return rc;
	rc = test_reject_invalid_binary_matrix();
	if (rc != 0)
		return rc;
	rc = test_syscall_table_bounds();
	if (rc != 0)
		return rc;
	rc = test_static_userprog_run();
	if (rc != 0)
		return rc;
	rc = test_abi_smoke();
	if (rc != 0)
		return rc;
	rc = test_cert_chain_validation();
	if (rc != 0)
		return rc;
	rc = test_hostname_verification();
	if (rc != 0)
		return rc;
	rc = test_https_success_path();
	if (rc != 0)
		return rc;
	rc = test_https_invalid_cert_path();
	if (rc != 0)
		return rc;
	rc = test_atomic_commit();
	if (rc != 0)
		return rc;
	rc = test_lock_contention();
	if (rc != 0)
		return rc;
	rc = test_crash_recovery_profile();
	if (rc != 0)
		return rc;
	return 0;
}
