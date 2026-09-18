/* Configuration validation, runtime application, and durable object replay. */
#include <anx/config.h>
#include <anx/theme.h>
#include <anx/wm.h>
#include <anx/input.h>
#include <anx/gui.h>
#include <anx/net.h>
#include <anx/model_client.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/objstore_disk.h>
#include <anx/mock_blk.h>
#include <anx/tools.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define CHECK(c) do { if (!(c)) { kprintf("config failed at %u\n", __LINE__); return -1; } } while (0)
static uint32_t dispatched_key;
static void record_key(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)arg;
	dispatched_key = key;
}

int test_config(void)
{
	struct anx_wm_tiling_config saved = anx_wm_tiling;
	struct anx_config_hotkey keys[ANX_CONFIG_HOTKEY_MAX];
	struct anx_model_endpoint ep;
	struct anx_net_config net;
	struct anx_state_object *obj;
	struct anx_blk_dev *disk;
	anx_oid_t oid, before;
	char text[ANX_CONFIG_TEXT_MAX], small[4];
	uint32_t i;
	int count;

	CHECK(anx_config_set("unknown", "x=1") == ANX_ENOENT);
	CHECK(anx_config_set("tiling", "gaps_in=17;unknown=1") == ANX_EINVAL);
	CHECK(anx_wm_tiling.gaps_in == saved.gaps_in);
	CHECK(anx_config_set("tiling", "gaps_in=17;gaps_in=18") == ANX_EINVAL);
	CHECK(anx_config_set("tiling", "gaps_in=4294967296") == ANX_EINVAL);
	CHECK(anx_config_set("tiling", "enabled=yes") == ANX_EINVAL);
	CHECK(anx_config_set("tiling", "split_ratio=99") == ANX_EINVAL);
	CHECK(anx_config_set("tiling", "enabled=false\ngaps_in=17;split_ratio=700") == ANX_OK);
	CHECK(!anx_wm_tiling.enabled && anx_wm_tiling.gaps_in == 17 && anx_wm_tiling.split_ratio == 700);
	CHECK(anx_config_show("tiling", small, sizeof(small)) == ANX_ERANGE);
	CHECK(anx_config_show("tiling", small, 1) == ANX_ERANGE);
	CHECK(anx_config_show("tiling", text, sizeof(text)) > 0);
	CHECK(anx_config_set("tiling", text) == ANX_OK);
	CHECK(anx_config_set("timezone", "offset_hours=-7") == ANX_OK);
	CHECK(anx_gui_get_tz_offset() == -7);
	CHECK(anx_config_set("timezone", "offset_hours=-13") == ANX_EINVAL);
	CHECK(anx_gui_get_tz_offset() == -7);

	anx_wm_hotkeys_init();
	CHECK(anx_config_show("hotkeys", text, sizeof(text)) > 0);
	CHECK(anx_config_set("hotkeys", text) == ANX_OK);
	CHECK(anx_config_set("hotkeys", "shell=8,20") == ANX_EEXIST);
	CHECK(anx_config_set("hotkeys", "shell=16,40") == ANX_EINVAL);
	CHECK(anx_config_set("hotkeys", "shell=8,100") == ANX_OK);
	count = anx_wm_hotkeys_snapshot(keys, ANX_CONFIG_HOTKEY_MAX);
	CHECK(count > 0);
	for (i = 0; i < (uint32_t)count; i++)
		if (!anx_strcmp(anx_wm_hotkey_name(i), "shell")) break;
	CHECK(i < (uint32_t)count && keys[i].keycode == 100);
	CHECK(anx_wm_hotkey_register(ANX_MOD_META, 101, record_key, NULL) == ANX_OK);
	count = anx_wm_hotkeys_snapshot(keys, ANX_CONFIG_HOTKEY_MAX);
	CHECK(count > 0);
	keys[count - 1].keycode = 102;
	CHECK(anx_wm_hotkeys_apply(keys, (uint32_t)count) == ANX_OK);
	CHECK(anx_wm_hotkey_dispatch(ANX_MOD_META | ANX_MOD_CAPSLOCK, 102));
	CHECK(dispatched_key == 101); /* Directional actions retain their meaning. */
	anx_wm_hotkeys_init();

	CHECK(anx_config_set("model", "enabled=true;host=10.0.2.2;port=8080;credential=model_key") == ANX_OK);
	CHECK(anx_model_client_ready());
	CHECK(anx_config_set("model", "host=other;port=70000") == ANX_EINVAL);
	anx_model_client_get_endpoint(&ep);
	CHECK(!anx_strcmp(ep.host, "10.0.2.2") && ep.port == 8080);
	CHECK(anx_config_set("model", "host=bad host") == ANX_EINVAL);
	CHECK(anx_config_set("model", "api_key=secret") == ANX_EINVAL);
	CHECK(anx_config_set("network", "mode=static;ip=10.1.2.3;netmask=255.255.255.0;gateway=10.1.2.1;dns=1.1.1.1") == ANX_OK);
	anx_ipv4_get_config(&net);
	CHECK(net.ip == 0x0a010203 && net.dns == 0x01010101);
	CHECK(anx_config_set("network", "ip=256.1.2.3") == ANX_EINVAL);
	CHECK(anx_config_set("network", "netmask=255.0.255.0") == ANX_EINVAL);
	CHECK(anx_config_set("network", "gateway=10.9.2.1") == ANX_EINVAL);
	anx_ipv4_get_config(&net);
	CHECK(net.ip == 0x0a010203 && net.netmask == 0xffffff00);

	test_mock_blk_init(16384);
	CHECK(anx_disk_format("config") == ANX_OK);
	CHECK(anx_config_save("tiling") == ANX_OK);
	CHECK(anx_ns_resolve("system", "config/tiling", &before) == ANX_OK);
	obj = anx_objstore_lookup(&before);
	CHECK(obj && obj->payload_size > 0 && ((char *)obj->payload)[0] == 'e');
	anx_objstore_release(obj);
	CHECK(anx_config_set("tiling", "gaps_in=3") == ANX_OK);
	disk = anx_blk_active();
	anx_blk_set_active(NULL);
	CHECK(anx_config_save("tiling") == ANX_EIO);
	CHECK(anx_ns_resolve("system", "config/tiling", &oid) == ANX_OK);
	CHECK(oid.hi == before.hi && oid.lo == before.lo);
	CHECK(anx_model_client_clear() == ANX_EIO);
	CHECK(anx_model_client_ready());
	anx_blk_set_active(disk);
	CHECK(anx_ns_unbind("system", "config/tiling") == ANX_OK);
	anx_uobj_load();
	CHECK(anx_config_load("tiling") == ANX_OK);
	CHECK(anx_wm_tiling.gaps_in == 17);
	CHECK(anx_config_save("model") == ANX_OK);
	CHECK(anx_model_client_clear() == ANX_OK);
	CHECK(!anx_model_client_ready());
	CHECK(anx_config_show("model", text, sizeof(text)) > 0);
	CHECK(!anx_strcmp(text, "enabled=false\n"));
	anx_uobj_load();
	anx_model_client_load();
	CHECK(!anx_model_client_ready());

	/* An object containing a valid prefix followed by NUL is not accepted. */
	{
		struct anx_so_create_params p = {0};
		p.object_type = ANX_OBJ_BYTE_DATA;
		p.payload = "gaps_in=2\0hidden";
		p.payload_size = 16;
		CHECK(anx_so_create(&p, &obj) == ANX_OK);
		CHECK(anx_ns_bind("system", "config/tiling", &obj->oid) == ANX_OK);
		anx_objstore_release(obj);
		CHECK(anx_config_load("tiling") == ANX_EINVAL);
		CHECK(anx_wm_tiling.gaps_in == 17);
	}
	anx_wm_tiling = saved;
	anx_gui_set_tz_offset(0);
	return 0;
}
