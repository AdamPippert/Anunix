/* Live, named configuration documents. Secrets stay in the credential store. */
#include <anx/config.h>
#include <anx/config_templates.h>
#include <anx/theme.h>
#include <anx/wm.h>
#include <anx/gui.h>
#include <anx/net.h>
#include <anx/model_client.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/tools.h>
#include <anx/string.h>
#include <anx/kprintf.h>

static const char *const names[] = {
	"theme", "tiling", "hotkeys", "timezone", "model", "network"
};
static bool network_dhcp = true;

const char *anx_config_name(uint32_t index)
{
	return index < sizeof(names) / sizeof(names[0]) ? names[index] : NULL;
}

static int config_index(const char *name)
{
	uint32_t i;
	if (!name)
		return ANX_EINVAL;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!anx_strcmp(name, names[i]))
			return (int)i;
	return ANX_ENOENT;
}

static int number(const char *s, uint32_t max, uint32_t *out)
{
	uint32_t n = 0;
	if (!*s)
		return ANX_EINVAL;
	for (; *s; s++) {
		if (*s < '0' || *s > '9' || n > max / 10 ||
		    (n == max / 10 && (uint32_t)(*s - '0') > max % 10))
			return ANX_EINVAL;
		n = n * 10 + (uint32_t)(*s - '0');
	}
	*out = n;
	return ANX_OK;
}

static int boolean(const char *s, bool *out)
{
	if (!anx_strcmp(s, "true"))
		*out = true;
	else if (!anx_strcmp(s, "false"))
		*out = false;
	else
		return ANX_EINVAL;
	return ANX_OK;
}

static int address(const char *s, uint32_t *out)
{
	uint32_t ip = 0, i;
	for (i = 0; i < 4; i++) {
		char octet[4];
		uint32_t n = 0, v;
		while (*s && *s != '.' && n < 3)
			octet[n++] = *s++;
		octet[n] = 0;
		if (number(octet, 255, &v) || (i < 3 ? *s != '.' : *s != 0))
			return ANX_EINVAL;
		ip = (ip << 8) | v;
		if (i < 3)
			s++;
	}
	*out = ip;
	return ANX_OK;
}

struct pending_config {
	int kind;
	struct anx_wm_tiling_config tiling;
	struct anx_config_hotkey keys[ANX_CONFIG_HOTKEY_MAX];
	uint32_t key_count;
	int32_t timezone;
	bool model_enabled;
	char host[128];
	char credential[128];
	uint16_t port;
	bool dhcp;
	struct anx_net_config net;
};

static int set_pair(struct pending_config *p, const char *key, const char *value)
{
	uint32_t n;
	if (p->kind == 1) {
		if (!anx_strcmp(key, "enabled"))
			return boolean(value, &p->tiling.enabled);
		if (number(value, 900, &n))
			return ANX_EINVAL;
		if (!anx_strcmp(key, "gaps_in") && n <= 128)
			p->tiling.gaps_in = n;
		else if (!anx_strcmp(key, "gaps_out") && n <= 128)
			p->tiling.gaps_out = n;
		else if (!anx_strcmp(key, "border_w") && n <= 16)
			p->tiling.border_w = n;
		else if (!anx_strcmp(key, "split_ratio") && n >= 100)
			p->tiling.split_ratio = n;
		else if (!anx_strcmp(key, "resize_step") && n >= 1 && n <= 400)
			p->tiling.resize_step = n;
		else
			return ANX_EINVAL;
	} else if (p->kind == 2) {
		uint32_t i, mods, code;
		char chord[32], *comma;
		if (anx_strlcpy(chord, value, sizeof(chord)) >= sizeof(chord))
			return ANX_EINVAL;
		for (comma = chord; *comma && *comma != ','; comma++) {}
		if (*comma != ',')
			return ANX_EINVAL;
		*comma++ = 0;
		if (number(chord, 15, &mods) || number(comma, 0xDF, &code) || code < 4)
			return ANX_EINVAL;
		for (i = 0; i < p->key_count; i++)
			if (!anx_strcmp(key, anx_wm_hotkey_name(i))) {
				p->keys[i].modifiers = mods;
				p->keys[i].keycode = code;
				return ANX_OK;
			}
		return ANX_EINVAL;
	} else if (p->kind == 3) {
		bool negative = *value == '-';
		if (anx_strcmp(key, "offset_hours") ||
		    number(value + (negative ? 1 : 0), 14, &n) ||
		    (negative && n > 12))
			return ANX_EINVAL;
		p->timezone = negative ? -(int32_t)n : (int32_t)n;
	} else if (p->kind == 4) {
		if (!anx_strcmp(key, "enabled"))
			return boolean(value, &p->model_enabled);
		if (!anx_strcmp(key, "host")) {
			if (anx_strlcpy(p->host, value, sizeof(p->host)) >= sizeof(p->host))
				return ANX_EINVAL;
		} else if (!anx_strcmp(key, "credential")) {
			if (anx_strlcpy(p->credential, value, sizeof(p->credential)) >= sizeof(p->credential))
				return ANX_EINVAL;
		} else if (!anx_strcmp(key, "port") && !number(value, 65535, &n) && n) {
			p->port = (uint16_t)n;
		} else {
			return ANX_EINVAL;
		}
	} else if (p->kind == 5) {
		if (!anx_strcmp(key, "mode")) {
			if (anx_strcmp(value, "dhcp") && anx_strcmp(value, "static"))
				return ANX_EINVAL;
			p->dhcp = !anx_strcmp(value, "dhcp");
		} else {
			if (address(value, &n))
				return ANX_EINVAL;
			if (!anx_strcmp(key, "ip")) p->net.ip = n;
			else if (!anx_strcmp(key, "netmask")) p->net.netmask = n;
			else if (!anx_strcmp(key, "gateway")) p->net.gateway = n;
			else if (!anx_strcmp(key, "dns")) p->net.dns = n;
			else return ANX_EINVAL;
		}
	} else {
		return ANX_EINVAL;
	}
	return ANX_OK;
}

/* Parse into a candidate: malformed or duplicate fields cannot partially apply. */
static int parse(struct pending_config *p, const char *text)
{
	char seen[64][32];
	uint32_t count = 0;
	while (*text) {
		char pair[192], *eq;
		uint32_t len = 0, i;
		while (*text && *text != ';' && *text != '\n') {
			if (len + 1 >= sizeof(pair)) return ANX_EINVAL;
			pair[len++] = *text++;
		}
		if (*text) text++;
		if (!len) continue;
		pair[len] = 0;
		for (eq = pair; *eq && *eq != '='; eq++) {}
		if (*eq != '=' || eq == pair || (uint32_t)(eq - pair) >= sizeof(seen[0]))
			return ANX_EINVAL;
		*eq++ = 0;
		for (i = 0; i < count; i++)
			if (!anx_strcmp(seen[i], pair)) return ANX_EINVAL;
		if (count == 64) return ANX_EINVAL;
		anx_strlcpy(seen[count++], pair, sizeof(seen[0]));
		if (set_pair(p, pair, eq)) return ANX_EINVAL;
	}
	return count ? ANX_OK : ANX_EINVAL;
}

static int config_set(const char *name, const char *text, bool reuse_lease)
{
	struct pending_config p;
	struct anx_model_endpoint ep;
	int rc, kind = config_index(name);
	if (kind < 0) return kind;
	if (!text || anx_strlen(text) >= ANX_CONFIG_TEXT_MAX) return ANX_EINVAL;
	if (!kind) {
		rc = anx_theme_apply_config_checked(text);
		if (!rc) anx_wm_repaint_all();
		return rc;
	}
	anx_memset(&p, 0, sizeof(p));
	p.kind = kind;
	p.tiling = anx_wm_tiling;
	p.timezone = anx_gui_get_tz_offset();
	p.dhcp = network_dhcp;
	anx_ipv4_get_config(&p.net);
	reuse_lease = reuse_lease && p.net.ip != 0;
	anx_model_client_get_endpoint(&ep);
	p.model_enabled = anx_model_client_ready();
	anx_strlcpy(p.host, ep.host, sizeof(p.host));
	anx_strlcpy(p.credential, ep.cred_name, sizeof(p.credential));
	p.port = ep.port;
	if (kind == 2) {
		rc = anx_wm_hotkeys_snapshot(p.keys, ANX_CONFIG_HOTKEY_MAX);
		if (rc < 0) return rc;
		p.key_count = (uint32_t)rc;
	}
	rc = parse(&p, text);
	if (rc) return rc;
	switch (kind) {
	case 1:
		anx_wm_tiling = p.tiling;
		anx_wm_retile();
		anx_wm_repaint_all();
		break;
	case 2:
		return anx_wm_hotkeys_apply(p.keys, p.key_count);
	case 3:
		anx_gui_set_tz_offset(p.timezone);
		break;
	case 4:
		ep.host = p.host; ep.port = p.port; ep.cred_name = p.credential;
		/* Validate even disabled fields so config cannot carry hidden junk. */
		if ((p.host[0] || p.credential[0] || p.model_enabled) &&
		    anx_model_endpoint_validate(&ep)) return ANX_EINVAL;
		return anx_model_client_configure(p.model_enabled ? &ep : NULL);
	case 5:
		if (!p.dhcp) {
			uint32_t inverse = ~p.net.netmask;
			if (!p.net.ip || !p.net.netmask || (inverse & (inverse + 1)) ||
			    (p.net.gateway && (p.net.gateway & p.net.netmask) !=
			     (p.net.ip & p.net.netmask))) return ANX_EINVAL;
			anx_net_configure(&p.net);
		} else if (!reuse_lease) {
			rc = anx_net_dhcp();
			if (rc) return rc;
		}
		network_dhcp = p.dhcp;
		break;
	}
	return ANX_OK;
}

int anx_config_set(const char *name, const char *text)
{
	return config_set(name, text, false);
}

#define EMIT(...) do { \
	int n = anx_snprintf(out + used, capacity - used, __VA_ARGS__); \
	if (n < 0 || (uint32_t)n >= capacity - used - 1) return ANX_ERANGE; \
	used += (uint32_t)n; \
} while (0)
#define IP_ARGS(v) ((v) >> 24) & 255u, ((v) >> 16) & 255u, ((v) >> 8) & 255u, (v) & 255u

int anx_config_show(const char *name, char *out, uint32_t capacity)
{
	uint32_t used = 0;
	int kind = config_index(name);
	if (kind < 0) return kind;
	if (!out || !capacity) return ANX_EINVAL;
	if (!kind) return anx_theme_serialize(out, capacity);
	out[0] = 0;
	switch (kind) {
	case 1:
		EMIT("enabled=%s\ngaps_in=%u\ngaps_out=%u\nborder_w=%u\nsplit_ratio=%u\nresize_step=%u\n",
		     anx_wm_tiling.enabled ? "true" : "false", anx_wm_tiling.gaps_in,
		     anx_wm_tiling.gaps_out, anx_wm_tiling.border_w,
		     anx_wm_tiling.split_ratio, anx_wm_tiling.resize_step);
		break;
	case 2: {
		struct anx_config_hotkey keys[ANX_CONFIG_HOTKEY_MAX];
		int count = anx_wm_hotkeys_snapshot(keys, ANX_CONFIG_HOTKEY_MAX), i;
		if (count < 0) return count;
		for (i = 0; i < count; i++)
			EMIT("%s=%u,%u\n", anx_wm_hotkey_name((uint32_t)i),
			     keys[i].modifiers, keys[i].keycode);
		break;
	}
	case 3:
		EMIT("offset_hours=%d\n", anx_gui_get_tz_offset());
		break;
	case 4: {
		struct anx_model_endpoint ep;
		anx_model_client_get_endpoint(&ep);
		EMIT("enabled=%s\n", anx_model_client_ready() ? "true" : "false");
		if (anx_model_client_ready())
			EMIT("host=%s\nport=%u\ncredential=%s\n", ep.host, (uint32_t)ep.port, ep.cred_name);
		break;
	}
	case 5: {
		struct anx_net_config net;
		anx_ipv4_get_config(&net);
		EMIT("mode=%s\n", network_dhcp ? "dhcp" : "static");
		EMIT("ip=%u.%u.%u.%u\n", IP_ARGS(net.ip));
		EMIT("netmask=%u.%u.%u.%u\n", IP_ARGS(net.netmask));
		EMIT("gateway=%u.%u.%u.%u\n", IP_ARGS(net.gateway));
		EMIT("dns=%u.%u.%u.%u\n", IP_ARGS(net.dns));
		break;
	}
	}
	return (int)used;
}

int anx_config_save(const char *name)
{
	char text[ANX_CONFIG_TEXT_MAX], path[64];
	struct anx_so_create_params params;
	struct anx_state_object *obj;
	anx_oid_t old;
	int rc, len = anx_config_show(name, text, sizeof(text));
	bool had_old;
	if (len < 0) return len;
	anx_snprintf(path, sizeof(path), "config/%s", name);
	had_old = anx_ns_resolve("system", path, &old) == ANX_OK;
	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = text;
	params.payload_size = (uint32_t)len;
	rc = anx_so_create(&params, &obj);
	if (rc) return rc;
	rc = anx_ns_bind("system", path, &obj->oid);
	if (!rc) {
		rc = anx_uobj_record_checked("system", path, text, (uint32_t)len);
		if (rc) {
			if (had_old) anx_ns_bind("system", path, &old);
			else anx_ns_unbind("system", path);
		}
	}
	anx_objstore_release(obj);
	return rc;
}

static int config_load(const char *name, bool reuse_lease)
{
	char text[ANX_CONFIG_TEXT_MAX], path[64];
	struct anx_state_object *obj;
	anx_oid_t oid;
	int rc = config_index(name);
	uint32_t i;
	if (rc < 0) return rc;
	anx_snprintf(path, sizeof(path), "config/%s", name);
	rc = anx_ns_resolve("system", path, &oid);
	if (rc) return rc;
	obj = anx_objstore_lookup(&oid);
	if (!obj) return ANX_ENOENT;
	if (!obj->payload || !obj->payload_size || obj->payload_size >= sizeof(text)) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}
	anx_memcpy(text, obj->payload, obj->payload_size);
	text[obj->payload_size] = 0;
	for (i = 0; i < obj->payload_size; i++)
		if (!text[i]) {
			anx_objstore_release(obj);
			return ANX_EINVAL;
		}
	anx_objstore_release(obj);
	return config_set(name, text, reuse_lease);
}

int anx_config_load(const char *name)
{
	return config_load(name, false);
}

int anx_config_load_network(void)
{
	return config_load("network", true);
}

void anx_config_load_desktop(void)
{
	uint32_t i;
	for (i = 0; i < 4; i++) {
		int rc = anx_config_load(names[i]);
		if (rc != ANX_OK && rc != ANX_ENOENT)
			kprintf("config: cannot load %s (%d)\n", names[i], rc);
	}
}

int cmd_config(int argc, char **argv)
{
	int rc;
	if (argc == 2 && !anx_strcmp(argv[1], "templates")) {
		uint32_t i;
		const char *name;
		for (i = 0; (name = anx_config_template_name(i)) != NULL; i++)
			kprintf("%s: %s\n", name, anx_config_template_describe(name));
		return ANX_OK;
	}
	if (argc == 2 && !anx_strcmp(argv[1], "list")) {
		uint32_t i;
		for (i = 0; anx_config_name(i); i++)
			kprintf("system:config/%s\n", anx_config_name(i));
		return ANX_OK;
	}
	if (argc == 3 && !anx_strcmp(argv[1], "show")) {
		char text[ANX_CONFIG_TEXT_MAX];
		rc = anx_config_show(argv[2], text, sizeof(text));
		if (rc >= 0) { kprintf("%s", text); return ANX_OK; }
	} else if (argc == 4 && !anx_strcmp(argv[1], "set")) {
		rc = anx_config_set(argv[2], argv[3]);
	} else if (argc == 3 && !anx_strcmp(argv[1], "save")) {
		rc = anx_config_save(argv[2]);
	} else if (argc == 3 && !anx_strcmp(argv[1], "load")) {
		rc = anx_config_load(argv[2]);
	} else if (argc == 3 && !anx_strcmp(argv[1], "apply")) {
		rc = anx_config_template_apply(argv[2]);
	} else {
		kprintf("usage: config list | show/save/load <name> | set <name> 'key=value;...' | apply <template>\n");
		return ANX_EINVAL;
	}
	if (rc) kprintf("config: %s failed (%d)\n", argv[1], rc);
	else kprintf("config: %s %s ok\n", argv[1], argv[2]);
	return rc;
}
