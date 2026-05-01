---
name: anunix-screenshot
description: Boot Anunix in QEMU on Hyde with a virtual VGA display and capture a screenshot or full boot video. Use to visually verify the WM, font rendering, or any UI change. Works headlessly — no window opens.
disable-model-invocation: false
allowed-tools: Bash(ssh *) Bash(scp *) Bash(pkill *) Bash(kill *) Read(*.jpg) Read(*.mp4)
when_to_use: take screenshot, see the display, visually verify rendering, check font, check WM, see what it looks like, record boot video, capture frames
---

# Anunix QEMU Screenshot / Boot Video

Boots the kernel on Hyde with a virtual VGA device (no window) and captures
the framebuffer via the QEMU monitor `screendump` command. Screenshot is
copied back to this Mac for viewing.

## Single Screenshot

```bash
# Kill any stale QEMU session on Hyde
ssh adam@hyde "pkill -f 'anunix-qemu-mon' 2>/dev/null; true"

# Boot and capture screenshot on Hyde
ssh adam@hyde "cd ~/Development/Anunix/Anunix && python3 tools/kvm/qemu_screenshot.py /tmp/anunix-screenshot.jpg"

# Copy screenshot to this Mac for viewing
scp adam@hyde:/tmp/anunix-screenshot.jpg /tmp/anunix-screenshot.jpg
```
Then view: `Read /tmp/anunix-screenshot.jpg`

## Boot Video (20 fps, auto-stops when display goes idle)

```bash
ssh adam@hyde "pkill -f 'anunix-qemu-mon' 2>/dev/null; true"
ssh adam@hyde "cd ~/Development/Anunix/Anunix && python3 tools/kvm/qemu_screenshot.py --video /tmp/anunix-boot.mp4"
scp adam@hyde:/tmp/anunix-boot.mp4 /tmp/anunix-boot.mp4
```

Captures frames at 20 fps from boot until the screen hasn't changed for
10 seconds (or 120s hard cap). Assembles to MP4 via ffmpeg.

## Notes

- Hyde has QEMU 10.2.2 and runs headless — the monitor socket approach works fine
- Uses port 19080 to avoid conflicting with headless test sessions (18080)
- Boot detection uses serial log — watches for "kernel init complete"
- `convert` (ImageMagick) handles PPM→JPEG conversion on Hyde (Linux)
- Framebuffer active: kernel detects Bochs VBE via `-vga std` and starts WM
