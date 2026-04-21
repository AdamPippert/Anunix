/*
 * efi_stub.c — Minimal UEFI stub loader for Anunix.
 *
 * 1. Enumerates ALL GOP handles — activates eDP + HDMI + any expansion cards
 * 2. Finds ACPI RSDP from EFI configuration table
 * 3. Gets memory map, exits boot services (robust retry)
 * 4. Sets up 4 GiB identity-mapped page tables
 * 5. Copies embedded kernel to 0x100000 and jumps to it
 *
 * Boot info is passed at physical address 0x1000 (ANXF magic).
 */

#include "efi_types.h"

void __chkstk(void) {}

#define BOOT_INFO_ADDR		0x1000
#define BOOT_INFO_MAGIC		0x414E5846	/* "ANXF" */

struct anx_gop_mode_entry {
	UINT32	width;
	UINT32	height;
	UINT32	pixel_format;
	UINT32	mode_number;
};

#define ANX_BOOT_GOP_MODES_MAX	16

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
	UINT8	gop_mode_count;		/* 0x1040 */
	UINT8	gop_current_mode;	/* 0x1041 */
	UINT8	_pad2[2];		/* 0x1042 */
	struct anx_gop_mode_entry gop_modes[ANX_BOOT_GOP_MODES_MAX];
};

extern UINT8 _kernel_start[];
extern UINT8 _kernel_end[];

#define KERNEL_LOAD_ADDR	0x100000
#define PML4_ADDR		0x70000
#define PDPT_ADDR		0x71000

static void conout(EFI_SYSTEM_TABLE *ST, const CHAR16 *s)
{
	if (ST->ConOut)
		ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s);
}

/*
 * Activate one GOP handle: find best BGRX8888 mode, call SetMode,
 * record framebuffer in boot info if this is the primary (first) display.
 */
static void setup_one_gop(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
			   struct anx_boot_info *info, int is_primary)
{
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info;
	UINTN info_size;
	UINT32 m, best_mode, best_w, best_h;
	UINT8  mode_count;

	if (!gop || !gop->Mode)
		return;

	best_mode  = gop->Mode->Mode;
	best_w     = 0;
	best_h     = 0;
	mode_count = 0;

	for (m = 0; m < gop->Mode->MaxMode; m++) {
		EFI_STATUS qs = gop->QueryMode(gop, m, &info_size, &mode_info);

		if (qs != EFI_SUCCESS)
			continue;
		if (mode_info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor)
			continue;

		if (is_primary && mode_count < ANX_BOOT_GOP_MODES_MAX) {
			info->gop_modes[mode_count].width        = mode_info->HorizontalResolution;
			info->gop_modes[mode_count].height       = mode_info->VerticalResolution;
			info->gop_modes[mode_count].pixel_format = 1;
			info->gop_modes[mode_count].mode_number  = m;
			mode_count++;
		}

		if (mode_info->HorizontalResolution > best_w ||
		    (mode_info->HorizontalResolution == best_w &&
		     mode_info->VerticalResolution   > best_h)) {
			best_w    = mode_info->HorizontalResolution;
			best_h    = mode_info->VerticalResolution;
			best_mode = m;
		}
	}

	if (best_w > 0 && best_mode != gop->Mode->Mode)
		gop->SetMode(gop, best_mode);
	else if (best_w > 0)
		; /* already in best mode */
	else
		/* No BGRX8888 mode — set mode 0 to at least activate output */
		gop->SetMode(gop, 0);

	if (is_primary) {
		info->gop_mode_count = mode_count;

		for (m = 0; m < mode_count; m++) {
			if (info->gop_modes[m].mode_number == gop->Mode->Mode) {
				info->gop_current_mode = (UINT8)m;
				break;
			}
		}

		info->flags             |= (1 << 12);
		info->framebuffer_addr   = gop->Mode->FrameBufferBase;
		info->framebuffer_pitch  = gop->Mode->Info->PixelsPerScanLine * 4;
		info->framebuffer_width  = gop->Mode->Info->HorizontalResolution;
		info->framebuffer_height = gop->Mode->Info->VerticalResolution;
		info->framebuffer_bpp    = 32;
		info->framebuffer_type   = 1;
	}
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle,
			    EFI_SYSTEM_TABLE *SystemTable)
{
	EFI_BOOT_SERVICES *BS = SystemTable->BootServices;
	struct anx_boot_info *info = (struct anx_boot_info *)BOOT_INFO_ADDR;
	EFI_GUID gop_guid  = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	EFI_GUID acpi_guid = EFI_ACPI_20_TABLE_GUID;
	EFI_STATUS status;
	UINTN map_size, map_alloc, map_key, desc_size;
	UINT32 desc_version;
	EFI_MEMORY_DESCRIPTOR *mmap;
	VOID *mmap_buf;
	UINTN i;

	/* Clear boot info block */
	for (i = 0; i < sizeof(struct anx_boot_info); i++)
		((UINT8 *)info)[i] = 0;
	info->magic = BOOT_INFO_MAGIC;

	conout(SystemTable, (CHAR16 *)L"Anunix EFI stub loading...\r\n");

	/* --- GOP: enumerate ALL handles, activate each display --- */
	conout(SystemTable, (CHAR16 *)L"  GOP: locating displays...\r\n");
	{
		EFI_HANDLE *handles = NULL;
		UINTN       handle_count = 0;

		status = BS->LocateHandleBuffer(ByProtocol, &gop_guid,
						NULL, &handle_count, &handles);

		if (status == EFI_SUCCESS && handle_count > 0) {
			UINTN h;
			/*
			 * Activate every GOP handle so that all connected
			 * outputs (eDP, HDMI expansion cards, etc.) receive
			 * a video signal.  Use handle 0 as the primary
			 * framebuffer for the kernel.
			 */
			for (h = 0; h < handle_count; h++) {
				EFI_GRAPHICS_OUTPUT_PROTOCOL *gop_h = NULL;
				status = BS->HandleProtocol(handles[h],
							    &gop_guid,
							    (VOID **)&gop_h);
				if (status == EFI_SUCCESS)
					setup_one_gop(gop_h, info,
						      (h == 0) ? 1 : 0);
			}
			BS->FreePool(handles);
		} else {
			/* Fallback: single LocateProtocol */
			EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
			status = BS->LocateProtocol(&gop_guid, NULL,
						    (VOID **)&gop);
			if (status == EFI_SUCCESS)
				setup_one_gop(gop, info, 1);
		}
	}
	conout(SystemTable, (CHAR16 *)L"  GOP: done\r\n");

	/* --- ACPI RSDP --- */
	conout(SystemTable, (CHAR16 *)L"  ACPI: searching...\r\n");
	for (i = 0; i < SystemTable->NumberOfTableEntries; i++) {
		EFI_CONFIGURATION_TABLE *t = &SystemTable->ConfigurationTable[i];

		if (t->VendorGuid.Data1 == acpi_guid.Data1 &&
		    t->VendorGuid.Data2 == acpi_guid.Data2 &&
		    t->VendorGuid.Data3 == acpi_guid.Data3) {
			info->acpi_rsdp = (UINT64)t->VendorTable;
			break;
		}
	}
	conout(SystemTable, (CHAR16 *)L"  ACPI: done\r\n");

	/* --- Memory map --- */
	conout(SystemTable, (CHAR16 *)L"  MemMap: allocating...\r\n");
	map_size = 0;
	BS->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_version);
	map_alloc = map_size + 8 * desc_size;	/* generous slack */
	BS->AllocatePool(2 /* EfiLoaderData */, map_alloc, &mmap_buf);
	mmap = (EFI_MEMORY_DESCRIPTOR *)mmap_buf;

	/* --- Exit boot services (up to 3 attempts) --- */
	conout(SystemTable, (CHAR16 *)L"  ExitBootServices...\r\n");
	{
		int attempt;
		for (attempt = 0; attempt < 3; attempt++) {
			UINTN cur = map_alloc;
			status = BS->GetMemoryMap(&cur, mmap, &map_key,
						  &desc_size, &desc_version);
			if (status != EFI_SUCCESS)
				break;
			map_size = cur;
			status = BS->ExitBootServices(ImageHandle, map_key);
			if (status == EFI_SUCCESS)
				break;
			/* Map key changed — retry without allocating */
		}
	}

	if (status != EFI_SUCCESS) {
		/* ConOut may be gone, but try anyway */
		SystemTable->ConOut->OutputString(SystemTable->ConOut,
			(CHAR16 *)L"ExitBootServices failed!\r\n");
		return status;
	}

	/* ===== We own the machine. No more EFI calls. ===== */

	/* Write framebuffer confirmation stripe: top 8 rows = solid blue */
	if (info->framebuffer_addr && info->framebuffer_width) {
		UINT32 *fb  = (UINT32 *)(UINTN)info->framebuffer_addr;
		UINT32  row, col;
		UINT32  pitch_px = info->framebuffer_pitch / 4;

		for (row = 0; row < 8; row++)
			for (col = 0; col < info->framebuffer_width; col++)
				fb[row * pitch_px + col] = 0x000000FFU; /* blue */
	}

	/* Store memory map in boot info */
	info->memory_map      = (UINT64)(UINTN)mmap;
	info->memory_map_size = (UINT32)map_size;
	info->memory_desc_size= (UINT32)desc_size;

	/* Copy kernel to load address */
	{
		UINT64 kernel_size = (UINT64)(_kernel_end - _kernel_start);
		UINT8 *src = _kernel_start;
		UINT8 *dst = (UINT8 *)KERNEL_LOAD_ADDR;

		for (i = 0; i < kernel_size; i++)
			dst[i] = src[i];
	}

	/*
	 * Identity-mapped page tables (4 GiB).
	 * PML4[0] → PDPT; PDPT[0..3] → four 1-GiB pages.
	 */
	{
		UINT64 *pml4 = (UINT64 *)PML4_ADDR;
		UINT64 *pdpt = (UINT64 *)PDPT_ADDR;

		for (i = 0; i < 512; i++) {
			pml4[i] = 0;
			pdpt[i] = 0;
		}

		pml4[0] = PDPT_ADDR | 0x3;
		pdpt[0] = 0x0000000000000083ULL;
		pdpt[1] = 0x0000000040000083ULL;
		pdpt[2] = 0x0000000080000083ULL;
		pdpt[3] = 0x00000000C0000083ULL;

		__asm__ volatile("mov %0, %%cr3" : : "r"((UINT64)PML4_ADDR));
	}

	/* Jump to kernel */
	{
		typedef void (*kernel_entry_t)(void);
		kernel_entry_t entry = (kernel_entry_t)KERNEL_LOAD_ADDR;
		entry();
	}

	for (;;)
		__asm__ volatile("hlt");

	return EFI_SUCCESS;
}
