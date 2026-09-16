/*
 * anx/acpi.h — ACPI table parsing.
 *
 * Discovers and parses ACPI tables from firmware (RSDP → RSDT/XSDT).
 * Extracts CPU topology from MADT and basic system info from FADT.
 */

#ifndef ANX_ACPI_H
#define ANX_ACPI_H

#include <anx/types.h>

/* Parsed ACPI system info */
struct anx_acpi_info {
	uint32_t cpu_count;		/* number of local APIC entries */
	uint32_t ioapic_count;
	uint32_t ioapic_addr;		/* IOAPIC base address */
	bool     has_8259;		/* dual 8259 PICs present */
	uint32_t lapic_addr;		/* local APIC base address */
	uint8_t  acpi_revision;		/* ACPI revision (2+ = XSDT) */
	bool     valid;
};

/* Scan for ACPI tables and parse MADT */
int anx_acpi_init(void);

/* Get parsed ACPI info */
const struct anx_acpi_info *anx_acpi_get_info(void);

/*
 * Find a checksummed ACPI table by signature, for example "SSDT". index
 * selects among tables that share a signature. "DSDT" is found through the
 * FADT and accepts only index 0. Returns the table, header included, or
 * NULL. Valid after anx_acpi_init().
 */
/*
 * Restart the machine through the FADT reset register.
 * Returns ANX_ENOTSUP when firmware does not offer one; on success the
 * machine resets and this never returns.
 */
int anx_acpi_reset(void);

const uint8_t *anx_acpi_find_table(const char *sig, uint32_t index,
				   uint32_t *length);

#endif /* ANX_ACPI_H */
