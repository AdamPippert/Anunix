/*
 * test_cap_flow_label.c — Information-flow labels and Sinks (RFC-0028).
 */

#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/capability.h>
#include <anx/uuid.h>
#include <anx/string.h>

int test_cap_flow_label(void)
{
	struct anx_state_object *plain;
	struct anx_state_object *secret;
	struct anx_state_object *derived;
	struct anx_so_create_params params;
	struct anx_sink *low_sink;
	struct anx_sink *high_sink;
	int ret;

	anx_objstore_init();
	anx_sink_registry_init();

	/* --- Default sensitivity is PUBLIC with no origin --- */

	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_BYTE_DATA;
	ret = anx_so_create(&params, &plain);
	if (ret != ANX_OK)
		return -1;
	if (anx_object_get_sensitivity(&plain->oid) != ANX_SENSITIVITY_PUBLIC)
		return -2;
	if (!anx_uuid_is_nil(&plain->sensitivity_origin))
		return -3;

	/* --- Explicit set/get round-trip --- */

	ret = anx_object_set_sensitivity(&plain->oid, ANX_SENSITIVITY_CONFIDENTIAL);
	if (ret != ANX_OK)
		return -4;
	if (anx_object_get_sensitivity(&plain->oid) != ANX_SENSITIVITY_CONFIDENTIAL)
		return -5;
	/* Explicit set records nil origin (direct declaration, not inherited) */
	if (!anx_uuid_is_nil(&plain->sensitivity_origin))
		return -6;

	/* --- Unresolvable OID reads as PUBLIC --- */

	{
		anx_oid_t bogus;

		anx_uuid_generate(&bogus);
		if (anx_object_get_sensitivity(&bogus) != ANX_SENSITIVITY_PUBLIC)
			return -7;
	}

	/* --- Inheritance: derived object with no explicit override takes
	 * the max sensitivity among its parents --- */

	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_BYTE_DATA;
	ret = anx_so_create(&params, &secret);
	if (ret != ANX_OK)
		return -10;
	ret = anx_object_set_sensitivity(&secret->oid, ANX_SENSITIVITY_RESTRICTED);
	if (ret != ANX_OK)
		return -11;

	{
		anx_oid_t parents[2];

		parents[0] = plain->oid;	/* CONFIDENTIAL */
		parents[1] = secret->oid;	/* RESTRICTED */

		anx_memset(&params, 0, sizeof(params));
		params.object_type = ANX_OBJ_BYTE_DATA;
		params.parent_oids = parents;
		params.parent_count = 2;
		/* params.sensitivity left at PUBLIC (0) -> inherit */
		ret = anx_so_create(&params, &derived);
		if (ret != ANX_OK)
			return -12;
	}

	if (derived->sensitivity != ANX_SENSITIVITY_RESTRICTED)
		return -13;
	if (anx_uuid_compare(&derived->sensitivity_origin, &secret->oid) != 0)
		return -14;

	/* --- Sinks: registration and CAN_SEND --- */

	ret = anx_sink_register("low", ANX_SENSITIVITY_INTERNAL, &low_sink);
	if (ret != ANX_OK)
		return -20;
	ret = anx_sink_register("high", ANX_SENSITIVITY_RESTRICTED, &high_sink);
	if (ret != ANX_OK)
		return -21;

	if (anx_sink_lookup("low") != low_sink)
		return -22;
	if (anx_sink_lookup("nonexistent") != NULL)
		return -23;

	/* plain is CONFIDENTIAL: denied to "low" (max INTERNAL), allowed to "high" */
	if (anx_sink_check_send(low_sink, &plain->oid) != ANX_EPERM)
		return -24;
	if (anx_sink_check_send(high_sink, &plain->oid) != ANX_OK)
		return -25;

	/* derived is RESTRICTED: denied to "low", allowed to "high" (equal ceiling) */
	if (anx_sink_check_send(low_sink, &derived->oid) != ANX_EPERM)
		return -26;
	if (anx_sink_check_send(high_sink, &derived->oid) != ANX_OK)
		return -27;

	/* Re-registering an existing name updates it in place rather than
	 * creating a duplicate slot. */
	{
		struct anx_sink *again;

		ret = anx_sink_register("low", ANX_SENSITIVITY_RESTRICTED, &again);
		if (ret != ANX_OK)
			return -28;
		if (again != low_sink)
			return -29;
		if (anx_sink_check_send(low_sink, &plain->oid) != ANX_OK)
			return -30;
	}

	/* Bad args */
	if (anx_sink_check_send(NULL, &plain->oid) != ANX_EINVAL)
		return -31;
	if (anx_sink_register("", ANX_SENSITIVITY_PUBLIC, NULL) != ANX_EINVAL)
		return -32;

	anx_objstore_release(plain);
	anx_objstore_release(secret);
	anx_objstore_release(derived);

	return 0;
}
