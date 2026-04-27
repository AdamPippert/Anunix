/*
 * installer.c — Text-based Anunix installer.
 *
 * Installs Anunix to a block device. Supports two modes:
 * 1. Automated: JSON provisioning config (kickstart-style)
 * 2. Interactive: prompts the user for each setting
 *
 * Install flow:
 *   1. Display welcome banner
 *   2. Load provisioning config (JSON or interactive)
 *   3. Detect hardware (ACPI + PCI)
 *   4. Select target disk
 *   5. Partition disk (GPT: EFI + Anunix data)
 *   6. Format Anunix object store
 *   7. Create user account
 *   8. Provision credentials
 *   9. Write network config
 *  10. Display summary and prompt for reboot
 */

#include <anx/types.h>
#include <anx/installer.h>
#include <anx/json.h>
#include <anx/gpt.h>
#include <anx/objstore_disk.h>
#include <anx/virtio_blk.h>
#include <anx/auth.h>
#include <anx/credential.h>
#include <anx/acpi.h>
#include <anx/pci.h>
#include <anx/arch.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/memory.h>
#include <anx/driver_table.h>
#include <anx/mt7925.h>
#include <anx/xdna.h>

/* Build PAL prime hardware flags from currently-detected hardware. */
static uint32_t detect_hw_flags(void)
{
	uint32_t flags = 0;

	if (anx_xdna_present())
		flags |= ANX_PAL_PRIME_HW_NPU;
	if (anx_net_probe_ok() &&
	    anx_mt7925_state() >= MT7925_STATE_FW_UP)
		flags |= ANX_PAL_PRIME_HW_WIFI;
	if (anx_blk_ready())
		flags |= ANX_PAL_PRIME_HW_STORAGE;
	return flags;
}

/* --- TUI helpers --- */

static void banner(const char *text)
{
	kprintf("\n=== %s ===\n\n", text);
}

static void status(const char *msg)
{
	kprintf("  [*] %s\n", msg);
}

static void ok(const char *msg)
{
	kprintf("  [OK] %s\n", msg);
}

static void fail(const char *msg, int err)
{
	kprintf("  [FAIL] %s (%d)\n", msg, err);
}

static int prompt_line(const char *label, char *buf, uint32_t size)
{
	uint32_t pos = 0;
	int c;

	kprintf("  %s: ", label);

	while (pos < size - 1) {
		c = arch_console_getc();
		if (c < 0)
			break;
		if (c == '\r' || c == '\n') {
			arch_console_putc('\n');
			break;
		}
		if (c == 0x7F || c == '\b') {
			if (pos > 0) {
				pos--;
				kprintf("\b \b");
			}
			continue;
		}
		if (c >= 0x20 && c < 0x7F) {
			buf[pos++] = (char)c;
			arch_console_putc((char)c);
		}
	}
	buf[pos] = '\0';
	return (int)pos;
}

static int prompt_password(const char *label, char *buf, uint32_t size)
{
	uint32_t pos = 0;
	int c;

	kprintf("  %s: ", label);

	while (pos < size - 1) {
		c = arch_console_getc();
		if (c < 0)
			break;
		if (c == '\r' || c == '\n') {
			arch_console_putc('\n');
			break;
		}
		if (c == 0x7F || c == '\b') {
			if (pos > 0)
				pos--;
			continue;
		}
		if (c >= 0x20 && c < 0x7F)
			buf[pos++] = (char)c;
	}
	buf[pos] = '\0';
	return (int)pos;
}

static bool prompt_confirm(const char *question)
{
	char buf[8];

	kprintf("  %s [y/N]: ", question);
	prompt_line("", buf, sizeof(buf));
	return (buf[0] == 'y' || buf[0] == 'Y');
}

/* --- Hardware detection summary --- */

static void show_hardware(void)
{
	const struct anx_acpi_info *acpi = anx_acpi_get_info();

	banner("Hardware Detection");

	if (acpi && acpi->valid) {
		kprintf("  CPUs:    %u\n", acpi->cpu_count);
		kprintf("  IOAPICs: %u\n", acpi->ioapic_count);
	}

	if (anx_blk_ready()) {
		kprintf("  Disk:    %u MiB (virtio-blk)\n",
			(uint32_t)(anx_blk_capacity() * 512 /
				   (1024 * 1024)));
	} else {
		kprintf("  Disk:    not detected\n");
	}

	{
		struct anx_list_head *pos;
		struct anx_list_head *list = anx_pci_device_list();
		uint32_t count = 0;

		ANX_LIST_FOR_EACH(pos, list)
			count++;
		kprintf("  PCI:     %u devices\n", count);
	}
}

/* --- Provisioned install (from JSON) --- */

int anx_installer_run(const char *provision_json, uint32_t json_len)
{
	struct anx_json_value root;
	struct anx_json_value *hostname_val, *auth_val, *creds_val;
	struct anx_json_value *install_val;
	const char *hostname;
	int ret;

	banner("Anunix Installer (Automated)");

	/* Parse provisioning config */
	status("parsing provisioning config...");
	ret = anx_json_parse(provision_json, json_len, &root);
	if (ret != ANX_OK) {
		fail("JSON parse error", ret);
		return ret;
	}
	ok("provisioning config loaded");

	/* Extract hostname */
	hostname_val = anx_json_get(&root, "hostname");
	hostname = anx_json_string(hostname_val);
	if (!hostname)
		hostname = "anunix";
	kprintf("  Hostname: %s\n", hostname);

	/* Show hardware */
	show_hardware();

	/* Check for disk */
	if (!anx_blk_ready()) {
		fail("no block device detected", ANX_ENOENT);
		anx_json_free(&root);
		return ANX_ENOENT;
	}

	/* Partition disk */
	banner("Disk Partitioning");
	install_val = anx_json_get(&root, "install");
	{
		const char *label = hostname;

		if (install_val) {
			struct anx_json_value *lbl;

			lbl = anx_json_get(install_val, "label");
			if (anx_json_string(lbl))
				label = anx_json_string(lbl);
		}

		status("creating GPT partition table...");
		ret = anx_gpt_create_default(label);
		if (ret != ANX_OK) {
			fail("GPT creation failed", ret);
			anx_json_free(&root);
			return ret;
		}
		ok("GPT partitions created");
	}

	/* Format object store on the Anunix data partition */
	banner("Object Store");
	status("formatting object store...");
	ret = anx_disk_format(hostname);
	if (ret != ANX_OK) {
		fail("format failed", ret);
		anx_json_free(&root);
		return ret;
	}
	ok("object store formatted");

	/* Create user accounts from provisioning config */
	banner("User Accounts");
	auth_val = anx_json_get(&root, "auth");
	if (auth_val) {
		struct anx_json_value *method, *keys;
		const char *user = "admin";
		struct anx_json_value *user_val;

		user_val = anx_json_get(auth_val, "username");
		if (anx_json_string(user_val))
			user = anx_json_string(user_val);

		ret = anx_auth_create_user(user);
		if (ret == ANX_OK || ret == ANX_EEXIST)
			kprintf("  User: %s\n", user);

		method = anx_json_get(auth_val, "method");
		if (method && anx_json_string(method) &&
		    anx_strcmp(anx_json_string(method), "password") == 0) {
			struct anx_json_value *pw;

			pw = anx_json_get(auth_val, "password");
			if (anx_json_string(pw)) {
				anx_auth_add_password(user,
					anx_json_string(pw),
					ANX_SCOPE_ADMIN);
				ok("password set");
			}
		}

		/* SSH keys */
		keys = anx_json_get(auth_val, "authorized_keys");
		if (keys && keys->type == ANX_JSON_ARRAY) {
			uint32_t i;

			for (i = 0; i < anx_json_array_len(keys); i++) {
				struct anx_json_value *k;

				k = anx_json_array_get(keys, i);
				if (anx_json_string(k)) {
					anx_auth_add_ssh_key(user,
						anx_json_string(k),
						ANX_SCOPE_ADMIN);
					kprintf("  SSH key %u added\n", i + 1);
				}
			}
		}
	}

	/* Provision credentials */
	banner("Credentials");
	creds_val = anx_json_get(&root, "credentials");
	if (creds_val && creds_val->type == ANX_JSON_ARRAY) {
		uint32_t i;

		for (i = 0; i < anx_json_array_len(creds_val); i++) {
			struct anx_json_value *cred, *name_v, *val_v, *type_v;
			const char *name, *val;

			cred = anx_json_array_get(creds_val, i);
			name_v = anx_json_get(cred, "name");
			val_v = anx_json_get(cred, "value");
			type_v = anx_json_get(cred, "type");
			name = anx_json_string(name_v);
			val = anx_json_string(val_v);

			if (name && val) {
				ret = anx_credential_create(name,
					ANX_CRED_API_KEY, val,
					(uint32_t)anx_strlen(val));
				if (ret == ANX_OK)
					kprintf("  %s: stored\n", name);
				else
					kprintf("  %s: failed (%d)\n",
						name, ret);
			}
			(void)type_v;
		}
	}

	/* Seed PAL priors from detected hardware so first boot is warm-started */
	anx_pal_prime_install(detect_hw_flags());
	/* Immediately persist so the primed state survives into first boot */
	anx_pal_persist_save();

	/* Done */
	banner("Installation Complete");
	kprintf("  Hostname: %s\n", hostname);
	kprintf("  Object store: formatted\n");
	kprintf("  System is ready.\n\n");

	anx_json_free(&root);
	return ANX_OK;
}

/* --- Interactive install --- */

int anx_installer_interactive(void)
{
	char hostname[64] = "anunix";
	char username[64] = "admin";
	char password[128];
	int ret;

	banner("Anunix Installer (Interactive)");

	kprintf("  Welcome to Anunix. This will install the operating system\n");
	kprintf("  to the detected block device.\n\n");

	/* Show hardware */
	show_hardware();

	if (!anx_blk_ready()) {
		fail("no block device detected — cannot install", ANX_ENOENT);
		return ANX_ENOENT;
	}

	kprintf("\n");

	/* Hostname */
	prompt_line("Hostname", hostname, sizeof(hostname));
	if (hostname[0] == '\0')
		anx_strlcpy(hostname, "anunix", sizeof(hostname));

	/* Confirm disk wipe */
	kprintf("\n  WARNING: This will erase ALL data on the disk (%u MiB).\n",
		(uint32_t)(anx_blk_capacity() * 512 / (1024 * 1024)));

	if (!prompt_confirm("Proceed with installation?")) {
		kprintf("\n  Installation cancelled.\n");
		return ANX_EPERM;
	}

	/* Partition and format */
	banner("Installing");

	status("creating partitions...");
	ret = anx_gpt_create_default(hostname);
	if (ret != ANX_OK) {
		fail("partition failed", ret);
		return ret;
	}
	ok("partitions created");

	status("formatting object store...");
	ret = anx_disk_format(hostname);
	if (ret != ANX_OK) {
		fail("format failed", ret);
		return ret;
	}
	ok("object store ready");

	/* Create user account */
	banner("User Setup");

	prompt_line("Username", username, sizeof(username));
	if (username[0] == '\0')
		anx_strlcpy(username, "admin", sizeof(username));

	prompt_password("Password", password, sizeof(password));

	ret = anx_auth_create_user(username);
	if (ret == ANX_OK || ret == ANX_EEXIST) {
		anx_auth_add_password(username, password, ANX_SCOPE_ADMIN);
		ok("user created with admin scope");
	}

	/* Zero password */
	anx_memset(password, 0, sizeof(password));

	/* Seed PAL priors from detected hardware so first boot is warm-started */
	anx_pal_prime_install(detect_hw_flags());
	/* Persist so the primed state survives into first boot */
	anx_pal_persist_save();

	/* Done */
	banner("Installation Complete");
	kprintf("  Hostname: %s\n", hostname);
	kprintf("  User:     %s\n", username);
	kprintf("  Object store: formatted\n");
	kprintf("  PAL state: persisted\n");
	kprintf("\n  You can now reboot into the installed system.\n\n");

	return ANX_OK;
}
