/*
 * model_client.c — Claude Messages API client.
 *
 * Builds HTTP POST requests for the Claude Messages API, sends them
 * through the HTTP client with credential injection, and parses the
 * JSON response to extract the assistant's message content.
 *
 * The API endpoint is typically a TLS-terminating proxy on the QEMU
 * host that forwards to api.anthropic.com:443.
 */

#include <anx/types.h>
#include <anx/model_client.h>
#include <anx/http.h>
#include <anx/json.h>
#include <anx/credential.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/arch.h>
#include <anx/kprintf.h>

static struct anx_model_endpoint endpoint;
static bool configured;

/* --- JSON request body construction --- */

/*
 * Build the Messages API request body. We construct JSON by string
 * concatenation since we don't have a JSON serializer.
 *
 * Format:
 * {
 *   "model": "...",
 *   "max_tokens": N,
 *   "system": "...",        (optional)
 *   "messages": [
 *     {"role": "user", "content": "..."}
 *   ]
 * }
 */

/* Append a string, escaping JSON special characters */
static uint32_t json_escape_append(char *buf, uint32_t off, uint32_t cap,
				    const char *s)
{
	while (*s && off < cap - 2) {
		switch (*s) {
		case '"':
			if (off + 2 >= cap) return off;
			buf[off++] = '\\'; buf[off++] = '"';
			break;
		case '\\':
			if (off + 2 >= cap) return off;
			buf[off++] = '\\'; buf[off++] = '\\';
			break;
		case '\n':
			if (off + 2 >= cap) return off;
			buf[off++] = '\\'; buf[off++] = 'n';
			break;
		case '\r':
			if (off + 2 >= cap) return off;
			buf[off++] = '\\'; buf[off++] = 'r';
			break;
		case '\t':
			if (off + 2 >= cap) return off;
			buf[off++] = '\\'; buf[off++] = 't';
			break;
		default:
			buf[off++] = *s;
			break;
		}
		s++;
	}
	return off;
}

static uint32_t str_append(char *buf, uint32_t off, uint32_t cap,
			    const char *s)
{
	while (*s && off < cap - 1)
		buf[off++] = *s++;
	return off;
}

static uint32_t int_append(char *buf, uint32_t off, uint32_t cap,
			    uint32_t val)
{
	char tmp[12];
	int pos = 0;
	int i;

	if (val == 0) {
		tmp[pos++] = '0';
	} else {
		while (val > 0 && pos < 11) {
			tmp[pos++] = '0' + (char)(val % 10);
			val /= 10;
		}
	}

	for (i = pos - 1; i >= 0 && off < cap - 1; i--)
		buf[off++] = tmp[i];
	return off;
}

static int build_request_body(const struct anx_model_request *req,
			       char *buf, uint32_t cap)
{
	uint32_t off = 0;

	off = str_append(buf, off, cap, "{\"model\":\"");
	off = json_escape_append(buf, off, cap, req->model);
	off = str_append(buf, off, cap, "\",\"max_tokens\":");
	off = int_append(buf, off, cap, req->max_tokens);

	if (req->system) {
		off = str_append(buf, off, cap, ",\"system\":\"");
		off = json_escape_append(buf, off, cap, req->system);
		off = str_append(buf, off, cap, "\"");
	}

	off = str_append(buf, off, cap,
		",\"messages\":[{\"role\":\"user\",\"content\":\"");
	off = json_escape_append(buf, off, cap, req->user_message);
	off = str_append(buf, off, cap, "\"}]}");

	if (off >= cap - 1)
		return ANX_ENOMEM;

	buf[off] = '\0';
	return (int)off;
}

/* --- Response parsing --- */

static int parse_response(const char *body, uint32_t body_len,
			   struct anx_model_response *resp)
{
	struct anx_json_value root;
	struct anx_json_value *content_arr;
	struct anx_json_value *first_block;
	struct anx_json_value *text_val;
	struct anx_json_value *usage;
	struct anx_json_value *stop;
	const char *text;
	int ret;

	ret = anx_json_parse(body, body_len, &root);
	if (ret != ANX_OK)
		return ret;

	/* Check for error response */
	{
		struct anx_json_value *err = anx_json_get(&root, "error");

		if (err) {
			struct anx_json_value *msg;

			msg = anx_json_get(err, "message");
			if (msg && msg->type == ANX_JSON_STRING)
				kprintf("model: API error: %s\n",
					anx_json_string(msg));
			anx_json_free(&root);
			return ANX_EIO;
		}
	}

	/* Extract content[0].text */
	content_arr = anx_json_get(&root, "content");
	if (!content_arr || content_arr->type != ANX_JSON_ARRAY ||
	    anx_json_array_len(content_arr) == 0) {
		anx_json_free(&root);
		return ANX_EINVAL;
	}

	first_block = anx_json_array_get(content_arr, 0);
	text_val = anx_json_get(first_block, "text");
	text = anx_json_string(text_val);

	if (text) {
		uint32_t len = (uint32_t)anx_strlen(text);

		resp->content = anx_alloc(len + 1);
		if (resp->content) {
			anx_memcpy(resp->content, text, len);
			resp->content[len] = '\0';
			resp->content_len = len;
		}
	}

	/* Extract usage */
	usage = anx_json_get(&root, "usage");
	if (usage) {
		struct anx_json_value *in_tok, *out_tok;

		in_tok = anx_json_get(usage, "input_tokens");
		out_tok = anx_json_get(usage, "output_tokens");
		resp->input_tokens = (int32_t)anx_json_number(in_tok);
		resp->output_tokens = (int32_t)anx_json_number(out_tok);
	}

	/* Extract stop_reason */
	stop = anx_json_get(&root, "stop_reason");
	if (stop && stop->type == ANX_JSON_STRING) {
		const char *sr = anx_json_string(stop);
		uint32_t sr_len = (uint32_t)anx_strlen(sr);

		resp->stop_reason = anx_alloc(sr_len + 1);
		if (resp->stop_reason) {
			anx_memcpy(resp->stop_reason, sr, sr_len);
			resp->stop_reason[sr_len] = '\0';
		}
	}

	anx_json_free(&root);
	return ANX_OK;
}

/* --- Public API --- */

void anx_model_client_init(const struct anx_model_endpoint *ep)
{
	endpoint = *ep;
	configured = true;
	kprintf("model: endpoint %s:%u (credential: %s)\n",
		ep->host, (uint32_t)ep->port, ep->cred_name);
}

bool anx_model_client_ready(void)
{
	return configured;
}

int anx_model_call(const struct anx_model_request *req,
		    struct anx_model_response *resp)
{
	char *body_buf;
	int body_len;
	char key_buf[256];
	uint32_t key_len;
	char headers[512];
	uint32_t hdr_off;
	struct anx_http_response http_resp;
	int ret;

	if (!configured)
		return ANX_EINVAL;

	anx_memset(resp, 0, sizeof(*resp));

	/* Build JSON request body */
	body_buf = anx_alloc(4096);
	if (!body_buf)
		return ANX_ENOMEM;

	body_len = build_request_body(req, body_buf, 4096);
	if (body_len < 0) {
		anx_free(body_buf);
		return ANX_ENOMEM;
	}

	/* Read API key from credential store */
	ret = anx_credential_read(endpoint.cred_name, key_buf,
				   sizeof(key_buf) - 1, &key_len);
	if (ret != ANX_OK) {
		anx_free(body_buf);
		kprintf("model: credential '%s' not found\n",
			endpoint.cred_name);
		return ret;
	}
	key_buf[key_len] = '\0';

	/* Build auth headers */
	hdr_off = 0;
	anx_memcpy(headers + hdr_off, "x-api-key: ", 11);
	hdr_off += 11;
	anx_memcpy(headers + hdr_off, key_buf, key_len);
	hdr_off += key_len;
	anx_memcpy(headers + hdr_off,
		   "\r\nanthropic-version: 2023-06-01\r\n", 33);
	hdr_off += 33;
	headers[hdr_off] = '\0';

	/* Zero key immediately */
	anx_memset(key_buf, 0, sizeof(key_buf));

	/* Send POST request */
	ret = anx_http_post_authed(endpoint.host, endpoint.port,
				    "/v1/messages", headers,
				    "application/json",
				    body_buf, (uint32_t)body_len,
				    &http_resp);

	/* Zero headers containing key */
	anx_memset(headers, 0, sizeof(headers));
	anx_free(body_buf);

	if (ret != ANX_OK)
		return ret;

	resp->status_code = http_resp.status_code;

	if (http_resp.status_code != 200) {
		if (http_resp.body) {
			kprintf("model: HTTP %d: %s\n",
				http_resp.status_code, http_resp.body);
		}
		anx_http_response_free(&http_resp);
		return ANX_EIO;
	}

	/* Parse JSON response */
	if (http_resp.body && http_resp.body_len > 0) {
		ret = parse_response(http_resp.body, http_resp.body_len, resp);
	} else {
		ret = ANX_EIO;
	}

	anx_http_response_free(&http_resp);
	return ret;
}

void anx_model_response_free(struct anx_model_response *resp)
{
	if (resp->content) {
		anx_free(resp->content);
		resp->content = NULL;
	}
	if (resp->stop_reason) {
		anx_free(resp->stop_reason);
		resp->stop_reason = NULL;
	}
	resp->content_len = 0;
}
