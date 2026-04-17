/*
 * search.c — Search across State Object payloads.
 *
 * Iterates all objects in the store and matches payload content
 * against a search pattern. Can filter by object type and state.
 *
 * USAGE
 *   search <pattern>                Search all objects
 *   search -t byte <pattern>        Search only byte_data objects
 *   search -i <pattern>             Case-insensitive
 *   search -n default:/ <pattern>   Search within a namespace
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct search_ctx {
	const char *pattern;
	uint32_t pattern_len;
	bool case_insensitive;
	int type_filter;	/* -1 = any */
	uint32_t match_count;
};

static char to_lower(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c + ('a' - 'A');
	return c;
}

static bool payload_contains(const char *haystack, uint32_t hay_len,
			      const char *needle, uint32_t needle_len,
			      bool icase)
{
	uint32_t i, j;

	if (needle_len == 0 || needle_len > hay_len)
		return false;

	for (i = 0; i <= hay_len - needle_len; i++) {
		bool match = true;

		for (j = 0; j < needle_len; j++) {
			char a = haystack[i + j];
			char b = needle[j];

			if (icase) {
				a = to_lower(a);
				b = to_lower(b);
			}
			if (a != b) {
				match = false;
				break;
			}
		}
		if (match)
			return true;
	}
	return false;
}

static int search_callback(struct anx_state_object *obj, void *arg)
{
	struct search_ctx *ctx = (struct search_ctx *)arg;
	char oid_str[37];

	/* Skip non-active objects */
	if (obj->state != ANX_OBJ_ACTIVE && obj->state != ANX_OBJ_SEALED)
		return 0;

	/* Type filter */
	if (ctx->type_filter >= 0 &&
	    (int)obj->object_type != ctx->type_filter)
		return 0;

	/* Skip credential payloads (RFC-0008: never expose) */
	if (obj->object_type == ANX_OBJ_CREDENTIAL)
		return 0;

	/* Search payload */
	if (!obj->payload || obj->payload_size == 0)
		return 0;

	if (payload_contains((const char *)obj->payload,
			     (uint32_t)obj->payload_size,
			     ctx->pattern, ctx->pattern_len,
			     ctx->case_insensitive)) {
		anx_uuid_to_string(&obj->oid, oid_str, sizeof(oid_str));
		kprintf("  %s  %u bytes\n", oid_str,
			(uint32_t)obj->payload_size);
		ctx->match_count++;
	}

	return 0;
}

void cmd_search(int argc, char **argv)
{
	struct search_ctx ctx;
	const char *pattern = NULL;
	int i;

	ctx.case_insensitive = false;
	ctx.type_filter = -1;
	ctx.match_count = 0;

	for (i = 1; i < argc; i++) {
		if (anx_strcmp(argv[i], "-i") == 0) {
			ctx.case_insensitive = true;
		} else if (anx_strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			i++;
			if (anx_strcmp(argv[i], "byte") == 0)
				ctx.type_filter = ANX_OBJ_BYTE_DATA;
			else if (anx_strcmp(argv[i], "structured") == 0)
				ctx.type_filter = ANX_OBJ_STRUCTURED_DATA;
		} else {
			pattern = argv[i];
		}
	}

	if (!pattern) {
		kprintf("usage: search [-i] [-t type] <pattern>\n");
		return;
	}

	ctx.pattern = pattern;
	ctx.pattern_len = (uint32_t)anx_strlen(pattern);

	kprintf("searching for '%s'...\n", pattern);
	anx_objstore_iterate(search_callback, &ctx);

	if (ctx.match_count == 0)
		kprintf("no matches\n");
	else
		kprintf("%u matches\n", ctx.match_count);
}
