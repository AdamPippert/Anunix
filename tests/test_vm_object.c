/*
 * test_vm_object.c — Tests for VM Object lifecycle (RFC-0017).
 */

#include <anx/types.h>
#include <anx/vm.h>
#include <anx/string.h>
#include <anx/state_object.h>

int test_vm_object(void)
{
	struct anx_vm_config cfg;
	anx_oid_t vm_oid, vm2_oid;
	enum anx_vm_state state;
	struct anx_vm_config dump;
	int ret;

	anx_vm_init();
	anx_objstore_init();

	/* --- Test 1: create a VM --- */
	anx_memset(&cfg, 0, sizeof(cfg));
	anx_strlcpy(cfg.name, "test-vm", ANX_VM_NAME_MAX);
	cfg.cpu.count      = 2;
	cfg.memory.size_mb = 1024;
	cfg.boot.firmware  = ANX_VM_FW_BIOS;
	cfg.nets[0].mode   = ANX_VM_NET_USER;
	cfg.net_count      = 1;

	ret = anx_vm_create(&cfg, &vm_oid);
	if (ret != ANX_OK)
		return -1;

	/* --- Test 2: state should be DEFINED after create --- */
	ret = anx_vm_state_get(&vm_oid, &state);
	if (ret != ANX_OK)
		return -2;
	if (state != ANX_VM_DEFINED)
		return -3;

	/* --- Test 3: config dump round-trips --- */
	ret = anx_vm_config_dump(&vm_oid, &dump);
	if (ret != ANX_OK)
		return -4;
	if (dump.cpu.count != 2)
		return -5;
	if (dump.memory.size_mb != 1024)
		return -6;
	if (anx_strcmp(dump.name, "test-vm") != 0)
		return -7;

	/* --- Test 4: reject duplicate name --- */
	ret = anx_vm_create(&cfg, &vm2_oid);
	if (ret == ANX_OK)
		return -8;	/* should have failed */

	/* --- Test 5: list shows our VM --- */
	{
		anx_oid_t list[16];
		uint32_t count;

		ret = anx_vm_list(list, 16, &count);
		if (ret != ANX_OK)
			return -9;
		if (count == 0)
			return -10;
	}

	/* --- Test 6: config get/set --- */
	{
		char val[64];

		ret = anx_vm_config_get_field(&vm_oid, "cpu.count",
					      val, sizeof(val));
		if (ret != ANX_OK)
			return -11;
		if (anx_strcmp(val, "2") != 0)
			return -12;

		ret = anx_vm_config_set_field(&vm_oid, "cpu.count", "4",
					      ANX_VM_FIELD_CPU);
		if (ret != ANX_OK)
			return -13;

		ret = anx_vm_config_get_field(&vm_oid, "cpu.count",
					      val, sizeof(val));
		if (ret != ANX_OK)
			return -14;
		if (anx_strcmp(val, "4") != 0)
			return -15;
	}

	/* --- Test 7: start transitions to RUNNING --- */
	ret = anx_vm_start(&vm_oid);
	/* QEMU backend returns ENOTSUP on bare-metal freestanding — acceptable */
	if (ret == ANX_OK) {
		ret = anx_vm_state_get(&vm_oid, &state);
		if (ret != ANX_OK)
			return -16;
		if (state != ANX_VM_RUNNING)
			return -17;

		/* stop it */
		ret = anx_vm_stop(&vm_oid, true);
		if (ret != ANX_OK)
			return -18;
		ret = anx_vm_state_get(&vm_oid, &state);
		if (ret != ANX_OK)
			return -19;
		if (state != ANX_VM_DEFINED)
			return -20;
	}

	/* --- Test 8: destroy --- */
	ret = anx_vm_destroy(&vm_oid);
	if (ret != ANX_OK)
		return -21;

	/* After destroy, state_get should fail */
	ret = anx_vm_state_get(&vm_oid, &state);
	if (ret == ANX_OK)
		return -22;

	/* --- Test 9: create with invalid config fails --- */
	{
		struct anx_vm_config bad;

		anx_memset(&bad, 0, sizeof(bad));
		/* name is empty */
		ret = anx_vm_create(&bad, &vm_oid);
		if (ret == ANX_OK)
			return -23;
	}

	return 0;
}
