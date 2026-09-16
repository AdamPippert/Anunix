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
#include <anx/namespace.h>
#include <anx/acpi.h>
#include <anx/string.h>
#include <anx/credential.h>
#include <anx_boot_secrets.h>
#include <anx/auth.h>
#include <anx/blk.h>
#include <anx/md.h>
#include <anx/part.h>
#include <anx/blk_probe.h>
#include <anx/objstore_disk.h>
#include <anx/driver_table.h>
#include <anx/mt7925.h>
#include <anx/xdna.h>
#include <anx/net.h>
#include <anx/splash.h>
#include <anx/perf.h>
#include <anx/gui.h>
#include <anx/interface_plane.h>
#include <anx/usb_mouse.h>
#include <anx/i2c_input.h>
#include <anx/shell.h>
#include <anx/vm.h>
#include <anx/wm.h>
#include <anx/workflow.h>
#include <anx/workflow_library.h>
#include <anx/theme.h>
#include <anx/kickstart.h>
#include <anx/httpd.h>
#include <anx/sshd.h>
#include <anx/jepa.h>
#include <anx/loop.h>
#include <anx/ebm.h>
#include <anx/memory.h>
#include <anx/installer.h>
#include <anx/model_client.h>
#include <anx/tools.h>
#include <anx/bootlog.h>
#include <anx/audio.h>
#include <anx/video.h>
#include <anx/twin.h>
#include <anx/regime.h>

/*
 * Partition scan, RAID assembly, store mount and the boot-log ring.
 *
 * The driver probe calls this as soon as the storage drivers have run and
 * before any network driver starts, so the boot log is on disk before code
 * that might hang or fault. A crash in a network driver used to leave no
 * record at all, because the log was only claimed after every driver had
 * returned. Runs once; later calls do nothing.
 */
void anx_drivers_storage_done(void)
{
	static bool done;

	if (done)
		return;
	done = true;

	/* 10b-2. Partition scan (RFC-0031) — register a block device per GPT
	 * entry on every drive, so Anunix can live in a partition beside
	 * another operating system. Runs before RAID assembly so an array
	 * can take partitions as members. */
	PERF_BEGIN("part_scan");
	anx_part_scan_all();
	PERF_END();

	/* 10c. Software RAID — assemble arrays from member superblocks.
	 * An assembled array takes over as the active block device, so the
	 * object store below mounts from the array, not from one member. */
	PERF_BEGIN("md_init");
	anx_md_init();
	PERF_END();

	if (anx_blk_ready()) {
		int ds_ret;

		/*
		 * Pick the device carrying our superblock rather than
		 * whichever registered first (RFC-0031 section 4). Without
		 * this the active device on a partitioned machine is
		 * typically the firmware's EFI system partition.
		 */
		anx_disk_select_store();

		ds_ret = anx_disk_store_init();

		if (ds_ret != ANX_OK) {
			/*
			 * Do not format. This path used to write a fresh
			 * object store over the active device whenever it
			 * failed to find one, reading nothing first -- which
			 * destroyed the partition table of any disk Anunix
			 * was booted beside (RFC-0031 section 8). Running
			 * without a store is always recoverable; formatting
			 * someone's disk is not.
			 */
			char what[ANX_PROBE_DESC_MAX];

			(void)anx_blk_probe(anx_blk_active(), what,
					    sizeof(what));
			kprintf("disk: no object store on %s (holds %s)\n",
				anx_blk_active_name(), what);
			kprintf("disk: running without persistence; "
				"install to create a store\n");
		}
		if (ds_ret == ANX_OK) {
			kprintf("disk: object store mounted\n");
			anx_bootlog_disk_init();
		}

		/*
		 * The ring does not depend on the store having mounted, so it
		 * is claimed either way. This is the copy that survives when
		 * the rest of the chain does not.
		 */
		if (anx_bootlog_ring_init() == ANX_OK) {
			int fr = anx_bootlog_ring_flush();

			if (fr != ANX_OK)
				kprintf("bootlog: ring flush failed (%d)\n", fr);
			else
				kprintf("bootlog: ring flush ok\n");
		}
	}

}

void kernel_main(void)
{
	/* Boot session ring buffer — must be first, before any kprintf output */
	anx_bootlog_early_init();

	/* Early hardware init (serial/UART so kprintf works) */
	arch_early_init();

	/* Detect and initialize framebuffer if available */
	{
		struct anx_fb_info fb_info;

		arch_fb_detect(&fb_info);

		if (fb_info.available && anx_fb_init(&fb_info) == ANX_OK) {
			anx_fbcon_init();
			kprintf("framebuffer console: %ux%u @ %ubpp addr=%llx pitch=%u\n",
				fb_info.width, fb_info.height, fb_info.bpp,
				(unsigned long long)fb_info.addr, fb_info.pitch);
		}
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
	anx_ns_init();
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

	/* 6b. Sinks (RFC-0028 Protected Operation ABI) */
	anx_sink_registry_init();

	/* 6c. Resource Twin + Regime Detector (RFC-0029) */
	anx_twin_init();
	anx_regime_init();

	/* 7a. Interface Plane (RFC-0012) — after engine registry */
	if (anx_iface_init() == ANX_OK) {
		anx_renderer_headless_register();
		if (anx_fb_available())
			anx_renderer_gpu_register();
		anx_iface_env_define("visual-desktop",
		                      "anx:env/visual-desktop/v1",
		                      ANX_ENGINE_RENDERER_GPU);
		anx_iface_env_define("headless-agent",
		                      "anx:env/headless-agent/v1",
		                      ANX_ENGINE_RENDERER_HEADLESS);
		if (anx_fb_available()) {
			anx_iface_env_activate("visual-desktop");
			anx_iface_compositor_start("visual-desktop");
			anx_iface_frame_scheduler_init(30);
		} else {
			anx_iface_env_activate("headless-agent");
			anx_iface_compositor_start("headless-agent");
		}
		kprintf("interface plane initialized\n");
	}

	/* 7b. USB HID mouse (non-fatal if no controller found) */
	anx_usb_mouse_init();

	/* 8. POSIX compatibility layer */
	anx_posix_init();
	kprintf("posix compatibility layer initialized\n");

	/* 8. Authentication + Credential store (RFC-0008) */
	anx_auth_init();
	anx_credstore_init();

	/* Parse boot command line for credentials: cred:name=value, and flags */
	bool install_mode = false;
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

				/* Look for "install" flag */
				if (p[0] == 'i' && p[1] == 'n' &&
				    p[2] == 's' && p[3] == 't' &&
				    p[4] == 'a' && p[5] == 'l' &&
				    p[6] == 'l' &&
				    (p[7] == ' ' || p[7] == '\0')) {
					install_mode = true;
					p += 7;
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

	/* 10. Driver probe — storage, network, accelerators */
	PERF_BEGIN("drivers_probe");
	anx_drivers_probe();
	PERF_END();

	/* 10a. HID-over-I2C input (touchpads). ACPI describes these, not
	 * PCI, so the driver table cannot probe them. Non-fatal. */
	PERF_BEGIN("i2c_input_init");
	anx_i2c_input_init();
	PERF_END();

	/* 10b. Audio subsystem — initializes the mixer and probes hardware
	 * audio sinks (HDA on x86_64 / arm64-with-PCIe-HDA, Apple Silicon
	 * native on M1/M2 once codec drivers land).  Always succeeds: if
	 * no HW backend binds, the null/capture/wav sinks remain. */
	PERF_BEGIN("audio_init");
	anx_audio_init();
	anx_video_init();
	PERF_END();

	/* Normally already done from inside the driver probe; see below. */
	anx_drivers_storage_done();

	/* Load previously persisted PAL state before hardware priming so that
	 * organic session data from prior boots is not overwritten by priming */
	anx_pal_persist_load();
	anx_credstore_load();
	anx_model_client_load();
	anx_uobj_load();

	/* Prime PAL cross-session priors from detected hardware */
	anx_pal_prime_hardware();

	/* 11. Workflow Objects (RFC-0018) + built-in template library
	 * Must come before JEPA: anx_jepa_init() registers agent workflows
	 * via anx_wf_create(), which requires wf_initialized == true. */
	anx_wf_init();
	anx_wf_lib_init();
	kprintf("workflow subsystem initialized\n");

	/* 12. AI accelerators (non-fatal — hardware may not be present)
	 * JEPA now runs after workflow init so jepa-agent-loop can register. */
	anx_xdna_init();	/* AMD XDNA NPU (Ryzen AI) */
	anx_jepa_init();	/* JEPA latent-state world model (non-fatal) */

	/* 13. VM subsystem (RFC-0017) */
	anx_vm_init();
	kprintf("vm subsystem initialized\n");

	/* 16. IBAL loop session primitive + EBM scorer registry (RFC-0020) */
	anx_loop_init();
	anx_ebm_init();

	/* 15. Visual theme (RFC-0019) — default Pretty on FB, Boring headless */
	anx_theme_init(anx_fb_available() ? ANX_THEME_PRETTY : ANX_THEME_BORING);
	kprintf("theme subsystem initialized\n");

	/* 12. Network bring-up (drivers already probed above) */
	{
		/*
		 * Apply locally-embedded Wi-Fi credentials, if this kernel
		 * was built with config/boot-secrets.conf present. Existing
		 * stored credentials win, so a machine that has been
		 * configured properly is never silently overridden by a
		 * stale build-time value.
		 */
		if (ANX_BOOT_SECRET_COUNT > 0) {
			uint32_t bi;

			kprintf("\n");
			kprintf("*** This kernel carries embedded Wi-Fi "
				"credentials in plaintext.\n");
			kprintf("*** Do not publish this image. Rebuild "
				"without config/boot-secrets.conf.\n\n");

			for (bi = 0; anx_boot_secrets[bi].name; bi++) {
				const char *n = anx_boot_secrets[bi].name;
				const char *v = anx_boot_secrets[bi].value;
				uint32_t vlen = 0;

				while (v[vlen])
					vlen++;
				if (anx_credential_exists(n)) {
					kprintf("boot-secrets: %s already "
						"stored; keeping it\n", n);
					continue;
				}
				if (anx_credential_create(n, ANX_CRED_OPAQUE,
							   v, vlen) == ANX_OK)
					kprintf("boot-secrets: %s applied\n", n);
				else
					kprintf("boot-secrets: %s failed\n", n);
			}
		}

		/* Auto-connect WiFi if cred:wifi-ssid was passed on cmdline.
		 * Only runs when MT7925 is the active driver (virtio/e1000 absent). */
		if (anx_net_probe_ok() && anx_mt7925_state() == MT7925_STATE_FW_UP) {
			char wifi_ssid[64]  = {0};
			char wifi_pass[128] = {0};
			uint32_t ssid_len = 0, pass_len = 0;

			if (anx_credential_read_system("wifi-ssid", wifi_ssid,
						sizeof(wifi_ssid) - 1,
						&ssid_len) == ANX_OK &&
			    ssid_len > 0) {
				anx_credential_read_system("wifi-pass", wifi_pass,
						    sizeof(wifi_pass) - 1,
						    &pass_len);
				anx_mt7925_connect(wifi_ssid,
						   pass_len > 0 ? wifi_pass
						                : NULL);
			}
		}

		if (anx_net_probe_ok()) {
			struct anx_net_config net_cfg;

			anx_arp_init();
			anx_udp_init();

			bool leased;

			PERF_BEGIN("dhcp_discover");
			kprintf("dhcp: discovering...\n");
			leased = anx_dhcp_discover(&net_cfg) == ANX_OK;
			if (!leased) {
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

			/*
			 * Only reach for the network when a lease says there
			 * is one. Without this, a machine with no DHCP server
			 * went on to spend a DNS timeout, and then an NTP
			 * timeout, talking to a gateway invented two lines
			 * above -- all of it during boot.
			 */
			if (leased) {
				uint32_t ntp_ip;

				PERF_BEGIN("time_sync");
				if (anx_dns_resolve("pool.ntp.org", &ntp_ip) == ANX_OK)
					anx_ntp_sync(ntp_ip);
				PERF_END();
			} else {
				kprintf("net: no lease; skipping DNS and NTP\n");
			}
		}
	}

	/*
	 * Second flush: everything from the store mounting through driver
	 * bring-up and networking is now on disk even if the machine hangs
	 * or is power-cycled before an orderly shutdown.
	 */
	anx_bootlog_ring_flush();

	/* Start HTTP API server (after network init) */
	anx_httpd_init(8080);

	/* Start SSH server (after network + credential store) */
	anx_sshd_init(22);

	/* Window manager (after iface, theme, and network) */
	if (anx_fb_available()) {
		anx_wm_init();
		kprintf("window manager initialized\n");
	}

	kprintf("kernel init complete -- all subsystems online\n");

	/* Show boot performance profile */
	anx_perf_report();

	/* Install mode: run text installer instead of desktop */
	if (install_mode) {
		kprintf("install: starting installer\n");
		anx_installer_interactive();
		return;
	}

	/* Boot into desktop on framebuffer hardware; fall back to shell */
	if (anx_fb_available())
		anx_wm_run();
	else
		anx_shell_run();
}
