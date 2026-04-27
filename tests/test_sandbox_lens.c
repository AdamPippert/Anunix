/*
 * test_sandbox_lens.c — Tests for the sandbox lens + object groups.
 *
 * Covers:
 *   - Group create/grant/seal lifecycle.
 *   - Lens denies anx_objstore_lookup for unauthorized OIDs.
 *   - Lens permits the sandbox's declared inputs.
 *   - Lens permits OIDs granted via an attached group.
 *   - Sandbox VM's own OID is always reachable to the cell.
 *   - Lens is cleared after stop().
 *   - Raw lookup bypasses the lens.
 */

#include <anx/types.h>
#include <anx/vm.h>
#include <anx/object_group.h>
#include <anx/sandbox_lens.h>
#include <anx/string.h>
#include <anx/state_object.h>

static int make_byte_obj(const char *bytes, anx_oid_t *oid_out)
{
	struct anx_so_create_params p;
	struct anx_state_object *obj;
	int ret;

	anx_memset(&p, 0, sizeof(p));
	p.object_type = ANX_OBJ_BYTE_DATA;
	p.payload = bytes;
	p.payload_size = anx_strlen(bytes);
	ret = anx_so_create(&p, &obj);
	if (ret != ANX_OK)
		return ret;
	*oid_out = obj->oid;
	return ANX_OK;
}

int test_sandbox_lens(void)
{
	int ret;
	anx_oid_t in_oid, denied_oid, group_oid, granted_oid;

	anx_vm_init();
	anx_objstore_init();
	anx_object_group_init();

	/* --- Test 1: group create / grant / seal --- */
	{
		struct anx_sbx_grant g;
		struct anx_object_group *gp;

		ret = anx_object_group_create("test-group", &group_oid);
		if (ret != ANX_OK) return -1;

		gp = anx_object_group_get(&group_oid);
		if (!gp) return -2;
		if (gp->sealed) return -3;
		if (gp->grant_count != 0) return -4;

		/* empty grant rejected */
		anx_memset(&g, 0, sizeof(g));
		ret = anx_object_group_grant(&group_oid, &g);
		if (ret == ANX_OK) return -5;

		/* OID-only grant accepted */
		ret = make_byte_obj("granted", &granted_oid);
		if (ret != ANX_OK) return -6;
		anx_memset(&g, 0, sizeof(g));
		g.oid = granted_oid;
		g.op_mask = ANX_SBX_READ;
		ret = anx_object_group_grant(&group_oid, &g);
		if (ret != ANX_OK) return -7;

		ret = anx_object_group_seal(&group_oid);
		if (ret != ANX_OK) return -8;

		/* sealed → further grants rejected */
		ret = anx_object_group_grant(&group_oid, &g);
		if (ret == ANX_OK) return -9;
	}

	/* --- Test 2: create two objects, one input, one not --- */
	ret = make_byte_obj("input", &in_oid);
	if (ret != ANX_OK) return -10;
	ret = make_byte_obj("denied", &denied_oid);
	if (ret != ANX_OK) return -11;

	/* --- Test 3: with no lens, both lookups succeed --- */
	{
		struct anx_state_object *obj;

		obj = anx_objstore_lookup(&in_oid);
		if (!obj) return -12;
		anx_objstore_release(obj);

		obj = anx_objstore_lookup(&denied_oid);
		if (!obj) return -13;
		anx_objstore_release(obj);
	}

	/* --- Test 4: install a lens with one input + the group --- */
	{
		struct anx_sandbox_lens lens;
		struct anx_sandbox_lens_save save;
		struct anx_state_object *obj;

		anx_memset(&lens, 0, sizeof(lens));
		lens.vm_oid.hi = 0xabcd; lens.vm_oid.lo = 0x1234;
		lens.inputs[0] = in_oid;
		lens.input_count = 1;
		lens.groups[0] = group_oid;
		lens.group_count = 1;

		ret = anx_sandbox_lens_install(&save, &lens);
		if (ret != ANX_OK) return -14;
		if (anx_sandbox_lens_active() != &lens) return -15;

		/* declared input is reachable */
		obj = anx_objstore_lookup(&in_oid);
		if (!obj) return -16;
		anx_objstore_release(obj);

		/* group-granted OID is reachable */
		obj = anx_objstore_lookup(&granted_oid);
		if (!obj) return -17;
		anx_objstore_release(obj);

		/* unauthorized OID is denied */
		obj = anx_objstore_lookup(&denied_oid);
		if (obj) return -18;

		/* the sandbox's own VM OID is allowed */
		obj = anx_objstore_lookup(&lens.vm_oid);
		/* it won't be in the store, but the lens check itself
		 * permits it — we verify via the public check API */
		(void)obj;
		if (!anx_sandbox_lens_check(&lens.vm_oid, ANX_SBX_READ))
			return -19;

		/* raw lookup ignores the lens */
		obj = anx_objstore_lookup_raw(&denied_oid);
		if (!obj) return -20;
		anx_objstore_release(obj);

		anx_sandbox_lens_restore(&save);
		if (anx_sandbox_lens_active() != NULL) return -21;
	}

	/* --- Test 5: nested install/restore returns to outer lens --- */
	{
		struct anx_sandbox_lens outer, inner;
		struct anx_sandbox_lens_save s_outer, s_inner;
		struct anx_state_object *obj;

		anx_memset(&outer, 0, sizeof(outer));
		outer.inputs[0] = in_oid;
		outer.input_count = 1;

		anx_memset(&inner, 0, sizeof(inner));
		/* inner has no grants → default-deny everything */

		anx_sandbox_lens_install(&s_outer, &outer);
		obj = anx_objstore_lookup(&in_oid);
		if (!obj) return -22;
		anx_objstore_release(obj);

		anx_sandbox_lens_install(&s_inner, &inner);
		obj = anx_objstore_lookup(&in_oid);
		if (obj) return -23;	/* inner denies */

		anx_sandbox_lens_restore(&s_inner);
		obj = anx_objstore_lookup(&in_oid);
		if (!obj) return -24;	/* outer permits again */
		anx_objstore_release(obj);

		anx_sandbox_lens_restore(&s_outer);
	}

	/* --- Test 6: anx-native backend installs/clears the lens --- */
	{
		struct anx_vm_config cfg;
		anx_oid_t vm_oid;
		struct anx_state_object *obj;

		anx_memset(&cfg, 0, sizeof(cfg));
		anx_strlcpy(cfg.name, "lens-vm", ANX_VM_NAME_MAX);
		cfg.sandbox_mode = ANX_VM_SANDBOX_ANX_NATIVE;
		cfg.cpu.count = 1;
		cfg.memory.size_mb = 64;
		cfg.sandbox.inputs[0] = in_oid;
		cfg.sandbox.input_count = 1;

		ret = anx_vm_create(&cfg, &vm_oid);
		if (ret != ANX_OK) return -25;

		ret = anx_vm_agent_sandbox_attach_group(&vm_oid, &group_oid);
		if (ret != ANX_OK) return -26;

		ret = anx_vm_start(&vm_oid);
		if (ret != ANX_OK) return -27;

		/* lens is now active — denied_oid invisible */
		obj = anx_objstore_lookup(&denied_oid);
		if (obj) return -28;

		/* input + group-granted OIDs visible */
		obj = anx_objstore_lookup(&in_oid);
		if (!obj) return -29;
		anx_objstore_release(obj);

		obj = anx_objstore_lookup(&granted_oid);
		if (!obj) return -30;
		anx_objstore_release(obj);

		ret = anx_vm_stop(&vm_oid, true);
		if (ret != ANX_OK) return -31;

		/* lens cleared — denied_oid visible again */
		obj = anx_objstore_lookup(&denied_oid);
		if (!obj) return -32;
		anx_objstore_release(obj);

		anx_vm_destroy(&vm_oid);
	}

	/* --- Test 7: attach_group rejects when running --- */
	{
		struct anx_vm_config cfg;
		anx_oid_t vm_oid;
		anx_oid_t g2_oid;

		anx_object_group_create("g2", &g2_oid);

		anx_memset(&cfg, 0, sizeof(cfg));
		anx_strlcpy(cfg.name, "running-vm", ANX_VM_NAME_MAX);
		cfg.sandbox_mode = ANX_VM_SANDBOX_ANX_NATIVE;
		cfg.cpu.count = 1;
		cfg.memory.size_mb = 64;

		ret = anx_vm_create(&cfg, &vm_oid);
		if (ret != ANX_OK) return -33;

		ret = anx_vm_start(&vm_oid);
		if (ret != ANX_OK) return -34;

		ret = anx_vm_agent_sandbox_attach_group(&vm_oid, &g2_oid);
		if (ret == ANX_OK) return -35;

		anx_vm_stop(&vm_oid, true);
		anx_vm_destroy(&vm_oid);
	}

	return 0;
}
