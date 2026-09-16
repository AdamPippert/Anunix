/* Behavioral capability dependencies are mandatory at validation and install. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/capability.h>
#include <anx/engine.h>
#include <anx/string.h>
#include <anx/uuid.h>

static void retire(struct anx_capability *cap)
{
	if (!cap)
		return;
	if (cap->status == ANX_CAP_INSTALLED || cap->status == ANX_CAP_SUSPENDED)
		anx_cap_uninstall(cap);
	anx_cap_transition(cap, ANX_CAP_RETIRED);
}

static int package(const char *name, const anx_eid_t *dependency, uint32_t authority,
		   uint32_t ceiling, struct anx_capability **out)
{
	int ret = anx_cap_create(name, "1", out);
	if (ret != ANX_OK)
		return ret;
	(*out)->required_engines[0] = *dependency;
	(*out)->required_engine_count = 1;
	(*out)->required_authority = authority;
	ret = anx_cap_set_authority_ceiling(*out, ceiling);
	if (ret == ANX_OK)
		ret = anx_cap_validate(*out);
	return ret;
}

int anx_research_day027(void)
{
	struct anx_capability *missing = NULL, *checked = NULL, *wide = NULL, *good = NULL;
	struct anx_engine *dependency = NULL;
	anx_eid_t absent;
	int ret;
	anx_uuid_generate(&absent);
	ret = anx_cap_create("research-day-027-missing", "1", &missing);
	if (ret != ANX_OK)
		goto out;
	missing->required_engines[0] = absent;
	missing->required_engine_count = 1;
	ret = -2701;
	if (anx_cap_validate(missing) != ANX_ENOENT || missing->status != ANX_CAP_DRAFT ||
	    missing->validation_score != 0 || !anx_uuid_is_nil(&missing->installed_engine_id))
		goto out;
	ret = anx_engine_register("research-day-027-dependency", ANX_ENGINE_DETERMINISTIC_TOOL,
				  ANX_CAP_SUMMARIZATION, &dependency);
	if (ret != ANX_OK)
		goto out;
	missing->required_engines[0] = dependency->eid;
	missing->validation_score = 73;
	for (uint32_t malformed = 0; malformed < 3; malformed++) {
		missing->required_engine_count = malformed == 0 ? ANX_CAP_REQUIRED_ENGINES_MAX + 1 : malformed;
		missing->required_engines[0] = malformed == 1 ? ANX_UUID_NIL : dependency->eid;
		missing->required_engines[1] = dependency->eid;
		ret = -2702;
		if (anx_cap_validate(missing) != ANX_EINVAL || missing->status != ANX_CAP_DRAFT ||
		    missing->validation_score != 73)
			goto out;
	}
	missing->required_engines[0] = dependency->eid;
	missing->required_engine_count = 1;
	anx_memset(missing->version, 'v', sizeof(missing->version));
	ret = -2707;
	if (anx_cap_validate(missing) != ANX_EINVAL || missing->status != ANX_CAP_DRAFT ||
	    missing->validation_score != 73)
		goto out;
	anx_strlcpy(missing->version, "1", sizeof(missing->version));
	missing->name[0] = 0;
	if (anx_cap_validate(missing) != ANX_EINVAL || missing->validation_score != 73)
		goto out;
	anx_strlcpy(missing->name, "research-day-027-missing", sizeof(missing->name));
	dependency->status = ANX_ENGINE_OFFLINE;
	ret = -2708;
	if (anx_cap_validate(missing) != ANX_EPERM || missing->status != ANX_CAP_DRAFT ||
	    missing->validation_score != 0)
		goto out;
	dependency->status = ANX_ENGINE_AVAILABLE;
	ret = anx_cap_validate(missing);
	if (ret != ANX_OK)
		goto out;
	missing->required_engine_count = ANX_CAP_REQUIRED_ENGINES_MAX + 1;
	ret = -2703;
	if (anx_cap_install(missing) != ANX_EINVAL || missing->status != ANX_CAP_VALIDATED ||
	    !anx_uuid_is_nil(&missing->installed_engine_id))
		goto out;
	missing->required_engine_count = 1;
	dependency->status = ANX_ENGINE_DRAINING;
	if (anx_cap_install(missing) != ANX_EPERM || missing->status != ANX_CAP_VALIDATED ||
	    !anx_uuid_is_nil(&missing->installed_engine_id))
		goto out;
	dependency->status = ANX_ENGINE_AVAILABLE;
	ret = anx_cap_install(missing);
	if (ret != ANX_OK)
		goto out;
	anx_cap_uninstall(missing);
	ret = package("research-day-027-checked", &dependency->eid, 0, 0, &checked);
	if (ret != ANX_OK)
		goto out;
	anx_engine_unregister(dependency);
	dependency = NULL;
	ret = -2704;
	if (anx_cap_install(checked) != ANX_ENOENT || checked->status != ANX_CAP_VALIDATED ||
	    !anx_uuid_is_nil(&checked->installed_engine_id))
		goto out;
	ret = anx_engine_register("research-day-027-replacement", ANX_ENGINE_DETERMINISTIC_TOOL,
				  ANX_CAP_SUMMARIZATION, &dependency);
	if (ret != ANX_OK)
		goto out;
	ret = package("research-day-027-wide", &dependency->eid,
		      ANX_CAP_AUTH_SIDE_EFFECT | ANX_CAP_AUTH_NETWORK, ANX_CAP_AUTH_SIDE_EFFECT, &wide);
	if (ret != ANX_OK)
		goto out;
	ret = -2705;
	if (anx_cap_install(wide) != ANX_EPERM || wide->status != ANX_CAP_VALIDATED ||
	    !anx_uuid_is_nil(&wide->installed_engine_id))
		goto out;
	dependency->status = ANX_ENGINE_DEGRADED;
	ret = package("research-day-027-good", &dependency->eid,
		      ANX_CAP_AUTH_SIDE_EFFECT, ANX_CAP_AUTH_SIDE_EFFECT, &good);
	if (ret != ANX_OK)
		goto out;
	ret = -2706;
	if (anx_cap_install(good) != ANX_OK || good->status != ANX_CAP_INSTALLED ||
	    anx_uuid_is_nil(&good->installed_engine_id) || !anx_engine_lookup(&good->installed_engine_id))
		goto out;
	ret = ANX_OK;
out:
	retire(good);
	retire(wide);
	retire(checked);
	retire(missing);
	if (dependency)
		anx_engine_unregister(dependency);
	return ret;
}
#endif
