#ifndef ANX_KERNEL_PROFILE_H
#define ANX_KERNEL_PROFILE_H

#include <anx/types.h>

enum anx_kernel_arch {
	ANX_KERNEL_X86_64 = 1,
	ANX_KERNEL_ARM64 = 2,
	ANX_KERNEL_HETERIS = 3,
};

/* Build identity, not runtime policy or permission to promote an image. */
struct anx_kernel_profile {
	uint32_t schema;
	uint32_t architecture;
	uint32_t research_test;
};

const struct anx_kernel_profile *anx_kernel_profile_current(void);
const char *anx_kernel_profile_marker(void);
/* EINVAL: malformed profile; ENOTSUP: incompatible compiled configuration. */
int anx_kernel_profile_check(const struct anx_kernel_profile *required);

#endif
