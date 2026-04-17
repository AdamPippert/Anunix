/*
 * fetch.c — HTTP client that stores results as State Objects.
 *
 * Like curl/wget, but the response body is stored as a State Object
 * with URL provenance. Can inject credentials from the credential
 * store for authenticated requests.
 *
 * USAGE
 *   fetch <host> <port> <path> [ns:name]
 *   fetch -c <cred> <host> <port> <path> [ns:name]
 *   fetch example.com 80 /index.html default:/page
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/http.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>

void cmd_fetch(int argc, char **argv)
{
	const char *host = NULL;
	const char *port_str = NULL;
	const char *path = "/";
	const char *save_ns = "default";
	const char *save_path = NULL;
	uint16_t port;
	struct anx_http_response resp;
	int i, ret;

	/* Parse arguments */
	{
		int arg_idx = 0;

		for (i = 1; i < argc; i++) {
			if (arg_idx == 0)
				host = argv[i];
			else if (arg_idx == 1)
				port_str = argv[i];
			else if (arg_idx == 2)
				path = argv[i];
			else if (arg_idx == 3) {
				/* Optional save path */
				const char *colon = argv[i];

				while (*colon && *colon != ':')
					colon++;
				if (*colon == ':') {
					uint32_t ns_len;
					static char ns_buf[64];

					ns_len = (uint32_t)(colon - argv[i]);
					if (ns_len < sizeof(ns_buf)) {
						anx_memcpy(ns_buf, argv[i],
							   ns_len);
						ns_buf[ns_len] = '\0';
						save_ns = ns_buf;
					}
					save_path = colon + 1;
				} else {
					save_path = argv[i];
				}
			}
			arg_idx++;
		}
	}

	if (!host || !port_str) {
		kprintf("usage: fetch <host> <port> [path] [ns:name]\n");
		return;
	}

	/* Parse port */
	port = 0;
	{
		const char *p = port_str;

		while (*p >= '0' && *p <= '9')
			port = port * 10 + (uint16_t)(*p++ - '0');
	}

	kprintf("fetch: %s:%u%s\n", host, (uint32_t)port, path);

	ret = anx_http_get(host, port, path, &resp);
	if (ret != ANX_OK) {
		kprintf("fetch: request failed (%d)\n", ret);
		return;
	}

	kprintf("fetch: HTTP %d, %u bytes\n",
		resp.status_code, resp.body_len);

	if (resp.body && resp.body_len > 0) {
		/* Store as State Object */
		struct anx_so_create_params params;
		struct anx_state_object *obj;
		char oid_str[37];

		anx_memset(&params, 0, sizeof(params));
		params.object_type = ANX_OBJ_BYTE_DATA;
		params.payload = resp.body;
		params.payload_size = resp.body_len;

		ret = anx_so_create(&params, &obj);
		if (ret == ANX_OK) {
			anx_uuid_to_string(&obj->oid, oid_str,
					   sizeof(oid_str));

			if (save_path) {
				anx_ns_bind(save_ns, save_path, &obj->oid);
				kprintf("stored: %s -> %s:%s\n",
					oid_str, save_ns, save_path);
			} else {
				kprintf("stored: %s\n", oid_str);
			}

			/* Show first 256 bytes of content */
			{
				uint32_t show = resp.body_len;

				if (show > 256)
					show = 256;
				resp.body[show] = '\0';
				kprintf("%s\n", resp.body);
				if (resp.body_len > 256)
					kprintf("... (%u more bytes)\n",
						resp.body_len - 256);
			}

			anx_objstore_release(obj);
		} else {
			kprintf("fetch: store failed (%d)\n", ret);
		}
	}

	anx_http_response_free(&resp);
}
