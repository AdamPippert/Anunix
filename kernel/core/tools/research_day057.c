/* A sustained regime exit must not silently validate a different workload. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/regime.h>

int anx_research_day057(void)
{
	anx_regime_reset();
	for (uint32_t i = 0; i < 200; i++) anx_regime_observe(1000);
	for (uint32_t i = 0; i < 220; i++) anx_regime_observe(5000);
	int ret = anx_regime_current() == ANX_REGIME_ESCALATED ? ANX_OK : -5701;
	anx_regime_reset();
	return ret;
}
#endif
