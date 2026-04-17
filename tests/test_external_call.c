/*
 * test_external_call.c — External-call cell task type (RFC-0003 Section 7).
 *
 * Exercises the handler registry and the runtime wiring that fires
 * it from a cell whose type is ANX_CELL_TASK_EXTERNAL_CALL.
 */

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/state_object.h>
#include <anx/string.h>

/* Handler that echoes request bytes back into the response buffer. */
static int echo_handler(struct anx_external_call *call, void *ctx)
{
	uint32_t n;
	uint32_t *invocations = ctx;

	(*invocations)++;

	n = call->request_size;
	if (n > sizeof(call->response_buf))
		n = sizeof(call->response_buf);
	if (call->request_body && n > 0) {
		const uint8_t *src = call->request_body;
		uint32_t i;

		for (i = 0; i < n; i++)
			call->response_buf[i] = src[i];
	}
	call->response_size = n;
	call->status_code = 200;
	return ANX_OK;
}

static int failing_handler(struct anx_external_call *call, void *ctx)
{
	(void)call; (void)ctx;
	return ANX_EIO;
}

int test_external_call(void)
{
	struct anx_external_call call;
	uint32_t invocations = 0;
	int ret;

	anx_external_init();
	anx_objstore_init();
	anx_cell_store_init();

	/* --- Direct registry API --- */

	if (anx_external_register_handler("mock", echo_handler,
					  &invocations) != ANX_OK)
		return -1;

	anx_memset(&call, 0, sizeof(call));
	anx_strlcpy(call.endpoint, "mock://echo", sizeof(call.endpoint));
	anx_strlcpy(call.method, "POST", sizeof(call.method));
	call.request_body = "hello";
	call.request_size = 5;

	ret = anx_external_invoke(&call);
	if (ret != ANX_OK)
		return -2;
	if (invocations != 1)
		return -3;
	if (call.response_size != 5)
		return -4;
	if (anx_memcmp(call.response_buf, "hello", 5) != 0)
		return -5;
	if (call.status_code != 200)
		return -6;

	/* Unknown scheme -> ENOENT */
	anx_memset(&call, 0, sizeof(call));
	anx_strlcpy(call.endpoint, "pg://nohandler", sizeof(call.endpoint));
	if (anx_external_invoke(&call) != ANX_ENOENT)
		return -7;

	/* Malformed endpoint (no scheme separator) -> EINVAL */
	anx_memset(&call, 0, sizeof(call));
	anx_strlcpy(call.endpoint, "garbage", sizeof(call.endpoint));
	if (anx_external_invoke(&call) != ANX_EINVAL)
		return -8;

	/* Unregister */
	if (anx_external_unregister_handler("mock") != ANX_OK)
		return -9;
	if (anx_external_unregister_handler("mock") != ANX_ENOENT)
		return -10;

	anx_memset(&call, 0, sizeof(call));
	anx_strlcpy(call.endpoint, "mock://gone", sizeof(call.endpoint));
	if (anx_external_invoke(&call) != ANX_ENOENT)
		return -11;

	/* --- Runtime integration: cell dispatches the call --- */
	{
		struct anx_cell *cell;
		struct anx_cell_intent intent;
		struct anx_external_call c2;

		invocations = 0;
		if (anx_external_register_handler("mock", echo_handler,
						  &invocations) != ANX_OK)
			return -20;

		anx_memset(&intent, 0, sizeof(intent));
		anx_strlcpy(intent.name, "ext_call_cell", sizeof(intent.name));
		if (anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent,
				    &cell) != ANX_OK)
			return -21;

		anx_memset(&c2, 0, sizeof(c2));
		anx_strlcpy(c2.endpoint, "mock://run", sizeof(c2.endpoint));
		anx_strlcpy(c2.method, "QUERY", sizeof(c2.method));
		c2.request_body = "abcd";
		c2.request_size = 4;
		cell->ext_call = &c2;

		if (anx_cell_run(cell) != ANX_OK)
			return -22;
		if (cell->status != ANX_CELL_COMPLETED)
			return -23;
		if (invocations != 1)
			return -24;
		if (c2.response_size != 4)
			return -25;
		if (anx_memcmp(c2.response_buf, "abcd", 4) != 0)
			return -26;

		anx_cell_destroy(cell);
		anx_external_unregister_handler("mock");
	}

	/* --- Runtime integration: handler error propagates --- */
	{
		struct anx_cell *cell;
		struct anx_cell_intent intent;
		struct anx_external_call c3;

		if (anx_external_register_handler("bad", failing_handler,
						  NULL) != ANX_OK)
			return -30;

		anx_memset(&intent, 0, sizeof(intent));
		anx_strlcpy(intent.name, "ext_fail", sizeof(intent.name));
		if (anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent,
				    &cell) != ANX_OK)
			return -31;

		anx_memset(&c3, 0, sizeof(c3));
		anx_strlcpy(c3.endpoint, "bad://err", sizeof(c3.endpoint));
		cell->ext_call = &c3;

		if (anx_cell_run(cell) != ANX_EIO)
			return -32;
		if (cell->status != ANX_CELL_FAILED)
			return -33;

		anx_cell_destroy(cell);
		anx_external_unregister_handler("bad");
	}

	return 0;
}
