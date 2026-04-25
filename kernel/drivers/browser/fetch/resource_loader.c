/*
 * resource_loader.c — Sub-resource pre-scan and fetch queue.
 *
 * Pre-scan algorithm (O(n)):
 *   Walk the raw HTML byte stream looking for '<' characters.  For each
 *   tag, check if the tag name is "link", "script", or "img" (case-
 *   insensitive).  If so, extract relevant attributes within the tag's
 *   byte span (up to the closing '>') and resolve the URL.
 *
 * This runs on the raw response body before the HTML tokenizer starts,
 * so CSS fetch latency overlaps with the tokenizer/parser pipeline.
 *
 * URL resolution:
 *   Absolute http:// URLs are used as-is.
 *   Root-relative (/path) URLs are resolved against the scheme+host of base.
 *   Relative paths are resolved against the directory part of base URL.
 *   https:// URLs are skipped (in-kernel TLS not yet supported).
 *   data: URIs are skipped.
 */

#include "resource_loader.h"
#include "https_proxy.h"
#include <anx/alloc.h>
#include <anx/http.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* ── URL helpers ─────────────────────────────────────────────────── */

static void resolve_url(const char *base, const char *rel,
			  char *out, uint32_t out_cap)
{
	out[0] = '\0';

	/* Skip data: and javascript: URIs */
	if (anx_strncmp(rel, "data:", 5) == 0 ||
	    anx_strncmp(rel, "javascript:", 11) == 0)
		return;

	/* Absolute URL */
	if (anx_strncmp(rel, "http://", 7) == 0 ||
	    anx_strncmp(rel, "https://", 8) == 0) {
		anx_strlcpy(out, rel, out_cap);
		return;
	}

	/* Need base for relative resolution */
	if (!base || !base[0]) {
		anx_strlcpy(out, rel, out_cap);
		return;
	}

	if (rel[0] == '/') {
		/* Root-relative: scheme + host from base */
		const char *p = base;
		while (*p && *p != ':') p++;
		if (*p == ':') p += 3; /* skip "://" */
		while (*p && *p != '/') p++;
		uint32_t prefix = (uint32_t)(p - base);
		if (prefix >= out_cap) return;
		anx_memcpy(out, base, prefix);
		anx_strlcpy(out + prefix, rel, out_cap - prefix);
		return;
	}

	/* Relative: resolve against directory of base */
	const char *last_slash = base;
	const char *p = base;
	while (*p) {
		if (*p == '/') last_slash = p;
		p++;
	}
	uint32_t dir_len = (uint32_t)(last_slash + 1 - base);
	if (dir_len >= out_cap) return;
	anx_memcpy(out, base, dir_len);
	anx_strlcpy(out + dir_len, rel, out_cap - dir_len);
}

/*
 * Parse host, port, and path from an http:// or https:// URL.
 * Sets *is_https on output.
 * Returns 0 on success, -1 if unsupported scheme or malformed.
 */
static int rl_parse_url(const char *url,
			  char *host, uint32_t host_cap,
			  uint16_t *port,
			  char *path, uint32_t path_cap,
			  bool *is_https)
{
	const char *p = url;

	*is_https = false;
	if (anx_strncmp(p, "http://", 7) == 0) {
		p += 7;
		*port = 80;
	} else if (anx_strncmp(p, "https://", 8) == 0) {
		p += 8;
		*port     = 443;
		*is_https = true;
	} else {
		return -1;
	}

	const char *hs = p;
	while (*p && *p != '/' && *p != ':') p++;
	uint32_t hlen = (uint32_t)(p - hs);
	if (hlen == 0 || hlen >= host_cap) return -1;
	anx_memcpy(host, hs, hlen);
	host[hlen] = '\0';

	if (*p == ':') {
		p++;
		uint16_t pv = 0;
		while (*p >= '0' && *p <= '9')
			pv = (uint16_t)(pv * 10 + (*p++ - '0'));
		*port = pv;
	}

	anx_strlcpy(path, (*p == '/') ? p : "/", path_cap);
	return 0;
}

/* ── Attribute extraction ────────────────────────────────────────── */

/*
 * Find the value of attr_name within a tag's raw byte span [tag, tag+len).
 * Handles both single- and double-quoted values, and unquoted values.
 * Case-insensitive attribute name match.
 */
static void extract_attr(const char *tag, uint32_t len,
			   const char *attr_name,
			   char *out, uint32_t out_cap)
{
	uint32_t alen = (uint32_t)anx_strlen(attr_name);
	uint32_t i;

	out[0] = '\0';
	if (len < alen + 1) return;

	for (i = 0; i + alen < len; i++) {
		/* Case-insensitive attr name match */
		uint32_t j;
		bool match = true;
		for (j = 0; j < alen; j++) {
			char tc = tag[i + j] | 0x20; /* fold to lowercase */
			if (tc != attr_name[j]) { match = false; break; }
		}
		if (!match) continue;

		/* Attr name must be preceded by whitespace or be at tag start */
		if (i > 0 && tag[i-1] != ' ' && tag[i-1] != '\t' &&
		    tag[i-1] != '\n' && tag[i-1] != '\r')
			continue;

		uint32_t k = i + alen;
		while (k < len && (tag[k] == ' ' || tag[k] == '\t')) k++;
		if (k >= len || tag[k] != '=') continue;
		k++;
		while (k < len && (tag[k] == ' ' || tag[k] == '\t')) k++;
		if (k >= len) return;

		char q = tag[k];
		if (q == '"' || q == '\'') {
			k++;
			uint32_t vs = k;
			while (k < len && tag[k] != q) k++;
			uint32_t vl = k - vs;
			if (vl >= out_cap) vl = out_cap - 1;
			anx_memcpy(out, tag + vs, vl);
			out[vl] = '\0';
		} else {
			uint32_t vs = k;
			while (k < len && tag[k] != ' ' && tag[k] != '>' &&
			       tag[k] != '\t') k++;
			uint32_t vl = k - vs;
			if (vl >= out_cap) vl = out_cap - 1;
			anx_memcpy(out, tag + vs, vl);
			out[vl] = '\0';
		}
		return;
	}
}

/* ── URL deduplication ───────────────────────────────────────────── */

static bool rq_has_url(const struct resource_queue *q, const char *url)
{
	uint32_t i;
	for (i = 0; i < q->n_res; i++)
		if (anx_strcmp(q->res[i].url, url) == 0)
			return true;
	return false;
}

static void rq_add(struct resource_queue *q, uint8_t type,
		    const char *base_url, const char *raw_url)
{
	if (q->n_res >= RES_MAX) return;

	char resolved[RES_URL_MAX];
	resolve_url(base_url, raw_url, resolved, sizeof(resolved));
	if (!resolved[0]) return;
	if (rq_has_url(q, resolved)) return;

	struct sub_resource *r = &q->res[q->n_res++];
	anx_memset(r, 0, sizeof(*r));
	r->type = type;
	anx_strlcpy(r->url, resolved, RES_URL_MAX);
}

/* ── Public API ──────────────────────────────────────────────────── */

void rq_prescan(struct resource_queue *q, const char *base_url,
		 const char *html, uint32_t html_len)
{
	uint32_t i = 0;

	while (i < html_len && q->n_res < RES_MAX) {
		if (html[i] != '<') { i++; continue; }

		uint32_t tag_start = i + 1; /* skip '<' */
		/* Find closing '>' */
		uint32_t j = tag_start;
		while (j < html_len && html[j] != '>') j++;
		if (j >= html_len) break;

		const char *tag = html + tag_start;
		uint32_t    tlen = j - tag_start;
		i = j + 1; /* advance past '>' */

		if (tlen < 3) continue;

		char url[RES_URL_MAX];
		char rel_buf[64];
		uint8_t c0 = (uint8_t)(tag[0] | 0x20);

		/* <link rel="stylesheet" href="URL"> */
		if (c0 == 'l' && tlen >= 4 &&
		    (tag[1]|0x20)=='i' && (tag[2]|0x20)=='n' && (tag[3]|0x20)=='k' &&
		    (tlen < 5 || tag[4] == ' ' || tag[4] == '\t')) {
			extract_attr(tag, tlen, "rel", rel_buf, sizeof(rel_buf));
			/* rel may be "stylesheet" or "stylesheet preload" etc. */
			bool is_css = false;
			const char *rp = rel_buf;
			while (*rp) {
				while (*rp == ' ') rp++;
				if (anx_strncmp(rp, "stylesheet", 10) == 0)
					{ is_css = true; break; }
				while (*rp && *rp != ' ') rp++;
			}
			if (is_css) {
				extract_attr(tag, tlen, "href", url, sizeof(url));
				if (url[0]) rq_add(q, RES_TYPE_CSS, base_url, url);
			}
			continue;
		}

		/* <script src="URL"> */
		if (c0 == 's' && tlen >= 6 &&
		    (tag[1]|0x20)=='c' && (tag[2]|0x20)=='r' &&
		    (tag[3]|0x20)=='i' && (tag[4]|0x20)=='p' && (tag[5]|0x20)=='t' &&
		    (tlen < 7 || tag[6] == ' ' || tag[6] == '\t')) {
			extract_attr(tag, tlen, "src", url, sizeof(url));
			if (url[0]) rq_add(q, RES_TYPE_JS, base_url, url);
			continue;
		}

		/* <img src="URL"> */
		if (c0 == 'i' && tlen >= 3 &&
		    (tag[1]|0x20)=='m' && (tag[2]|0x20)=='g' &&
		    (tlen < 4 || tag[3] == ' ' || tag[3] == '\t')) {
			extract_attr(tag, tlen, "src", url, sizeof(url));
			if (url[0]) rq_add(q, RES_TYPE_IMG, base_url, url);
			continue;
		}

		/* <source src="URL"> (video/audio/picture) */
		if (c0 == 's' && tlen >= 6 &&
		    (tag[1]|0x20)=='o' && (tag[2]|0x20)=='u' &&
		    (tag[3]|0x20)=='r' && (tag[4]|0x20)=='c' && (tag[5]|0x20)=='e' &&
		    (tlen < 7 || tag[6] == ' ' || tag[6] == '\t')) {
			extract_attr(tag, tlen, "src", url, sizeof(url));
			if (url[0]) rq_add(q, RES_TYPE_IMG, base_url, url);
		}
	}

	kprintf("resource_loader: prescan found %u sub-resources\n", q->n_res);
}

static uint32_t fetch_by_type(struct resource_queue *q, uint8_t type)
{
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < q->n_res; i++) {
		struct sub_resource *r = &q->res[i];
		if (r->type != type || r->fetched || r->failed) continue;

		char     host[256], path[512];
		uint16_t port;
		bool     is_https;
		if (rl_parse_url(r->url, host, sizeof(host), &port,
				  path, sizeof(path), &is_https) != 0) {
			r->failed = true;
			kprintf("resource_loader: skip %s (unsupported scheme)\n",
				r->url);
			continue;
		}

		struct anx_http_response resp;
		anx_memset(&resp, 0, sizeof(resp));
		int rc = is_https ? https_fetch(host, path, &resp)
			          : anx_http_get(host, port, path, &resp);
		if (rc == 0 && resp.body &&
		    resp.status_code >= 200 && resp.status_code < 300) {
			r->body     = resp.body;
			r->body_len = resp.body_len;
			r->fetched  = true;
			resp.body   = NULL; /* transfer ownership */
			count++;
			kprintf("resource_loader: fetched %s (%u bytes)\n",
				r->url, r->body_len);
		} else {
			r->failed = true;
			kprintf("resource_loader: failed %s (rc=%d status=%d)\n",
				r->url, rc, resp.status_code);
		}
		if (resp.body) anx_http_response_free(&resp);
	}
	return count;
}

uint32_t rq_fetch_blocking(struct resource_queue *q)
{
	uint32_t n = fetch_by_type(q, RES_TYPE_CSS);
	n += fetch_by_type(q, RES_TYPE_JS);
	return n;
}

uint32_t rq_fetch_images(struct resource_queue *q)
{
	return fetch_by_type(q, RES_TYPE_IMG);
}

void rq_free(struct resource_queue *q)
{
	uint32_t i;
	for (i = 0; i < q->n_res; i++) {
		if (q->res[i].body) {
			anx_free(q->res[i].body);
			q->res[i].body = NULL;
		}
	}
	q->n_res = 0;
}
