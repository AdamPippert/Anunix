/* model_client credential-boundary regression tests. */

#include <anx/types.h>
#include <anx/model_client.h>
#include <anx/credential.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define CHECK(c) do { if (!(c)) { \
	kprintf("model_client failed at %u\n", __LINE__); return -1; \
} } while (0)

int test_model_client(void)
{
	struct anx_model_endpoint ep = {
		.host = "127.0.0.1",
		.port = 8080,
		.cred_name = "oversized-model-key",
	};
	struct anx_model_request req = {
		.model = "test-model",
		.system = NULL,
		.user_message = "hello",
		.max_tokens = 8,
	};
	struct anx_model_response resp;
	char key[256];

	anx_credstore_init();
	anx_memset(key, 'x', sizeof(key));
	CHECK(anx_credential_create(ep.cred_name, ANX_CRED_API_KEY,
				    key, sizeof(key)) == ANX_OK);
	anx_memset(key, 0, sizeof(key));
	CHECK(anx_model_client_configure(&ep) == ANX_OK);
	CHECK(anx_model_call(&req, &resp) == ANX_ERANGE);
	CHECK(anx_model_client_configure(NULL) == ANX_OK);

	/* Provider-prefixed models select their stable provider credential rather
	 * than the endpoint's legacy credential reference. */
	anx_credstore_init();
	anx_memset(key, 'x', sizeof(key));
	CHECK(anx_credential_create("openai-api-key", ANX_CRED_API_KEY,
				    key, sizeof(key)) == ANX_OK);
	anx_memset(key, 0, sizeof(key));
	ep.cred_name = "missing-legacy-key";
	req.model = "openai/test-model";
	CHECK(anx_model_client_configure(&ep) == ANX_OK);
	CHECK(anx_model_call(&req, &resp) == ANX_ERANGE);
	CHECK(anx_model_client_configure(NULL) == ANX_OK);
	return 0;
}
