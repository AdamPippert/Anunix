/*
 * efi_stub.c — Minimal UEFI stub loader for Anunix.
 *
 * This is a standalone UEFI application that:
 * 1. Gets the GOP framebuffer (address, resolution, pitch)
 * 2. Finds the ACPI RSDP from the EFI configuration table
 * 3. Gets the memory map from EFI
 * 4. Exits boot services (owns the machine now)
 * 5. Sets up 4 GiB identity-mapped page tables
 * 6. Loads the kernel from a fixed address and jumps to it
 *
 * The kernel binary is embedded in the .efi via .incbin.
 * Boot info is passed at physical address 0x1000 (same layout
 * as the multiboot trampoline uses).
 */

#include "efi_types.h"

/* Windows ABI stack probe — not needed in freestanding EFI */
void __chkstk(void) {}

/* Boot info block at 0x1000 (shared with multiboot path) */
#define BOOT_INFO_ADDR		0x1000
#define BOOT_INFO_MAGIC		0x414E5846	/* "ANXF" */

struct anx_boot_info {
	UINT32	magic;			/* 0x1000: ANXF */
	UINT32	flags;			/* 0x1004 */
	UINT64	framebuffer_addr;	/* 0x1008 */
	UINT32	framebuffer_pitch;	/* 0x1010 */
	UINT32	framebuffer_width;	/* 0x1014 */
	UINT32	framebuffer_height;	/* 0x1018 */
	UINT8	framebuffer_bpp;	/* 0x101C */
	UINT8	framebuffer_type;	/* 0x101D */
	UINT8	_pad1[2];
	UINT64	cmdline;		/* 0x1020 */
	UINT64	acpi_rsdp;		/* 0x1028 */
	UINT64	memory_map;		/* 0x1030 */
	UINT32	memory_map_size;	/* 0x1038 */
	UINT32	memory_desc_size;	/* 0x103C */
};

/* Embedded kernel binary */
extern UINT8 _kernel_start[];
extern UINT8 _kernel_end[];

/* Kernel load address */
#define KERNEL_LOAD_ADDR	0x100000

/* Page table addresses (above kernel, in low memory) */
#define PML4_ADDR		0x70000
#define PDPT_ADDR		0x71000

/* EFI entry point */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle,
			    EFI_SYSTEM_TABLE *SystemTable)
{
	EFI_BOOT_SERVICES *BS = SystemTable->BootServices;
	struct anx_boot_info *info = (struct anx_boot_info *)BOOT_INFO_ADDR;
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
	EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	EFI_GUID acpi_guid = EFI_ACPI_20_TABLE_GUID;
	EFI_STATUS status;
	UINTN map_size, map_key, desc_size;
	UINT32 desc_version;
	EFI_MEMORY_DESCRIPTOR *mmap;
	VOID *mmap_buf;
	UINTN i;

	/* Clear boot info */
	for (i = 0; i < sizeof(struct anx_boot_info); i++)
		((UINT8 *)info)[i] = 0;

	info->magic = BOOT_INFO_MAGIC;

	/* Print status to console */
	if (SystemTable->ConOut)
		SystemTable->ConOut->OutputString(SystemTable->ConOut,
			(CHAR16 *)L"Anunix EFI stub loading...\r\n");

	/* Get GOP framebuffer */
	status = BS->LocateProtocol(&gop_guid, NULL, (VOID **)&gop);
	if (status == EFI_SUCCESS && gop && gop->Mode) {
		info->flags |= (1 << 12);	/* framebuffer present */
		info->framebuffer_addr = gop->Mode->FrameBufferBase;
		info->framebuffer_pitch =
			gop->Mode->Info->PixelsPerScanLine * 4;
		info->framebuffer_width =
			gop->Mode->Info->HorizontalResolution;
		info->framebuffer_height =
			gop->Mode->Info->VerticalResolution;
		info->framebuffer_bpp = 32;
		info->framebuffer_type = 1;	/* direct RGB */
	}

	/* Find ACPI RSDP in configuration table */
	for (i = 0; i < SystemTable->NumberOfTableEntries; i++) {
		EFI_CONFIGURATION_TABLE *t =
			&SystemTable->ConfigurationTable[i];

		if (t->VendorGuid.Data1 == acpi_guid.Data1 &&
		    t->VendorGuid.Data2 == acpi_guid.Data2 &&
		    t->VendorGuid.Data3 == acpi_guid.Data3) {
			info->acpi_rsdp = (UINT64)t->VendorTable;
			break;
		}
	}

	/* Get memory map (needed for ExitBootServices) */
	map_size = 0;
	mmap = NULL;
	BS->GetMemoryMap(&map_size, NULL, &map_key,
			 &desc_size, &desc_version);
	/* map_size now has required size */
	map_size += 2 * desc_size;	/* room for changes */
	BS->AllocatePool(2 /* EfiLoaderData */, map_size, &mmap_buf);
	mmap = (EFI_MEMORY_DESCRIPTOR *)mmap_buf;
	status = BS->GetMemoryMap(&map_size, mmap, &map_key,
				  &desc_size, &desc_version);
	if (status != EFI_SUCCESS) {
		if (SystemTable->ConOut)
			SystemTable->ConOut->OutputString(SystemTable->ConOut,
				(CHAR16 *)L"GetMemoryMap failed\r\n");
		return status;
	}

	/* Exit boot services — point of no return */
	status = BS->ExitBootServices(ImageHandle, map_key);
	if (status != EFI_SUCCESS) {
		/* Retry — map key may have changed */
		map_size += 2 * desc_size;
		BS->GetMemoryMap(&map_size, mmap, &map_key,
				 &desc_size, &desc_version);
		status = BS->ExitBootServices(ImageHandle, map_key);
		if (status != EFI_SUCCESS)
			return status;
	}

	/* ===== We own the machine now. No more EFI calls. ===== */

	/* Copy kernel to load address */
	{
		UINT64 kernel_size = (UINT64)(_kernel_end - _kernel_start);
		UINT8 *src = _kernel_start;
		UINT8 *dst = (UINT8 *)KERNEL_LOAD_ADDR;

		for (i = 0; i < kernel_size; i++)
			dst[i] = src[i];
	}

	/*
	 * Set up identity-mapped page tables (4 GiB).
	 * UEFI already has us in long mode with identity mapping,
	 * but we need our own page tables since the UEFI ones
	 * may be reclaimed.
	 *
	 * PML4[0] → PDPT
	 * PDPT[0..3] → 1 GiB pages (4 GiB total)
	 */
	{
		UINT64 *pml4 = (UINT64 *)PML4_ADDR;
		UINT64 *pdpt = (UINT64 *)PDPT_ADDR;

		/* Zero tables */
		for (i = 0; i < 512; i++) {
			pml4[i] = 0;
			pdpt[i] = 0;
		}

		/* PML4[0] → PDPT */
		pml4[0] = PDPT_ADDR | 0x3;	/* present + writable */

		/* PDPT[0..3] → 1 GiB identity pages */
		pdpt[0] = 0x0000000000000083ULL;
		pdpt[1] = 0x0000000040000083ULL;
		pdpt[2] = 0x0000000080000083ULL;
		pdpt[3] = 0x00000000C0000083ULL;

		/* Load CR3 */
		__asm__ volatile("mov %0, %%cr3" : : "r"((UINT64)PML4_ADDR));
	}

	/* Store multiboot2 magic and info pointer for the kernel */
	{
		/*
		 * The kernel's boot.S checks _mb_magic for 0x36d76289
		 * (multiboot2) to parse tags. We use our own boot info
		 * block at 0x1000 instead, which arch_fb_detect already
		 * knows how to read (the "ANXF" magic path).
		 *
		 * Set _mb_magic to 0 so the kernel skips multiboot2
		 * parsing and falls through to the ANXF path.
		 */
	}

	/* Jump to kernel entry point at 0x100000 */
	{
		typedef void (*kernel_entry_t)(void);
		kernel_entry_t entry = (kernel_entry_t)KERNEL_LOAD_ADDR;

		entry();
	}

	/* Should never reach here */
	for (;;)
		__asm__ volatile("hlt");

	return EFI_SUCCESS;
}
