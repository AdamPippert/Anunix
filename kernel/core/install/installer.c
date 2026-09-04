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
 *   4. Select target disk, building a RAID array first if one is asked for
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
#include <anx/md.h>
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
		uint32_t i;

		for (i = 0; i < anx_blk_dev_count(); i++) {
			struct anx_blk_dev *dev = anx_blk_dev_at(i);

			if (!dev)
				continue;
			kprintf("  Disk:    %-8s %u MiB (%s)%s\n",
				dev->name,
				(uint32_t)(anx_blk_dev_capacity(dev) / 2048),
				dev->ops->name ? dev->ops->name : "?",
				dev == anx_blk_active() ? " [target]" : "");
		}
	} else {
		kprintf("  Disk:    not detected\n");
	}

	if (anx_md_count() > 0) {
		uint32_t i;

		for (i = 0; i < anx_md_count(); i++) {
			const struct anx_md_array *a = anx_md_at(i);

			kprintf("  Array:   %-8s %s, %u members, %s\n",
				a->name, anx_md_level_name(a->level),
				a->member_count,
				anx_md_state_name(anx_md_state(a)));
		}
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

/* --- RAID provisioning --- */

/*
 * Build the array described by the "install.raid" object:
 *
 *   "raid": {
 *     "level":    "raid0",              // or "raid1", "0", "1"
 *     "chunk_kib": 64,                  // raid0 only, power of two
 *     "metadata": "head",               // or "tail" for a mirrored ESP
 *     "members":  ["nvme0", "nvme1"]
 *   }
 *
 * Every listed member is erased. On success the new array is the active
 * block device.
 */
static int build_raid(struct anx_json_value *raid_val)
{
	struct anx_blk_dev *members[ANX_MD_MAX_MEMBERS];
	struct anx_md_array *a = NULL;
	struct anx_json_value *members_val;
	uint32_t chunk_sectors = 0;
	uint32_t count;
	uint32_t i;
	bool tail = false;
	int level;
	int ret;

	level = anx_md_parse_level(anx_json_string(
			anx_json_get(raid_val, "level")));
	if (level < 0) {
		fail("raid level must be raid0 or raid1", ANX_EINVAL);
		return ANX_EINVAL;
	}

	if (level == ANX_MD_LEVEL_RAID0) {
		int64_t kib = anx_json_number(anx_json_get(raid_val,
							   "chunk_kib"));

		if (kib > 0)
			chunk_sectors = (uint32_t)kib * 2;
	}

	{
		const char *meta = anx_json_string(anx_json_get(raid_val,
								"metadata"));

		if (meta && anx_strcmp(meta, "tail") == 0)
			tail = true;
	}

	members_val = anx_json_get(raid_val, "members");
	count = anx_json_array_len(members_val);
	if (count < 2 || count > ANX_MD_MAX_MEMBERS) {
		fail("raid needs 2 to 8 members", ANX_EINVAL);
		return ANX_EINVAL;
	}

	for (i = 0; i < count; i++) {
		const char *name = anx_json_string(
			anx_json_array_get(members_val, i));

		members[i] = name ? anx_blk_dev_find(name) : NULL;
		if (!members[i]) {
			kprintf("  [FAIL] no such block device: %s\n",
				name ? name : "(null)");
			return ANX_ENODEV;
		}
	}

	/* The array takes over from whichever member currently answers the
	 * whole-system API. */
	anx_blk_set_active(NULL);

	status("creating array...");
	ret = anx_md_create(NULL, (uint32_t)level, chunk_sectors,
			    members, count, tail, &a);
	if (ret != ANX_OK) {
		anx_blk_set_active(anx_blk_dev_at(0));
		return ret;
	}

	anx_blk_set_active(a->bdev);
	kprintf("  [OK] %s: %s over %u members, %u MiB\n",
		a->name, anx_md_level_name(a->level), a->member_count,
		(uint32_t)(a->array_sectors / 2048));
	return ANX_OK;
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

	/* Build a RAID array when the config asks for one. The array becomes
	 * the active block device, so the partitioning and format steps below
	 * land on the array rather than on one member. */
	install_val = anx_json_get(&root, "install");
	if (install_val) {
		struct anx_json_value *raid_val;

		raid_val = anx_json_get(install_val, "raid");
		if (raid_val) {
			banner("Software RAID");
			ret = build_raid(raid_val);
			if (ret != ANX_OK) {
				fail("RAID setup failed", ret);
				anx_json_free(&root);
				return ret;
			}
		}
	}

	/* Partition disk */
	banner("Disk Partitioning");
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
