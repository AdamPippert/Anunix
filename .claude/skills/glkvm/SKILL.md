---
name: glkvm
description: Operate the GL.iNet KVM (glkvm on Tailscale) — take screenshots, type text, send keystrokes, run shell commands on the PiKVM itself, and check hardware state. Use when the user asks to see, control, or reboot the machine connected to the KVM.
argument-hint: [screenshot|type|key|shell|hid|atx|start-streamer] [args...]
allowed-tools: Bash Read
when_to_use: KVM, remote desktop, Framework Desktop, glkvm, take screenshot, send keys, type on remote machine, hardware reboot, check screen
---

# glkvm — GL.iNet KVM Operator

Control the GL.iNet KVM at `glkvm` (Tailscale: 100.123.44.77) which is physically connected via HDMI and USB-C to the Framework Desktop.

## Hardware layout

| Component | Detail |
|-----------|--------|
| KVM host  | `glkvm` (100.123.44.77 on Tailscale) |
| KVM model | GL.iNet KVM — PiKVM KVMD v4.82, Rockchip RV1126B |
| Target machine | Framework Desktop at 192.168.0.197 (routed via `superrouter` Tailscale subnet) |
| Video chip | LT6911C HDMI capture — detected at `/sys/bus/i2c/devices/1-002b/` |
| ATX state | Cable NOT connected (`enabled: false`) — no power button control via KVM |
| SSH to target | Port 22 open on 192.168.0.197, key auth only |

## CLI tool

```
tools/kvm/glkvm.py <subcommand> [args]
```

Password stored in `~/.glkvm_pass`. Token can be passed via `GLKVM_TOKEN` env var.

## Subcommands

```bash
# Take a screenshot (saves to screenshot.jpg by default)
python3 tools/kvm/glkvm.py screenshot [output.jpg]

# Type text via HID keyboard
python3 tools/kvm/glkvm.py type "text to type"

# Tap a single key (press + release)
python3 tools/kvm/glkvm.py key ShiftLeft

# Run a command on the PiKVM shell (not the target machine)
python3 tools/kvm/glkvm.py shell "command"

# Run a background command on PiKVM shell
python3 tools/kvm/glkvm.py shell-bg "command"

# Show HID/keyboard LED state (confirms target machine is on)
python3 tools/kvm/glkvm.py hid

# Show streamer status
python3 tools/kvm/glkvm.py streamer

# Start ustreamer manually (needed before screenshots if it died)
python3 tools/kvm/glkvm.py start-streamer

# Show ATX power state (always shows 'off' — cable not connected)
python3 tools/kvm/glkvm.py atx
```

## Steps for common tasks

### Take a screenshot
```bash
python3 tools/kvm/glkvm.py screenshot /tmp/snap.jpg
# Then view it:
# Read /tmp/snap.jpg
```
The tool auto-starts ustreamer if needed. HDMI must have signal (target machine display must be active).

### Wake display + screenshot
```bash
python3 tools/kvm/glkvm.py key ShiftLeft
sleep 5
python3 tools/kvm/glkvm.py screenshot /tmp/snap.jpg
```

### Type a command on the target machine
```bash
python3 tools/kvm/glkvm.py type "sudo shutdown -r now"
python3 tools/kvm/glkvm.py key Return
sleep 3
python3 tools/kvm/glkvm.py type "REDACTED"
python3 tools/kvm/glkvm.py key Return
```

### Reboot target machine
The target machine (192.168.0.197) runs Linux. Use HID type to send the command:
```bash
python3 tools/kvm/glkvm.py type "sudo shutdown -r now"
python3 tools/kvm/glkvm.py key Return
sleep 3
python3 tools/kvm/glkvm.py type "REDACTED"
python3 tools/kvm/glkvm.py key Return
```
ATX hard reset is NOT available (cable not connected).

### Hardware power cycle (if needed)
Not available via KVM — ATX cable is disconnected. Would need physical access or user to connect ATX cable to the KVM.

### Run a command on the PiKVM shell itself (not the target)
```bash
# Check HDMI signal
python3 tools/kvm/glkvm.py shell "cat /sys/bus/i2c/devices/1-002b/resolution"

# Check ARP table (find machines on local network)
python3 tools/kvm/glkvm.py shell "arp -n"

# SSH from PiKVM to target (requires key on PiKVM — usually not available)
python3 tools/kvm/glkvm.py shell "ssh root@192.168.0.197 'uname -a'"
```

## Important quirks

- **ustreamer must be running** for screenshots. It runs as a daemon on the PiKVM but dies on reboot. `screenshot` auto-starts it; `start-streamer` forces a restart.
- **kvmd proxy snapshot is broken** on this device — the tool bypasses kvmd and hits the ustreamer unix socket directly via the PiKVM shell.
- **HID `/api/hid/events/send-key` returns 404** on this GL.iNet device. Use `/api/hid/print` for text (via `type` subcommand) and individual key taps via `key` subcommand (which handles the 404 fallback).
- **HDMI "no signal"** means the target's display is off/sleeping. Send a key press to wake it, wait 5 seconds, then screenshot.
- **ATX shows `enabled: false`** always — the ATX control cable is not connected to the KVM. Power state readout is meaningless.
- **webterm input type byte is ASCII `0` (0x30)**, not raw `\x00`. The subprotocol must be `tty`.

## API reference (direct HTTPS to glkvm)

```
Auth:      POST /api/auth/login   form: user=admin&passwd=<pass>
           Response: {"ok":true,"result":{"token":"<token>"}}
           Header:   Token: <token>

HID:       GET  /api/hid
           POST /api/hid/print    body: {"text":"...","limit":0}

Streamer:  GET  /api/streamer
           GET  /api/streamer/snapshot   (503 if ustreamer not running via kvmd)

ATX:       GET  /api/atx
           POST /api/atx/power?action=on|off|off_hard|reset_hard

Info:      GET  /api/info
```

## Key names for HID key taps

Common key names: `Return`, `Space`, `Escape`, `ShiftLeft`, `ShiftRight`,
`ControlLeft`, `AltLeft`, `SuperLeft`, `Tab`, `BackSpace`, `Delete`,
`ArrowUp`, `ArrowDown`, `ArrowLeft`, `ArrowRight`,
`F1`–`F12`, `KeyA`–`KeyZ`, `Digit0`–`Digit9`.
