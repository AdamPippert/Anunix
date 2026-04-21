/*
 * vm.c — 'vm' shell builtin (RFC-0017 Section 16).
 *
 * Exposes VM lifecycle, config, and snapshot operations to the ANX shell.
 */

#include <anx/types.h>
#include <anx/vm.h>
#include <anx/vm_backend.h>
#include <anx/string.h>
#include <anx/kprintf.h>
/* errno codes in anx/types.h */

static void vm_usage(void)
{
	kprintf("usage: vm <subcommand> [args]\n");
	kprintf("  create <name> [--cpu N] [--mem MB] [--cmdline STR]\n");
	kprintf("  list\n");
	kprintf("  start  <name>\n");
	kprintf("  stop   <name> [--force]\n");
	kprintf("  pause  <name>\n");
	kprintf("  resume <name>\n");
	kprintf("  snapshot <name> [--tag TAG]\n");
	kprintf("  clone  <snap-name> <new-name>\n");
	kprintf("  config get <name> <field>\n");
	kprintf("  config set <name> <field> <value>\n");
	kprintf("  config dump <name>\n");
	kprintf("  disk create <name> <size-mb>\n");
	kprintf("  destroy <name>\n");
	kprintf("  exec   <name> <command>\n");
}

static const char *vm_state_name(enum anx_vm_state s)
{
	switch (s) {
	case ANX_VM_DEFINED:  return "defined";
	case ANX_VM_RUNNING:  return "running";
	case ANX_VM_PAUSED:   return "paused";
	case ANX_VM_SAVED:    return "saved";
	case ANX_VM_DELETED:  return "deleted";
	default:              return "unknown";
	}
}

/* Find vm oid by name — scans the list */
static int vm_find_by_name(const char *name, anx_oid_t *oid_out)
{
	anx_oid_t list[32];
	uint32_t count, i;
	int ret;

	ret = anx_vm_list(list, 32, &count);
	if (ret != ANX_OK)
		return ret;

	for (i = 0; i < count; i++) {
		struct anx_vm_config cfg;

		if (anx_vm_config_dump(&list[i], &cfg) == ANX_OK &&
		    anx_strcmp(cfg.name, name) == 0) {
			*oid_out = list[i];
			return ANX_OK;
		}
	}
	return ANX_ENOENT;
}

static int cmd_vm_create(int argc, char **argv)
{
	struct anx_vm_config cfg;
	anx_oid_t vm_oid;
	int i;

	if (argc < 1) {
		kprintf("vm create: missing name\n");
		return ANX_EINVAL;
	}

	anx_memset(&cfg, 0, sizeof(cfg));
	anx_strlcpy(cfg.name, argv[0], ANX_VM_NAME_MAX);

	/* Defaults */
	cfg.cpu.count      = 1;
	anx_strlcpy(cfg.cpu.model, "qemu64", sizeof(cfg.cpu.model));
	cfg.memory.size_mb = 512;
	cfg.boot.firmware  = ANX_VM_FW_BIOS;

	/* Default user-mode network */
	cfg.nets[0].mode = ANX_VM_NET_USER;
	cfg.net_count    = 1;

	for (i = 1; i < argc - 1; i++) {
		if (anx_strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
			cfg.cpu.count = (uint32_t)anx_strtoul(argv[++i],
							      NULL, 10);
		} else if (anx_strcmp(argv[i], "--mem") == 0 && i + 1 < argc) {
			cfg.memory.size_mb = anx_strtoull(argv[++i], NULL, 10);
		} else if (anx_strcmp(argv[i], "--cmdline") == 0 &&
			   i + 1 < argc) {
			anx_strlcpy(cfg.boot.cmdline, argv[++i],
				    sizeof(cfg.boot.cmdline) - 1);
			cfg.boot.firmware = ANX_VM_FW_DIRECT_KERNEL;
		}
	}

	if (anx_vm_create(&cfg, &vm_oid) != ANX_OK) {
		kprintf("vm create: failed\n");
		return ANX_EINVAL;
	}

	kprintf("created vm '%s'\n", cfg.name);
	return ANX_OK;
}

static int cmd_vm_list(void)
{
	anx_oid_t list[32];
	uint32_t count, i;

	if (anx_vm_list(list, 32, &count) != ANX_OK) {
		kprintf("vm list: error\n");
		return ANX_EINVAL;
	}

	if (count == 0) {
		kprintf("no VMs defined\n");
		return ANX_OK;
	}

	kprintf("%-20s  %-10s  %5s  %9s\n",
		"NAME", "STATE", "CPUS", "MEM (MB)");
	kprintf("%-20s  %-10s  %5s  %9s\n",
		"--------------------", "----------", "-----", "---------");

	for (i = 0; i < count; i++) {
		struct anx_vm_config cfg;
		enum anx_vm_state state;

		if (anx_vm_config_dump(&list[i], &cfg) != ANX_OK)
			continue;
		if (anx_vm_state_get(&list[i], &state) != ANX_OK)
			state = ANX_VM_DEFINED;

		kprintf("%-20s  %-10s  %5u  %9u\n",
			cfg.name, vm_state_name(state),
			cfg.cpu.count, (uint32_t)cfg.memory.size_mb);
	}
	return ANX_OK;
}

static int cmd_vm_lifecycle(const char *subcmd, const char *name, bool force)
{
	anx_oid_t oid;
	int ret;

	ret = vm_find_by_name(name, &oid);
	if (ret != ANX_OK) {
		kprintf("vm %s: VM '%s' not found\n", subcmd, name);
		return ANX_ENOENT;
	}

	if (anx_strcmp(subcmd, "start") == 0)
		ret = anx_vm_start(&oid);
	else if (anx_strcmp(subcmd, "stop") == 0)
		ret = anx_vm_stop(&oid, force);
	else if (anx_strcmp(subcmd, "pause") == 0)
		ret = anx_vm_pause(&oid);
	else if (anx_strcmp(subcmd, "resume") == 0)
		ret = anx_vm_resume(&oid);
	else
		return ANX_EINVAL;

	if (ret != ANX_OK)
		kprintf("vm %s '%s': error %d\n", subcmd, name, ret);
	return ret;
}

static int cmd_vm_config(int argc, char **argv)
{
	anx_oid_t oid;
	int ret;

	if (argc < 2) {
		kprintf("vm config: get|set|dump <name> ...\n");
		return ANX_EINVAL;
	}

	if (anx_strcmp(argv[0], "dump") == 0) {
		struct anx_vm_config cfg;

		ret = vm_find_by_name(argv[1], &oid);
		if (ret != ANX_OK) {
			kprintf("vm config dump: '%s' not found\n", argv[1]);
			return ANX_ENOENT;
		}
		if (anx_vm_config_dump(&oid, &cfg) != ANX_OK)
			return ANX_EINVAL;

		kprintf("name:          %s\n", cfg.name);
		kprintf("cpu.count:     %u\n", cfg.cpu.count);
		kprintf("cpu.model:     %s\n", cfg.cpu.model);
		kprintf("memory.size_mb:%u\n", (uint32_t)(uint32_t)cfg.memory.size_mb);
		kprintf("disk_count:    %u\n", cfg.disk_count);
		kprintf("net_count:     %u\n", cfg.net_count);
		kprintf("boot.cmdline:  %s\n", cfg.boot.cmdline);
		return ANX_OK;
	}

	if (anx_strcmp(argv[0], "get") == 0 && argc >= 3) {
		char val[256];

		ret = vm_find_by_name(argv[1], &oid);
		if (ret != ANX_OK) {
			kprintf("vm config get: '%s' not found\n", argv[1]);
			return ANX_ENOENT;
		}
		ret = anx_vm_config_get_field(&oid, argv[2], val, sizeof(val));
		if (ret != ANX_OK) {
			kprintf("vm config get: unknown field '%s'\n", argv[2]);
			return ret;
		}
		kprintf("%s\n", val);
		return ANX_OK;
	}

	if (anx_strcmp(argv[0], "set") == 0 && argc >= 4) {
		ret = vm_find_by_name(argv[1], &oid);
		if (ret != ANX_OK) {
			kprintf("vm config set: '%s' not found\n", argv[1]);
			return ANX_ENOENT;
		}
		ret = anx_vm_config_set_field(&oid, argv[2], argv[3],
					      ANX_VM_FIELD_ALL);
		if (ret != ANX_OK) {
			kprintf("vm config set: error %d\n", ret);
			return ret;
		}
		kprintf("ok\n");
		return ANX_OK;
	}

	kprintf("vm config: usage: get|set|dump <name> [field] [value]\n");
	return ANX_EINVAL;
}

static int cmd_vm_destroy(const char *name)
{
	anx_oid_t oid;
	int ret;

	ret = vm_find_by_name(name, &oid);
	if (ret != ANX_OK) {
		kprintf("vm destroy: '%s' not found\n", name);
		return ANX_ENOENT;
	}
	ret = anx_vm_destroy(&oid);
	if (ret != ANX_OK)
		kprintf("vm destroy: error %d\n", ret);
	return ret;
}

static int cmd_vm_exec(const char *name, const char *command)
{
	anx_oid_t oid;
	char output[2048];
	int ret;

	ret = vm_find_by_name(name, &oid);
	if (ret != ANX_OK) {
		kprintf("vm exec: '%s' not found\n", name);
		return ANX_ENOENT;
	}
	ret = anx_vm_exec(&oid, command, output, sizeof(output));
	if (ret != ANX_OK) {
		kprintf("vm exec: error %d\n", ret);
		return ret;
	}
	kprintf("%s\n", output);
	return ANX_OK;
}

int cmd_vm(int argc, char **argv)
{
	if (argc < 2) {
		vm_usage();
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "create") == 0)
		return cmd_vm_create(argc - 2, argv + 2);

	if (anx_strcmp(argv[1], "list") == 0)
		return cmd_vm_list();

	if (anx_strcmp(argv[1], "start") == 0 && argc >= 3)
		return cmd_vm_lifecycle("start", argv[2], false);

	if (anx_strcmp(argv[1], "stop") == 0 && argc >= 3) {
		bool force = (argc >= 4 &&
			      anx_strcmp(argv[3], "--force") == 0);
		return cmd_vm_lifecycle("stop", argv[2], force);
	}

	if (anx_strcmp(argv[1], "pause") == 0 && argc >= 3)
		return cmd_vm_lifecycle("pause", argv[2], false);

	if (anx_strcmp(argv[1], "resume") == 0 && argc >= 3)
		return cmd_vm_lifecycle("resume", argv[2], false);

	if (anx_strcmp(argv[1], "config") == 0)
		return cmd_vm_config(argc - 2, argv + 2);

	if (anx_strcmp(argv[1], "destroy") == 0 && argc >= 3)
		return cmd_vm_destroy(argv[2]);

	if (anx_strcmp(argv[1], "exec") == 0 && argc >= 4)
		return cmd_vm_exec(argv[2], argv[3]);

	vm_usage();
	return ANX_EINVAL;
}
