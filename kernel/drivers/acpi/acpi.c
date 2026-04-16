/*
 * acpi.c — ACPI table discovery and MADT parsing.
 *
 * Scans for the RSDP in low memory (BIOS) or reads it from the
 * multiboot info. Traverses RSDT/XSDT to find tables. Parses MADT
 * for CPU count and IOAPIC information.
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

/* --- State --- */

static struct anx_acpi_info acpi_info;

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

static const struct acpi_sdt_header *find_table(const struct acpi_rsdp *rsdp,
						 const char *sig)
{
	uint32_t entries;
	uint32_t i;

	if (rsdp->revision >= 2 && rsdp->xsdt_addr != 0) {
		/* XSDT: 64-bit pointers */
		const struct acpi_sdt_header *xsdt;
		const uint64_t *ptrs;

		xsdt = (const struct acpi_sdt_header *)
			(uintptr_t)rsdp->xsdt_addr;
		if (!sdt_checksum_valid(xsdt))
			return NULL;

		entries = (xsdt->length - sizeof(*xsdt)) / 8;
		ptrs = (const uint64_t *)((const uint8_t *)xsdt +
					  sizeof(*xsdt));

		for (i = 0; i < entries; i++) {
			const struct acpi_sdt_header *hdr;

			hdr = (const struct acpi_sdt_header *)
				(uintptr_t)ptrs[i];
			if (anx_strncmp(hdr->signature, sig, 4) == 0)
				return hdr;
		}
	} else {
		/* RSDT: 32-bit pointers */
		const struct acpi_sdt_header *rsdt;
		const uint32_t *ptrs;

		rsdt = (const struct acpi_sdt_header *)
			(uintptr_t)rsdp->rsdt_addr;
		if (!sdt_checksum_valid(rsdt))
			return NULL;

		entries = (rsdt->length - sizeof(*rsdt)) / 4;
		ptrs = (const uint32_t *)((const uint8_t *)rsdt +
					  sizeof(*rsdt));

		for (i = 0; i < entries; i++) {
			const struct acpi_sdt_header *hdr;

			hdr = (const struct acpi_sdt_header *)
				(uintptr_t)ptrs[i];
			if (anx_strncmp(hdr->signature, sig, 4) == 0)
				return hdr;
		}
	}

	return NULL;
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

	acpi_info.acpi_revision = rsdp->revision;
	{
		char oem[7];

		anx_memcpy(oem, rsdp->oem_id, 6);
		oem[6] = '\0';
		kprintf("acpi: revision %u, oem %s\n",
			(uint32_t)rsdp->revision, oem);
	}

	madt_hdr = find_table(rsdp, "APIC");
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
