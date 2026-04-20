# Anunix 2026.4.19 Release Notes

Milestone: **QEMU graphical browser rendering** — Anunix can now render a live Chromium browser session directly to the framebuffer, streamed from an anxbrowserd daemon running on the QEMU host machine.

## Highlights

- **Browser Renderer Cell** — new Execution Cell (`kernel/core/exec/browser_cell.c`) streams JPEG frames from anxbrowserd over TCP and blits them directly to the framebuffer at ~30 FPS.
- **GOP mode enumeration and selection** — EFI stub now enumerates all GOP display modes, picks the highest-resolution BGRX8888 mode automatically, and stores the full mode list in the boot block for kernel inspection.
- **DPI-aware font scaling** — GUI terminal font scale is computed at runtime from framebuffer width: 1× (<1920), 2× (≥1920), 3× (≥2560), 4× (≥3840). Readable at Framework Laptop 16 native resolution (2560×1600).
- **PIT frame scheduler** — compositor repaint is driven by a 30 FPS PIT timer callback rather than polling, reducing CPU waste in the idle loop.
- **Display diagnostics** — `fb_info`, `gop_list`, `fb_test` shell commands for SSH-based display debugging.
- **QEMU networking** — e1000 NIC + host-forwarded port 8080 in QEMU targets so the kernel can reach anxbrowserd on the host.
- **HTTP API display endpoints** — `GET /api/v1/fb` and `GET /api/v1/display/modes` expose framebuffer geometry and GOP mode list to external tools.

## New Features

### Browser Renderer Cell (`browser_cell.c`)

The Browser Renderer Cell bridges anxbrowserd and the Anunix framebuffer:

1. `browser_init [host [port]]` — creates a Playwright/Chromium session via `POST /api/v1/sessions`, opens a persistent TCP connection to `GET /api/v1/sessions/{sid}/stream_raw`, and begins receiving `[4-byte BE length][JPEG]` frames.
2. `browser <url>` — navigates the active session via `POST /api/v1/sessions/{sid}/navigate`.
3. `browser_stop` — tears down the TCP connection and frees the receive buffer.
4. `anx_browser_cell_tick()` is called from the shell idle poll loop; it performs a non-blocking 1 ms TCP receive, skips HTTP response headers on first arrival, and decodes + blits complete JPEG frames via `anx_jpeg_blit_scaled()`.

Default host: `10.0.2.2` (QEMU NAT host). Default port: 9090.

### EFI GOP Mode Selection

The EFI stub (`kernel/boot/efi/efi_stub.c`) now:
- Enumerates all GOP modes at boot time (up to 16)
- Selects the highest-resolution `PixelBltOnly` or BGRX8888 mode automatically
- Stores mode list in the boot block (`anx_gop_mode_entry[16]`) at `0x1044`
- Calls `gop->SetMode(best_mode)` and re-reads `FrameBufferBase` and pitch post-switch

### Display Diagnostics

Three new ansh commands:

| Command | Description |
|---------|-------------|
| `fb_info` | JSON: width, height, pitch, bpp |
| `gop_list` | All GOP modes from boot block; `*` marks selected |
| `fb_test` | Paints 8-bar SMPTE color pattern to full framebuffer |

### DPI-Aware Font Scaling

`anx_gui_init()` computes `term_font_scale` at runtime:

```
 <1920 px wide → scale 1×  (QEMU default)
≥1920 px wide → scale 2×  (1080p)
≥2560 px wide → scale 3×  (Framework Laptop 16 native)
≥3840 px wide → scale 4×  (4K)
```

### PIT Frame Scheduler

`anx_iface_frame_scheduler_init(30)` installs a callback on the PIT IRQ0 (100 Hz) that fires the compositor repaint every 3 ticks (≈ 33 ms / 30 FPS). The shell idle loop no longer calls `anx_iface_compositor_repaint()` directly — the callback handles it.

### HTTP API Display Endpoints

`kernel/drivers/net/httpd.c` adds:
- `GET /api/v1/fb` — JSON framebuffer geometry (width, height, pitch, bpp)
- `GET /api/v1/display/modes` — JSON array of all GOP modes with `current` index

### QEMU Networking

`Makefile` adds `-netdev user,id=net0,hostfwd=tcp::8080-:8080 -device e1000,netdev=net0` to x86_64 QEMU targets and the equivalent virtio-net device to ARM64 targets. Port 8080 is forwarded host↔guest so anxbrowserd on the host is reachable at `10.0.2.2:9090` and the Anunix HTTP API on `localhost:8080` is reachable from the host.

### PCI Bus Scan Range

`PCI_MAX_BUS` increased from 1 to 8 to cover secondary buses used by AMD FCH USB controllers and GPU devices on real hardware.

## Shell Commands Added

```
fb_info             Framebuffer geometry (JSON)
gop_list            List GOP modes available at boot
fb_test             Paint 8-bar color test pattern
browser_init [host [port]]  Connect to anxbrowserd and start streaming
browser <url>       Navigate active session to URL
browser status      Show browser cell state
browser_stop        Stop streaming
```

## Companion: Anunix-Browser 2026.4.19

- `anxbrowserd` now binds `0.0.0.0` by default (was `127.0.0.1`), making it reachable from QEMU guest at `10.0.2.2`.
- New binary stream endpoint `GET /api/v1/sessions/{sid}/stream_raw` streams `[4-byte BE length][JPEG]` frames — no base64, no JSON, designed for kernel consumption.
- Frame rate increased from 1 FPS to ~30 FPS (`FRAME_INTERVAL_S = 0.033`).
- `anxbrowserd` queries `GET /api/v1/fb` at session creation and uses Anunix framebuffer dimensions as the default Playwright viewport.

## Statistics

- **22 tests passing, 0 failing**
- 6 new kernel source files
- 3 new Python source files
- 1 new header: `anx/browser_cell.h`

## Known Issues

- Bare-metal UEFI GOP mode switch untested — requires Framework Laptop 16 Gen 2 live boot.
- Browser cell does not handle TCP disconnection gracefully — call `browser_stop` then `browser_init` to reconnect.
- anxbrowserd bind address `0.0.0.0` allows LAN access on real hardware; set `ANXB_HOST=127.0.0.1` if that is not desired.
