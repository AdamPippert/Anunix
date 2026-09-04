/*
 * test_config.c — RLM-driven configuration over the world graph (RFC-0027).
 *
 * Verifies that configuration round-trips as a governed config.* world-graph
 * slice, that the schema validator type-checks declared keys at the commit gate,
 * that a text proposal applies atomically, and that an RLM rollout (with a test
 * inference double) drives an evidence-backed change through the same gate.
 */

#include <anx/types.h>
#include <anx/config.h>
#include <anx/worldgraph.h>
#include <anx/rlm.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/memplane.h>
#include <anx/sched.h>
#include <anx/string.h>
#include <anx/uuid.h>

/* Inference double: return a fixed configuration proposal, then stop. */
static int cfg_infer(const struct anx_rlm_infer_req *req,
		     struct anx_rlm_infer_resp *resp)
{
	const char *out = "SET system greeting fromrlm";

	(void)req;
	if (!resp)
		return ANX_EINVAL;
	anx_strlcpy(resp->content, out, sizeof(resp->content));
	resp->content_len = (uint32_t)anx_strlen(out);
	resp->input_tokens = 1;
	resp->output_tokens = 1;
	resp->stop = true;
	resp->status = ANX_OK;
	return ANX_OK;
}

static int count_cb(const char *area, const char *key, const char *value,
		    void *arg)
{
	(void)area;
	(void)key;
	(void)value;
	(*(int *)arg)++;
	return 0;
}

int test_config(void)
{
	char val[ANX_CONFIG_VAL_MAX];
	struct anx_world_commit_report rep;
	int n;

	anx_objstore_init();
	anx_cell_store_init();
	anx_memplane_init();
	anx_sched_init();
	anx_rlm_init();
	anx_world_runtime_init();	/* clears providers; do before config */
	anx_config_init();

	/* --- Basic set / get / update --- */
	if (anx_config_set("system", "greeting", "hello") != ANX_OK)
		return -1;
	if (anx_config_get("system", "greeting", val, sizeof(val)) != ANX_OK)
		return -2;
	if (anx_strcmp(val, "hello") != 0)
		return -3;
	if (anx_config_set("system", "greeting", "hi") != ANX_OK)
		return -4;
	if (anx_config_get("system", "greeting", val, sizeof(val)) != ANX_OK)
		return -5;
	if (anx_strcmp(val, "hi") != 0)
		return -6;

	/* Missing key. */
	if (anx_config_get("system", "nope", val, sizeof(val)) != ANX_ENOENT)
		return -7;

	/* --- Typed schema: INT --- */
	if (anx_config_declare("net", "mtu", ANX_CFG_INT, NULL) != ANX_OK)
		return -8;
	if (anx_config_set("net", "mtu", "1500") != ANX_OK)
		return -9;
	/* A non-integer is rejected by the commit gate. */
	if (anx_config_set("net", "mtu", "abc") == ANX_OK)
		return -10;
	/* And the old value survives the rejection. */
	if (anx_config_get("net", "mtu", val, sizeof(val)) != ANX_OK)
		return -11;
	if (anx_strcmp(val, "1500") != 0)
		return -12;

	/* --- Typed schema: ENUM --- */
	if (anx_config_declare("system", "log_level", ANX_CFG_ENUM,
			       "debug,info,warn,error") != ANX_OK)
		return -13;
	if (anx_config_set("system", "log_level", "warn") != ANX_OK)
		return -14;
	if (anx_config_set("system", "log_level", "loud") == ANX_OK)
		return -15;
	if (anx_config_get("system", "log_level", val, sizeof(val)) != ANX_OK)
		return -16;
	if (anx_strcmp(val, "warn") != 0)
		return -17;

	/* --- list --- */
	n = 0;
	if (anx_config_list(NULL, count_cb, &n) != ANX_OK)
		return -18;
	if (n != 3)	/* greeting, mtu, log_level */
		return -19;
	n = 0;
	if (anx_config_list("net", count_cb, &n) != ANX_OK)
		return -20;
	if (n != 1)
		return -21;

	/* --- Proposal apply: atomic multi-set, prose ignored --- */
	if (anx_configurator_apply(
		"here is my plan:\n"
		"SET system greeting bonjour\n"
		"SET net mtu 9000\n"
		"done.\n", &rep) != ANX_OK)
		return -22;
	if (anx_config_get("system", "greeting", val, sizeof(val)) != ANX_OK ||
	    anx_strcmp(val, "bonjour") != 0)
		return -23;
	if (anx_config_get("net", "mtu", val, sizeof(val)) != ANX_OK ||
	    anx_strcmp(val, "9000") != 0)
		return -24;

	/* A proposal that violates a type is rejected as a whole. */
	if (anx_configurator_apply("SET net mtu notanint\n", &rep) == ANX_OK)
		return -25;
	if (anx_config_get("net", "mtu", val, sizeof(val)) != ANX_OK ||
	    anx_strcmp(val, "9000") != 0)
		return -26;

	/* A proposal with no directives is a clean no-op error. */
	if (anx_configurator_apply("just chatting, no directives\n", &rep)
	    != ANX_ENOENT)
		return -27;

	/* --- RLM-driven: rollout proposes, gate applies --- */
	anx_rlm_set_infer(cfg_infer);
	if (anx_configurator_run("make the greeting friendlier", &rep) != ANX_OK)
		return -28;
	if (anx_config_get("system", "greeting", val, sizeof(val)) != ANX_OK)
		return -29;
	if (anx_strcmp(val, "fromrlm") != 0)
		return -30;
	anx_rlm_set_infer(NULL);

	return 0;
}
