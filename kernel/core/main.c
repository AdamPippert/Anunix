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
#include <anx/acpi.h>
#include <anx/string.h>
#include <anx/credential.h>
#include <anx/auth.h>
#include <anx/virtio_net.h>
#include <anx/virtio_blk.h>
#include <anx/net.h>
#include <anx/splash.h>
#include <anx/perf.h>
#include <anx/gui.h>
#include <anx/shell.h>

#define ANX_VERSION "2026.4.16"

/*
 * Emergency framebuffer write — draw a colored bar directly to
 * the multiboot2 framebuffer before any subsystem is initialized.
 * Used to prove the kernel is running on hardware with no serial.
 */
extern uint32_t _mb_magic;
extern uint64_t _mb_info;

static void early_fb_debug(uint32_t color, uint32_t row)
{
	uint8_t *ptr, *end;
	uint32_t total_size;
	uint64_t fb_addr = 0;
	uint32_t fb_pitch = 0;
	uint32_t fb_width = 0;
	uint32_t x, y;
	uint32_t *pixel;

	if (_mb_magic != 0x36d76289 || _mb_info == 0)
		return;

	/* Parse multiboot2 tags for framebuffer */
	ptr = (uint8_t *)(uintptr_t)_mb_info;
	total_size = *(uint32_t *)ptr;
	end = ptr + total_size;
	ptr += 8;

	while (ptr + 8 <= end) {
		uint32_t tag_type = *(uint32_t *)ptr;
		uint32_t tag_size = *(uint32_t *)(ptr + 4);

		if (tag_type == 0)
			break;
		if (tag_type == 8 && tag_size >= 32) {
			fb_addr  = *(uint64_t *)(ptr + 8);
			fb_pitch = *(uint32_t *)(ptr + 16);
			fb_width = *(uint32_t *)(ptr + 20);
			break;
		}
		ptr += (tag_size + 7) & ~7u;
	}

	if (fb_addr == 0 || fb_width == 0)
		return;

	/* Draw a colored bar at the specified row */
	for (y = row * 20; y < row * 20 + 20; y++) {
		pixel = (uint32_t *)(uintptr_t)(fb_addr + y * fb_pitch);
		for (x = 0; x < fb_width && x < 400; x++)
			pixel[x] = color;
	}
}

void kernel_main(void)
{
	/* Immediate visual debug — proves kernel is running on bare metal */
	early_fb_debug(0x00FF0000, 0);	/* red bar: kernel_main entered */

	/* Early hardware init (serial/UART so kprintf works) */
	arch_early_init();

	early_fb_debug(0x0000FF00, 1);	/* green bar: serial init done */

	/* Detect and initialize framebuffer if available */
	{
		struct anx_fb_info fb_info;

		arch_fb_detect(&fb_info);

		early_fb_debug(0x000000FF, 2);	/* blue bar: fb detect done */

		if (fb_info.available && anx_fb_init(&fb_info) == ANX_OK) {
			anx_fbcon_init();
			kprintf("framebuffer console: %ux%u @ %ubpp\n",
				fb_info.width, fb_info.height, fb_info.bpp);
		}

		early_fb_debug(0x00FFFF00, 3);	/* yellow bar: fb init done */
	}

	/* Architecture-specific full init (page allocator, etc.) */
	PERF_BEGIN("arch_init");
	arch_init();
	PERF_END();

	/* Boot splash (after arch_init so page allocator is ready for JPEG) */
	PERF_BEGIN("splash");
	anx_splash();
	PERF_END();

	/* Initialize graphical environment (after splash, before text output) */
	if (anx_fb_available()) {
		PERF_BEGIN("gui_init");
		anx_gui_init();
		PERF_END();
	}

	kprintf("  Anunix %s booting\n\n", ANX_VERSION);

	kprintf("arch init complete\n");

	/* 1. State Object Layer (RFC-0002) */
	PERF_BEGIN("objstore_init");
	anx_objstore_init();
	PERF_END();
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

	/* 8. Authentication + Credential store (RFC-0008) */
	anx_auth_init();
	anx_credstore_init();

	/* Parse boot command line for credentials: cred:name=value */
	{
		const char *cmdline = arch_boot_cmdline();

		if (cmdline) {
			const char *p = cmdline;

			while (*p) {
				/* Look for "tz=" prefix (e.g., tz=-7) */
				if (p[0] == 't' && p[1] == 'z' &&
				    p[2] == '=') {
					const char *v = p + 3;
					int32_t offset = 0;
					bool neg = false;

					if (*v == '-') {
						neg = true;
						v++;
					} else if (*v == '+') {
						v++;
					}
					while (*v >= '0' && *v <= '9') {
						offset = offset * 10 +
							 (*v - '0');
						v++;
					}
					if (neg) offset = -offset;
					anx_gui_set_tz_offset(offset);
					kprintf("timezone: UTC%s%d\n",
						offset >= 0 ? "+" : "",
						(int)offset);
					p = v;
					continue;
				}

				/* Look for "cred:" prefix */
				if (p[0] == 'c' && p[1] == 'r' &&
				    p[2] == 'e' && p[3] == 'd' &&
				    p[4] == ':') {
					const char *name_start = p + 5;
					const char *eq = name_start;
					const char *val;
					const char *end;
					char name[128];
					uint32_t nlen, vlen;

					while (*eq && *eq != '=' && *eq != ' ')
						eq++;
					if (*eq != '=')
						goto next_token;
					val = eq + 1;
					end = val;
					while (*end && *end != ' ')
						end++;

					nlen = (uint32_t)(eq - name_start);
					vlen = (uint32_t)(end - val);
					if (nlen > 0 && nlen < 128 && vlen > 0) {
						anx_memcpy(name, name_start, nlen);
						name[nlen] = '\0';
						anx_credential_create(name,
							ANX_CRED_API_KEY,
							val, vlen);
					}
					p = end;
					continue;
				}
			next_token:
				while (*p && *p != ' ')
					p++;
				while (*p == ' ')
					p++;
			}
		}
	}

	kprintf("credential store initialized\n");

	/* 9. Hardware discovery */
	PERF_BEGIN("acpi_init");
	anx_acpi_init();
	PERF_END();
	PERF_BEGIN("pci_init");
	anx_pci_init();
	PERF_END();

	/* 10. Block device */
	PERF_BEGIN("virtio_blk_init");
	anx_virtio_blk_init();
	PERF_END();

	/* 11. Network device and IP stack */
	if (anx_virtio_net_init() == ANX_OK) {
		struct anx_net_config net_cfg;

		/* Initialize minimal stack for DHCP */
		anx_arp_init();
		anx_udp_init();

		/* Try DHCP first (5s timeout) */
		PERF_BEGIN("dhcp_discover");
		kprintf("dhcp: discovering...\n");
		if (anx_dhcp_discover(&net_cfg) != ANX_OK) {
			/* Fall back to QEMU user-mode defaults */
			net_cfg.ip      = ANX_IP4(10, 0, 2, 15);
			net_cfg.netmask = ANX_IP4(255, 255, 255, 0);
			net_cfg.gateway = ANX_IP4(10, 0, 2, 2);
			net_cfg.dns     = ANX_IP4(10, 0, 2, 3);
			kprintf("dhcp: timeout, using static 10.0.2.15\n");
		}
		PERF_END();
		PERF_BEGIN("net_stack_init");
		anx_net_stack_init(&net_cfg);
		PERF_END();
	}

	kprintf("kernel init complete -- all subsystems online\n");

	/* Show boot performance profile */
	anx_perf_report();

	/* Enter interactive monitor shell */
	anx_shell_run();
}
