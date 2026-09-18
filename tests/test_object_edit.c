/* Named-object byte editing and checked disk persistence. */
#include <anx/types.h>
#include <anx/tools.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/objstore_disk.h>
#include <anx/mock_blk.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define CHECK(c) do { if (!(c)) { kprintf("object edit failed at %u\n", __LINE__); return -1; } } while (0)

int test_object_edit(void)
{
	struct anx_so_create_params p = {0};
	struct anx_state_object *obj, *restored;
	anx_oid_t oid;
	struct anx_blk_dev *disk;

	test_mock_blk_init(16384);
	CHECK(anx_disk_format("object-edit") == ANX_OK);
	p.object_type = ANX_OBJ_BYTE_DATA;
	p.payload = "hello world";
	p.payload_size = 11;
	CHECK(anx_so_create(&p, &obj) == ANX_OK);
	CHECK(anx_ns_bind("default", "edit-test", &obj->oid) == ANX_OK);
	CHECK(anx_uobj_write_at("default:edit-test", 6, "earth", 5) == ANX_OK);
	CHECK(!anx_memcmp(obj->payload, "hello earth", 11));
	CHECK(anx_ns_resolve("default", "edit-test", &oid) == ANX_OK);
	CHECK(oid.hi == obj->oid.hi && oid.lo == obj->oid.lo);
	CHECK(anx_uobj_write_at("default:edit-test", 10, "XX", 2) == ANX_EINVAL);
	CHECK(anx_uobj_write_at("default:edit-test", 0xffffffffu, "X", 1) == ANX_EINVAL);
	CHECK(!anx_memcmp(obj->payload, "hello earth", 11));
	CHECK(anx_uobj_record_checked("default", "edit-test", "X", 2049) == ANX_EINVAL);
	disk = anx_blk_active();
	anx_blk_set_active(NULL);
	CHECK(anx_uobj_write_at("edit-test", 0, "H", 1) == ANX_EIO);
	CHECK(!anx_memcmp(obj->payload, "Hello earth", 11));
	anx_blk_set_active(disk);
	anx_uobj_load();
	CHECK(anx_ns_resolve("default", "edit-test", &oid) == ANX_OK);
	restored = anx_objstore_lookup(&oid);
	CHECK(restored && restored->payload_size == 11);
	CHECK(!anx_memcmp(restored->payload, "hello earth", 11));
	CHECK(anx_so_seal(&restored->oid) == ANX_OK);
	CHECK(anx_uobj_write_at("edit-test", 0, "X", 1) == ANX_EPERM);
	anx_objstore_release(restored);
	anx_objstore_release(obj);
	anx_ns_unbind("default", "edit-test");
	uobj_remove("default", "/edit-test");
	anx_uobj_load();
	CHECK(anx_ns_resolve("default", "edit-test", &oid) == ANX_ENOENT);
	return 0;
}
