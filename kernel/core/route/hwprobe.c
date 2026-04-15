/*
 * hwprobe.c — Hardware capability probing.
 *
 * Calls arch_probe_hw() once at boot to discover hardware.
 * Benchmark support is stubbed until model servers can run inference.
 */

#include <anx/types.h>
#include <anx/hwprobe.h>
#include <anx/arch.h>
#include <anx/string.h>

static struct anx_hw_inventory hw_inv;

void anx_hwprobe_init(void)
{
	anx_memset(&hw_inv, 0, sizeof(hw_inv));
	arch_probe_hw(&hw_inv);
}

const struct anx_hw_inventory *anx_hwprobe_get(void)
{
	return &hw_inv;
}

int anx_hwprobe_bench_engine(struct anx_engine *engine)
{
	(void)engine;
	return ANX_ENOSYS;
}
