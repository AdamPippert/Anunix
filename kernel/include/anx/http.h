/*
 * anx/http.h — Minimal HTTP/1.1 client.
 *
 * Provides GET and POST over TCP with Connection: close semantics.
 * No chunked transfer encoding or redirects — plain HTTP only.
 */

#ifndef ANX_HTTP_H
#define ANX_HTTP_H

#include <anx/types.h>

struct anx_http_response {
	int status_code;
	char *body;
	uint32_t body_len;
};

/* Perform an HTTP GET request */
int anx_http_get(const char *host, uint16_t port, const char *path,
		  struct anx_http_response *resp);

/* GET with extra headers (pre-formatted, each terminated with \r\n) */
int anx_http_get_authed(const char *host, uint16_t port, const char *path,
			 const char *extra_headers,
			 struct anx_http_response *resp);

/* Perform an HTTP POST request */
int anx_http_post(const char *host, uint16_t port, const char *path,
		   const char *content_type,
		   const void *body, uint32_t body_len,
		   struct anx_http_response *resp);

/* POST with extra headers */
int anx_http_post_authed(const char *host, uint16_t port, const char *path,
			  const char *extra_headers,
			  const char *content_type,
			  const void *body, uint32_t body_len,
			  struct anx_http_response *resp);

/* Free the body buffer in a response */
void anx_http_response_free(struct anx_http_response *resp);

#endif /* ANX_HTTP_H */
