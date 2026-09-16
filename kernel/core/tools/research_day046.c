/* The running image identifies the source fingerprint compiled into it. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/kernel_profile.h>
#include <anx/string.h>

int anx_research_day046(void)
{
	const char *prefix = "ANUNIX_SOURCE_PROFILE_V1 sha256=";
	const char *found = anx_strstr(anx_kernel_profile_marker(), prefix);
	if (!found) return -4601;
	found += anx_strlen(prefix);
	bool nonzero = false;
	for (uint32_t i = 0; i < 64; i++) {
		if (!((found[i] >= '0' && found[i] <= '9') || (found[i] >= 'a' && found[i] <= 'f')))
			return -4602;
		if (found[i] != '0') nonzero = true;
	}
	if (!nonzero || found[64] != '\n' || anx_strstr(found + 65, prefix)) return -4603;
	return ANX_OK;
}
#endif
