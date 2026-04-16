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

#endif /* ANX_ACPI_H */
