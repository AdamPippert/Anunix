/*
 * httpd.c — HTTP API server for programmatic shell access.
 *
 * Listens on a configurable port (default 8080) and accepts
 * HTTP requests to execute shell commands and return output
 * as JSON. Single connection, synchronous.
 *
 * Endpoints:
 *   GET  /api/v1/health   — health check
 *   POST /api/v1/exec     — execute a shell command
 */

#include <anx/types.h>
#include <anx/httpd.h>
#include <anx/net.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/json.h>
#include <anx/shell.h>
#include <anx/fb.h>
#include <anx/workflow.h>
#include <anx/workflow_library.h>
#include <anx/agent_cell.h>
#include <anx/jepa.h>

#define HTTPD_REQ_BUF_SIZE	4096
#define HTTPD_RESP_BUF_SIZE	65536
#define HTTPD_CAPTURE_SIZE	32768
#define HTTPD_RECV_TIMEOUT_MS	5000

/* Current pending connection for request processing */
static struct anx_tcp_conn *pending_conn;
static bool httpd_ready;

/* Simple integer-to-string */
static int itoa_simple(uint32_t val, char *buf, uint32_t bufsize)
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

	if ((uint32_t)pos >= bufsize)
		return -1;

	for (i = 0; i < pos; i++)
		buf[i] = tmp[pos - 1 - i];
	buf[pos] = '\0';
	return pos;
}

/* Append string to buffer at offset, return ANX_OK or ANX_ENOMEM */
static int buf_append(char *buf, uint32_t *off, uint32_t cap,
		       const char *s)
{
	uint32_t len = (uint32_t)anx_strlen(s);

	if (*off + len >= cap)
		return ANX_ENOMEM;
	anx_memcpy(buf + *off, s, len);
	*off += len;
	return ANX_OK;
}

/* Append len bytes from src to buffer at offset */
static int buf_append_n(char *buf, uint32_t *off, uint32_t cap,
			 const char *src, uint32_t len)
{
	if (*off + len >= cap)
		return ANX_ENOMEM;
	anx_memcpy(buf + *off, src, len);
	*off += len;
	return ANX_OK;
}

/*
 * Escape a string for JSON output. Handles \n, \r, \t, \\, \", and
 * control characters. Returns bytes written (not including NUL) or
 * -1 on overflow.
 */
static int json_escape(const char *src, uint32_t src_len,
			char *dst, uint32_t dst_cap)
{
	uint32_t di = 0;
	uint32_t si;

	for (si = 0; si < src_len && src[si]; si++) {
		char c = src[si];

		switch (c) {
		case '"':
			if (di + 2 > dst_cap) return -1;
			dst[di++] = '\\';
			dst[di++] = '"';
			break;
		case '\\':
			if (di + 2 > dst_cap) return -1;
			dst[di++] = '\\';
			dst[di++] = '\\';
			break;
		case '\n':
			if (di + 2 > dst_cap) return -1;
			dst[di++] = '\\';
			dst[di++] = 'n';
			break;
		case '\r':
			if (di + 2 > dst_cap) return -1;
			dst[di++] = '\\';
			dst[di++] = 'r';
			break;
		case '\t':
			if (di + 2 > dst_cap) return -1;
			dst[di++] = '\\';
			dst[di++] = 't';
			break;
		default:
			if ((uint8_t)c < 0x20) {
				/* Skip other control characters */
				continue;
			}
			if (di + 1 > dst_cap) return -1;
			dst[di++] = c;
			break;
		}
	}

	if (di < dst_cap)
		dst[di] = '\0';
	return (int)di;
}

/* Find "\r\n\r\n" header/body separator */
static int find_body(const char *data, uint32_t len)
{
	uint32_t i;

	for (i = 0; i + 3 < len; i++) {
		if (data[i] == '\r' && data[i + 1] == '\n' &&
		    data[i + 2] == '\r' && data[i + 3] == '\n')
			return (int)(i + 4);
	}
	return -1;
}

/* Parse Content-Length from headers */
static uint32_t parse_content_length(const char *headers, uint32_t hdr_len)
{
	const char *p = headers;
	const char *end = headers + hdr_len;

	while (p < end) {
		/* Case-insensitive match for "Content-Length:" */
		if ((p + 16 <= end) &&
		    (p[0] == 'C' || p[0] == 'c') &&
		    (p[1] == 'o' || p[1] == 'O') &&
		    (p[2] == 'n' || p[2] == 'N') &&
		    (p[3] == 't' || p[3] == 'T') &&
		    (p[4] == 'e' || p[4] == 'E') &&
		    (p[5] == 'n' || p[5] == 'N') &&
		    (p[6] == 't' || p[6] == 'T') &&
		    p[7] == '-' &&
		    (p[8] == 'L' || p[8] == 'l') &&
		    (p[9] == 'e' || p[9] == 'E') &&
		    (p[10] == 'n' || p[10] == 'N') &&
		    (p[11] == 'g' || p[11] == 'G') &&
		    (p[12] == 't' || p[12] == 'T') &&
		    (p[13] == 'h' || p[13] == 'H') &&
		    p[14] == ':') {
			const char *v = p + 15;
			uint32_t val = 0;

			while (v < end && *v == ' ')
				v++;
			while (v < end && *v >= '0' && *v <= '9') {
				val = val * 10 + (uint32_t)(*v - '0');
				v++;
			}
			return val;
		}
		/* Skip to next line */
		while (p < end && *p != '\n')
			p++;
		if (p < end)
			p++;
	}
	return 0;
}

/* Send an HTTP response */
static void httpd_send_response(struct anx_tcp_conn *conn,
				 int status_code, const char *status_text,
				 const char *content_type,
				 const char *body, uint32_t body_len)
{
	char *resp;
	uint32_t off = 0;
	uint32_t cap = 512 + body_len;
	char len_str[12];

	resp = anx_alloc(cap);
	if (!resp)
		return;

	buf_append(resp, &off, cap, "HTTP/1.1 ");

	itoa_simple((uint32_t)status_code, len_str, sizeof(len_str));
	buf_append(resp, &off, cap, len_str);
	buf_append(resp, &off, cap, " ");
	buf_append(resp, &off, cap, status_text);
	buf_append(resp, &off, cap, "\r\n");

	buf_append(resp, &off, cap, "Content-Type: ");
	buf_append(resp, &off, cap, content_type);
	buf_append(resp, &off, cap, "\r\n");

	buf_append(resp, &off, cap, "Content-Length: ");
	itoa_simple(body_len, len_str, sizeof(len_str));
	buf_append(resp, &off, cap, len_str);
	buf_append(resp, &off, cap, "\r\n");

	buf_append(resp, &off, cap, "Connection: close\r\n");
	buf_append(resp, &off, cap, "Access-Control-Allow-Origin: *\r\n");
	buf_append(resp, &off, cap, "\r\n");

	if (body && body_len > 0)
		buf_append_n(resp, &off, cap, body, body_len);

	anx_tcp_srv_send(conn, resp, off);
	anx_free(resp);
}

/* Handle GET /api/v1/health */
static void handle_health(struct anx_tcp_conn *conn)
{
	const char *body = "{\"status\": \"healthy\"}";
	uint32_t body_len = (uint32_t)anx_strlen(body);

	httpd_send_response(conn, 200, "OK",
			     "application/json", body, body_len);
}

/* Handle GET /api/v1/fb */
static void handle_fb_info(struct anx_tcp_conn *conn)
{
	const struct anx_fb_info *fb = anx_fb_get_info();
	char body[128];
	uint32_t off = 0;

	if (!fb || !fb->available) {
		const char *s = "{\"available\":false}";

		httpd_send_response(conn, 200, "OK",
				     "application/json",
				     s, (uint32_t)anx_strlen(s));
		return;
	}

	buf_append(body, &off, sizeof(body), "{\"available\":true,\"width\":");
	{ char n[12]; itoa_simple(fb->width,  n, sizeof(n)); buf_append(body, &off, sizeof(body), n); }
	buf_append(body, &off, sizeof(body), ",\"height\":");
	{ char n[12]; itoa_simple(fb->height, n, sizeof(n)); buf_append(body, &off, sizeof(body), n); }
	buf_append(body, &off, sizeof(body), ",\"pitch\":");
	{ char n[12]; itoa_simple(fb->pitch,  n, sizeof(n)); buf_append(body, &off, sizeof(body), n); }
	buf_append(body, &off, sizeof(body), ",\"bpp\":");
	{ char n[12]; itoa_simple((uint32_t)fb->bpp, n, sizeof(n)); buf_append(body, &off, sizeof(body), n); }
	buf_append(body, &off, sizeof(body), "}");

	httpd_send_response(conn, 200, "OK",
			     "application/json", body, off);
}

/* Handle GET /api/v1/display/modes */
static void handle_display_modes(struct anx_tcp_conn *conn)
{
	const struct anx_gop_mode *modes;
	uint8_t  count, current;
	char    *body;
	uint32_t off = 0;
	uint32_t cap = 64 + (uint32_t)ANX_GOP_MODES_MAX * 64;
	uint8_t  i;

	modes = anx_fb_get_gop_modes(&count, &current);

	body = anx_alloc(cap);
	if (!body) {
		const char *err = "{\"error\":\"out of memory\"}";

		httpd_send_response(conn, 500, "Internal Server Error",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
		return;
	}

	buf_append(body, &off, cap, "{\"count\":");
	{ char n[12]; itoa_simple((uint32_t)count, n, sizeof(n)); buf_append(body, &off, cap, n); }
	buf_append(body, &off, cap, ",\"current\":");
	{ char n[12]; itoa_simple((uint32_t)current, n, sizeof(n)); buf_append(body, &off, cap, n); }
	buf_append(body, &off, cap, ",\"modes\":[");

	for (i = 0; i < count; i++) {
		char n[12];

		if (i > 0)
			buf_append(body, &off, cap, ",");
		buf_append(body, &off, cap, "{\"index\":");
		itoa_simple((uint32_t)i, n, sizeof(n));
		buf_append(body, &off, cap, n);
		buf_append(body, &off, cap, ",\"width\":");
		itoa_simple(modes[i].width, n, sizeof(n));
		buf_append(body, &off, cap, n);
		buf_append(body, &off, cap, ",\"height\":");
		itoa_simple(modes[i].height, n, sizeof(n));
		buf_append(body, &off, cap, n);
		buf_append(body, &off, cap, ",\"selected\":");
		buf_append(body, &off, cap, (i == current) ? "true" : "false");
		buf_append(body, &off, cap, "}");
	}
	buf_append(body, &off, cap, "]}");

	httpd_send_response(conn, 200, "OK",
			     "application/json", body, off);
	anx_free(body);
}

/* Handle POST /api/v1/exec */
static void handle_exec(struct anx_tcp_conn *conn,
			 const char *body, uint32_t body_len)
{
	struct anx_json_value root;
	struct anx_json_value *cmd_val;
	const char *command;
	char *capture;
	char *escaped;
	char *resp_body;
	uint32_t resp_off = 0;
	uint32_t resp_cap;
	uint32_t captured;
	int esc_len;
	int ret;

	if (!body || body_len == 0) {
		const char *err = "{\"status\": \"error\", \"output\": \"empty request body\"}";

		httpd_send_response(conn, 400, "Bad Request",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
		return;
	}

	/* Parse JSON request body */
	ret = anx_json_parse(body, body_len, &root);
	if (ret != ANX_OK) {
		const char *err = "{\"status\": \"error\", \"output\": \"invalid JSON\"}";

		httpd_send_response(conn, 400, "Bad Request",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
		return;
	}

	/* Extract "command" field */
	cmd_val = anx_json_get(&root, "command");
	command = anx_json_string(cmd_val);
	if (!command || command[0] == '\0') {
		const char *err = "{\"status\": \"error\", \"output\": \"missing 'command' field\"}";

		anx_json_free(&root);
		httpd_send_response(conn, 400, "Bad Request",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
		return;
	}

	/* Capture kprintf output during command execution */
	capture = anx_alloc(HTTPD_CAPTURE_SIZE);
	if (!capture) {
		const char *err = "{\"status\": \"error\", \"output\": \"out of memory\"}";

		anx_json_free(&root);
		httpd_send_response(conn, 500, "Internal Server Error",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
		return;
	}

	anx_kprintf_capture_start(capture, HTTPD_CAPTURE_SIZE);
	anx_shell_execute(command);
	captured = anx_kprintf_capture_stop();

	anx_json_free(&root);

	/* Escape captured output for JSON */
	escaped = anx_alloc(HTTPD_CAPTURE_SIZE * 2);
	if (!escaped) {
		anx_free(capture);
		return;
	}

	esc_len = json_escape(capture, captured, escaped,
			       HTTPD_CAPTURE_SIZE * 2 - 1);
	if (esc_len < 0)
		esc_len = 0;

	anx_free(capture);

	/* Build JSON response: {"status": "ok", "output": "..."} */
	resp_cap = (uint32_t)esc_len + 64;
	resp_body = anx_alloc(resp_cap);
	if (!resp_body) {
		anx_free(escaped);
		return;
	}

	resp_off = 0;
	buf_append(resp_body, &resp_off, resp_cap,
		    "{\"status\": \"ok\", \"output\": \"");
	buf_append_n(resp_body, &resp_off, resp_cap, escaped,
		      (uint32_t)esc_len);
	buf_append(resp_body, &resp_off, resp_cap, "\"}");

	anx_free(escaped);

	httpd_send_response(conn, 200, "OK",
			     "application/json", resp_body, resp_off);

	anx_free(resp_body);
}

/* ------------------------------------------------------------------ */
/* Shared helpers for new endpoints                                    */
/* ------------------------------------------------------------------ */

/* Serialize an OID as a 32-char lowercase hex string. */
static void oid_to_hex(const anx_oid_t *oid, char out[33])
{
	static const char hx[] = "0123456789abcdef";
	uint64_t hi = oid->hi;
	uint64_t lo = oid->lo;
	int i;

	for (i = 0; i < 16; i++) {
		out[i]      = hx[(hi >> (60 - i * 4)) & 0xf];
		out[i + 16] = hx[(lo >> (60 - i * 4)) & 0xf];
	}
	out[32] = '\0';
}

/* Find a workflow OID by name; returns ANX_OK or ANX_ENOENT. */
static int httpd_wf_find(const char *name, anx_oid_t *oid_out)
{
	anx_oid_t all[ANX_WF_MAX_WFS];
	uint32_t count = 0;
	uint32_t i;

	if (anx_wf_list(all, ANX_WF_MAX_WFS, &count) != ANX_OK)
		return ANX_ENOENT;

	for (i = 0; i < count; i++) {
		struct anx_wf_object *wf = anx_wf_object_get(&all[i]);

		if (wf && anx_strcmp(wf->name, name) == 0) {
			*oid_out = all[i];
			return ANX_OK;
		}
	}
	return ANX_ENOENT;
}

/* Return the string name for a workflow run state. */
static const char *wf_state_str(enum anx_wf_run_state s)
{
	switch (s) {
	case ANX_WF_RUN_IDLE:           return "idle";
	case ANX_WF_RUN_RUNNING:        return "running";
	case ANX_WF_RUN_WAITING_HUMAN:  return "waiting_human";
	case ANX_WF_RUN_SUSPENDED:      return "suspended";
	case ANX_WF_RUN_COMPLETED:      return "completed";
	case ANX_WF_RUN_FAILED:         return "failed";
	default:                        return "unknown";
	}
}

/*
 * Parse the workflow name and sub-command from a path of the form:
 *   /api/v1/workflow[/<name>[/<sub>]]
 * Returns 1 if the path starts with the prefix, 0 otherwise.
 */
static int parse_wf_path(const char *path,
			  char *name_out, uint32_t name_max,
			  char *sub_out,  uint32_t sub_max)
{
	const char *p;
	uint32_t i;

	if (anx_strncmp(path, "/api/v1/workflow", 16) != 0)
		return 0;

	p = path + 16;
	name_out[0] = '\0';
	if (sub_out) sub_out[0] = '\0';

	if (*p != '/')
		return 1;	/* bare /api/v1/workflow */

	p++;
	i = 0;
	while (*p && *p != '/' && *p != ' ' && *p != '\r' && i + 1 < name_max)
		name_out[i++] = *p++;
	name_out[i] = '\0';

	if (sub_out && *p == '/') {
		p++;
		i = 0;
		while (*p && *p != ' ' && *p != '\r' && i + 1 < sub_max)
			sub_out[i++] = *p++;
		sub_out[i] = '\0';
	}

	return 1;
}

/* ------------------------------------------------------------------ */
/* GET /api/v1/jepa                                                    */
/* ------------------------------------------------------------------ */

static void handle_jepa_status(struct anx_tcp_conn *conn)
{
	char *body;
	uint32_t off = 0;
	uint32_t cap = 1024;
	const struct anx_jepa_world_profile *active;
	const char *world_uris[16];
	uint32_t world_count = 0;
	uint32_t i;
	const char *status_str;

	body = anx_alloc(cap);
	if (!body) {
		const char *err = "{\"error\":\"out of memory\"}";
		httpd_send_response(conn, 500, "Internal Server Error",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	switch (anx_jepa_status_get()) {
	case ANX_JEPA_READY:        status_str = "ready";        break;
	case ANX_JEPA_TRAINING:     status_str = "training";     break;
	case ANX_JEPA_DEGRADED:     status_str = "degraded";     break;
	case ANX_JEPA_UNAVAILABLE:  status_str = "unavailable";  break;
	default:                    status_str = "initializing"; break;
	}

	active = anx_jepa_world_get_active();
	anx_jepa_world_list(world_uris, 16, &world_count);

	buf_append(body, &off, cap, "{\"status\":\"");
	buf_append(body, &off, cap, status_str);
	buf_append(body, &off, cap, "\",\"available\":");
	buf_append(body, &off, cap, anx_jepa_available() ? "true" : "false");
	buf_append(body, &off, cap, ",\"active_world\":\"");
	buf_append(body, &off, cap, active ? active->uri : "");
	buf_append(body, &off, cap, "\",\"worlds\":[");

	for (i = 0; i < world_count; i++) {
		if (i > 0) buf_append(body, &off, cap, ",");
		buf_append(body, &off, cap, "\"");
		buf_append(body, &off, cap, world_uris[i]);
		buf_append(body, &off, cap, "\"");
	}
	buf_append(body, &off, cap, "]}");

	httpd_send_response(conn, 200, "OK", "application/json", body, off);
	anx_free(body);
}

/* ------------------------------------------------------------------ */
/* GET /api/v1/workflow                                                */
/* ------------------------------------------------------------------ */

static void handle_workflow_list(struct anx_tcp_conn *conn)
{
	anx_oid_t all[ANX_WF_MAX_WFS];
	uint32_t count = 0;
	char *body;
	uint32_t off = 0;
	uint32_t cap = 2048;
	uint32_t i;

	body = anx_alloc(cap);
	if (!body) {
		const char *err = "{\"error\":\"out of memory\"}";
		httpd_send_response(conn, 500, "Internal Server Error",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	anx_wf_list(all, ANX_WF_MAX_WFS, &count);

	buf_append(body, &off, cap, "{\"count\":");
	{ char n[12]; itoa_simple(count, n, sizeof(n)); buf_append(body, &off, cap, n); }
	buf_append(body, &off, cap, ",\"workflows\":[");

	for (i = 0; i < count; i++) {
		struct anx_wf_object *wf = anx_wf_object_get(&all[i]);
		char hex[33];

		if (!wf) continue;
		if (i > 0) buf_append(body, &off, cap, ",");
		oid_to_hex(&all[i], hex);
		buf_append(body, &off, cap, "{\"name\":\"");
		buf_append(body, &off, cap, wf->name);
		buf_append(body, &off, cap, "\",\"state\":\"");
		buf_append(body, &off, cap, wf_state_str(wf->run_state));
		buf_append(body, &off, cap, "\",\"nodes\":");
		{ char n[12]; itoa_simple((uint32_t)wf->node_count, n, sizeof(n)); buf_append(body, &off, cap, n); }
		buf_append(body, &off, cap, ",\"edges\":");
		{ char n[12]; itoa_simple((uint32_t)wf->edge_count, n, sizeof(n)); buf_append(body, &off, cap, n); }
		buf_append(body, &off, cap, ",\"oid\":\"");
		buf_append(body, &off, cap, hex);
		buf_append(body, &off, cap, "\"}");
	}
	buf_append(body, &off, cap, "]}");

	httpd_send_response(conn, 200, "OK", "application/json", body, off);
	anx_free(body);
}

/* ------------------------------------------------------------------ */
/* GET /api/v1/workflow/<name>                                         */
/* ------------------------------------------------------------------ */

static void handle_workflow_status(struct anx_tcp_conn *conn, const char *name)
{
	anx_oid_t oid;
	struct anx_wf_object *wf;
	char body[512];
	char hex[33];
	uint32_t off = 0;

	if (httpd_wf_find(name, &oid) != ANX_OK) {
		const char *err = "{\"error\":\"not found\"}";
		httpd_send_response(conn, 404, "Not Found",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	wf = anx_wf_object_get(&oid);
	if (!wf) {
		const char *err = "{\"error\":\"not found\"}";
		httpd_send_response(conn, 404, "Not Found",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	oid_to_hex(&oid, hex);
	buf_append(body, &off, sizeof(body), "{\"name\":\"");
	buf_append(body, &off, sizeof(body), wf->name);
	buf_append(body, &off, sizeof(body), "\",\"description\":\"");
	buf_append(body, &off, sizeof(body), wf->description);
	buf_append(body, &off, sizeof(body), "\",\"state\":\"");
	buf_append(body, &off, sizeof(body), wf_state_str(wf->run_state));
	buf_append(body, &off, sizeof(body), "\",\"nodes\":");
	{ char n[12]; itoa_simple((uint32_t)wf->node_count, n, sizeof(n)); buf_append(body, &off, sizeof(body), n); }
	buf_append(body, &off, sizeof(body), ",\"edges\":");
	{ char n[12]; itoa_simple((uint32_t)wf->edge_count, n, sizeof(n)); buf_append(body, &off, sizeof(body), n); }
	buf_append(body, &off, sizeof(body), ",\"last_status\":");
	{ char n[12]; itoa_simple((uint32_t)wf->last_status, n, sizeof(n)); buf_append(body, &off, sizeof(body), n); }
	buf_append(body, &off, sizeof(body), ",\"oid\":\"");
	buf_append(body, &off, sizeof(body), hex);
	buf_append(body, &off, sizeof(body), "\"}");

	httpd_send_response(conn, 200, "OK", "application/json", body, off);
}

/* ------------------------------------------------------------------ */
/* GET /api/v1/workflow/<name>/show                                    */
/* ------------------------------------------------------------------ */

static void handle_workflow_show(struct anx_tcp_conn *conn, const char *name)
{
	anx_oid_t oid;
	char *buf;
	int rc;

	if (httpd_wf_find(name, &oid) != ANX_OK) {
		const char *err = "{\"error\":\"not found\"}";
		httpd_send_response(conn, 404, "Not Found",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	buf = anx_alloc(8192);
	if (!buf) {
		const char *err = "{\"error\":\"out of memory\"}";
		httpd_send_response(conn, 500, "Internal Server Error",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	rc = anx_wf_serialize(&oid, buf, 8192);
	if (rc != ANX_OK) {
		anx_free(buf);
		const char *err = "{\"error\":\"serialize failed\"}";
		httpd_send_response(conn, 500, "Internal Server Error",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	httpd_send_response(conn, 200, "OK", "text/plain",
			    buf, (uint32_t)anx_strlen(buf));
	anx_free(buf);
}

/* ------------------------------------------------------------------ */
/* GET /api/v1/workflow/<name>/graph                                   */
/* ------------------------------------------------------------------ */

static void handle_workflow_graph(struct anx_tcp_conn *conn, const char *name)
{
	anx_oid_t oid;
	char *buf;
	int rc;

	if (httpd_wf_find(name, &oid) != ANX_OK) {
		const char *err = "{\"error\":\"not found\"}";
		httpd_send_response(conn, 404, "Not Found",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	buf = anx_alloc(8192);
	if (!buf) {
		const char *err = "{\"error\":\"out of memory\"}";
		httpd_send_response(conn, 500, "Internal Server Error",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	rc = anx_wf_render_ascii(&oid, buf, 8192);
	if (rc != ANX_OK) {
		anx_free(buf);
		const char *err = "{\"error\":\"render failed\"}";
		httpd_send_response(conn, 500, "Internal Server Error",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	httpd_send_response(conn, 200, "OK", "text/plain",
			    buf, (uint32_t)anx_strlen(buf));
	anx_free(buf);
}

/* ------------------------------------------------------------------ */
/* POST /api/v1/workflow  — create                                     */
/* ------------------------------------------------------------------ */

static void handle_workflow_create(struct anx_tcp_conn *conn,
				   const char *body, uint32_t body_len)
{
	struct anx_json_value root;
	const char *name;
	const char *desc;
	anx_oid_t oid;
	char resp[256];
	uint32_t off = 0;
	char hex[33];
	int rc;

	if (!body || body_len == 0) {
		const char *err = "{\"error\":\"empty body\"}";
		httpd_send_response(conn, 400, "Bad Request",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	if (anx_json_parse(body, body_len, &root) != ANX_OK) {
		const char *err = "{\"error\":\"invalid JSON\"}";
		httpd_send_response(conn, 400, "Bad Request",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	name = anx_json_string(anx_json_get(&root, "name"));
	desc = anx_json_string(anx_json_get(&root, "description"));
	if (!name || !name[0]) {
		anx_json_free(&root);
		const char *err = "{\"error\":\"missing name\"}";
		httpd_send_response(conn, 400, "Bad Request",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	rc = anx_wf_create(name, desc ? desc : "", &oid);
	anx_json_free(&root);

	if (rc != ANX_OK) {
		const char *err = "{\"error\":\"create failed\"}";
		httpd_send_response(conn, 500, "Internal Server Error",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	oid_to_hex(&oid, hex);
	buf_append(resp, &off, sizeof(resp), "{\"status\":\"ok\",\"name\":\"");
	buf_append(resp, &off, sizeof(resp), name);
	buf_append(resp, &off, sizeof(resp), "\",\"oid\":\"");
	buf_append(resp, &off, sizeof(resp), hex);
	buf_append(resp, &off, sizeof(resp), "\"}");
	httpd_send_response(conn, 201, "Created", "application/json", resp, off);
}

/* ------------------------------------------------------------------ */
/* POST /api/v1/workflow/<name>/run                                    */
/* ------------------------------------------------------------------ */

static void handle_workflow_run(struct anx_tcp_conn *conn, const char *name)
{
	anx_oid_t oid;
	char resp[128];
	uint32_t off = 0;
	int rc;

	if (httpd_wf_find(name, &oid) != ANX_OK) {
		const char *err = "{\"error\":\"not found\"}";
		httpd_send_response(conn, 404, "Not Found",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	rc = anx_wf_run(&oid, NULL);
	if (rc != ANX_OK) {
		buf_append(resp, &off, sizeof(resp), "{\"error\":\"run failed\",\"code\":");
		{ char n[12]; itoa_simple((uint32_t)rc, n, sizeof(n)); buf_append(resp, &off, sizeof(resp), n); }
		buf_append(resp, &off, sizeof(resp), "}");
		httpd_send_response(conn, 500, "Internal Server Error",
				    "application/json", resp, off);
		return;
	}

	{
		struct anx_wf_object *wf = anx_wf_object_get(&oid);
		const char *state = wf ? wf_state_str(wf->run_state) : "unknown";

		buf_append(resp, &off, sizeof(resp), "{\"status\":\"ok\",\"state\":\"");
		buf_append(resp, &off, sizeof(resp), state);
		buf_append(resp, &off, sizeof(resp), "\"}");
	}
	httpd_send_response(conn, 200, "OK", "application/json", resp, off);
}

/* ------------------------------------------------------------------ */
/* POST /api/v1/agent                                                  */
/* ------------------------------------------------------------------ */

static void handle_agent(struct anx_tcp_conn *conn,
			  const char *body, uint32_t body_len)
{
	struct anx_json_value root;
	const char *goal;
	anx_oid_t out_oids[ANX_WF_MAX_PORTS];
	uint32_t out_count = 0;
	char *resp;
	uint32_t off = 0;
	uint32_t cap;
	uint32_t i;
	int rc;

	if (!body || body_len == 0) {
		const char *err = "{\"error\":\"empty body\"}";
		httpd_send_response(conn, 400, "Bad Request",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	if (anx_json_parse(body, body_len, &root) != ANX_OK) {
		const char *err = "{\"error\":\"invalid JSON\"}";
		httpd_send_response(conn, 400, "Bad Request",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	goal = anx_json_string(anx_json_get(&root, "goal"));
	if (!goal || !goal[0]) {
		anx_json_free(&root);
		const char *err = "{\"error\":\"missing goal\"}";
		httpd_send_response(conn, 400, "Bad Request",
				    "application/json",
				    err, (uint32_t)anx_strlen(err));
		return;
	}

	rc = anx_agent_cell_dispatch(goal, out_oids, ANX_WF_MAX_PORTS, &out_count);
	anx_json_free(&root);

	cap = 128 + out_count * 40;
	resp = anx_alloc(cap);
	if (!resp)
		return;

	if (rc != ANX_OK) {
		buf_append(resp, &off, cap, "{\"status\":\"error\",\"code\":");
		{ char n[12]; itoa_simple((uint32_t)rc, n, sizeof(n)); buf_append(resp, &off, cap, n); }
		buf_append(resp, &off, cap, "}");
		httpd_send_response(conn, 200, "OK", "application/json", resp, off);
	} else {
		buf_append(resp, &off, cap, "{\"status\":\"ok\",\"output_count\":");
		{ char n[12]; itoa_simple(out_count, n, sizeof(n)); buf_append(resp, &off, cap, n); }
		buf_append(resp, &off, cap, ",\"oids\":[");
		for (i = 0; i < out_count; i++) {
			char hex[33];
			if (i > 0) buf_append(resp, &off, cap, ",");
			oid_to_hex(&out_oids[i], hex);
			buf_append(resp, &off, cap, "\"");
			buf_append(resp, &off, cap, hex);
			buf_append(resp, &off, cap, "\"");
		}
		buf_append(resp, &off, cap, "]}");
		httpd_send_response(conn, 200, "OK", "application/json", resp, off);
	}

	anx_free(resp);
}

/* Handle a single HTTP request on a connected socket */
static void httpd_handle_request(struct anx_tcp_conn *conn)
{
	char *req_buf;
	uint32_t total = 0;
	int n;
	int body_off;
	uint32_t content_len;
	bool is_get;
	bool is_post;

	req_buf = anx_alloc(HTTPD_REQ_BUF_SIZE);
	if (!req_buf) {
		anx_tcp_srv_close(conn);
		return;
	}

	/* Read request headers (wait for \r\n\r\n) */
	while (total < HTTPD_REQ_BUF_SIZE - 1) {
		n = anx_tcp_srv_recv(conn, req_buf + total,
				      HTTPD_REQ_BUF_SIZE - 1 - total,
				      HTTPD_RECV_TIMEOUT_MS);
		if (n <= 0)
			break;
		total += (uint32_t)n;
		req_buf[total] = '\0';

		/* Check if we have complete headers */
		body_off = find_body(req_buf, total);
		if (body_off > 0) {
			/* Read remaining body if Content-Length says more */
			content_len = parse_content_length(req_buf,
							    (uint32_t)body_off);
			if (content_len > 0) {
				uint32_t body_have = total - (uint32_t)body_off;

				while (body_have < content_len &&
				       total < HTTPD_REQ_BUF_SIZE - 1) {
					n = anx_tcp_srv_recv(conn,
							      req_buf + total,
							      HTTPD_REQ_BUF_SIZE - 1 - total,
							      HTTPD_RECV_TIMEOUT_MS);
					if (n <= 0)
						break;
					total += (uint32_t)n;
					body_have += (uint32_t)n;
				}
				req_buf[total] = '\0';
			}
			break;
		}
	}

	if (total == 0) {
		anx_free(req_buf);
		anx_tcp_srv_close(conn);
		return;
	}

	body_off = find_body(req_buf, total);

	/* Parse method and path from first line */
	is_get = (anx_strncmp(req_buf, "GET ", 4) == 0);
	is_post = (anx_strncmp(req_buf, "POST ", 5) == 0);

	if (is_get) {
		const char *path = req_buf + 4;	/* skip "GET " */
		char wf_name[ANX_WF_NAME_MAX];
		char wf_sub[32];

		if (anx_strncmp(path, "/api/v1/health", 14) == 0) {
			handle_health(conn);
		} else if (anx_strncmp(path, "/api/v1/fb", 10) == 0) {
			handle_fb_info(conn);
		} else if (anx_strncmp(path, "/api/v1/display/modes", 21) == 0) {
			handle_display_modes(conn);
		} else if (anx_strncmp(path, "/api/v1/jepa", 12) == 0) {
			handle_jepa_status(conn);
		} else if (parse_wf_path(path, wf_name, sizeof(wf_name),
					 wf_sub, sizeof(wf_sub))) {
			if (wf_name[0] == '\0') {
				handle_workflow_list(conn);
			} else if (anx_strcmp(wf_sub, "show") == 0) {
				handle_workflow_show(conn, wf_name);
			} else if (anx_strcmp(wf_sub, "graph") == 0) {
				handle_workflow_graph(conn, wf_name);
			} else {
				handle_workflow_status(conn, wf_name);
			}
		} else {
			const char *err = "{\"error\":\"not found\"}";

			httpd_send_response(conn, 404, "Not Found",
					     "application/json",
					     err, (uint32_t)anx_strlen(err));
		}
	} else if (is_post) {
		const char *path = req_buf + 5;	/* skip "POST " */
		const char *body = NULL;
		uint32_t body_len = 0;
		char wf_name[ANX_WF_NAME_MAX];
		char wf_sub[32];

		if (body_off > 0 && (uint32_t)body_off < total) {
			body = req_buf + body_off;
			body_len = total - (uint32_t)body_off;
		}

		if (anx_strncmp(path, "/api/v1/exec", 12) == 0) {
			handle_exec(conn, body, body_len);
		} else if (anx_strncmp(path, "/api/v1/agent", 13) == 0) {
			handle_agent(conn, body, body_len);
		} else if (parse_wf_path(path, wf_name, sizeof(wf_name),
					  wf_sub, sizeof(wf_sub))) {
			if (wf_name[0] == '\0') {
				handle_workflow_create(conn, body, body_len);
			} else if (anx_strcmp(wf_sub, "run") == 0) {
				handle_workflow_run(conn, wf_name);
			} else {
				const char *err = "{\"error\":\"unknown workflow action\"}";
				httpd_send_response(conn, 404, "Not Found",
						     "application/json",
						     err, (uint32_t)anx_strlen(err));
			}
		} else {
			const char *err = "{\"error\":\"not found\"}";

			httpd_send_response(conn, 404, "Not Found",
					     "application/json",
					     err, (uint32_t)anx_strlen(err));
		}
	} else {
		const char *err = "{\"error\":\"method not allowed\"}";

		httpd_send_response(conn, 405, "Method Not Allowed",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
	}

	anx_free(req_buf);
	anx_tcp_srv_close(conn);
}

/* Accept callback — invoked when a TCP handshake completes */
static void httpd_accept(struct anx_tcp_conn *conn, void *arg)
{
	(void)arg;
	pending_conn = conn;
}

int anx_httpd_init(uint16_t port)
{
	int ret;

	ret = anx_tcp_listen(port, httpd_accept, NULL);
	if (ret != ANX_OK) {
		kprintf("httpd: failed to listen on port %u (%d)\n",
			(uint32_t)port, ret);
		return ret;
	}

	httpd_ready = true;
	kprintf("httpd: API server listening on port %u\n", (uint32_t)port);
	return ANX_OK;
}

void anx_httpd_poll(void)
{
	if (!httpd_ready)
		return;

	if (pending_conn) {
		struct anx_tcp_conn *conn = pending_conn;

		pending_conn = NULL;
		httpd_handle_request(conn);
	}
}
