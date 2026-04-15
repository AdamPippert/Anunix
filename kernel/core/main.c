/*
 * main.c — Anunix kernel entry point.
 *
 * Called by architecture-specific boot code after basic hardware init.
 * This is the architecture-independent starting point for the kernel.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/kprintf.h>
#include <anx/fb.h>
#include <anx/fbcon.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/memplane.h>
#include <anx/engine.h>
#include <anx/route.h>
#include <anx/sched.h>
#include <anx/netplane.h>
#include <anx/capability.h>
#include <anx/engine_lease.h>
#include <anx/hwprobe.h>
#include <anx/model_server.h>
#include <anx/route_feedback.h>
#include <anx/posix.h>
#include <anx/pci.h>
#include <anx/virtio_net.h>
#include <anx/net.h>
#include <anx/splash.h>
#include <anx/shell.h>

#define ANX_VERSION "2026.4.15"

void kernel_main(void)
{
	/* Early hardware init (serial/UART so kprintf works) */
	arch_early_init();

	/* Detect and initialize framebuffer if available */
	{
		struct anx_fb_info fb_info;

		arch_fb_detect(&fb_info);
		if (fb_info.available && anx_fb_init(&fb_info) == ANX_OK) {
			anx_fbcon_init();
			kprintf("framebuffer console: %ux%u @ %ubpp\n",
				fb_info.width, fb_info.height, fb_info.bpp);
		}
	}

	/* Boot splash */
	anx_splash();
	kprintf("  Anunix %s booting\n\n", ANX_VERSION);

	/* Architecture-specific full init (page allocator, etc.) */
	arch_init();

	kprintf("arch init complete\n");

	/* 1. State Object Layer (RFC-0002) */
	anx_objstore_init();
	kprintf("state object layer initialized\n");

	/* 2. Execution Cell Runtime (RFC-0003) */
	anx_cell_store_init();
	kprintf("execution cell runtime initialized\n");

	/* 3. Memory Control Plane (RFC-0004) */
	anx_memplane_init();
	kprintf("memory control plane initialized\n");

	/* 4. Routing Plane + Scheduler (RFC-0005) */
	anx_engine_registry_init();
	anx_route_planner_init();
	anx_sched_init();
	anx_lease_init();
	anx_hwprobe_init();
	anx_msrv_init();
	anx_route_feedback_init();
	kprintf("routing plane and scheduler initialized\n");

	/* 5. Network Plane (RFC-0006) */
	anx_netplane_init();
	kprintf("network plane initialized\n");

	/* 6. Capability Objects (RFC-0007) */
	anx_cap_store_init();
	kprintf("capability store initialized\n");

	/* 7. POSIX compatibility layer */
	anx_posix_init();
	kprintf("posix compatibility layer initialized\n");

	/* 8. PCI bus enumeration */
	anx_pci_init();

	/* 9. Network device and IP stack */
	if (anx_virtio_net_init() == ANX_OK) {
		struct anx_net_config net_cfg;

		net_cfg.ip      = ANX_IP4(10, 0, 2, 15);
		net_cfg.netmask = ANX_IP4(255, 255, 255, 0);
		net_cfg.gateway = ANX_IP4(10, 0, 2, 2);
		net_cfg.dns     = ANX_IP4(10, 0, 2, 3);
		anx_net_stack_init(&net_cfg);
	}

	kprintf("kernel init complete -- all subsystems online\n");

	/* Enter interactive monitor shell */
	anx_shell_run();
}
