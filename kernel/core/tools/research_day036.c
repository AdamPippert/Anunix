/* Delegated reservations share their root allocation and retain parent bounds. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/engine_lease.h>
#include <anx/uuid.h>

#define MIB (1024ULL * 1024)
int anx_research_day036(void)
{
	struct anx_engine_lease *root = NULL, *a = NULL, *b = NULL, *grand = NULL, *replacement = NULL, *extra = NULL;
	struct anx_engine_lease *chain[ANX_LEASE_DEPTH_MAX + 1] = {0};
	struct anx_engine_lease fake = {0};
	anx_eid_t ids[7], id;
	uint64_t before, available;
	uint32_t pct_before, pct;
	int ret = anx_lease_avail_mem(ANX_MEM_L1, &before);
	if (ret == ANX_OK) ret = anx_lease_avail_accel(ANX_ACCEL_GPU, &pct_before);
	if (ret != ANX_OK) return ret;
	for (uint32_t i = 0; i < 7; i++) anx_uuid_generate(&ids[i]);
	ret = anx_lease_grant(&ids[0], ANX_MEM_L1, 64 * MIB, ANX_ACCEL_GPU, 30, &root);
	if (ret != ANX_OK) goto out;
	ret = -3601;
	if (anx_lease_grant_child(root, &ids[1], 65 * MIB, 1, &extra) != ANX_ENOMEM || extra) goto out;
	ret = -3602;
	if (anx_lease_grant_child(root, &ids[1], MIB, 31, &extra) != ANX_ENOMEM || extra) goto out;
	ret = anx_lease_grant_child(root, &ids[1], 40 * MIB, 20, &a);
	if (ret == ANX_OK) ret = anx_lease_grant_child(a, &ids[2], 20 * MIB, 10, &grand);
	if (ret == ANX_OK) ret = anx_lease_grant_child(root, &ids[3], 20 * MIB, 10, &b);
	if (ret != ANX_OK) goto out;
	ret = -3603;
	if (anx_lease_grant_child(root, &ids[4], 5 * MIB, 0, &extra) != ANX_ENOMEM || extra ||
	    anx_lease_grant_child(root, &ids[4], MIB, 1, &extra) != ANX_ENOMEM || extra ||
	    anx_lease_avail_mem(ANX_MEM_L1, &available) != ANX_OK || available != before - 64 * MIB ||
	    anx_lease_avail_accel(ANX_ACCEL_GPU, &pct) != ANX_OK || pct != pct_before - 30 ||
	    grand->parent != a || grand->depth != 2 || anx_lease_release(root) != ANX_EBUSY) goto out;
	grand->mem_used_bytes = 1;
	ret = -3604;
	if (anx_lease_revoke(root) != ANX_EBUSY || root->revoked || a->revoked || grand->revoked) goto out;
	grand->mem_used_bytes = 0;
	ret = anx_lease_revoke(a);
	if (ret != ANX_OK) goto out;
	ret = -3605;
	if (!a->revoked || !grand->revoked || b->revoked || root->revoked ||
	    anx_lease_lookup(&ids[1]) || anx_lease_lookup(&ids[2]) ||
	    anx_lease_grant_child(a, &ids[4], MIB, 1, &extra) != ANX_EPERM || extra ||
	    anx_lease_avail_mem(ANX_MEM_L1, &available) != ANX_OK || available != before - 64 * MIB) goto out;
	ret = anx_lease_grant_child(root, &ids[4], 40 * MIB, 20, &replacement);
	if (ret == ANX_OK) ret = anx_lease_revoke(root);
	if (ret != ANX_OK) goto out;
	ret = -3606;
	if (!b->revoked || !replacement->revoked || anx_lease_lookup(&ids[0]) ||
	    anx_lease_avail_mem(ANX_MEM_L1, &available) != ANX_OK || available != before ||
	    anx_lease_avail_accel(ANX_ACCEL_GPU, &pct) != ANX_OK || pct != pct_before ||
	    anx_lease_grant_child(root, &ids[5], MIB, 1, &extra) != ANX_EPERM || extra ||
	    anx_lease_grant_child(&fake, &ids[5], MIB, 0, &extra) != ANX_ENOENT || extra ||
	    anx_lease_release(&fake) != ANX_ENOENT) goto out;
	ret = anx_lease_grant(&ids[5], ANX_MEM_L1, 64 * MIB, ANX_ACCEL_GPU, 30, &chain[0]);
	if (ret != ANX_OK) goto out;
	for (uint32_t i = 1; i <= ANX_LEASE_DEPTH_MAX; i++) {
		anx_uuid_generate(&id);
		ret = anx_lease_grant_child(chain[i - 1], &id, 64 * MIB, 30, &chain[i]);
		if (ret != ANX_OK) goto out;
	}
	ret = -3607;
	if (anx_lease_grant_child(chain[ANX_LEASE_DEPTH_MAX], &ids[6], MIB, 1, &extra) != ANX_EPERM || extra)
		goto out;
	ret = ANX_OK;
out:
	if (grand) grand->mem_used_bytes = 0;
	if (extra) anx_lease_release(extra);
	for (int i = ANX_LEASE_DEPTH_MAX; i >= 0; i--) if (chain[i]) anx_lease_release(chain[i]);
	if (grand) anx_lease_release(grand);
	if (a) anx_lease_release(a);
	if (b) anx_lease_release(b);
	if (replacement) anx_lease_release(replacement);
	if (root) anx_lease_release(root);
	if (ret == ANX_OK && (anx_lease_avail_mem(ANX_MEM_L1, &available) != ANX_OK || available != before ||
	    anx_lease_avail_accel(ANX_ACCEL_GPU, &pct) != ANX_OK || pct != pct_before)) return -3608;
	return ret;
}
#endif
