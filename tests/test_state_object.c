/*
 * test_state_object.c — Tests for State Object Layer (RFC-0002).
 */

#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/access.h>
#include <anx/uuid.h>
#include <anx/string.h>

int test_state_object(void)
{
	struct anx_state_object *obj;
	struct anx_object_handle handle;
	struct anx_so_create_params params;
	const char *payload = "hello anunix";
	char buf[64];
	int ret;

	anx_objstore_init();

	/* Create */
	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = payload;
	params.payload_size = 12;

	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK)
		return -1;

	if (obj->state != ANX_OBJ_ACTIVE)
		return -2;
	if (obj->payload_size != 12)
		return -3;

	/* Open and read */
	ret = anx_so_open(&obj->oid, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK)
		return -4;

	anx_memset(buf, 0, sizeof(buf));
	ret = anx_so_read_payload(&handle, 0, buf, 12);
	if (ret < 0)
		return -5;

	/* Verify payload content */
	if (anx_memcmp(buf, payload, 12) != 0)
		return -6;

	anx_so_close(&handle);

	/* Seal */
	ret = anx_so_seal(&obj->oid);
	if (ret != ANX_OK)
		return -7;

	if (obj->state != ANX_OBJ_SEALED)
		return -8;

	/* Write should fail on sealed object */
	ret = anx_so_open(&obj->oid, ANX_OPEN_WRITE, &handle);
	if (ret == ANX_OK) {
		ret = anx_so_write_payload(&handle, 0, "x", 1);
		anx_so_close(&handle);
		if (ret == ANX_OK)
			return -9;	/* should have failed */
	}

	/* Delete */
	ret = anx_so_delete(&obj->oid, false);
	if (ret != ANX_OK)
		return -10;

	/* ------------------------------------------------------------------ */
	/* Access policy enforcement (Phase 14)                               */
	/* ------------------------------------------------------------------ */

	/* Test 11: DENY rule on nil principal blocks read open */
	{
		struct anx_state_object *obj2;
		struct anx_object_handle h2;

		anx_memset(&params, 0, sizeof(params));
		params.object_type   = ANX_OBJ_BYTE_DATA;
		params.payload       = "secret";
		params.payload_size  = 6;

		ret = anx_so_create(&params, &obj2);
		if (ret != ANX_OK)
			return -11;

		obj2->access_policy.rules[0].principal  = ANX_UUID_NIL;
		obj2->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
		obj2->access_policy.rules[0].effect     = ANX_EFFECT_DENY;
		obj2->access_policy.rule_count = 1;

		ret = anx_so_open(&obj2->oid, ANX_OPEN_READ, &h2);
		if (ret == ANX_OK) {
			anx_so_close(&h2);
			return -11;	/* must have been denied */
		}
	}

	/* Test 12: ALLOW before DENY permits access (first-match-wins) */
	{
		struct anx_state_object *obj3;
		struct anx_object_handle h3;

		anx_memset(&params, 0, sizeof(params));
		params.object_type   = ANX_OBJ_BYTE_DATA;
		params.payload       = "data";
		params.payload_size  = 4;

		ret = anx_so_create(&params, &obj3);
		if (ret != ANX_OK)
			return -12;

		obj3->access_policy.rules[0].principal  = ANX_UUID_NIL;
		obj3->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
		obj3->access_policy.rules[0].effect     = ANX_EFFECT_ALLOW;
		obj3->access_policy.rules[1].principal  = ANX_UUID_NIL;
		obj3->access_policy.rules[1].operations = ANX_ACCESS_READ_PAYLOAD;
		obj3->access_policy.rules[1].effect     = ANX_EFFECT_DENY;
		obj3->access_policy.rule_count = 2;

		ret = anx_so_open(&obj3->oid, ANX_OPEN_READ, &h3);
		if (ret != ANX_OK)
			return -12;
		anx_so_close(&h3);
	}

	/* Test 13: op-mask mismatch means rule is skipped (write op, read rule) */
	{
		struct anx_state_object *obj4;
		struct anx_object_handle h4;

		anx_memset(&params, 0, sizeof(params));
		params.object_type   = ANX_OBJ_BYTE_DATA;
		params.payload       = "rw";
		params.payload_size  = 2;

		ret = anx_so_create(&params, &obj4);
		if (ret != ANX_OK)
			return -13;

		/* Only deny READ, not WRITE — write open should succeed */
		obj4->access_policy.rules[0].principal  = ANX_UUID_NIL;
		obj4->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
		obj4->access_policy.rules[0].effect     = ANX_EFFECT_DENY;
		obj4->access_policy.rule_count = 1;

		ret = anx_so_open(&obj4->oid, ANX_OPEN_READWRITE, &h4);
		if (ret != ANX_OK)
			return -13;
		anx_so_close(&h4);
	}

	return 0;
}
