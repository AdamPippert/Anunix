/*
 * test_state_object.c — Tests for State Object Layer (RFC-0002).
 */

#include <anx/types.h>
#include <anx/state_object.h>
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

	return 0;
}
