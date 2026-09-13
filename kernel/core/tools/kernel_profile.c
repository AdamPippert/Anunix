#include <anx/kernel_profile.h>

#if defined(__x86_64__)
#define PROFILE_ARCH ANX_KERNEL_X86_64
#define PROFILE_ARCH_NAME "x86_64"
#elif defined(__aarch64__)
#define PROFILE_ARCH ANX_KERNEL_ARM64
#define PROFILE_ARCH_NAME "arm64"
#elif defined(__riscv) && __riscv_xlen == 64
#define PROFILE_ARCH ANX_KERNEL_HETERIS
#define PROFILE_ARCH_NAME "heteris"
#else
#error Unsupported kernel profile architecture
#endif

#ifdef ANX_RESEARCH_TEST
#define PROFILE_RESEARCH 1
#define PROFILE_RESEARCH_NAME "1"
#else
#define PROFILE_RESEARCH 0
#define PROFILE_RESEARCH_NAME "0"
#endif

static const struct anx_kernel_profile current = {1, PROFILE_ARCH, PROFILE_RESEARCH};
static const char marker[] = "ANUNIX_KERNEL_PROFILE_V1 arch=" PROFILE_ARCH_NAME
	" research_test=" PROFILE_RESEARCH_NAME "\n";

const struct anx_kernel_profile *anx_kernel_profile_current(void)
{
	return &current;
}

const char *anx_kernel_profile_marker(void)
{
	return marker;
}

int anx_kernel_profile_check(const struct anx_kernel_profile *required)
{
	(void)required;
	return ANX_ENOSYS;
}
