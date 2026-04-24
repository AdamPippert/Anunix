/*
 * test_transfer_policy.c — Deterministic tests for P1-005:
 * transfer/import/export policy hooks + object integration.
 *
 * Tests:
 * - P1-005-U01: invalid destination policy rejected
 * - P1-005-I02: interrupted transfer resumes; final hash matches expected
 * - P1-005-I03: object metadata/provenance recorded for completed transfer
 */

#include <anx/types.h>
#include <anx/xfer.h>
#include <anx/crypto.h>
#include <anx/string.h>

#define ASSERT(cond, code)    do { if (!(cond)) return (code); } while (0)
#define ASSERT_EQ(a, b, code) do { if ((a) != (b)) return (code); } while (0)

/* ------------------------------------------------------------------ */
/* P1-005-U01: invalid destination policy rejected                     */
/* ------------------------------------------------------------------ */

static int test_policy_enforcement(void)
{
	struct anx_xfer_policy policy;
	struct anx_xfer_session sess;
	int rc;

	/* Empty policy — all destinations denied. */
	anx_xfer_policy_init(&policy, "strict", 0);

	rc = anx_xfer_policy_check(&policy, "anx://local/file.txt");
	ASSERT_EQ(rc, ANX_EPERM, -100);

	rc = anx_xfer_policy_check(&policy, "anx://remote/file.txt");
	ASSERT_EQ(rc, ANX_EPERM, -101);

	/* Allow local/ prefix. */
	rc = anx_xfer_policy_allow(&policy, "anx://local/");
	ASSERT_EQ(rc, ANX_OK, -102);

	/* Exact prefix match: allowed. */
	rc = anx_xfer_policy_check(&policy, "anx://local/file.txt");
	ASSERT_EQ(rc, ANX_OK, -103);

	/* Nested path within allowed prefix: allowed. */
	rc = anx_xfer_policy_check(&policy, "anx://local/a/b/c.bin");
	ASSERT_EQ(rc, ANX_OK, -104);

	/* Different scheme: still rejected. */
	rc = anx_xfer_policy_check(&policy, "anx://remote/file.txt");
	ASSERT_EQ(rc, ANX_EPERM, -105);

	/* Partially matching prefix (no trailing slash match): rejected. */
	rc = anx_xfer_policy_check(&policy, "anx://localhost/file.txt");
	ASSERT_EQ(rc, ANX_EPERM, -106);

	/* Begin to rejected destination returns ANX_EPERM. */
	rc = anx_xfer_begin(&policy, "src://data", "anx://remote/out.bin",
	                     "test", &sess);
	ASSERT_EQ(rc, ANX_EPERM, -107);

	/* Begin to allowed destination succeeds. */
	rc = anx_xfer_begin(&policy, "src://data", "anx://local/out.bin",
	                     "test", &sess);
	ASSERT_EQ(rc, ANX_OK, -108);
	ASSERT_EQ(sess.state, ANX_XFER_ACTIVE, -109);

	/* Overflow the allow-list. */
	anx_xfer_policy_init(&policy, "overflow-test", 0);
	uint32_t i;
	for (i = 0; i < ANX_XFER_POLICY_RULES_MAX; i++) {
		rc = anx_xfer_policy_allow(&policy, "anx://");
		ASSERT_EQ(rc, ANX_OK, -110);
	}
	rc = anx_xfer_policy_allow(&policy, "anx://");
	ASSERT_EQ(rc, ANX_EFULL, -111);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-005-I02: interrupted transfer resumes; hash matches              */
/* ------------------------------------------------------------------ */

static int test_resume_hash_integrity(void)
{
	struct anx_xfer_policy policy;
	struct anx_xfer_session sess;
	struct anx_xfer_result result;
	uint8_t expected[32];
	int rc;

	anx_xfer_policy_init(&policy, "resume-test", 0);
	rc = anx_xfer_policy_allow(&policy, "anx://local/");
	ASSERT_EQ(rc, ANX_OK, -200);

	rc = anx_xfer_begin(&policy, "src://stream",
	                     "anx://local/out.bin", "", &sess);
	ASSERT_EQ(rc, ANX_OK, -201);

	/* Write first chunk. */
	rc = anx_xfer_write(&sess, "hello", 5);
	ASSERT_EQ(rc, ANX_OK, -202);
	ASSERT_EQ(sess.bytes_written, 5u, -203);

	/* Interrupt — saves resume point. */
	rc = anx_xfer_interrupt(&sess);
	ASSERT_EQ(rc, ANX_OK, -204);
	ASSERT_EQ(sess.state, ANX_XFER_INTERRUPTED, -205);
	ASSERT_EQ(sess.resume_offset, 5u, -206);

	/* Write while interrupted must fail. */
	rc = anx_xfer_write(&sess, "X", 1);
	ASSERT_EQ(rc, ANX_EINVAL, -207);

	/* Resume. */
	rc = anx_xfer_resume(&sess);
	ASSERT_EQ(rc, ANX_OK, -208);
	ASSERT_EQ(sess.state, ANX_XFER_ACTIVE, -209);

	/* Write second chunk. */
	rc = anx_xfer_write(&sess, ", world", 7);
	ASSERT_EQ(rc, ANX_OK, -210);
	ASSERT_EQ(sess.bytes_written, 12u, -211);

	/* Commit and get result. */
	rc = anx_xfer_commit(&sess, &result);
	ASSERT_EQ(rc, ANX_OK, -212);
	ASSERT_EQ(sess.state, ANX_XFER_COMMITTED, -213);

	ASSERT(result.hash_valid, -214);
	ASSERT_EQ(result.bytes_transferred, 12u, -215);

	/* Hash must equal SHA-256("hello, world"). */
	anx_sha256("hello, world", 12, expected);
	ASSERT(anx_memcmp(result.final_hash.bytes, expected, 32) == 0, -216);

	/* Commit again must fail (already committed). */
	rc = anx_xfer_commit(&sess, &result);
	ASSERT_EQ(rc, ANX_EINVAL, -217);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-005-I03: provenance and metadata recorded on commit              */
/* ------------------------------------------------------------------ */

static int test_provenance_recorded(void)
{
	struct anx_xfer_policy policy;
	struct anx_xfer_session sess;
	struct anx_xfer_result result;
	static const char payload[] = "artifact-data";
	uint8_t expected[32];
	int rc;

	anx_xfer_policy_init(&policy, "provenance-test", 0);
	rc = anx_xfer_policy_allow(&policy, "anx://store/");
	ASSERT_EQ(rc, ANX_OK, -300);

	rc = anx_xfer_begin(&policy,
	                     "src://artifact",
	                     "anx://store/artifact.bin",
	                     "build-2026-04-23",
	                     &sess);
	ASSERT_EQ(rc, ANX_OK, -301);
	ASSERT(anx_strcmp(sess.provenance_tag, "build-2026-04-23") == 0, -302);

	rc = anx_xfer_write(&sess, payload, (uint32_t)anx_strlen(payload));
	ASSERT_EQ(rc, ANX_OK, -303);

	rc = anx_xfer_commit(&sess, &result);
	ASSERT_EQ(rc, ANX_OK, -304);

	/* Provenance tag preserved in result. */
	ASSERT(anx_strcmp(result.provenance_tag, "build-2026-04-23") == 0, -305);

	/* Destination URI preserved. */
	ASSERT(anx_strcmp(result.dest_uri, "anx://store/artifact.bin") == 0, -306);

	/* Byte count correct. */
	ASSERT_EQ(result.bytes_transferred, (uint64_t)anx_strlen(payload), -307);

	/* Hash valid and correct. */
	ASSERT(result.hash_valid, -308);
	anx_sha256(payload, (uint32_t)anx_strlen(payload), expected);
	ASSERT(anx_memcmp(result.final_hash.bytes, expected, 32) == 0, -309);

	/* Reset and verify IDLE. */
	anx_xfer_reset(&sess);
	ASSERT_EQ(sess.state, ANX_XFER_IDLE, -310);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Test suite entry point                                               */
/* ------------------------------------------------------------------ */

int test_transfer_policy(void)
{
	int rc;

	rc = test_policy_enforcement();
	if (rc != 0)
		return rc;

	rc = test_resume_hash_integrity();
	if (rc != 0)
		return rc;

	rc = test_provenance_recorded();
	if (rc != 0)
		return rc;

	return 0;
}
