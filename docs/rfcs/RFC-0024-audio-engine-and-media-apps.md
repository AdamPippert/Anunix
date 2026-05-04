# RFC-0024: Audio Engine and Media Player Apps

| Field      | Value                                                     |
|------------|-----------------------------------------------------------|
| RFC        | 0024                                                      |
| Title      | Audio Engine and Media Player Apps                        |
| Author     | Adam Pippert                                              |
| Status     | Draft                                                     |
| Created    | 2026-05-02                                                |
| Updated    | 2026-05-02                                                |
| Depends On | RFC-0002, RFC-0003, RFC-0010, RFC-0012, RFC-0018          |

---

## Executive Summary

Anunix needs first-class media support that fits the State Object model. Existing media work in `kernel/core/iface/media.c` provides session lifecycle and a circular audio queue but no engine, no mixer, no decode pipeline, and no app surface. This RFC defines:

1. **Audio engine** (`kernel/core/audio/`) — software mixer, sample-format negotiation, device routing through a thin driver layer. Pull-based: a sink driver requests N samples at a target rate; the mixer combines all live `ANX_OBJ_AUDIO_CLIP` streams and returns the buffer.
2. **Media object types** — `ANX_OBJ_AUDIO_CLIP` (PCM payload + format header) and `ANX_OBJ_VIDEO_CLIP` (raw RGBA frame container with frame index).
3. **Audio player app** — opens an audio clip OID, registers it with the mixer, runs to completion. Cell type `ANX_CELL_AUDIO_PLAYER`.
4. **Video player app** — opens a video clip OID, decodes frames into an iface Canvas surface at the clip's frame rate. Cell type `ANX_CELL_VIDEO_PLAYER`.
5. **Workflow integration** — both apps publish workflow templates that compose with workflow editing (clips become OID inputs to graph nodes).

Custom code is preferred over vendored decoders. v1 ships PCM only; vendored libopus/dav1d are options for a future codec phase.

---

## 1. Problem Statement

`iface/media.c` is a baseline session struct and a queue. Nothing produces or consumes audio frames. There is no concept of "play this OID through the speakers" because there is no mixer, no device endpoint, and no media object schema. Video has no representation at all. The user-visible app surface is empty.

---

## 2. Audio Engine

### 2.1 Sample Format

Internal mixer format is **signed 16-bit little-endian PCM**, interleaved channels, fixed sample rate of 48 kHz. All input streams are converted on enqueue.

```c
struct anx_audio_format {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;   /* 8, 16, 24, 32 */
    uint8_t  is_float;          /* 0=int, 1=float (only on input) */
    uint8_t  reserved[3];
};
```

### 2.2 Stream

```c
struct anx_audio_stream {
    anx_oid_t              source_oid;     /* ANX_OBJ_AUDIO_CLIP */
    struct anx_audio_format src_fmt;
    uint64_t               src_offset;     /* current read offset (samples) */
    uint64_t               src_total;      /* total samples in source */
    int16_t                volume_q15;     /* 0..32767, 1.0 = 32767 */
    bool                   loop;
    bool                   paused;
    bool                   in_use;
    uint16_t               id;             /* assigned by mixer */
};
```

Up to `ANX_AUDIO_MAX_STREAMS = 16` concurrent streams.

### 2.3 Mixer API

```c
void anx_audio_init(void);
int  anx_audio_stream_add(const anx_oid_t *src, struct anx_audio_stream **out);
int  anx_audio_stream_remove(uint16_t id);
int  anx_audio_stream_pause(uint16_t id, bool paused);
int  anx_audio_stream_set_volume(uint16_t id, int16_t volume_q15);

/* Pull `frames` stereo samples into `dst` (frames * 2 * sizeof(int16_t)). */
int  anx_audio_mix_pull(int16_t *dst, uint32_t frames);
```

The mixer reads each live stream's source bytes via the State Object payload, converts to the internal format, applies volume in Q15, sums into a 32-bit accumulator, clamps to int16, and writes interleaved.

### 2.4 Device Sink

A device sink is a driver that periodically calls `anx_audio_mix_pull` and forwards the buffer to hardware. v1 ships a **null sink** (drains the mixer with no output) and a **wav-file sink** (writes to a State Object) so tests can verify the mix is correct without hardware.

```c
struct anx_audio_sink_ops {
    int  (*open)(struct anx_audio_format *neg_fmt);
    int  (*write)(const int16_t *frames, uint32_t frame_count);
    void (*close)(void);
};

int anx_audio_sink_register(const char *name, const struct anx_audio_sink_ops *ops);
int anx_audio_sink_select(const char *name);
```

---

## 3. Media Object Types

### 3.1 `ANX_OBJ_AUDIO_CLIP`

Payload layout:

```
[ struct anx_audio_clip_hdr ][ raw PCM samples ]
```

```c
#define ANX_AUDIO_CLIP_MAGIC 0x4143_4C50U  /* "ACLP" */

struct anx_audio_clip_hdr {
    uint32_t magic;
    uint32_t version;             /* 1 */
    struct anx_audio_format fmt;
    uint64_t frame_count;         /* one frame == channels samples */
    uint64_t payload_offset;      /* always sizeof(hdr) */
};
```

### 3.2 `ANX_OBJ_VIDEO_CLIP`

Payload layout:

```
[ struct anx_video_clip_hdr ][ raw RGBA8888 frames, frame_count * w*h*4 bytes ]
```

```c
#define ANX_VIDEO_CLIP_MAGIC 0x56434C50U  /* "VCLP" */

struct anx_video_clip_hdr {
    uint32_t magic;
    uint32_t version;             /* 1 */
    uint32_t width;
    uint32_t height;
    uint32_t fps_x256;            /* fps in 24.8 fixed point */
    uint64_t frame_count;
    uint64_t payload_offset;
};
```

(A future codec phase replaces raw RGBA with compressed frames + an index of keyframes.)

---

## 4. Cell Types

```c
ANX_CELL_AUDIO_PLAYER
ANX_CELL_VIDEO_PLAYER
```

Both are dispatched by `workflow_exec.c` on intent prefixes:

| Intent              | Inputs                  | Output                              |
|---------------------|-------------------------|-------------------------------------|
| `audio-play`        | clip OID                | trace OID (with sample count)       |
| `audio-mix`         | clip OID list           | mixed clip OID (`ANX_OBJ_AUDIO_CLIP`) |
| `video-play`        | clip OID, surface OID   | trace OID                           |
| `video-frame`       | clip OID, frame index   | RGBA byte-data OID                  |

`audio-mix` and `video-frame` are pure transforms — useful for workflow composition (e.g. concat two clips, or extract a thumbnail from a video).

---

## 5. Workflow Templates

### 5.1 `anx:app/audio-player`

```
TRIGGER ──► STATE_REF (clip OID) ──► CELL_CALL "audio-play" ──► OUTPUT (trace)
```

### 5.2 `anx:app/video-player`

```
TRIGGER ──► STATE_REF (clip OID) ──► CELL_CALL "video-play" ──► OUTPUT (trace)
```

### 5.3 `anx:app/audio-mix`

```
TRIGGER ──► STATE_REF (clip A) ──┐
                                  FAN_IN ──► CELL_CALL "audio-mix" ──► OUTPUT
TRIGGER ──► STATE_REF (clip B) ──┘
```

---

## 6. Test Plan

1. `test_audio_mixer` — single stream pulls correct samples; two streams sum and clamp; volume Q15 attenuation.
2. `test_audio_clip` — pack/unpack `anx_audio_clip_hdr`; size validation; bad magic rejected.
3. `test_audio_player_cell` — `audio-play` cell consumes the entire clip and reports frame count.
4. `test_video_clip` — pack/unpack `anx_video_clip_hdr`; frame extraction returns correct bytes.
5. `test_video_player_cell` — `video-play` walks all frames and writes to a sink surface.

---

## 7. Implementation Plan

| Phase | Deliverable                                                  | Status |
|-------|--------------------------------------------------------------|--------|
| 1     | RFC + scaffolding + Makefile wiring                          | this RFC |
| 2     | Mixer + null/wav sink + clip object types                    | v1     |
| 3     | Audio player cell + workflow dispatch + template             | v1     |
| 4     | Video player cell + iface canvas integration + template      | v1     |
| 5     | ALSA / Core Audio sinks                                      | future |
| 6     | Vendored libopus / dav1d for compressed payloads             | future |

---

## 8. References

- RFC-0012 §4 (Surface and content node taxonomy — audio as content node)
- RFC-0018 §5 (Workflow template wire format)
- existing `kernel/core/iface/media.c` (baseline session)
