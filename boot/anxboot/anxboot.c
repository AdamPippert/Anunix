/*
 * anxboot.c — Anunix UEFI loader.
 *
 * Replaces GRUB on the Anunix UEFI boot path. Loads /boot/anunix.elf
 * from the same ESP we were booted from, fills in a minimal multiboot2
 * info structure (memory map + bootloader name + end tag), exits UEFI
 * boot services, and jumps to the kernel's multiboot2 EFI64 entry
 * with RAX = 0x36d76289, RBX = info pointer.
 *
 * Self-contained — no GNU EFI, no edk2, no libc. Built as a PE/COFF
 * UEFI application via clang -target x86_64-unknown-windows + lld-link.
 */

#include "efi.h"

/* --- Globals (anxboot is single-threaded) --- */
static EFI_SYSTEM_TABLE  *gST;
static EFI_BOOT_SERVICES *gBS;
static EFI_HANDLE         gImageHandle;

/* --- ELF64 minimal --- */
#define ELF_MAGIC   0x464C457FU	/* "\x7FELF" little-endian */
#define ET_EXEC     2
#define ET_DYN      3
#define EM_X86_64   62
#define PT_LOAD     1

typedef struct {
	UINT8  e_ident[16];
	UINT16 e_type;
	UINT16 e_machine;
	UINT32 e_version;
	UINT64 e_entry;
	UINT64 e_phoff;
	UINT64 e_shoff;
	UINT32 e_flags;
	UINT16 e_ehsize;
	UINT16 e_phentsize;
	UINT16 e_phnum;
	UINT16 e_shentsize;
	UINT16 e_shnum;
	UINT16 e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	UINT32 p_type;
	UINT32 p_flags;
	UINT64 p_offset;
	UINT64 p_vaddr;
	UINT64 p_paddr;
	UINT64 p_filesz;
	UINT64 p_memsz;
	UINT64 p_align;
} Elf64_Phdr;

/* --- Multiboot2 info wire format (subset we emit) --- */
#define MB2_BOOTLOADER_MAGIC  0x36d76289U	/* RAX value passed to kernel */

#define MB2_TAG_END           0
#define MB2_TAG_MEMINFO       4
#define MB2_TAG_BOOTLDR_NAME  2
#define MB2_TAG_MMAP          6
#define MB2_TAG_EFI64         12
#define MB2_TAG_EFI_MMAP      17

typedef struct {
	UINT32 type;
	UINT32 size;
} mb2_tag;

typedef struct {
	UINT32 type;
	UINT32 size;
	UINT32 entry_size;
	UINT32 entry_version;
	/* entries follow */
} mb2_tag_mmap;

typedef struct {
	UINT64 base_addr;
	UINT64 length;
	UINT32 type;	/* 1=available, 2=reserved, 3=acpi, 4=nvs, 5=bad */
	UINT32 reserved;
} mb2_mmap_entry;

/* --- Tiny libc: memset/memcpy/strlen/utf8-to-utf16-puts --- */

static void *anx_memset(void *dst, int c, UINTN n)
{
	UINT8 *p = dst;

	while (n--)
		*p++ = (UINT8)c;
	return dst;
}

static void *anx_memcpy(void *dst, const void *src, UINTN n)
{
	UINT8 *d = dst;
	const UINT8 *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

static UINTN anx_strlen8(const CHAR8 *s)
{
	UINTN n = 0;

	while (s[n])
		n++;
	return n;
}

/* Print an ASCII string to ConOut — converts to UCS-2 in a stack buf. */
static void puts8(const CHAR8 *s)
{
	CHAR16 buf[256];
	UINTN i = 0;

	while (s[i] && i < 255) {
		CHAR8 c = s[i];

		if (c == '\n') {
			buf[i] = '\r';
			i++;
			if (i >= 255)
				break;
		}
		buf[i] = (CHAR16)(UINT8)s[i];
		i++;
	}
	buf[i] = 0;
	gST->ConOut->OutputString(gST->ConOut, buf);
}

static void put_hex(UINT64 v)
{
	static const CHAR8 hex[] = "0123456789abcdef";
	CHAR8 buf[19];
	int i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 16; i++)
		buf[2 + i] = hex[(v >> (60 - i * 4)) & 0xf];
	buf[18] = 0;
	puts8(buf);
}

static void put_dec(UINT64 v)
{
	CHAR8 buf[24];
	int i = 23;

	buf[i--] = 0;
	if (v == 0) {
		buf[i--] = '0';
	} else {
		while (v > 0) {
			buf[i--] = '0' + (v % 10);
			v /= 10;
		}
	}
	puts8(&buf[i + 1]);
}

/* --- ELF loading --- */

/*
 * Validate the ELF header without copying anything yet, and report
 * the entry point and the page-aligned [lo, hi) span of all PT_LOAD
 * segments. The actual segment copy happens after ExitBootServices
 * (see place_kernel below) because the kernel target range crosses
 * UEFI-reserved holes (ACPI NVS, BootServicesData) that AllocatePages
 * cannot span. Once Boot Services are down, the kernel owns every
 * byte of physical RAM and we can write wherever we please.
 */
static EFI_STATUS validate_elf(void *elf_buf, UINTN elf_size,
			       EFI_PHYSICAL_ADDRESS *out_entry,
			       UINT64 *out_lo, UINT64 *out_hi)
{
	Elf64_Ehdr *eh = elf_buf;
	Elf64_Phdr *ph;
	UINT16 i;
	UINT64 lo = ~(UINT64)0, hi = 0;

	if (elf_size < sizeof(*eh))
		return EFI_LOAD_ERROR;
	if (*(UINT32 *)eh->e_ident != ELF_MAGIC)
		return EFI_LOAD_ERROR;
	if (eh->e_ident[4] != 2)	/* EI_CLASS == ELFCLASS64 */
		return EFI_LOAD_ERROR;
	if (eh->e_machine != EM_X86_64)
		return EFI_LOAD_ERROR;
	if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
		return EFI_LOAD_ERROR;
	if (eh->e_phoff + (UINTN)eh->e_phnum * eh->e_phentsize > elf_size)
		return EFI_LOAD_ERROR;

	ph = (Elf64_Phdr *)((UINT8 *)elf_buf + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++,
	     ph = (Elf64_Phdr *)((UINT8 *)ph + eh->e_phentsize)) {
		if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
			continue;
		if (ph->p_offset + ph->p_filesz > elf_size)
			return EFI_LOAD_ERROR;
		if (ph->p_paddr < lo)
			lo = ph->p_paddr;
		if (ph->p_paddr + ph->p_memsz > hi)
			hi = ph->p_paddr + ph->p_memsz;
	}
	if (lo == ~(UINT64)0)
		return EFI_LOAD_ERROR;

	*out_entry = eh->e_entry;
	*out_lo    = lo & ~(UINT64)(EFI_PAGE_SIZE - 1);
	*out_hi    = (hi + EFI_PAGE_SIZE - 1) & ~(UINT64)(EFI_PAGE_SIZE - 1);
	return EFI_SUCCESS;
}

/*
 * Post-ExitBootServices kernel placement. Zero the destination range,
 * then for each PT_LOAD copy file bytes from the staging buffer to
 * the segment's physical address. No UEFI services are available at
 * this point — only direct memory operations.
 *
 * Marked noinline + no-stack-protector because we cannot tolerate any
 * implicit Boot Services calls (e.g. stack guard checks would fault).
 */
static void place_kernel(void *elf_buf, UINT64 lo, UINT64 hi)
__attribute__((noinline));

static void place_kernel(void *elf_buf, UINT64 lo, UINT64 hi)
{
	Elf64_Ehdr *eh = elf_buf;
	Elf64_Phdr *ph;
	UINT16 i;

	anx_memset((void *)(UINTN)lo, 0, (UINTN)(hi - lo));

	ph = (Elf64_Phdr *)((UINT8 *)elf_buf + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++,
	     ph = (Elf64_Phdr *)((UINT8 *)ph + eh->e_phentsize)) {
		if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
			continue;
		anx_memcpy((void *)(UINTN)ph->p_paddr,
			   (UINT8 *)elf_buf + ph->p_offset,
			   ph->p_filesz);
	}
}

/* --- Multiboot2 info builder --- */

#define MB2_INFO_PAGES 4	/* 16 KiB headroom for memory map */

/*
 * Build a minimal multiboot2 info structure into a UEFI pool buffer.
 * Layout per the multiboot2 spec:
 *   total_size (u32)
 *   reserved   (u32)
 *   tags (each: type u32, size u32, then payload, padded to 8 bytes)
 *   end tag (type=0, size=8)
 *
 * Tags emitted (minimal — kernel will tolerate missing optional tags):
 *   - bootloader name "anxboot 0.1" (type 2)
 *   - basic memory info (type 4)         — total available memory
 *   - memory map (type 6)                — distilled from EFI map
 *   - end tag (type 0)
 */
static void *build_mb2_info(EFI_MEMORY_DESCRIPTOR *map, UINTN map_size,
			    UINTN desc_size, UINTN *out_size_max)
{
	UINTN cap = MB2_INFO_PAGES * EFI_PAGE_SIZE;
	void *info = NULL;

	if (gBS->AllocatePool(EfiLoaderData, cap, &info) != EFI_SUCCESS)
		return NULL;

	UINT8 *p = (UINT8 *)info;
	anx_memset(p, 0, cap);

	/* Reserve 8 bytes for total_size + reserved (filled at end). */
	UINT32 *total_size_ptr = (UINT32 *)p;
	p += 8;

	/* --- Bootloader name tag (type 2) --- */
	{
		const CHAR8 *name = "anxboot 0.1";
		UINT32 nlen = (UINT32)anx_strlen8(name) + 1;
		UINT32 sz = 8 + nlen;

		mb2_tag *t = (mb2_tag *)p;
		t->type = MB2_TAG_BOOTLDR_NAME;
		t->size = sz;
		anx_memcpy(p + 8, name, nlen);
		p += (sz + 7) & ~7u;
	}

	/* --- Basic memory info tag (type 4) ---
	 * mem_lower (KiB below 1 MiB), mem_upper (KiB above 1 MiB cap).
	 * Approximate from the EFI map. The kernel mostly cares about the
	 * full map (tag 6) anyway. */
	{
		UINT64 lower_kib = 640;	/* common BIOS-era cap */
		UINT64 upper_kib = 0;
		UINTN entries = map_size / desc_size;
		UINTN i;

		for (i = 0; i < entries; i++) {
			EFI_MEMORY_DESCRIPTOR *d =
				(EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * desc_size);
			if (d->Type == EfiConventionalMemory) {
				UINT64 end = d->PhysicalStart +
					     d->NumberOfPages * EFI_PAGE_SIZE;
				if (end > 0x100000ULL && end > upper_kib * 1024)
					upper_kib = (end - 0x100000ULL) / 1024;
			}
		}
		mb2_tag *t = (mb2_tag *)p;
		t->type = MB2_TAG_MEMINFO;
		t->size = 16;
		*(UINT32 *)(p + 8)  = (UINT32)lower_kib;
		*(UINT32 *)(p + 12) = (UINT32)upper_kib;
		p += 16;
	}

	/* --- Memory map tag (type 6) --- */
	{
		UINTN entries = map_size / desc_size;
		mb2_tag_mmap *m = (mb2_tag_mmap *)p;
		mb2_mmap_entry *e = (mb2_mmap_entry *)(p + sizeof(*m));
		UINT32 emitted = 0;
		UINTN i;

		m->type = MB2_TAG_MMAP;
		m->entry_size = sizeof(mb2_mmap_entry);
		m->entry_version = 0;

		for (i = 0; i < entries; i++) {
			EFI_MEMORY_DESCRIPTOR *d =
				(EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * desc_size);
			UINT32 mb_type;

			switch (d->Type) {
			case EfiLoaderCode:
			case EfiLoaderData:
			case EfiBootServicesCode:
			case EfiBootServicesData:
			case EfiConventionalMemory:
				mb_type = 1;	/* available */
				break;
			case EfiACPIReclaimMemory:
				mb_type = 3;
				break;
			case EfiACPIMemoryNVS:
				mb_type = 4;
				break;
			case EfiUnusableMemory:
				mb_type = 5;
				break;
			default:
				mb_type = 2;	/* reserved */
				break;
			}
			e[emitted].base_addr = d->PhysicalStart;
			e[emitted].length    = d->NumberOfPages * EFI_PAGE_SIZE;
			e[emitted].type      = mb_type;
			e[emitted].reserved  = 0;
			emitted++;
		}
		m->size = (UINT32)(sizeof(*m) + emitted * sizeof(mb2_mmap_entry));
		p += (m->size + 7) & ~7u;
	}

	/* --- End tag --- */
	{
		mb2_tag *t = (mb2_tag *)p;
		t->type = MB2_TAG_END;
		t->size = 8;
		p += 8;
	}

	*total_size_ptr = (UINT32)((UINTN)p - (UINTN)info);
	*out_size_max = cap;
	return info;
}

/* --- Kernel handoff (asm) --- */

extern void EFIAPI anx_jump_to_kernel(UINT64 entry, UINT32 magic,
				       UINT64 info_ptr) __attribute__((noreturn));

/* --- efi_main --- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
	EFI_STATUS s;
	EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_GUID sfsp_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_LOADED_IMAGE_PROTOCOL *lip;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfsp;
	EFI_FILE_PROTOCOL *root;
	void *kernel_buf = NULL;
	UINTN kernel_size = 0;
	EFI_PHYSICAL_ADDRESS entry = 0;
	void *mb2_info = NULL;
	UINTN mb2_info_max = 0;
	EFI_MEMORY_DESCRIPTOR *map = NULL;
	UINTN map_size = 0, map_key = 0, desc_size = 0;
	UINT32 desc_ver = 0;
	CHAR16 path[] = u"\\boot\\anunix.elf";

	gImageHandle = ImageHandle;
	gST = SystemTable;
	gBS = SystemTable->BootServices;

	gST->ConOut->ClearScreen(gST->ConOut);
	puts8("anxboot 0.1 -- Anunix UEFI loader\n");

	/* 1. Find the partition we booted from. */
	s = gBS->OpenProtocol(ImageHandle, &lip_guid, (void **)&lip,
			      ImageHandle, NULL,
			      EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR(s)) {
		puts8("anxboot: OpenProtocol(LoadedImage) failed\n");
		return s;
	}

	s = gBS->OpenProtocol(lip->DeviceHandle, &sfsp_guid, (void **)&sfsp,
			      ImageHandle, NULL,
			      EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR(s)) {
		puts8("anxboot: OpenProtocol(SimpleFS) failed\n");
		return s;
	}

	s = sfsp->OpenVolume(sfsp, &root);
	if (EFI_ERROR(s)) {
		puts8("anxboot: OpenVolume failed\n");
		return s;
	}

	/* 2. Read \boot\anunix.elf into a HIGH staging buffer. We use
	 *    AllocateMaxAddress with a 4 GiB ceiling so the staging
	 *    buffer lives well above the kernel's load range
	 *    (0x100000..~0xc282000) — otherwise the post-EBS copy would
	 *    overwrite its own source. */
	{
		EFI_FILE_PROTOCOL *file = NULL;
		EFI_GUID file_info_guid = EFI_FILE_INFO_GUID;
		UINT8 info_buf[512];
		UINTN info_size = sizeof(info_buf);
		EFI_FILE_INFO *info;
		EFI_PHYSICAL_ADDRESS stage = 0xFFFFFFFF;	/* below 4 GiB */

		s = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
		if (EFI_ERROR(s)) {
			puts8("anxboot: \\boot\\anunix.elf open failed\n");
			return s;
		}
		s = file->GetInfo(file, &file_info_guid, &info_size, info_buf);
		if (EFI_ERROR(s)) {
			puts8("anxboot: GetInfo failed\n");
			file->Close(file);
			return s;
		}
		info = (EFI_FILE_INFO *)info_buf;
		kernel_size = info->FileSize;

		s = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
				       EFI_SIZE_TO_PAGES(kernel_size), &stage);
		if (EFI_ERROR(s)) {
			puts8("anxboot: stage AllocatePages failed (");
			put_hex(s);
			puts8(")\n");
			file->Close(file);
			return s;
		}
		kernel_buf = (void *)(UINTN)stage;

		{
			UINTN read_size = kernel_size;

			s = file->Read(file, &read_size, kernel_buf);
			if (EFI_ERROR(s) || read_size != kernel_size) {
				puts8("anxboot: kernel Read failed\n");
				file->Close(file);
				return EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
			}
		}
		file->Close(file);
		puts8("anxboot: kernel staged @ ");
		put_hex(stage);
		puts8(", ");
		put_dec(kernel_size);
		puts8(" bytes\n");
	}

	/* 3. Validate ELF and pull out entry + page-aligned span. */
	UINT64 lo = 0, hi = 0;
	s = validate_elf(kernel_buf, kernel_size, &entry, &lo, &hi);
	if (EFI_ERROR(s)) {
		puts8("anxboot: ELF validation failed\n");
		return s;
	}
	puts8("anxboot: entry=");
	put_hex(entry);
	puts8(" span=");
	put_hex(lo);
	puts8("..");
	put_hex(hi);
	puts8("\n");

	/* 4. Get memory map (twice — once to size it, again after pool
	 *    allocation to capture the AllocatePool entry). The MapKey
	 *    returned by the LAST call is what ExitBootServices accepts. */
	{
		UINTN want = 0;
		EFI_STATUS gs;

		gs = gBS->GetMemoryMap(&want, NULL, &map_key, &desc_size, &desc_ver);
		if (gs != EFI_BUFFER_TOO_SMALL) {
			puts8("anxboot: GetMemoryMap(probe) unexpected\n");
			return gs;
		}
		want += 4 * desc_size;	/* slack for the AllocatePool entry */
		s = gBS->AllocatePool(EfiLoaderData, want, (void **)&map);
		if (EFI_ERROR(s)) {
			puts8("anxboot: AllocatePool(map) failed\n");
			return s;
		}
		map_size = want;
		s = gBS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
		if (EFI_ERROR(s)) {
			puts8("anxboot: GetMemoryMap failed\n");
			return s;
		}
	}

	/* 5. Build multiboot2 info. */
	mb2_info = build_mb2_info(map, map_size, desc_size, &mb2_info_max);
	if (!mb2_info) {
		puts8("anxboot: mb2 info build failed\n");
		return EFI_OUT_OF_RESOURCES;
	}

	/*
	 * Build identity-RWX 0..4 GiB page tables. We stash the PML4
	 * physical address at fixed scratch slot 0x1FF8 so the asm
	 * trampoline can pick it up without a global variable. (A
	 * file-scope global causes a stack-spill regression in efi_main
	 * that blows the UEFI stack guard during the banner puts8.)
	 *
	 * Layout: 6 contiguous pages.
	 *   page 0      PML4
	 *   page 1      PDPT
	 *   pages 2..5  PD0..PD3, each 512 entries of 2 MiB pages
	 */
	{
		EFI_PHYSICAL_ADDRESS pt = 0;
		EFI_STATUS ps = gBS->AllocatePages(AllocateAnyPages,
						   EfiLoaderData, 6, &pt);
		puts8("anxboot: pt-alloc: status=");
		put_hex(ps);
		puts8(" base=");
		put_hex(pt);
		puts8("\n");
		if (!EFI_ERROR(ps)) {
			anx_memset((void *)(UINTN)pt, 0, 6 * EFI_PAGE_SIZE);
			UINT64 *pml4 = (UINT64 *)(UINTN)pt;
			pml4[0] = (pt + EFI_PAGE_SIZE) | 3;
			UINT64 *pdpt = (UINT64 *)(UINTN)(pt + EFI_PAGE_SIZE);
			UINT64 i;
			for (i = 0; i < 4; i++) {
				UINT64 pd_phys = pt + (2 + i) * EFI_PAGE_SIZE;
				pdpt[i] = pd_phys | 3;
				UINT64 *pd = (UINT64 *)(UINTN)pd_phys;
				UINT64 j;
				for (j = 0; j < 512; j++)
					pd[j] = ((i * 512 + j) * 0x200000ULL) | 3 | 0x80;
			}
			*(volatile UINT64 *)(UINTN)0x1FF8 = pt;
			puts8("anxboot: identity-RWX page tables ready\n");
		}
	}

	/* 6. ExitBootServices (re-fetch map if it changed). */
	{
		int tries;

		for (tries = 0; tries < 4; tries++) {
			s = gBS->ExitBootServices(ImageHandle, map_key);
			if (s == EFI_SUCCESS)
				break;
			map_size = mb2_info_max;	/* reuse buffer for refresh */
			(void)gBS->GetMemoryMap(&map_size, map, &map_key,
						 &desc_size, &desc_ver);
		}
		if (s != EFI_SUCCESS)
			return s;	/* console gone — return is moot */
	}

	/* 7. Place kernel segments now that we own the entire address
	 *    space. (Cannot do this before EBS — UEFI's page allocator
	 *    rejects the kernel target range, which crosses ACPI NVS
	 *    holes.) */
	place_kernel(kernel_buf, lo, hi);

	/* 8. Hand off. From here we never return. */
	anx_jump_to_kernel(entry, MB2_BOOTLOADER_MAGIC,
			   (UINT64)(UINTN)mb2_info);
}
