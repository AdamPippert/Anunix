#include <anx/kernel_profile.h>

#ifdef ANX_HOST_TEST
#define ANX_SOURCE_SHA256 "host-test-unavailable"
#else
#include <anx_source_identity.h>
#endif

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
	" research_test=" PROFILE_RESEARCH_NAME "\n"
	"ANUNIX_SOURCE_PROFILE_V1 sha256=" ANX_SOURCE_SHA256 "\n";

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
	if (!required || required->schema != 1 ||
	    required->architecture < ANX_KERNEL_X86_64 ||
	    required->architecture > ANX_KERNEL_HETERIS || required->research_test > 1)
		return ANX_EINVAL;
	if (required->architecture != current.architecture ||
	    required->research_test != current.research_test)
		return ANX_ENOTSUP;
	return ANX_OK;
}
