/*
 * update.c — OS update channel: check, fetch, stage, and apply.
 *
 * Uses the HTTP client to contact a superrouter instance.  Downloaded
 * kernel images are stored in the disk object store under a fixed "pending
 * update" OID.  The actual overwrite of the boot image happens when the
 * system reboots and a future early-boot stage detects the pending object.
 */

#include <anx/types.h>
#include <anx/update.h>
#include <anx/http.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/objstore_disk.h>
#include <anx/arch.h>
#include <anx/kickstart.h>

/* Fixed OID for the pending-update staging object (type tag 0x55504454 = "UPDT") */
#define UPDATE_OBJ_TYPE		0x55504454u

static const anx_oid_t g_update_oid = {
	.hi = 0x414E580000000001ULL,	/* "ANX" + sequence 1 */
	.lo = 0x5550445400000001ULL,	/* "UPDT" + sequence 1 */
};

/* ------------------------------------------------------------------ */
/* Version helpers                                                     */
/* ------------------------------------------------------------------ */

const char *
anx_update_running_version(void)
{
	return "2026.4.29";
}

/*
 * Compare two version strings of the form YYYY.M.D (numeric comparison,
 * field by field).  Returns negative if a < b, 0 if equal, positive if a > b.
 */
static int
version_cmp(const char *a, const char *b)
{
	uint32_t fa, fb;

	while (*a && *b) {
		fa = 0;
		fb = 0;
		while (*a >= '0' && *a <= '9')
			fa = fa * 10 + (uint32_t)(*a++ - '0');
		while (*b >= '0' && *b <= '9')
			fb = fb * 10 + (uint32_t)(*b++ - '0');

		if (fa != fb)
			return (fa > fb) ? 1 : -1;

		if (*a == '.')
			a++;
		if (*b == '.')
			b++;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON field extraction                                       */
/* ------------------------------------------------------------------ */

/*
 * Find the value of a JSON string field: "key":"<value>" → copies value
 * into buf.  Returns buf pointer on success, NULL on not-found.
 */
static const char *
json_string_field(const char *json, const char *key, char *buf, uint32_t bufsz)
{
	char needle[64];
	const char *p;
	uint32_t n;

	anx_snprintf(needle, sizeof(needle), "\"%s\":\"", key);
	p = json;
	while (*p) {
		if (anx_strncmp(p, needle, (uint32_t)anx_strlen(needle)) == 0) {
			p += anx_strlen(needle);
			n = 0;
			while (*p && *p != '"' && n < bufsz - 1)
				buf[n++] = *p++;
			buf[n] = '\0';
			return buf;
		}
		p++;
	}
	return NULL;
}

/*
 * Find the value of a JSON number field: "key":<number> → parses uint32.
 * Returns ANX_OK on success.
 */
static int
json_uint_field(const char *json, const char *key, uint32_t *out)
{
	char needle[64];
	const char *p;
	uint32_t n;

	anx_snprintf(needle, sizeof(needle), "\"%s\":", key);
	p = json;
	while (*p) {
		if (anx_strncmp(p, needle, (uint32_t)anx_strlen(needle)) == 0) {
			p += anx_strlen(needle);
			while (*p == ' ')
				p++;
			n = 0;
			while (*p >= '0' && *p <= '9')
				n = n * 10 + (uint32_t)(*p++ - '0');
			*out = n;
			return ANX_OK;
		}
		p++;
	}
	return ANX_ENOENT;
}

/* ------------------------------------------------------------------ */
/* URL construction                                                    */
/* ------------------------------------------------------------------ */

static const char *
running_arch(void)
{
#ifdef __aarch64__
	return "arm64";
#elif defined(__x86_64__)
	return "x86_64";
#else
	return "unknown";
#endif
}

/*
 * Build /updates/<channel>/<arch>/<filename> into path[].
 */
static void
make_update_path(const char *channel, const char *arch,
		 const char *filename, char *path, uint32_t pathsz)
{
	anx_snprintf(path, pathsz, "/updates/%s/%s/%s", channel, arch, filename);
}

/*
 * Parse http://host[:port]/... from a server string.
 * Port defaults to 8420 (superrouter default) if not specified.
 */
static int
parse_server(const char *server, char host[128], uint16_t *port)
{
	const char *p = server;
	const char *slash;
	const char *colon;
	uint32_t hlen, n;

	if (anx_strncmp(p, "http://", 7) == 0)
		p += 7;

	slash = p;
	while (*slash && *slash != '/')
		slash++;

	colon = p;
	while (colon < slash && *colon != ':')
		colon++;

	if (*colon == ':') {
		hlen = (uint32_t)(colon - p);
		if (hlen == 0 || hlen >= 128)
			return ANX_EINVAL;
		anx_memcpy(host, p, hlen);
		host[hlen] = '\0';
		n = 0;
		colon++;
		while (colon < slash && *colon >= '0' && *colon <= '9')
			n = n * 10 + (uint32_t)(*colon++ - '0');
		*port = (uint16_t)n;
	} else {
		hlen = (uint32_t)(slash - p);
		if (hlen == 0 || hlen >= 128)
			return ANX_EINVAL;
		anx_memcpy(host, p, hlen);
		host[hlen] = '\0';
		*port = 8420;
	}
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int
anx_update_check(const char *server, const char *channel, const char *arch,
		 const char *auth_header,
		 struct anx_update_manifest *out)
{
	char			host[128];
	char			path[192];
	uint16_t		port;
	struct anx_http_response resp;
	int			ret;

	if (!server || !channel || !arch || !out)
		return ANX_EINVAL;

	ret = parse_server(server, host, &port);
	if (ret != ANX_OK)
		return ret;

	make_update_path(channel, arch, "manifest.json", path, sizeof(path));

	anx_memset(&resp, 0, sizeof(resp));
	if (auth_header)
		ret = anx_http_get_authed(host, port, path, auth_header, &resp);
	else
		ret = anx_http_get(host, port, path, &resp);

	if (ret != ANX_OK) {
		kprintf("update: manifest fetch failed (%d)\n", ret);
		return ret;
	}

	if (resp.status_code != 200) {
		kprintf("update: manifest HTTP %d\n", resp.status_code);
		anx_http_response_free(&resp);
		return ANX_EIO;
	}

	anx_memset(out, 0, sizeof(*out));
	if (!json_string_field(resp.body, "version",
			       out->version, ANX_UPDATE_VERSION_LEN)) {
		kprintf("update: manifest missing 'version' field\n");
		anx_http_response_free(&resp);
		return ANX_EINVAL;
	}
	json_uint_field(resp.body, "size", &out->size);

	anx_http_response_free(&resp);
	return ANX_OK;
}

int
anx_update_fetch(const char *server, const char *channel, const char *arch,
		 const char *auth_header)
{
	char			host[128];
	char			path[192];
	uint16_t		port;
	struct anx_http_response resp;
	int			ret;

	if (!server || !channel || !arch)
		return ANX_EINVAL;

	ret = parse_server(server, host, &port);
	if (ret != ANX_OK)
		return ret;

	make_update_path(channel, arch, "anunix.bin", path, sizeof(path));

	kprintf("update: fetching %s%s\n", server, path);

	anx_memset(&resp, 0, sizeof(resp));
	if (auth_header)
		ret = anx_http_get_authed(host, port, path, auth_header, &resp);
	else
		ret = anx_http_get(host, port, path, &resp);

	if (ret != ANX_OK) {
		kprintf("update: binary fetch failed (%d)\n", ret);
		return ret;
	}

	if (resp.status_code != 200) {
		kprintf("update: binary HTTP %d\n", resp.status_code);
		anx_http_response_free(&resp);
		return ANX_EIO;
	}

	kprintf("update: staging %u bytes on disk\n", resp.body_len);

	ret = anx_disk_write_obj(&g_update_oid, UPDATE_OBJ_TYPE,
				  resp.body, resp.body_len);
	anx_http_response_free(&resp);

	if (ret != ANX_OK) {
		kprintf("update: disk stage failed (%d)\n", ret);
		return ret;
	}

	kprintf("update: staged; reboot to apply\n");
	return ANX_OK;
}

int
anx_update_auto_apply(const char *server, const char *channel,
		      const char *auth_header)
{
	struct anx_update_manifest manifest;
	const char *running;
	int ret;

	if (!server || !channel)
		return ANX_EINVAL;

	running = anx_update_running_version();

	ret = anx_update_check(server, channel, running_arch(),
				auth_header, &manifest);
	if (ret != ANX_OK)
		return ret;

	kprintf("update: running=%s  available=%s\n", running, manifest.version);

	if (version_cmp(manifest.version, running) <= 0) {
		kprintf("update: already up to date\n");
		return ANX_OK;
	}

	kprintf("update: newer version available (%s), fetching...\n",
		manifest.version);

	ret = anx_update_fetch(server, channel, running_arch(), auth_header);
	if (ret != ANX_OK)
		return ret;

	anx_update_reboot();
}

void
anx_update_reboot(void)
{
	kprintf("update: rebooting to apply staged kernel...\n");

#ifdef __x86_64__
	/* Keyboard controller reset */
	__asm__ volatile(
		"outb %%al, $0x64\n\t"
		:
		: "a"((uint8_t)0xFE)
	);
	/* Triple-fault fallback */
	{
		struct { uint16_t limit; uint64_t base; }
			__attribute__((packed)) null_idt = {0, 0};
		__asm__ volatile("lidt %0" : : "m"(null_idt));
		__asm__ volatile("int3");
	}
#else
	/* ARM64: spin (PSCI reset requires EL2/EL3 setup) */
	for (;;)
		__asm__ volatile("wfe");
#endif
	__builtin_unreachable();
}
