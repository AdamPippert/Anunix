---
name: glkvm-hermes
description: Hermes agent skill for operating the glkvm KVM — screenshot, type, key, shell. For use when Hermes needs to see or interact with the Framework Desktop hardware connected to the GL.iNet KVM.
argument-hint: screenshot|type <text>|key <keyname>|shell <cmd>|hid|start-streamer
allowed-tools: Bash Read
when_to_use: Hermes needs to see the Framework Desktop screen, send input to it, or run diagnostics via the KVM
---

# glkvm — Hermes KVM Skill

Hermes can control the GL.iNet KVM at `glkvm` on the Tailscale network to interact with the physically connected Framework Desktop.

## Invocation

All operations go through `tools/kvm/glkvm.py` in the Anunix repo:

```bash
cd /Users/adam/Development/Anunix
GLKVM_TOKEN=$(python3 tools/kvm/glkvm.py auth) python3 tools/kvm/glkvm.py <subcommand> [args]
```

Or with cached token:
```bash
export GLKVM_TOKEN=$(python3 tools/kvm/glkvm.py auth)
python3 tools/kvm/glkvm.py screenshot /tmp/snap.jpg
python3 tools/kvm/glkvm.py type "hello"
```

## Available operations

| Subcommand | Effect |
|------------|--------|
| `auth` | Authenticate and print token |
| `screenshot [path]` | Capture screen as JPEG |
| `type "text"` | Type text on the target machine via HID |
| `key KeyName` | Tap a single key (press + release) |
| `shell "cmd"` | Run a shell command on the PiKVM itself |
| `hid` | Show keyboard/LED state (confirms machine is on) |
| `streamer` | Show video streamer status |
| `start-streamer` | Start ustreamer for video capture |
| `atx` | Show ATX state (NOTE: cable not connected) |

## Standard Hermes workflow

### 1. See the current screen state
```bash
GLKVM_TOKEN=$(python3 tools/kvm/glkvm.py auth) \
  python3 tools/kvm/glkvm.py screenshot /tmp/kvm_snap.jpg
# Then read/show the image
```

### 2. Interact with the machine
```bash
# Wake sleeping display first if needed:
python3 tools/kvm/glkvm.py key ShiftLeft
sleep 5
python3 tools/kvm/glkvm.py screenshot /tmp/after_wake.jpg

# Type a command:
python3 tools/kvm/glkvm.py type "sudo reboot"
python3 tools/kvm/glkvm.py key Return
sleep 2
python3 tools/kvm/glkvm.py type "REDACTED"  # sudo password
python3 tools/kvm/glkvm.py key Return
```

### 3. Confirm machine state
```bash
# Check if machine is on (num_lock LED = on means powered):
python3 tools/kvm/glkvm.py hid | python3 -c "
import sys, json
d = json.load(sys.stdin)
leds = d.get('keyboard', {}).get('leds', {})
print('MACHINE ON:', leds.get('num', False))
"
```

### 4. Check HDMI signal
```bash
python3 tools/kvm/glkvm.py shell \
  "cat /sys/bus/i2c/devices/1-002b/resolution 2>/dev/null || echo 'no signal'"
```

## Context for Hermes

- **Target**: Framework Desktop at 192.168.0.197 running Linux
- **KVM**: GL.iNet KVM at `glkvm` (100.123.44.77 Tailscale)
- **Confirmed working**: SSH port 22 is open on 192.168.0.197 (reachable via Tailscale subnet through `superrouter`)
- **ATX**: NOT connected — cannot power cycle via KVM
- **sudo password**: `REDACTED`
- **Current OS**: Linux desktop (XFCE-like, deer wallpaper, workspaces 1-5)
- **Current resolution**: 2560x1440 (machine), captured at 1920x1080 by KVM

## Hermes HTTP API usage

If Hermes is calling this via the Anunix HTTP API, the KVM tool can be invoked as a shell tool:

```
POST /api/shell
{"cmd": "cd /Users/adam/Development/Anunix && GLKVM_TOKEN=$(python3 tools/kvm/glkvm.py auth) python3 tools/kvm/glkvm.py screenshot /tmp/snap.jpg"}
```

Then retrieve the image:
```
GET /api/file?path=/tmp/snap.jpg
```
