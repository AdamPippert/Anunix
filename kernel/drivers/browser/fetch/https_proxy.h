/*
 * https_proxy.h — HTTPS fetch via HTTP CONNECT tunnel to anxbproxy.
 *
 * The kernel browser driver does not implement TLS.  Instead, HTTPS
 * requests are tunnelled through a local HTTP CONNECT proxy (anxbproxy)
 * running on the host OS.  The proxy terminates TLS; the kernel sees only
 * plain HTTP on the loopback path.
 *
 * Proxy address: ANXBPROXY_IP:ANXBPROXY_PORT.
 * Default: 10.0.2.2:8118 (QEMU user-mode host → guest).
 */

#ifndef ANX_BROWSER_HTTPS_PROXY_H
#define ANX_BROWSER_HTTPS_PROXY_H

#include <anx/types.h>
#include <anx/http.h>
#include <anx/net.h>

/* Override at compile time if the proxy runs elsewhere. */
#ifndef ANXBPROXY_IP
#define ANXBPROXY_IP  ANX_IP4(10, 0, 2, 2)   /* QEMU user-mode host */
#endif
#ifndef ANXBPROXY_PORT
#define ANXBPROXY_PORT 8118
#endif

/*
 * Fetch https://host/path via the CONNECT proxy.
 * Populates resp on success (caller frees with anx_http_response_free).
 * Returns 0 on success, -1 on proxy/network error.
 */
int https_fetch(const char *host, const char *path,
		 struct anx_http_response *resp);

#endif /* ANX_BROWSER_HTTPS_PROXY_H */
