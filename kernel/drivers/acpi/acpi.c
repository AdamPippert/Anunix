/*
 * acpi.c — ACPI table discovery and MADT parsing.
 *
 * Takes the RSDP from the EFI loader's boot info, or scans the BIOS
 * area for it. Traverses RSDT/XSDT to find tables, and finds the DSDT
 * through the FADT. Parses MADT for CPU count and IOAPIC information.
 */

#include <anx/types.h>
#include <anx/acpi.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* --- ACPI table structures --- */

struct acpi_rsdp {
	char signature[8];	/* "RSD PTR " */
	uint8_t checksum;
	char oem_id[6];
	uint8_t revision;
	uint32_t rsdt_addr;
	/* ACPI 2.0+ fields */
	uint32_t length;
	uint64_t xsdt_addr;
	uint8_t ext_checksum;
	uint8_t reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
	char signature[4];
	uint32_t length;
	uint8_t revision;
	uint8_t checksum;
	char oem_id[6];
	char oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed));

/* MADT (Multiple APIC Description Table) */

struct acpi_madt {
	struct acpi_sdt_header header;
	uint32_t lapic_addr;
	uint32_t flags;		/* bit 0: dual 8259 PICs */
} __attribute__((packed));

/* MADT entry types */
#define MADT_LOCAL_APIC		0
#define MADT_IO_APIC		1
#define MADT_INT_OVERRIDE	2
#define MADT_LOCAL_APIC_64	5

struct madt_entry_header {
	uint8_t type;
	uint8_t length;
} __attribute__((packed));

struct madt_local_apic {
	struct madt_entry_header header;
	uint8_t processor_id;
	uint8_t apic_id;
	uint32_t flags;		/* bit 0: enabled */
} __attribute__((packed));

struct madt_io_apic {
	struct madt_entry_header header;
	uint8_t io_apic_id;
	uint8_t reserved;
	uint32_t address;
	uint32_t gsi_base;
} __attribute__((packed));

/* EFI loader boot info block (kernel/boot/efi/efi_stub.c) */
#define BOOT_INFO_ADDR		0x1000
#define BOOT_INFO_MAGIC		0x414E5846	/* "ANXF" */
#define BOOT_INFO_RSDP		0x1028

/* FADT fields holding the DSDT address */
#define FADT_DSDT_OFF		40
#define FADT_X_DSDT_OFF		140

/* --- State --- */

static struct anx_acpi_info acpi_info;
static const struct acpi_rsdp *g_rsdp;

/* --- RSDP discovery --- */

static bool rsdp_checksum_valid(const struct acpi_rsdp *rsdp)
{
	const uint8_t *p = (const uint8_t *)rsdp;
	uint8_t sum = 0;
	int i;

	for (i = 0; i < 20; i++)
		sum += p[i];
	return sum == 0;
}

static const struct acpi_rsdp *find_rsdp(void)
{
	/*
	 * Scan the EBDA (first KB pointed to by BDA at 0x40E) and
	 * the BIOS ROM area (0xE0000 - 0xFFFFF) for "RSD PTR ".
	 */
	const uint8_t *p;
	const uint8_t *end;

#if defined(__x86_64__)
	/*
	 * The EFI loader copies the RSDP address from the UEFI configuration
	 * table into its boot info block (kernel/boot/efi/efi_stub.c). UEFI
	 * firmware does not place the RSDP in the BIOS area, so on that path
	 * this is the only way to find it. The address must stay inside the
	 * low 4 GiB the loader always maps.
	 */
	{
		uint32_t magic;
		uint64_t addr;

		anx_memcpy(&magic, (const void *)(uintptr_t)BOOT_INFO_ADDR,
			   sizeof(magic));
		anx_memcpy(&addr, (const void *)(uintptr_t)BOOT_INFO_RSDP,
			   sizeof(addr));
		if (magic == BOOT_INFO_MAGIC && addr != 0 && addr < (1ULL << 32)) {
			const struct acpi_rsdp *rsdp =
				(const struct acpi_rsdp *)(uintptr_t)addr;

			if (anx_strncmp(rsdp->signature, "RSD PTR ", 8) == 0 &&
			    rsdp_checksum_valid(rsdp))
				return rsdp;
		}
	}
#endif

	/* BIOS ROM area */
	p = (const uint8_t *)0xE0000ULL;
	end = (const uint8_t *)0xFFFFFULL;

	while (p < end) {
		if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' &&
		    p[3] == ' ' && p[4] == 'P' && p[5] == 'T' &&
		    p[6] == 'R' && p[7] == ' ') {
			const struct acpi_rsdp *rsdp;

			rsdp = (const struct acpi_rsdp *)p;
			if (rsdp_checksum_valid(rsdp))
				return rsdp;
		}
		p += 16;	/* RSDP is always 16-byte aligned */
	}

	return NULL;
}

/* --- Table lookup --- */

static bool sdt_checksum_valid(const struct acpi_sdt_header *hdr)
{
	const uint8_t *p = (const uint8_t *)hdr;
	uint8_t sum = 0;
	uint32_t i;

	for (i = 0; i < hdr->length; i++)
		sum += p[i];
	return sum == 0;
}

/* The index-th table with signature sig; SSDTs share one signature. */
static const struct acpi_sdt_header *find_table(const struct acpi_rsdp *rsdp,
						 const char *sig,
						 uint32_t index)
{
	const struct acpi_sdt_header *root;
	bool wide = rsdp->revision >= 2 && rsdp->xsdt_addr != 0;
	uint32_t entries, i, seen = 0;

	/* XSDT holds 64-bit pointers, RSDT 32-bit ones. */
	root = (const struct acpi_sdt_header *)(uintptr_t)
		(wide ? rsdp->xsdt_addr : (uint64_t)rsdp->rsdt_addr);
	if (!sdt_checksum_valid(root))
		return NULL;
	entries = (root->length - sizeof(*root)) / (wide ? 8u : 4u);

	for (i = 0; i < entries; i++) {
		const uint8_t *slot = (const uint8_t *)root + sizeof(*root) +
				      i * (wide ? 8u : 4u);
		uint64_t addr = 0;
		const struct acpi_sdt_header *hdr;

		anx_memcpy(&addr, slot, wide ? 8u : 4u);
		hdr = (const struct acpi_sdt_header *)(uintptr_t)addr;
		if (anx_strncmp(hdr->signature, sig, 4) != 0)
			continue;
		if (seen++ == index)
			return hdr;
	}

	return NULL;
}

const uint8_t *anx_acpi_find_table(const char *sig, uint32_t index,
				   uint32_t *length)
{
	const struct acpi_sdt_header *hdr;

	if (!g_rsdp || !sig)
		return NULL;

	if (anx_strncmp(sig, "DSDT", 4) == 0) {
		/* The DSDT is not listed in the XSDT; the FADT points to it. */
		const struct acpi_sdt_header *fadt = find_table(g_rsdp, "FACP", 0);
		uint64_t addr = 0;

		if (!fadt || index != 0 || !sdt_checksum_valid(fadt))
			return NULL;
		if (fadt->length >= FADT_X_DSDT_OFF + 8)
			anx_memcpy(&addr, (const uint8_t *)fadt + FADT_X_DSDT_OFF, 8);
		if (addr == 0 && fadt->length >= FADT_DSDT_OFF + 4) {
			uint32_t addr32;

			anx_memcpy(&addr32, (const uint8_t *)fadt + FADT_DSDT_OFF, 4);
			addr = addr32;
		}
		if (addr == 0)
			return NULL;
		hdr = (const struct acpi_sdt_header *)(uintptr_t)addr;
		if (anx_strncmp(hdr->signature, "DSDT", 4) != 0)
			return NULL;
	} else {
		hdr = find_table(g_rsdp, sig, index);
	}

	if (!hdr || !sdt_checksum_valid(hdr))
		return NULL;
	if (length)
		*length = hdr->length;
	return (const uint8_t *)hdr;
}

/* --- MADT parsing --- */

static void parse_madt(const struct acpi_madt *madt)
{
	const uint8_t *p;
	const uint8_t *end;

	acpi_info.lapic_addr = madt->lapic_addr;
	acpi_info.has_8259 = (madt->flags & 1) != 0;

	p = (const uint8_t *)madt + sizeof(struct acpi_madt);
	end = (const uint8_t *)madt + madt->header.length;

	while (p + 2 <= end) {
		const struct madt_entry_header *entry;

		entry = (const struct madt_entry_header *)p;
		if (entry->length < 2 || p + entry->length > end)
			break;

		switch (entry->type) {
		case MADT_LOCAL_APIC: {
			const struct madt_local_apic *lapic;

			lapic = (const struct madt_local_apic *)p;
			if (lapic->flags & 1)	/* enabled */
				acpi_info.cpu_count++;
			break;
		}
		case MADT_IO_APIC: {
			const struct madt_io_apic *ioapic;

			ioapic = (const struct madt_io_apic *)p;
			if (acpi_info.ioapic_count == 0)
				acpi_info.ioapic_addr = ioapic->address;
			acpi_info.ioapic_count++;
			break;
		}
		default:
			break;
		}

		p += entry->length;
	}
}

/* --- Public API --- */

int anx_acpi_init(void)
{
	const struct acpi_rsdp *rsdp;
	const struct acpi_sdt_header *madt_hdr;

	anx_memset(&acpi_info, 0, sizeof(acpi_info));

	rsdp = find_rsdp();
	if (!rsdp) {
		kprintf("acpi: RSDP not found\n");
		return ANX_ENOENT;
	}

	g_rsdp = rsdp;
	acpi_info.acpi_revision = rsdp->revision;
	{
		char oem[7];

		anx_memcpy(oem, rsdp->oem_id, 6);
		oem[6] = '\0';
		kprintf("acpi: revision %u, oem %s\n",
			(uint32_t)rsdp->revision, oem);
	}

	madt_hdr = find_table(rsdp, "APIC", 0);
	if (madt_hdr) {
		parse_madt((const struct acpi_madt *)madt_hdr);
		kprintf("acpi: %u CPUs, %u IOAPICs, LAPIC at 0x%x\n",
			acpi_info.cpu_count,
			acpi_info.ioapic_count,
			acpi_info.lapic_addr);
	} else {
		kprintf("acpi: MADT not found\n");
	}

	acpi_info.valid = true;
	return ANX_OK;
}

const struct anx_acpi_info *anx_acpi_get_info(void)
{
	if (!acpi_info.valid)
		return NULL;
	return &acpi_info;
}
