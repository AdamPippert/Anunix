/*
 * test_capability.c — Tests for Capability Objects (RFC-0007).
 */

#include <anx/types.h>
#include <anx/capability.h>
#include <anx/engine.h>
#include <anx/state_object.h>
#include <anx/uuid.h>

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

	return 0;
}
