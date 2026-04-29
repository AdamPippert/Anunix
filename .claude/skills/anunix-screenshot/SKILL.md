---
name: anunix-screenshot
description: Boot Anunix in QEMU with a virtual VGA display and capture a screenshot or full boot video. Use to visually verify the WM, font rendering, or any UI change. Works headlessly — no window opens.
disable-model-invocation: false
allowed-tools: Bash(python3 *) Bash(pkill *) Bash(kill *) Bash(ffmpeg *) Read(*.jpg) Read(*.mp4)
when_to_use: take screenshot, see the display, visually verify rendering, check font, check WM, see what it looks like, record boot video, capture frames
---

# Anunix QEMU Screenshot / Boot Video

Boots the kernel with a virtual VGA device (no window) and captures the
framebuffer via the QEMU monitor `screendump` command.

## Single Screenshot

```bash
pkill -f "anunix-qemu-mon" 2>/dev/null; true
python3 tools/kvm/qemu_screenshot.py /tmp/anunix-screenshot.jpg
```
Then view: `Read /tmp/anunix-screenshot.jpg`

## Boot Video (20 fps, auto-stops when display goes idle)

```bash
pkill -f "anunix-qemu-mon" 2>/dev/null; true
python3 tools/kvm/qemu_screenshot.py --video /tmp/anunix-boot.mp4
```

Captures frames at 20 fps from boot until the screen hasn't changed for
10 seconds (or 120s hard cap). Assembles to MP4 via ffmpeg if available,
otherwise leaves numbered JPEGs in `/tmp/anunix-frames/`.

## Notes

- Uses port 19080 to avoid conflicting with headless test sessions (18080)
- Boot detection uses serial log — watches for "kernel init complete"
- Falls back gracefully when `user` netdev isn't compiled in (macOS)
- `sips` (macOS) or `convert` (ImageMagick) handles PPM→JPEG conversion
- Framebuffer active: kernel detects Bochs VBE via `-vga std` and starts WM
