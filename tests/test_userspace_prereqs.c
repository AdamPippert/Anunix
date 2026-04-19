/*
 * test_userspace_prereqs.c — P0-001/P0-002 acceptance tests.
 */

#include <anx/types.h>
#include <anx/posix.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/cell.h>
#include <anx/string.h>

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
	return 0;
}
