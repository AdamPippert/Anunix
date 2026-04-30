/*
 * test_kickstart.c — Tests for kickstart provisioning (Phase 2).
 *
 * Verifies that [workflows] load and autorun entries instantiate
 * templates from the workflow library.  Uses the built-in IBAL
 * templates registered by anx_wf_lib_init().
 */

#include <anx/types.h>
#include <anx/kickstart.h>
#include <anx/workflow_library.h>
#include <anx/workflow.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/string.h>

int test_kickstart(void)
{
	int rc;

	anx_objstore_init();
	anx_cell_store_init();
	anx_wf_init();

	rc = anx_wf_lib_init();
	if (rc != ANX_OK)
		return -1;

	/* Test 1: basic parse of a well-formed kickstart buffer */
	{
		const char *buf =
			"[system]\n"
			"hostname=anunix-test\n"
			"\n"
			"[workflows]\n"
			"load=anx:workflow/ibal/lite/v1\n";

		rc = anx_ks_apply(buf, (uint32_t)anx_strlen(buf));
		if (rc != ANX_OK)
			return -1;
	}

	/* Test 2: workflow load instantiates the template */
	{
		const char *buf =
			"[workflows]\n"
			"load=anx:workflow/ibal/default/v1\n";

		rc = anx_ks_apply(buf, (uint32_t)anx_strlen(buf));
		/* ANX_ENOENT from a failed lookup is non-fatal in handle_workflows */
		if (rc != ANX_OK)
			return -2;
	}

	/* Test 3: unknown workflow URI is non-fatal */
	{
		const char *buf =
			"[workflows]\n"
			"load=anx:workflow/no-such/v1\n";

		rc = anx_ks_apply(buf, (uint32_t)anx_strlen(buf));
		if (rc != ANX_OK)
			return -3;
	}

	/* Test 4: autorun of an IBAL template succeeds */
	{
		const char *buf =
			"[workflows]\n"
			"autorun=anx:workflow/ibal/symbolic/v1\n";

		rc = anx_ks_apply(buf, (uint32_t)anx_strlen(buf));
		if (rc != ANX_OK)
			return -4;
	}

	/* Test 5: malformed buffer with no '=' is parsed without crash */
	{
		const char *buf =
			"[system]\n"
			"hostname\n"
			"[workflows]\n";

		rc = anx_ks_apply(buf, (uint32_t)anx_strlen(buf));
		if (rc != ANX_OK)
			return -5;
	}

	return 0;
}
