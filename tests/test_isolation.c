/*
 * test_isolation.c — Tests for P2-003: untrusted app/process isolation.
 *
 * Tests:
 * - P2-003-U01: forbidden cross-domain access denied
 * - P2-003-U02: compromised sandboxed cell cannot reach trusted cell data
 * - P2-003-U03: violation events logged with provenance
 */

#include <anx/types.h>
#include <anx/isolation.h>
#include <anx/uuid.h>
#include <anx/string.h>

#define ASSERT(cond, code)    do { if (!(cond)) return (code); } while (0)
#define ASSERT_EQ(a, b, code) do { if ((a) != (b)) return (code); } while (0)

/* ------------------------------------------------------------------ */
/* P2-003-U01: forbidden cross-domain access denied                    */
/* ------------------------------------------------------------------ */

static int test_cross_domain_denied(void)
{
	struct anx_ipc_policy policy;
	anx_cid_t trusted_cell, untrusted_cell;
	int rc;

	anx_isolation_init();

	anx_uuid_generate(&trusted_cell);
	anx_uuid_generate(&untrusted_cell);

	/* Assign domains. */
	rc = anx_isolation_set_domain(trusted_cell,   ANX_DOMAIN_TRUSTED);
	ASSERT_EQ(rc, ANX_OK, -100);
	rc = anx_isolation_set_domain(untrusted_cell, ANX_DOMAIN_UNTRUSTED);
	ASSERT_EQ(rc, ANX_OK, -101);

	/* Default policy: deny all cross-domain access. */
	anx_memset(&policy, 0, sizeof(policy));

	/* Untrusted→Trusted denied. */
	rc = anx_isolation_check(&policy, untrusted_cell, trusted_cell,
	                          "kernel_heap");
	ASSERT_EQ(rc, ANX_EPERM, -102);

	/* Trusted→Trusted allowed once we set the policy entry. */
	policy.allow[ANX_DOMAIN_TRUSTED][ANX_DOMAIN_TRUSTED] = true;
	anx_cid_t trusted2;
	anx_uuid_generate(&trusted2);
	rc = anx_isolation_set_domain(trusted2, ANX_DOMAIN_TRUSTED);
	ASSERT_EQ(rc, ANX_OK, -103);

	rc = anx_isolation_check(&policy, trusted_cell, trusted2, "shared_mem");
	ASSERT_EQ(rc, ANX_OK, -104);

	/* Untrusted→Trusted still denied even though trusted→trusted allowed. */
	rc = anx_isolation_check(&policy, untrusted_cell, trusted2, "shared_mem");
	ASSERT_EQ(rc, ANX_EPERM, -105);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P2-003-U02: sandboxed cell cannot reach unrelated data              */
/* ------------------------------------------------------------------ */

static int test_sandboxed_isolation(void)
{
	struct anx_ipc_policy policy;
	anx_cid_t renderer, kernel, other_renderer;
	int rc;

	anx_isolation_init();

	anx_uuid_generate(&renderer);
	anx_uuid_generate(&kernel);
	anx_uuid_generate(&other_renderer);

	/* Renderer is sandboxed; kernel is trusted. */
	anx_isolation_set_domain(renderer,       ANX_DOMAIN_SANDBOXED);
	anx_isolation_set_domain(kernel,         ANX_DOMAIN_TRUSTED);
	anx_isolation_set_domain(other_renderer, ANX_DOMAIN_SANDBOXED);

	/* Policy: sandboxed cells may only talk to restricted domain. */
	anx_memset(&policy, 0, sizeof(policy));
	policy.allow[ANX_DOMAIN_SANDBOXED][ANX_DOMAIN_RESTRICTED] = true;

	/* Sandboxed→Trusted: denied. */
	rc = anx_isolation_check(&policy, renderer, kernel, "kernel_state");
	ASSERT_EQ(rc, ANX_EPERM, -200);

	/* Sandboxed→Sandboxed: denied (not in policy). */
	rc = anx_isolation_check(&policy, renderer, other_renderer, "surface_shm");
	ASSERT_EQ(rc, ANX_EPERM, -201);

	/* Sandboxed→Restricted: allowed. */
	anx_cid_t broker;
	anx_uuid_generate(&broker);
	anx_isolation_set_domain(broker, ANX_DOMAIN_RESTRICTED);

	rc = anx_isolation_check(&policy, renderer, broker, "ipc_channel");
	ASSERT_EQ(rc, ANX_OK, -202);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P2-003-U03: violation events logged with provenance                 */
/* ------------------------------------------------------------------ */

static int test_violation_log(void)
{
	struct anx_ipc_policy policy;
	struct anx_violation_event events[4];
	anx_cid_t bad_cell, good_cell;
	uint32_t count;
	int rc;

	anx_isolation_init();

	anx_uuid_generate(&bad_cell);
	anx_uuid_generate(&good_cell);

	anx_isolation_set_domain(bad_cell,  ANX_DOMAIN_UNTRUSTED);
	anx_isolation_set_domain(good_cell, ANX_DOMAIN_TRUSTED);

	/* Deny-all policy. */
	anx_memset(&policy, 0, sizeof(policy));

	/* No violations yet. */
	ASSERT_EQ(anx_isolation_violation_count(), 0u, -300);

	/* Trigger two violations. */
	rc = anx_isolation_check(&policy, bad_cell, good_cell, "resource_A");
	ASSERT_EQ(rc, ANX_EPERM, -301);

	rc = anx_isolation_check(&policy, bad_cell, good_cell, "resource_B");
	ASSERT_EQ(rc, ANX_EPERM, -302);

	ASSERT_EQ(anx_isolation_violation_count(), 2u, -303);

	/* Retrieve log. */
	rc = anx_isolation_violation_log(events, 4, &count);
	ASSERT_EQ(rc, ANX_OK, -304);
	ASSERT_EQ(count, 2u, -305);

	/* First violation: offender domain is UNTRUSTED, owner domain TRUSTED. */
	ASSERT_EQ(events[0].offender_domain, ANX_DOMAIN_UNTRUSTED, -306);
	ASSERT_EQ(events[0].owner_domain,    ANX_DOMAIN_TRUSTED,   -307);
	ASSERT(anx_strcmp(events[0].message, "resource_A") == 0,   -308);

	/* Second violation message. */
	ASSERT(anx_strcmp(events[1].message, "resource_B") == 0, -309);

	/* Reset clears the log. */
	anx_isolation_violation_reset();
	ASSERT_EQ(anx_isolation_violation_count(), 0u, -310);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int test_isolation(void)
{
	int rc;

	rc = test_cross_domain_denied();
	if (rc) return rc;

	rc = test_sandboxed_isolation();
	if (rc) return rc;

	rc = test_violation_log();
	if (rc) return rc;

	return 0;
}
