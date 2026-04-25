/*
 * test_vm_sandbox.c — Tests for the agent VM sandboxing modes
 * (RFC-0017 sandbox extension).
 *
 * Covers:
 *   - Creating a VM with each of the three explicit sandbox modes
 *     (firecracker, qemu, anx-native) selects the correct backend.
 *   - ANX_VM_SANDBOX_AUTO falls back to the first available backend.
 *   - The agent sandbox helper applies sane defaults.
 *   - The anx-native backend validates input OIDs and runs without
 *     forking (works under the host-native test harness).
 *   - Reading back the sandbox spec returns inputs/outputs.
 *   - Firecracker validation rejects configurations that don't fit the
 *     microVM shape (BIOS firmware, balloon, etc.).
 */

#include <anx/types.h>
#include <anx/vm.h>
#include <anx/vm_backend.h>
#include <anx/string.h>
#include <anx/state_object.h>

static int test_create_with_mode(enum anx_vm_sandbox_mode mode,
				 const char *name,
				 const char *expected_backend)
{
	struct anx_vm_config cfg;
	struct anx_vm_object *vm;
	anx_oid_t oid;
	int ret;

	anx_memset(&cfg, 0, sizeof(cfg));
	anx_strlcpy(cfg.name, name, ANX_VM_NAME_MAX);
	cfg.cpu.count      = 1;
	cfg.memory.size_mb = 128;
	cfg.boot.firmware  = ANX_VM_FW_DIRECT_KERNEL;
	cfg.sandbox_mode   = mode;

	ret = anx_vm_create(&cfg, &oid);
	if (ret != ANX_OK)
		return ret;

	vm = anx_vm_object_get(&oid);
	if (!vm)
		return -100;
	if (anx_strcmp(vm->backend->name, expected_backend) != 0)
		return -101;

	anx_vm_destroy(&oid);
	return ANX_OK;
}

int test_vm_sandbox(void)
{
	int ret;

	anx_vm_init();
	anx_objstore_init();

	/* --- Test 1: AUTO selects firecracker (highest-priority) --- */
	ret = test_create_with_mode(ANX_VM_SANDBOX_AUTO, "auto-vm",
				    "firecracker");
	if (ret != ANX_OK)
		return -1;

	/* --- Test 2: explicit FIRECRACKER mode --- */
	ret = test_create_with_mode(ANX_VM_SANDBOX_FIRECRACKER, "fc-vm",
				    "firecracker");
	if (ret != ANX_OK)
		return -2;

	/* --- Test 3: explicit QEMU mode --- */
	ret = test_create_with_mode(ANX_VM_SANDBOX_QEMU, "qemu-vm",
				    "qemu");
	if (ret != ANX_OK)
		return -3;

	/* --- Test 4: explicit ANX_NATIVE mode --- */
	ret = test_create_with_mode(ANX_VM_SANDBOX_ANX_NATIVE, "native-vm",
				    "anx-native");
	if (ret != ANX_OK)
		return -4;

	/* --- Test 5: backend_select returns NULL for invalid mode --- */
	{
		struct anx_vm_backend *b;

		b = anx_vm_backend_select((enum anx_vm_sandbox_mode)999);
		if (b != NULL)
			return -5;
	}

	/* --- Test 6: agent sandbox helper applies defaults --- */
	{
		struct anx_vm_config cfg;
		struct anx_vm_object *vm;
		anx_oid_t oid;

		anx_memset(&cfg, 0, sizeof(cfg));
		cfg.sandbox_mode = ANX_VM_SANDBOX_FIRECRACKER;
		/* leave name, cpu, memory unset to exercise defaults */
		ret = anx_vm_agent_sandbox_create(&cfg, &oid);
		if (ret != ANX_OK)
			return -6;
		vm = anx_vm_object_get(&oid);
		if (!vm)
			return -7;
		if (vm->config.cpu.count != 1)
			return -8;
		if (vm->config.memory.size_mb != 256)
			return -9;
		/* firecracker forces direct_kernel boot */
		if (vm->config.boot.firmware != ANX_VM_FW_DIRECT_KERNEL)
			return -10;
		/* helper auto-generates a name */
		if (vm->config.name[0] == '\0')
			return -11;
		anx_vm_destroy(&oid);
	}

	/* --- Test 7: memory cap clamps requested memory --- */
	{
		struct anx_vm_config cfg;
		struct anx_vm_object *vm;
		anx_oid_t oid;

		anx_memset(&cfg, 0, sizeof(cfg));
		anx_strlcpy(cfg.name, "capped", ANX_VM_NAME_MAX);
		cfg.sandbox_mode      = ANX_VM_SANDBOX_ANX_NATIVE;
		cfg.memory.size_mb    = 1024;          /* request 1 GB */
		cfg.sandbox.memory_kb_cap = 64 * 1024; /* 64 MB hard cap */

		ret = anx_vm_agent_sandbox_create(&cfg, &oid);
		if (ret != ANX_OK)
			return -12;
		vm = anx_vm_object_get(&oid);
		if (!vm)
			return -13;
		if (vm->config.memory.size_mb != 64)
			return -14;
		anx_vm_destroy(&oid);
	}

	/* --- Test 8: anx-native sandbox starts and stops without fork --- */
	{
		struct anx_vm_config cfg;
		anx_oid_t oid;
		enum anx_vm_state state;

		anx_memset(&cfg, 0, sizeof(cfg));
		anx_strlcpy(cfg.name, "native-run", ANX_VM_NAME_MAX);
		cfg.sandbox_mode = ANX_VM_SANDBOX_ANX_NATIVE;
		cfg.cpu.count    = 1;
		cfg.memory.size_mb = 64;

		ret = anx_vm_create(&cfg, &oid);
		if (ret != ANX_OK)
			return -15;

		ret = anx_vm_start(&oid);
		if (ret != ANX_OK)
			return -16;

		ret = anx_vm_state_get(&oid, &state);
		if (ret != ANX_OK)
			return -17;
		if (state != ANX_VM_RUNNING)
			return -18;

		ret = anx_vm_stop(&oid, true);
		if (ret != ANX_OK)
			return -19;
		ret = anx_vm_state_get(&oid, &state);
		if (ret != ANX_OK)
			return -20;
		if (state != ANX_VM_DEFINED)
			return -21;

		anx_vm_destroy(&oid);
	}

	/* --- Test 9: anx-native sandbox with input OIDs validates them --- */
	{
		struct anx_vm_config cfg;
		anx_oid_t oid;
		anx_oid_t bogus = { .hi = 0xdeadbeefULL, .lo = 0xfeedfaceULL };

		anx_memset(&cfg, 0, sizeof(cfg));
		anx_strlcpy(cfg.name, "bad-input", ANX_VM_NAME_MAX);
		cfg.sandbox_mode = ANX_VM_SANDBOX_ANX_NATIVE;
		cfg.sandbox.inputs[0] = bogus;
		cfg.sandbox.input_count = 1;

		ret = anx_vm_agent_sandbox_create(&cfg, &oid);
		if (ret != ANX_OK)
			return -22;

		/* start should fail because the input OID does not exist */
		ret = anx_vm_start(&oid);
		if (ret == ANX_OK)
			return -23;
		if (ret != ANX_ENOENT)
			return -24;

		anx_vm_destroy(&oid);
	}

	/* --- Test 10: sandbox spec round-trips through agent_sandbox_get --- */
	{
		struct anx_vm_config cfg;
		struct anx_vm_agent_sandbox out;
		anx_oid_t oid;
		anx_oid_t fake_input = { .hi = 0x11ULL, .lo = 0x22ULL };

		anx_memset(&cfg, 0, sizeof(cfg));
		anx_strlcpy(cfg.name, "spec-roundtrip", ANX_VM_NAME_MAX);
		cfg.sandbox_mode = ANX_VM_SANDBOX_ANX_NATIVE;
		cfg.sandbox.inputs[0] = fake_input;
		cfg.sandbox.input_count = 1;
		cfg.sandbox.timeout_ms  = 5000;

		ret = anx_vm_agent_sandbox_create(&cfg, &oid);
		if (ret != ANX_OK)
			return -25;

		ret = anx_vm_agent_sandbox_get(&oid, &out);
		if (ret != ANX_OK)
			return -26;
		if (out.input_count != 1)
			return -27;
		if (out.inputs[0].hi != fake_input.hi ||
		    out.inputs[0].lo != fake_input.lo)
			return -28;
		if (out.timeout_ms != 5000)
			return -29;

		anx_vm_destroy(&oid);
	}

	/* --- Test 11: firecracker rejects BIOS firmware --- */
	{
		struct anx_vm_config cfg;
		anx_oid_t oid;

		anx_memset(&cfg, 0, sizeof(cfg));
		anx_strlcpy(cfg.name, "fc-bad-fw", ANX_VM_NAME_MAX);
		cfg.sandbox_mode  = ANX_VM_SANDBOX_FIRECRACKER;
		cfg.cpu.count     = 1;
		cfg.memory.size_mb = 128;
		cfg.boot.firmware = ANX_VM_FW_BIOS;	/* not allowed */

		ret = anx_vm_create(&cfg, &oid);
		if (ret == ANX_OK)
			return -30;
	}

	/* --- Test 12: firecracker rejects balloon --- */
	{
		struct anx_vm_config cfg;
		anx_oid_t oid;

		anx_memset(&cfg, 0, sizeof(cfg));
		anx_strlcpy(cfg.name, "fc-balloon", ANX_VM_NAME_MAX);
		cfg.sandbox_mode  = ANX_VM_SANDBOX_FIRECRACKER;
		cfg.cpu.count     = 1;
		cfg.memory.size_mb = 128;
		cfg.boot.firmware = ANX_VM_FW_DIRECT_KERNEL;
		cfg.memory.balloon = true;

		ret = anx_vm_create(&cfg, &oid);
		if (ret == ANX_OK)
			return -31;
	}

	/* --- Test 13: sandbox.mode field readable via config_get_field --- */
	{
		struct anx_vm_config cfg;
		anx_oid_t oid;
		char val[32];

		anx_memset(&cfg, 0, sizeof(cfg));
		anx_strlcpy(cfg.name, "field-read", ANX_VM_NAME_MAX);
		cfg.sandbox_mode = ANX_VM_SANDBOX_ANX_NATIVE;
		cfg.cpu.count    = 1;
		cfg.memory.size_mb = 64;

		ret = anx_vm_create(&cfg, &oid);
		if (ret != ANX_OK)
			return -32;

		ret = anx_vm_config_get_field(&oid, "sandbox.mode",
					      val, sizeof(val));
		if (ret != ANX_OK)
			return -33;
		if (anx_strcmp(val, "anx-native") != 0)
			return -34;

		anx_vm_destroy(&oid);
	}

	return 0;
}
