/*
 * json.c — Minimal recursive descent JSON parser.
 *
 * Parses a complete JSON document into a tree of anx_json_value nodes.
 * All memory is heap-allocated via anx_alloc. Supports objects, arrays,
 * strings, numbers (integers), booleans, and null.
 *
 * Limitations: no floating point, no unicode escapes (\uXXXX), no
 * deeply nested structures (limited by kernel stack). Sufficient for
 * provisioning configs and API responses.
 */

#include <anx/types.h>
#include <anx/json.h>
#include <anx/alloc.h>
#include <anx/string.h>

/* --- Lexer state --- */

struct json_ctx {
	const char *input;
	uint32_t len;
	uint32_t pos;
};

static void skip_ws(struct json_ctx *ctx)
{
	while (ctx->pos < ctx->len) {
		char c = ctx->input[ctx->pos];

		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			ctx->pos++;
		else
			break;
	}
}

static char peek(struct json_ctx *ctx)
{
	skip_ws(ctx);
	if (ctx->pos >= ctx->len)
		return '\0';
	return ctx->input[ctx->pos];
}

static bool match(struct json_ctx *ctx, char expected)
{
	skip_ws(ctx);
	if (ctx->pos < ctx->len && ctx->input[ctx->pos] == expected) {
		ctx->pos++;
		return true;
	}
	return false;
}

static bool match_str(struct json_ctx *ctx, const char *s, uint32_t slen)
{
	skip_ws(ctx);
	if (ctx->pos + slen > ctx->len)
		return false;
	if (anx_strncmp(ctx->input + ctx->pos, s, slen) != 0)
		return false;
	ctx->pos += slen;
	return true;
}

/* Forward declaration */
static int parse_value(struct json_ctx *ctx, struct anx_json_value *out);

/* --- Parse string --- */

static int parse_string(struct json_ctx *ctx, char **str_out,
			 uint32_t *len_out)
{
	uint32_t start;
	uint32_t out_len = 0;
	uint32_t cap;
	char *buf;
	char c;

	if (!match(ctx, '"'))
		return ANX_EINVAL;

	start = ctx->pos;

	/* First pass: count length (handling escapes) */
	while (ctx->pos < ctx->len && ctx->input[ctx->pos] != '"') {
		if (ctx->input[ctx->pos] == '\\') {
			ctx->pos++;
			if (ctx->pos >= ctx->len)
				return ANX_EINVAL;
		}
		out_len++;
		ctx->pos++;
	}

	if (ctx->pos >= ctx->len)
		return ANX_EINVAL;
	ctx->pos++;	/* skip closing quote */

	/* Allocate and copy with escape handling */
	cap = out_len + 1;
	buf = anx_alloc(cap);
	if (!buf)
		return ANX_ENOMEM;

	{
		uint32_t src = start;
		uint32_t dst = 0;

		while (dst < out_len) {
			c = ctx->input[src++];
			if (c == '\\') {
				c = ctx->input[src++];
				switch (c) {
				case '"':  buf[dst++] = '"'; break;
				case '\\': buf[dst++] = '\\'; break;
				case '/':  buf[dst++] = '/'; break;
				case 'b':  buf[dst++] = '\b'; break;
				case 'f':  buf[dst++] = '\f'; break;
				case 'n':  buf[dst++] = '\n'; break;
				case 'r':  buf[dst++] = '\r'; break;
				case 't':  buf[dst++] = '\t'; break;
				default:   buf[dst++] = c; break;
				}
			} else {
				buf[dst++] = c;
			}
		}
		buf[dst] = '\0';
	}

	*str_out = buf;
	*len_out = out_len;
	return ANX_OK;
}

/* --- Parse number (integers only) --- */

static int parse_number(struct json_ctx *ctx, int64_t *out)
{
	int64_t val = 0;
	bool negative = false;

	skip_ws(ctx);

	if (ctx->pos < ctx->len && ctx->input[ctx->pos] == '-') {
		negative = true;
		ctx->pos++;
	}

	if (ctx->pos >= ctx->len ||
	    ctx->input[ctx->pos] < '0' || ctx->input[ctx->pos] > '9')
		return ANX_EINVAL;

	while (ctx->pos < ctx->len &&
	       ctx->input[ctx->pos] >= '0' && ctx->input[ctx->pos] <= '9') {
		val = val * 10 + (ctx->input[ctx->pos] - '0');
		ctx->pos++;
	}

	/* Skip fractional part if present (truncate to integer) */
	if (ctx->pos < ctx->len && ctx->input[ctx->pos] == '.') {
		ctx->pos++;
		while (ctx->pos < ctx->len &&
		       ctx->input[ctx->pos] >= '0' &&
		       ctx->input[ctx->pos] <= '9')
			ctx->pos++;
	}

	/* Skip exponent if present */
	if (ctx->pos < ctx->len &&
	    (ctx->input[ctx->pos] == 'e' || ctx->input[ctx->pos] == 'E')) {
		ctx->pos++;
		if (ctx->pos < ctx->len &&
		    (ctx->input[ctx->pos] == '+' ||
		     ctx->input[ctx->pos] == '-'))
			ctx->pos++;
		while (ctx->pos < ctx->len &&
		       ctx->input[ctx->pos] >= '0' &&
		       ctx->input[ctx->pos] <= '9')
			ctx->pos++;
	}

	*out = negative ? -val : val;
	return ANX_OK;
}

/* --- Parse array --- */

#define MAX_ARRAY_ITEMS	256

static int parse_array(struct json_ctx *ctx, struct anx_json_value *out)
{
	struct anx_json_value *items;
	uint32_t count = 0;
	uint32_t cap = 16;
	int ret;

	if (!match(ctx, '['))
		return ANX_EINVAL;

	items = anx_zalloc(cap * sizeof(struct anx_json_value));
	if (!items)
		return ANX_ENOMEM;

	if (peek(ctx) == ']') {
		match(ctx, ']');
		out->type = ANX_JSON_ARRAY;
		out->v.array.items = items;
		out->v.array.count = 0;
		return ANX_OK;
	}

	for (;;) {
		if (count >= cap) {
			uint32_t new_cap = cap * 2;
			struct anx_json_value *new_items;

			if (new_cap > MAX_ARRAY_ITEMS) {
				ret = ANX_ENOMEM;
				goto fail;
			}
			new_items = anx_zalloc(new_cap *
					       sizeof(struct anx_json_value));
			if (!new_items) {
				ret = ANX_ENOMEM;
				goto fail;
			}
			anx_memcpy(new_items, items,
				   count * sizeof(struct anx_json_value));
			anx_free(items);
			items = new_items;
			cap = new_cap;
		}

		ret = parse_value(ctx, &items[count]);
		if (ret != ANX_OK)
			goto fail;
		count++;

		if (!match(ctx, ','))
			break;
	}

	if (!match(ctx, ']')) {
		ret = ANX_EINVAL;
		goto fail;
	}

	out->type = ANX_JSON_ARRAY;
	out->v.array.items = items;
	out->v.array.count = count;
	return ANX_OK;

fail:
	/* Free parsed items on error */
	{
		uint32_t i;
		for (i = 0; i < count; i++)
			anx_json_free(&items[i]);
	}
	anx_free(items);
	return ret;
}

/* --- Parse object --- */

#define MAX_OBJECT_PAIRS	128

static int parse_object(struct json_ctx *ctx, struct anx_json_value *out)
{
	struct anx_json_kv *pairs;
	uint32_t count = 0;
	uint32_t cap = 8;
	int ret;

	if (!match(ctx, '{'))
		return ANX_EINVAL;

	pairs = anx_zalloc(cap * sizeof(struct anx_json_kv));
	if (!pairs)
		return ANX_ENOMEM;

	if (peek(ctx) == '}') {
		match(ctx, '}');
		out->type = ANX_JSON_OBJECT;
		out->v.object.pairs = pairs;
		out->v.object.count = 0;
		return ANX_OK;
	}

	for (;;) {
		if (count >= cap) {
			uint32_t new_cap = cap * 2;
			struct anx_json_kv *new_pairs;

			if (new_cap > MAX_OBJECT_PAIRS) {
				ret = ANX_ENOMEM;
				goto fail;
			}
			new_pairs = anx_zalloc(new_cap *
					       sizeof(struct anx_json_kv));
			if (!new_pairs) {
				ret = ANX_ENOMEM;
				goto fail;
			}
			anx_memcpy(new_pairs, pairs,
				   count * sizeof(struct anx_json_kv));
			anx_free(pairs);
			pairs = new_pairs;
			cap = new_cap;
		}

		/* Parse key */
		ret = parse_string(ctx, &pairs[count].key,
				    &pairs[count].key_len);
		if (ret != ANX_OK)
			goto fail;

		if (!match(ctx, ':')) {
			anx_free(pairs[count].key);
			ret = ANX_EINVAL;
			goto fail;
		}

		/* Parse value */
		ret = parse_value(ctx, &pairs[count].value);
		if (ret != ANX_OK) {
			anx_free(pairs[count].key);
			goto fail;
		}
		count++;

		if (!match(ctx, ','))
			break;
	}

	if (!match(ctx, '}')) {
		ret = ANX_EINVAL;
		goto fail;
	}

	out->type = ANX_JSON_OBJECT;
	out->v.object.pairs = pairs;
	out->v.object.count = count;
	return ANX_OK;

fail:
	{
		uint32_t i;
		for (i = 0; i < count; i++) {
			anx_free(pairs[i].key);
			anx_json_free(&pairs[i].value);
		}
	}
	anx_free(pairs);
	return ret;
}

/* --- Parse any value --- */

static int parse_value(struct json_ctx *ctx, struct anx_json_value *out)
{
	char c;

	anx_memset(out, 0, sizeof(*out));
	c = peek(ctx);

	switch (c) {
	case '"':
		out->type = ANX_JSON_STRING;
		return parse_string(ctx, &out->v.string.str,
				    &out->v.string.len);

	case '{':
		return parse_object(ctx, out);

	case '[':
		return parse_array(ctx, out);

	case 't':
		if (match_str(ctx, "true", 4)) {
			out->type = ANX_JSON_BOOL;
			out->v.boolean = true;
			return ANX_OK;
		}
		return ANX_EINVAL;

	case 'f':
		if (match_str(ctx, "false", 5)) {
			out->type = ANX_JSON_BOOL;
			out->v.boolean = false;
			return ANX_OK;
		}
		return ANX_EINVAL;

	case 'n':
		if (match_str(ctx, "null", 4)) {
			out->type = ANX_JSON_NULL;
			return ANX_OK;
		}
		return ANX_EINVAL;

	default:
		if ((c >= '0' && c <= '9') || c == '-') {
			out->type = ANX_JSON_NUMBER;
			return parse_number(ctx, &out->v.number);
		}
		return ANX_EINVAL;
	}
}

/* --- Public API --- */

int anx_json_parse(const char *input, uint32_t len,
		    struct anx_json_value *out)
{
	struct json_ctx ctx;
	int ret;

	if (!input || !out)
		return ANX_EINVAL;

	ctx.input = input;
	ctx.len = len;
	ctx.pos = 0;

	anx_memset(out, 0, sizeof(*out));
	ret = parse_value(&ctx, out);

	return ret;
}

void anx_json_free(struct anx_json_value *val)
{
	uint32_t i;

	if (!val)
		return;

	switch (val->type) {
	case ANX_JSON_STRING:
		if (val->v.string.str)
			anx_free(val->v.string.str);
		break;

	case ANX_JSON_ARRAY:
		for (i = 0; i < val->v.array.count; i++)
			anx_json_free(&val->v.array.items[i]);
		if (val->v.array.items)
			anx_free(val->v.array.items);
		break;

	case ANX_JSON_OBJECT:
		for (i = 0; i < val->v.object.count; i++) {
			if (val->v.object.pairs[i].key)
				anx_free(val->v.object.pairs[i].key);
			anx_json_free(&val->v.object.pairs[i].value);
		}
		if (val->v.object.pairs)
			anx_free(val->v.object.pairs);
		break;

	default:
		break;
	}

	anx_memset(val, 0, sizeof(*val));
}

struct anx_json_value *anx_json_get(struct anx_json_value *obj,
				     const char *key)
{
	uint32_t i;

	if (!obj || obj->type != ANX_JSON_OBJECT || !key)
		return NULL;

	for (i = 0; i < obj->v.object.count; i++) {
		if (anx_strcmp(obj->v.object.pairs[i].key, key) == 0)
			return &obj->v.object.pairs[i].value;
	}
	return NULL;
}

struct anx_json_value *anx_json_array_get(struct anx_json_value *arr,
					   uint32_t index)
{
	if (!arr || arr->type != ANX_JSON_ARRAY)
		return NULL;
	if (index >= arr->v.array.count)
		return NULL;
	return &arr->v.array.items[index];
}

const char *anx_json_string(struct anx_json_value *val)
{
	if (!val || val->type != ANX_JSON_STRING)
		return NULL;
	return val->v.string.str;
}

int64_t anx_json_number(struct anx_json_value *val)
{
	if (!val || val->type != ANX_JSON_NUMBER)
		return 0;
	return val->v.number;
}

bool anx_json_bool(struct anx_json_value *val)
{
	if (!val || val->type != ANX_JSON_BOOL)
		return false;
	return val->v.boolean;
}

uint32_t anx_json_array_len(struct anx_json_value *val)
{
	if (!val || val->type != ANX_JSON_ARRAY)
		return 0;
	return val->v.array.count;
}
