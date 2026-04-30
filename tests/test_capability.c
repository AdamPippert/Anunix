/*
 * test_capability.c — Tests for Capability Objects (RFC-0007).
 */

#include <anx/types.h>
#include <anx/capability.h>
#include <anx/engine.h>
#include <anx/state_object.h>
#include <anx/uuid.h>
#include <anx/string.h>

int test_capability(void)
{
	struct anx_capability *cap;
	int ret;

	anx_objstore_init();
	anx_engine_registry_init();
	anx_cap_store_init();

	/* Create a capability */
	ret = anx_cap_create("test_summarizer", "1.0", &cap);
	if (ret != ANX_OK)
		return -1;

	if (cap->status != ANX_CAP_DRAFT)
		return -2;

	/* Draft -> validating */
	ret = anx_cap_transition(cap, ANX_CAP_VALIDATING);
	if (ret != ANX_OK)
		return -3;

	/* Invalid: validating -> installed (must go through validated) */
	ret = anx_cap_transition(cap, ANX_CAP_INSTALLED);
	if (ret == ANX_OK)
		return -4;

	/* validating -> validated */
	ret = anx_cap_transition(cap, ANX_CAP_VALIDATED);
	if (ret != ANX_OK)
		return -5;

	/* Install (registers as engine) */
	cap->output_cap_mask = ANX_CAP_SUMMARIZATION;
	ret = anx_cap_install(cap);
	if (ret != ANX_OK)
		return -6;

	if (cap->status != ANX_CAP_INSTALLED)
		return -7;

	/* Engine should exist in registry */
	if (anx_uuid_is_nil(&cap->installed_engine_id))
		return -8;

	{
		struct anx_engine *eng;
		eng = anx_engine_lookup(&cap->installed_engine_id);
		if (!eng)
			return -9;
		if (eng->engine_class != ANX_ENGINE_INSTALLED_CAPABILITY)
			return -10;
	}

	/* Record invocations */
	anx_cap_record_invocation(cap, true);
	anx_cap_record_invocation(cap, true);
	anx_cap_record_invocation(cap, false);

	if (cap->invocation_count != 3)
		return -11;
	if (cap->success_count != 2)
		return -12;

	/* Uninstall */
	ret = anx_cap_uninstall(cap);
	if (ret != ANX_OK)
		return -13;

	/* Should have cleared engine ID */
	if (!anx_uuid_is_nil(&cap->installed_engine_id))
		return -14;

	/* ------------------------------------------------------------------ */
	/* Phase 15: Capability validation procedure                           */
	/* ------------------------------------------------------------------ */

	/* Test 15: validate with no required engines → score 100, VALIDATED */
	{
		struct anx_capability *cap2;

		ret = anx_cap_create("validator_test", "2.0", &cap2);
		if (ret != ANX_OK)
			return -15;

		ret = anx_cap_validate(cap2);
		if (ret != ANX_OK)
			return -15;
		if (cap2->status != ANX_CAP_VALIDATED)
			return -15;
		if (cap2->validation_score != 100)
			return -15;
	}

	/* Test 16: validate with 3 missing required engines → score 25 < 50 → DRAFT */
	{
		struct anx_capability *cap3;
		anx_eid_t fake_eid;

		ret = anx_cap_create("failing_cap", "1.0", &cap3);
		if (ret != ANX_OK)
			return -16;

		/* Point 3 required engine slots at a non-nil ID that won't exist */
		anx_memset(&fake_eid, 0xFF, sizeof(fake_eid));
		cap3->required_engines[0] = fake_eid;
		cap3->required_engines[1] = fake_eid;
		cap3->required_engines[2] = fake_eid;
		cap3->required_engine_count = 3;

		ret = anx_cap_validate(cap3);
		if (ret == ANX_OK)
			return -16;	/* must fail */
		if (cap3->status != ANX_CAP_DRAFT)
			return -16;	/* rolled back */
		if (cap3->validation_score >= 50)
			return -16;
	}

	/* Test 17: anx_cap_validate on non-DRAFT returns ANX_EPERM */
	{
		struct anx_capability *cap4;

		ret = anx_cap_create("double_validate", "1.0", &cap4);
		if (ret != ANX_OK)
			return -17;

		/* First validate succeeds */
		ret = anx_cap_validate(cap4);
		if (ret != ANX_OK)
			return -17;
		if (cap4->status != ANX_CAP_VALIDATED)
			return -17;

		/* Second validate on VALIDATED must fail */
		ret = anx_cap_validate(cap4);
		if (ret == ANX_OK)
			return -17;
	}

	return 0;
}
