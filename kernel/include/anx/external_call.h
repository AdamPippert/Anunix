/*
 * anx/external_call.h — External-call task type (RFC-0003 Section 7).
 *
 * Wires the ANX_CELL_TASK_EXTERNAL_CALL cell type to a per-scheme
 * handler registry. An external call is dispatched by the scheme
 * prefix of its endpoint (e.g. "pg://", "http://", "mock://"). The
 * handler is synchronous and writes its response bytes into the
 * call's response buffer.
 *
 * External calls are the designated bridge for pushing topology
 * objects to outside systems (for example, PostgreSQL via a pg://
 * handler) or pulling data into the Anunix state graph without
 * baking the transport into the kernel.
 */

#ifndef ANX_EXTERNAL_CALL_H
#define ANX_EXTERNAL_CALL_H

#include <anx/types.h>

#define ANX_EXT_ENDPOINT_MAX	256
#define ANX_EXT_METHOD_MAX	16
#define ANX_EXT_SCHEME_MAX	32
#define ANX_EXT_RESPONSE_MAX	4096

struct anx_external_call {
	char endpoint[ANX_EXT_ENDPOINT_MAX];	/* e.g. "pg://topo/scan?lo=0&hi=1023" */
	char method[ANX_EXT_METHOD_MAX];	/* "GET", "POST", "QUERY", ... */

	const void *request_body;		/* caller-owned */
	uint32_t request_size;

	uint8_t response_buf[ANX_EXT_RESPONSE_MAX];
	uint32_t response_size;			/* bytes written by handler */
	int status_code;			/* handler-defined; 0 = ok */
};

/*
 * Handler signature. The handler inspects call->endpoint and
 * call->method, performs the side effect, and writes up to
 * ANX_EXT_RESPONSE_MAX bytes into call->response_buf, setting
 * call->response_size and call->status_code. Return ANX_OK if the
 * call was dispatched (even if status_code is non-zero); return a
 * negative error for transport failure.
 */
typedef int (*anx_external_handler_fn)(struct anx_external_call *call,
				       void *ctx);

/* One-time registry init. Safe to call multiple times. */
void anx_external_init(void);

/*
 * Register a handler for `scheme` (without the "://" suffix).
 * Replaces any existing handler for the same scheme.
 *
 * Returns ANX_OK, ANX_EINVAL (bad args), or ANX_ENOMEM (registry full).
 */
int anx_external_register_handler(const char *scheme,
				  anx_external_handler_fn fn,
				  void *ctx);

/* Remove a handler. Returns ANX_ENOENT if not registered. */
int anx_external_unregister_handler(const char *scheme);

/*
 * Invoke an external call. Parses the scheme from call->endpoint,
 * looks up the handler, and delegates. Returns ANX_ENOENT if no
 * handler is registered for the scheme.
 */
int anx_external_invoke(struct anx_external_call *call);

#endif /* ANX_EXTERNAL_CALL_H */
