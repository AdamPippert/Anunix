/* Semantic transfer destinations must retain authority and exact byte identity. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/xfer.h>
#include <anx/string.h>

int anx_research_day035(void)
{
	struct anx_xfer_policy policy;
	struct anx_xfer_session session = {0};
	struct anx_xfer_result result;
	uint8_t digest[32];
	char long_uri[ANX_XFER_DST_MAX + 1], long_tag[ANX_XFER_TAG_MAX + 1];
	const char *invalid[] = {
		"", "anx:", "anx://", "anx://store/../secret", "anx://store/./out",
		"anx://store//out", "anx://store/%2e%2e/out", "anx://store\\evil/out",
		"anx://store@evil/out", "anx://store/out?target=evil", "anx://store/out#other",
		"anx://store/with space", "anx://store/line\nbreak",
		"anx://:80/out", "anx://store:bad/out", "anx://store:65536/out", "anx://store:/out",
	};
	anx_xfer_policy_init(&policy, "research-day-035", 0);
	if (anx_xfer_policy_allow(&policy, "anx://store") != ANX_OK) return -3500;
	if (anx_xfer_policy_check(&policy, "anx://store-extra/out") != ANX_EPERM) return -3501;
	if (anx_xfer_policy_check(&policy, "anx://store") != ANX_OK ||
	    anx_xfer_policy_check(&policy, "anx://store/a/b") != ANX_OK) return -3502;
	for (uint32_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		if (anx_xfer_policy_check(&policy, invalid[i]) != ANX_EINVAL) return -3503;
	if (anx_xfer_policy_allow(&policy, "") != ANX_EINVAL || policy.allowed_count != 1) return -3504;
	policy.allowed_count = ANX_XFER_POLICY_RULES_MAX + 1;
	if (anx_xfer_policy_check(&policy, "anx://store/a") != ANX_EINVAL) return -3504;
	policy.allowed_count = 2;
	policy.allowed[1].active = true;
	if (anx_xfer_policy_check(&policy, "anx://store/a") != ANX_EINVAL) return -3504;
	policy.allowed_count = 1;
	anx_memset(long_uri, 'x', sizeof(long_uri)); long_uri[sizeof(long_uri) - 1] = 0;
	anx_memset(long_tag, 't', sizeof(long_tag)); long_tag[sizeof(long_tag) - 1] = 0;
	if (anx_xfer_policy_allow(&policy, long_uri) != ANX_EINVAL ||
	    anx_xfer_begin(&policy, long_uri, "anx://store/out", NULL, &session) != ANX_EINVAL ||
	    anx_xfer_begin(&policy, "src://object", long_uri, NULL, &session) != ANX_EINVAL ||
	    anx_xfer_begin(&policy, "src://object", "anx://store/out", long_tag, &session) != ANX_EINVAL)
		return -3505;
	if (anx_xfer_begin(&policy, "src://object", "anx://store/out", "day035", &session) != ANX_OK ||
	    anx_xfer_write(&session, "semantic", 8) != ANX_OK ||
	    anx_xfer_interrupt(&session) != ANX_OK || session.resume_offset != 8 ||
	    anx_xfer_resume(&session) != ANX_OK || anx_xfer_write(&session, "-state", 6) != ANX_OK ||
	    anx_xfer_commit(&session, &result) != ANX_OK) return -3506;
	anx_sha256("semantic-state", 14, digest);
	if (!result.hash_valid || result.bytes_transferred != 14 ||
	    anx_memcmp(result.final_hash.bytes, digest, 32) ||
	    anx_strcmp(result.dest_uri, "anx://store/out") || anx_strcmp(result.provenance_tag, "day035")) return -3506;
	if (anx_xfer_begin(&policy, "src://object", "anx://store/out", NULL, &session) != ANX_OK ||
	    anx_xfer_write(&session, "semantic", 8) != ANX_OK) return -3507;
	policy.allowed[0].active = false;
	if (anx_xfer_write(&session, "-state", 6) != ANX_EPERM || session.bytes_written != 8 ||
	    session.state != ANX_XFER_ABORTED) return -3507;
	policy.allowed[0].active = true;
	if (anx_xfer_begin(&policy, "src://object", "anx://store/out", NULL, &session) != ANX_OK ||
	    anx_xfer_write(&session, "semantic", 8) != ANX_OK) return -3508;
	policy.allowed[0].active = false;
	anx_memset(&result, 0xa5, sizeof(result));
	if (anx_xfer_commit(&session, &result) != ANX_EPERM || result.hash_valid || result.bytes_transferred ||
	    session.state != ANX_XFER_ABORTED) return -3508;
	policy.allowed[0].active = true;
	if (anx_xfer_begin(&policy, "src://object", "anx://store/out", NULL, &session) != ANX_OK) return -3509;
	session.bytes_written = ~(uint64_t)0;
	if (anx_xfer_write(&session, "X", 1) != ANX_EFULL || session.state != ANX_XFER_ABORTED) return -3509;
	anx_xfer_policy_init(&policy, "research-day-035-path", 0);
	if (anx_xfer_policy_allow(&policy, "anx://store/allowed") != ANX_OK ||
	    anx_xfer_policy_check(&policy, "anx://store/allowed-more/out") != ANX_EPERM ||
	    anx_xfer_policy_check(&policy, "anx://store/allowed/out") != ANX_OK) return -3510;
	return ANX_OK;
}
#endif
