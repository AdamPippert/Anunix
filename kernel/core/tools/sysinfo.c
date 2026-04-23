/*
 * sysinfo.c — Unified system information display.
 *
 * Consolidates hardware, memory, network, and subsystem status
 * into a single overview.
 *
 * USAGE
 *   sysinfo              Full system overview
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/arch.h>
#include <anx/acpi.h>
#include <anx/pci.h>
#include <anx/page.h>
#include <anx/net.h>
#include <anx/virtio_net.h>
#include <anx/virtio_blk.h>
#include <anx/sched.h>
#include <anx/interface_plane.h>
#include <anx/kprintf.h>
#include <anx/list.h>

void cmd_sysinfo(int argc, char **argv)
{
	const struct anx_acpi_info *acpi;
	uint64_t total_pages, free_pages;

	(void)argc;
	(void)argv;

	kprintf("\n=== Anunix System Information ===\n\n");

	/* CPU */
	acpi = anx_acpi_get_info();
	if (acpi && acpi->valid) {
		kprintf("CPU:       %u cores\n", acpi->cpu_count);
		kprintf("LAPIC:     0x%x\n", acpi->lapic_addr);
		kprintf("IOAPICs:   %u\n", acpi->ioapic_count);
	}

	/* Memory */
	anx_page_stats(&total_pages, &free_pages);
	kprintf("Memory:    %u KiB free / %u KiB total (%u%% used)\n",
		(uint32_t)(free_pages * 4),
		(uint32_t)(total_pages * 4),
		total_pages > 0 ?
		(uint32_t)((total_pages - free_pages) * 100 / total_pages) : 0);

	/* Storage */
	if (anx_blk_ready()) {
		kprintf("Disk:      %u MiB (virtio-blk)\n",
			(uint32_t)(anx_blk_capacity() * 512 /
				   (1024 * 1024)));
	} else {
		kprintf("Disk:      none\n");
	}

	/* Network */
	if (anx_virtio_net_ready()) {
		uint8_t mac[6];

		anx_virtio_net_mac(mac);
		kprintf("Network:   %x:%x:%x:%x:%x:%x",
			(uint32_t)mac[0], (uint32_t)mac[1],
			(uint32_t)mac[2], (uint32_t)mac[3],
			(uint32_t)mac[4], (uint32_t)mac[5]);
		kprintf(" (ip %u.%u.%u.%u)\n",
			(anx_ipv4_local_ip() >> 24) & 0xFF,
			(anx_ipv4_local_ip() >> 16) & 0xFF,
			(anx_ipv4_local_ip() >> 8) & 0xFF,
			anx_ipv4_local_ip() & 0xFF);
	} else {
		kprintf("Network:   none\n");
	}

	/* PCI */
	{
		struct anx_list_head *pos;
		struct anx_list_head *list = anx_pci_device_list();
		uint32_t pci_count = 0;

		ANX_LIST_FOR_EACH(pos, list)
			pci_count++;
		kprintf("PCI:       %u devices\n", pci_count);
	}

	/* Scheduler */
	kprintf("Scheduler: int=%u bg=%u lat=%u batch=%u\n",
		anx_sched_queue_depth(ANX_QUEUE_INTERACTIVE),
		anx_sched_queue_depth(ANX_QUEUE_BACKGROUND),
		anx_sched_queue_depth(ANX_QUEUE_LATENCY_SENSITIVE),
		anx_sched_queue_depth(ANX_QUEUE_BATCH));

	/* Event queue telemetry */
	{
		struct anx_iface_event_stats es;
		uint32_t thresh = anx_iface_event_backpressure_threshold();

		anx_iface_event_stats_full(&es);
		kprintf("Event Queue: depth=%u/%u drops=%u threshold=%u%%\n",
			es.current_depth, ANX_IFACE_EVENT_RING_SIZE,
			(uint32_t)es.overflow_drops,
			(thresh * 100 + 128) / 256);
		kprintf("  Latency: <1ms=%u 1-5ms=%u 5-10ms=%u >10ms=%u\n",
			(uint32_t)es.latency_histogram[0],
			(uint32_t)es.latency_histogram[1],
			(uint32_t)es.latency_histogram[2],
			(uint32_t)es.latency_histogram[3]);
	}

	kprintf("\n");
}
