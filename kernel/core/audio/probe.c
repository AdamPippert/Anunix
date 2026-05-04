/*
 * probe.c — Hardware audio sink probe entry point.
 *
 * Each hardware backend has its own init that self-registers a named
 * sink only on success.  This file is the single hook the audio core
 * calls during anx_audio_init() so we don't sprinkle conditional
 * compile fences across the engine itself.
 *
 * The host test build (ANX_HOST_TEST) doesn't link the PCI / MMIO
 * driver code, so we no-op there.  The kernel builds fan out to the
 * appropriate backends based on the target architecture.
 */

#include <anx/audio.h>
#include <anx/types.h>

#if !defined(ANX_HOST_TEST)
#  include <anx/hda.h>
#  if defined(__aarch64__)
#    include <anx/apple_audio.h>
#  endif
#endif

int
anx_audio_probe_hw_sinks(void)
{
#if defined(ANX_HOST_TEST)
	/* Host tests run without real hardware. */
	return ANX_OK;
#else
	/* HDA — works on x86_64 and on arm64 QEMU virt with attached
	 * intel-hda PCIe device.  Returns ANX_ENODEV if no controller
	 * is found, which we ignore. */
	(void)anx_hda_init();

#  if defined(__aarch64__)
	/* Apple Silicon native audio (real M1/M2 hardware only). */
	(void)anx_apple_audio_init();
#  endif

	return ANX_OK;
#endif
}
