/*
 * anx/apple_audio.h — Apple Silicon native audio scaffold.
 *
 * The M1/M2 audio path is multi-stage:
 *   1) MCA  — Multi-Channel Audio IP block in the SoC, drives I2S buses.
 *   2) PDM  — Pulse-Density Modulation interface for built-in mics.
 *   3) DPLL — clock generator for the I2S domain.
 *   4) SMC  — System Management Controller, handles speaker amp power.
 *   5) Codec chips on the I2S buses:
 *         - TI TAS5770L / TAS5754M (built-in speakers, M1 / M1 Pro / M2)
 *         - Cirrus CS42L84       (mic preamp on M2)
 *         - HDMI audio stream    (when wired through DP-Alt-mode)
 *
 * All the codec ICs are I2C-controlled and require firmware register
 * sequences derived from Apple's calibration data shipped per-machine.
 *
 * v1 status: scaffold only.  This file gives the audio subsystem a
 * stable hook (anx_apple_audio_init) so the probe path compiles on
 * arm64 and can register an "apple" sink once the codec drivers are
 * available.  Returns ANX_ENODEV on every M1/M2 right now — the
 * Asahi-equivalent codec stack is tracked separately and lands in
 * staged drops (DPLL → MCA → I2S → codec → speaker calibration).
 */

#ifndef ANX_APPLE_AUDIO_H
#define ANX_APPLE_AUDIO_H

#include <anx/types.h>

int  anx_apple_audio_init(void);
bool anx_apple_audio_present(void);

#endif /* ANX_APPLE_AUDIO_H */
