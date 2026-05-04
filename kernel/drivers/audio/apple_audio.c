/*
 * apple_audio.c — Apple Silicon native audio scaffold.
 *
 * Detects the M1/M2 SoC family and prepares the hooks that future
 * codec, MCA, and DPLL drivers will register against.  See the matching
 * header for an overview of the full path.
 *
 * On x86_64 this translation unit compiles to nothing — the audio
 * probe never calls into it because the probe layer is itself guarded
 * with __aarch64__.
 */

#include <anx/apple_audio.h>
#include <anx/audio.h>
#include <anx/types.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#if defined(__aarch64__)

/*
 * Read MIDR_EL1 (Main ID Register) — the implementer field tells us
 * whether we're on Apple silicon.  Apple's implementer code is 0x61
 * ('a') and the part number identifies the specific SoC generation.
 */
static uint64_t
read_midr_el1(void)
{
	uint64_t v;
	__asm__ __volatile__("mrs %0, MIDR_EL1" : "=r"(v));
	return v;
}

#define APPLE_IMPLEMENTER	0x61u

static struct {
	bool     present;
	bool     ready;		/* codec stack online */
	uint16_t part_num;	/* MIDR part number */
} g_apple_audio;

bool
anx_apple_audio_present(void)
{
	return g_apple_audio.ready;
}

int
anx_apple_audio_init(void)
{
	uint64_t midr;
	uint8_t  implementer;
	uint16_t part;

	if (g_apple_audio.present)
		return g_apple_audio.ready ? ANX_OK : ANX_ENODEV;
	g_apple_audio.present = true;

	midr        = read_midr_el1();
	implementer = (uint8_t)((midr >> 24) & 0xFF);
	part        = (uint16_t)((midr >> 4) & 0xFFF);

	if (implementer != APPLE_IMPLEMENTER) {
		/* Not Apple silicon — nothing to register here. */
		return ANX_ENODEV;
	}
	g_apple_audio.part_num = part;

	/*
	 * The codec sequence (DPLL bring-up, MCA configuration, I2S
	 * routing, TAS5770L / CS42L84 register fills, SMC speaker-amp
	 * gating) is staged in a follow-on series of drivers under
	 * kernel/drivers/audio/apple/.  Until those land we leave the
	 * sink unregistered so anx_audio_sink_select("apple") returns
	 * ANX_ENOENT and the system happily falls back to the null/HDA
	 * sinks.  This is the same conservative approach Asahi Linux
	 * took for the first several months of M1 audio bring-up.
	 */
	kprintf("[apple-audio] M1/M2 (MIDR part 0x%x) detected; codec "
		"bring-up pending — using null sink\n", (unsigned)part);

	return ANX_ENODEV;	/* not yet ready */
}

#else  /* !defined(__aarch64__) */

/* x86_64 build sees a single trivial stub so the link still works
 * if (somehow) probe.c is compiled with the Apple header included. */
bool anx_apple_audio_present(void) { return false; }
int  anx_apple_audio_init(void)    { return ANX_ENODEV; }

#endif
