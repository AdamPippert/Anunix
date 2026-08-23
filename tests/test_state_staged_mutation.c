/*
 * test_state_staged_mutation.c — Tests for staged State Object mutation
 * (RFC-0002/RFC-0003 Execution Contracts extension).
 */

#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/uuid.h>
#include <anx/string.h>

int test_state_staged_mutation(void)
{
	struct anx_state_object *obj;
	struct anx_object_handle handle;
	struct anx_so_create_params params;
	const char *initial = "original";
	const char *updated = "changed!!";	/* deliberately different length */
	anx_cid_t staging_cell = ANX_UUID_NIL;
	uint32_t prov_before;
	int ret;

	anx_objstore_init();

	/* Create */
	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = initial;
	params.payload_size = 8;

	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK)
		return -1;

	/* --- stage -> write -> commit round-trip --- */
	{
		uint64_t version_before = obj->version;
		char buf[16];

		ret = anx_so_open(&obj->oid, ANX_OPEN_WRITE, &handle);
		if (ret != ANX_OK)
			return -2;

		ret = anx_object_stage(&handle, staging_cell);
		if (ret != ANX_OK)
			return -3;

		prov_before = anx_prov_log_count(obj->provenance);

		ret = anx_so_replace_payload(&handle, updated, 9);
		if (ret != ANX_OK)
			return -4;

		/* Live payload must be unchanged while staged. */
		if (obj->payload_size != 8)
			return -5;
		if (anx_memcmp(obj->payload, initial, 8) != 0)
			return -6;
		if (obj->version != version_before)
			return -7;
		if (anx_prov_log_count(obj->provenance) != prov_before)
			return -8;	/* no MUTATED event yet */

		ret = anx_object_commit(&handle);
		if (ret != ANX_OK)
			return -9;

		/* Now the mutation is visible. */
		if (obj->payload_size != 9)
			return -10;
		if (anx_memcmp(obj->payload, updated, 9) != 0)
			return -11;
		if (obj->version != version_before + 1)
			return -12;
		if (anx_prov_log_count(obj->provenance) != prov_before + 1)
			return -13;

		anx_memset(buf, 0, sizeof(buf));
		ret = anx_so_read_payload(&handle, 0, buf, 9);
		if (ret != 9 || anx_memcmp(buf, updated, 9) != 0)
			return -14;

		if (obj->staged != NULL)
			return -15;	/* stage must be cleared after commit */

		anx_so_close(&handle);
	}

	/* --- stage -> write -> abort leaves live state untouched --- */
	{
		uint64_t version_before = obj->version;
		uint64_t size_before = obj->payload_size;
		struct anx_hash hash_before = obj->content_hash;
		uint32_t prov_count_before;

		ret = anx_so_open(&obj->oid, ANX_OPEN_WRITE, &handle);
		if (ret != ANX_OK)
			return -16;

		ret = anx_object_stage(&handle, staging_cell);
		if (ret != ANX_OK)
			return -17;

		prov_count_before = anx_prov_log_count(obj->provenance);

		ret = anx_so_write_payload(&handle, 0, "X", 1);
		if (ret != 1)
			return -18;

		ret = anx_object_abort(&handle);
		if (ret != ANX_OK)
			return -19;

		if (obj->version != version_before)
			return -20;
		if (obj->payload_size != size_before)
			return -21;
		if (anx_memcmp(obj->content_hash.bytes, hash_before.bytes,
			       sizeof(hash_before.bytes)) != 0)
			return -22;
		if (obj->staged != NULL)
			return -23;

		/* Abort is still recorded — evidence, not silence. */
		if (anx_prov_log_count(obj->provenance) != prov_count_before + 1)
			return -24;
		{
			const struct anx_prov_event *ev =
				anx_prov_log_get(obj->provenance,
						 anx_prov_log_count(obj->provenance) - 1);
			if (!ev || ev->event_type != ANX_PROV_STAGE_ABORTED)
				return -25;
		}

		/* A fresh stage can begin immediately after abort. */
		ret = anx_object_stage(&handle, staging_cell);
		if (ret != ANX_OK)
			return -26;
		ret = anx_object_abort(&handle);
		if (ret != ANX_OK)
			return -27;

		anx_so_close(&handle);
	}

	/* --- double-stage rejection --- */
	{
		ret = anx_so_open(&obj->oid, ANX_OPEN_WRITE, &handle);
		if (ret != ANX_OK)
			return -28;

		ret = anx_object_stage(&handle, staging_cell);
		if (ret != ANX_OK)
			return -29;

		ret = anx_object_stage(&handle, staging_cell);
		if (ret != ANX_EBUSY)
			return -30;

		/* First stage must still be intact. */
		if (obj->staged == NULL)
			return -31;

		ret = anx_object_abort(&handle);
		if (ret != ANX_OK)
			return -32;

		anx_so_close(&handle);
	}

	/* --- commit/abort with no active stage --- */
	{
		ret = anx_so_open(&obj->oid, ANX_OPEN_WRITE, &handle);
		if (ret != ANX_OK)
			return -33;

		if (anx_object_commit(&handle) != ANX_EINVAL)
			return -34;
		if (anx_object_abort(&handle) != ANX_EINVAL)
			return -35;

		anx_so_close(&handle);
	}

	/* --- seal/delete rejected while staged --- */
	{
		ret = anx_so_open(&obj->oid, ANX_OPEN_WRITE, &handle);
		if (ret != ANX_OK)
			return -36;

		ret = anx_object_stage(&handle, staging_cell);
		if (ret != ANX_OK)
			return -37;

		if (anx_so_seal(&obj->oid) != ANX_EBUSY)
			return -38;
		if (anx_so_delete(&obj->oid, false) != ANX_EBUSY)
			return -39;

		ret = anx_object_abort(&handle);
		if (ret != ANX_OK)
			return -40;

		anx_so_close(&handle);
	}

	ret = anx_so_delete(&obj->oid, false);
	if (ret != ANX_OK)
		return -41;

	return 0;
}
