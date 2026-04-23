/*
 * resource_loader.h — Sub-resource pre-scan and fetch queue.
 *
 * Implements a two-phase fetch strategy:
 *  Phase 1 (blocking): scan raw HTML for <link rel="stylesheet"> and
 *    <script src=""> patterns; fetch CSS + JS before layout runs so
 *    the first paint has complete styles.
 *  Phase 2 (deferred): fetch <img src=""> resources after first paint.
 *
 * All fetches are synchronous (Anunix HTTP is single-threaded).  The
 * gain over the naive approach is that CSS/JS are fetched before the
 * tokenizer/layout pipeline starts, eliminating style-less first paints.
 */

#ifndef ANX_BROWSER_RESOURCE_LOADER_H
#define ANX_BROWSER_RESOURCE_LOADER_H

#include <anx/types.h>

#define RES_MAX     32     /* max sub-resources per page */
#define RES_URL_MAX 512    /* max absolute URL length */

#define RES_TYPE_CSS 0
#define RES_TYPE_JS  1
#define RES_TYPE_IMG 2

struct sub_resource {
	uint8_t  type;             /* RES_TYPE_* */
	char     url[RES_URL_MAX];
	char    *body;             /* heap-alloc (owned); NULL until fetched */
	uint32_t body_len;
	bool     fetched;
	bool     failed;
};

struct resource_queue {
	struct sub_resource res[RES_MAX];
	uint32_t            n_res;
};

/*
 * Scan raw HTML bytes for sub-resource URL patterns and populate q.
 * base_url is the page URL, used to resolve relative URLs.
 * O(n) single pass; no HTML tokenization required.
 */
void rq_prescan(struct resource_queue *q, const char *base_url,
		 const char *html, uint32_t html_len);

/*
 * Synchronously fetch all CSS and JS resources in q.
 * Returns count of successfully fetched resources.
 */
uint32_t rq_fetch_blocking(struct resource_queue *q);

/*
 * Synchronously fetch all image resources in q (call after first paint).
 * Returns count of successfully fetched resources.
 */
uint32_t rq_fetch_images(struct resource_queue *q);

/* Free all fetched resource bodies. */
void rq_free(struct resource_queue *q);

#endif /* ANX_BROWSER_RESOURCE_LOADER_H */
