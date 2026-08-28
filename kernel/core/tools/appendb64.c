/*
 * appendb64.c — Append base64-decoded bytes to a State Object.
 *
 * Shell command lines are capped at 256 bytes (MAX_LINE in shell.c),
 * far too small to inject a whole binary in one write. This command
 * lets a caller (shell or the /api/v1/exec HTTP endpoint) build up an
 * arbitrary-length binary-safe payload across repeated calls, unlike
 * `write` (cmd_write_obj), which uses anx_strlen() and can't carry
 * embedded NUL bytes.
 *
 * USAGE
 *   appendb64 <ns:path> <base64-chunk>
 *   appendb64 posix:/bin/hello AAAA...   (repeat to build up the file)
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/base64.h>

#define APPENDB64_CHUNK_MAX	256

void cmd_appendb64(int argc, char **argv)
{
	const char *ns_name = "posix";
	const char *path = NULL;
	const char *b64 = NULL;
	anx_oid_t oid;
	struct anx_state_object *obj;
	struct anx_object_handle handle;
	uint8_t decoded[APPENDB64_CHUNK_MAX];
	size_t decoded_len;
	uint64_t offset;
	int ret;

	if (argc < 3) {
		kprintf("usage: appendb64 [ns:]<path> <base64-chunk>\n");
		return;
	}

	{
		const char *arg = argv[1];
		const char *colon = arg;
		static char ns_buf[64];

		while (*colon && *colon != ':')
			colon++;
		if (*colon == ':') {
			uint32_t nlen = (uint32_t)(colon - arg);

			if (nlen < sizeof(ns_buf)) {
				anx_memcpy(ns_buf, arg, nlen);
				ns_buf[nlen] = '\0';
				ns_name = ns_buf;
			}
			path = colon + 1;
		} else {
			path = arg;
		}
	}
	b64 = argv[2];

	decoded_len = anx_base64_decode(b64, anx_strlen(b64), decoded, sizeof(decoded));
	if (decoded_len == 0 && anx_strlen(b64) > 0) {
		kprintf("appendb64: base64 decode failed\n");
		return;
	}

	ret = anx_ns_resolve(ns_name, path, &oid);
	if (ret != ANX_OK) {
		struct anx_so_create_params params;

		anx_memset(&params, 0, sizeof(params));
		params.object_type = ANX_OBJ_BYTE_DATA;
		ret = anx_so_create(&params, &obj);
		if (ret != ANX_OK) {
			kprintf("appendb64: create failed (%d)\n", ret);
			return;
		}
		kprintf("appendb64: dbg fresh obj state=%d right after create\n",
			(int)obj->state);
		oid = obj->oid;
		offset = 0;
		ret = anx_ns_bind(ns_name, path, &oid);
		anx_objstore_release(obj);
		if (ret != ANX_OK) {
			kprintf("appendb64: bind failed (%d)\n", ret);
			return;
		}
	} else {
		obj = anx_objstore_lookup(&oid);
		if (!obj) {
			kprintf("appendb64: lookup failed\n");
			return;
		}
		offset = obj->payload_size;
		anx_objstore_release(obj);
	}

	{
		struct anx_state_object *dbg = anx_objstore_lookup(&oid);

		if (dbg) {
			kprintf("appendb64: dbg state=%d rules=%u sealed=? size=%u\n",
				(int)dbg->state, (unsigned)dbg->access_policy.rule_count,
				(unsigned)dbg->payload_size);
			anx_objstore_release(dbg);
		} else {
			kprintf("appendb64: dbg lookup(oid) after resolve/create FAILED\n");
		}
	}

	ret = anx_so_open(&oid, ANX_OPEN_WRITE, &handle);
	if (ret != ANX_OK) {
		kprintf("appendb64: open failed (%d)\n", ret);
		return;
	}
	ret = anx_so_write_payload(&handle, offset, decoded, decoded_len);
	anx_so_close(&handle);
	if (ret != ANX_OK) {
		kprintf("appendb64: write failed (%d)\n", ret);
		return;
	}
	kprintf("appendb64: %s:%s now %u bytes\n", ns_name, path,
		(uint32_t)(offset + decoded_len));
}
