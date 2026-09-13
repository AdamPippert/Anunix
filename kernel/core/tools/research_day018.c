/* A required build profile cannot replace the running configuration. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/kernel_profile.h>
#include <anx/string.h>

int anx_research_day018(void)
{
	const struct anx_kernel_profile *current = anx_kernel_profile_current();
	struct anx_kernel_profile required = *current;
	struct anx_kernel_profile before = *current;

	required.architecture = current->architecture == ANX_KERNEL_X86_64 ?
		ANX_KERNEL_ARM64 : ANX_KERNEL_X86_64;
	if (anx_kernel_profile_check(&required) != ANX_ENOTSUP)
		return -1801;
	required = *current;
	required.research_test ^= 1;
	if (anx_kernel_profile_check(&required) != ANX_ENOTSUP)
		return -1802;
	required = *current;
	required.schema = 2;
	if (anx_kernel_profile_check(&required) != ANX_EINVAL)
		return -1803;
	required = *current;
	required.architecture = 0;
	if (anx_kernel_profile_check(&required) != ANX_EINVAL)
		return -1804;
	required.architecture = 4;
	if (anx_kernel_profile_check(&required) != ANX_EINVAL)
		return -1805;
	required = *current;
	required.research_test = 2;
	if (anx_kernel_profile_check(&required) != ANX_EINVAL ||
	    anx_kernel_profile_check(NULL) != ANX_EINVAL)
		return -1806;
	if (anx_kernel_profile_check(current) != ANX_OK ||
	    anx_memcmp(current, &before, sizeof(before)))
		return -1807;
	return ANX_OK;
}
#endif
