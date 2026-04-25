#include <anx/cell.h>
#include <anx/errno.h>
#include <anx/hwprobe.h>
#include <anx/pci.h>
#include <anx/list.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include "../lib/libanx/libanx.h"

/*
 * hwd — hardware discovery agent
 *
 * Probes hardware via deterministic tool cells (PCI, ACPI, device tree),
 * synthesizes a structured hw-profile/v1 State Object via a model cell,
 * generates driver stubs for unknown device classes, and posts results
 * to superrouter via anx-fetch.
 *
 * See RFC-0011 for full specification.
 *
 * Subcommands:
 *   hwd                   full discovery (boot mode)
 *   hwd rescan            re-probe and diff
 *   hwd show [--json]     print current profile
 *   hwd stubs list        list generated stubs
 *   hwd stubs show <oid>  dump stub source
 *   hwd push [--dry-run]  push profile to superrouter
 *   hwd status            cell lifecycle state of last run
 *   hwd trace <cid>       full execution trace
 */

/* ── Output helpers ─────────────────────────────────────────────────── */

static void print_oid(const anx_oid_t *oid)
{
	char buf[64];
	anx_fmt_oid(buf, sizeof(buf), oid);
	kprintf("%s", buf);
}

/* ── hwd show ───────────────────────────────────────────────────────── */

static int cmd_show(int json)
{
	const struct anx_hw_inventory *inv = anx_hwprobe_get();
	uint32_t i;

	if (!inv) {
		kprintf("hwd: hardware inventory not available\n");
		return ANX_EIO;
	}

	if (json) {
		kprintf("{\"hw-profile\":\"v1\",");
		kprintf("\"cpu_count\":%u,", inv->cpu_count);
		kprintf("\"ram_mb\":%llu,",
			(unsigned long long)(inv->ram_bytes / (1024ULL * 1024ULL)));
		kprintf("\"accelerators\":[");
		for (i = 0; i < inv->accel_count; i++) {
			const struct anx_hw_accel *a = &inv->accels[i];
			if (i) kprintf(",");
			kprintf("{\"name\":\"%s\",\"compute_units\":%u,\"mem_mb\":%llu}",
				a->name, a->compute_units,
				(unsigned long long)(a->mem_bytes / (1024ULL * 1024ULL)));
		}
		kprintf("]}\n");
	} else {
		kprintf("hw-profile v1\n");
		kprintf("  CPUs       : %u\n", inv->cpu_count);
		kprintf("  RAM        : %llu MB\n",
			(unsigned long long)(inv->ram_bytes / (1024ULL * 1024ULL)));
		kprintf("  Accelerators: %u\n", inv->accel_count);
		for (i = 0; i < inv->accel_count; i++) {
			const struct anx_hw_accel *a = &inv->accels[i];
			kprintf("    [%u] %s  %u CUs  %llu MB\n",
				i, a->name, a->compute_units,
				(unsigned long long)(a->mem_bytes / (1024ULL * 1024ULL)));
		}
	}
	return ANX_OK;
}

/* ── hwd (full discovery) ───────────────────────────────────────────── */

static int cmd_discover(void)
{
	kprintf("hwd: probing hardware...\n");
	anx_hwprobe_init();

	/* PCI scan */
	if (anx_pci_init() == ANX_OK) {
		struct anx_list_head *devs = anx_pci_device_list();
		if (devs) {
			struct anx_list_head *pos;
			ANX_LIST_FOR_EACH(pos, devs) {
				struct anx_pci_device *dev =
					ANX_CONTAINER_OF(pos,
					    struct anx_pci_device, link);
				const char *cls = anx_pci_class_name(
					dev->class_code, dev->subclass);
				kprintf("  pci %04x:%04x  %s\n",
					dev->vendor_id, dev->device_id,
					cls ? cls : "unknown");
			}
		}
	}

	return cmd_show(0);
}

/* ── hwd status ─────────────────────────────────────────────────────── */

static int cmd_status(void)
{
	/* Report the lifecycle state of the last hwd run cell */
	const struct anx_hw_inventory *inv = anx_hwprobe_get();
	kprintf("hwd: last probe status: %s\n",
		inv ? "completed" : "not-run");
	return ANX_OK;
}

/* ── main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	if (argc < 2)
		return cmd_discover();

	if (anx_strcmp(argv[1], "show") == 0) {
		int json = (argc >= 3 && anx_strcmp(argv[2], "--json") == 0);
		return cmd_show(json);
	}

	if (anx_strcmp(argv[1], "rescan") == 0) {
		kprintf("hwd: rescanning hardware...\n");
		return cmd_discover();
	}

	if (anx_strcmp(argv[1], "status") == 0)
		return cmd_status();

	if (anx_strcmp(argv[1], "stubs") == 0) {
		kprintf("hwd: stub generation not yet implemented\n");
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "push") == 0) {
		kprintf("hwd: superrouter push not yet implemented\n");
		return ANX_OK;
	}

	kprintf("hwd: unknown subcommand: %s\n", argv[1]);
	kprintf("usage: hwd [show [--json] | rescan | status | stubs | push]\n");
	return ANX_EINVAL;
}
