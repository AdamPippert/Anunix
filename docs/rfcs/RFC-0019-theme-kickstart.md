# RFC-0019: Visual Theme System and Kickstart Provisioning

| Field      | Value                                          |
|------------|------------------------------------------------|
| RFC        | 0019                                           |
| Title      | Visual Theme System and Kickstart Provisioning |
| Author     | Adam Pippert                                   |
| Status     | Draft                                          |
| Created    | 2026-04-21                                     |
| Updated    | 2026-04-21                                     |
| Depends On | RFC-0001, RFC-0002, RFC-0012                   |

---

## Part 1: Visual Theme System

### Motivation

Anunix targets radically different deployment surfaces: desktops with discrete GPUs, mobile phones
with power budgets, and embedded robots with VGA framebuffers or serial-only consoles. These
environments need opposite visual strategies. A desktop benefits from composited shadows, animation,
and large readable text. A robot controller needs the highest possible information density at the
lowest GPU cost. A single hard-coded look is wrong for at least one of these.

The theme system provides a **one-toggle switch** between two presets, with per-field override
capability, so the correct look ships by default on each platform with no renderer modifications.

### Design

The active theme lives in a single global `struct anx_theme` managed by `kernel/core/theme.c`.
All renderers query `anx_theme_get()` — a pointer to that struct — before drawing decorations.
No renderer contains hard-coded sizes, colors, or animation flags.

```
struct anx_theme {
    enum anx_theme_mode      mode;
    struct anx_color_palette palette;   /* 10 RRGGBB colors */
    struct anx_deco_style    deco;      /* corner radius, shadow, animation */
    struct anx_font_style    font;      /* scale, antialiasing */
};
```

#### Modes

| Mode          | Corner radius | Shadow | Animation | Font scale | Palette       |
|---------------|---------------|--------|-----------|------------|---------------|
| Pretty        | 8 px          | yes    | 150 ms    | 2x         | GitHub Dark   |
| Boring        | 0 px          | no     | none      | 1x         | Monochrome    |

Pretty targets GPU compositors. Boring targets TUI consoles, serial terminals, and
resource-constrained deployments.

Switching modes replaces the entire preset atomically via `anx_theme_set_mode()`. Individual
fields can then be overridden through `anx_theme_apply_config()` without changing the base preset.

### Renderer Integration

Every draw call that produces a decoration must branch on the active theme:

```c
const struct anx_theme *t = anx_theme_get();

if (t->deco.corner_radius > 0)
    gpu_draw_rounded_rect(x, y, w, h, t->deco.corner_radius,
                          t->palette.surface);
else
    gpu_draw_rect(x, y, w, h, t->palette.surface);

if (t->deco.shadow_enabled)
    gpu_draw_shadow(x, y, w, h,
                    t->deco.shadow_offset_x, t->deco.shadow_offset_y,
                    t->deco.shadow_blur, t->palette.shadow);
```

The GPU renderer exposes two rectangle primitives: `gpu_draw_rounded_rect()` (arc-corner path,
GPU path rasterizer) and `gpu_draw_rect()` (axis-aligned blit, trivial fill). The theme drives
which is called — the renderer never decides on its own.

### Window Manager Integration

The WM holds a decoration context per window. On `ANX_THEME_PRETTY`, it allocates a shadow
layer and composites it below the window surface. On `ANX_THEME_BORING`, the shadow layer is
not allocated. On `anx_theme_set_mode()`, the WM receives a `ANX_EV_THEME_CHANGED` event and
repaints all window borders on the next vsync. Windows do not need to redraw their contents;
only the server-side decorations change.

Titlebar height (`deco.titlebar_height`) is 28 px for Pretty and 16 px for Boring. The WM
reads this field when computing the client area rectangle — no WM code contains a literal height.

### Shell Tool

```
theme pretty              # activate Pretty preset
theme boring              # activate Boring preset
theme status              # dump all palette/deco/font fields
theme set opacity=200     # per-field override (kickstart format)
```

---

## Part 2: Kickstart Provisioning Format

### Motivation

Deploying Anunix at scale — to a rack of identical systems, to a fleet of robots, or to a
development machine being reprovisioned — should require a single text file dropped into
`/boot/`. The system should boot, read it, and configure itself: disks, network, drivers,
credentials, workflows, and UI preferences. No interactive installer, no network callback,
no discovery ambiguity.

### Format

INI-style plain text. Rationale: the parser is trivial in C11 freestanding. No JSON (requires
heap and a full grammar), no YAML (multi-line edge cases are pathological).

Rules:
- Lines starting with `#` are comments.
- Blank lines are ignored.
- `[section]` begins a section.
- `key=value` within a section sets a field.
- `key:subkey=value` sets a sub-keyed field (used for credentials and workflows).
- Values are unquoted strings; trailing whitespace is stripped.
- Unknown keys are silently skipped (forward compatibility).

### Sections

#### [system]

```ini
[system]
hostname=jekyll
timezone=America/Chicago
locale=en_US
```

#### [disk]

```ini
[disk]
layout=gpt
root_size=32G
swap_size=8G
data_size=rest
```

`layout`: `gpt` (default) or `mbr`.
`root_size`, `swap_size`, `data_size`: sizes in G/M/K, or `rest` for remainder.

#### [ui]

```ini
[ui]
theme=boring
font_scale=1
```

`theme`: `pretty` or `boring`. Applied via `anx_theme_apply_config()`.
Other keys are passed through directly as a config string to `anx_theme_apply_config()`.

#### [network]

```ini
[network]
interface=eth0
mode=dhcp
# static example:
# mode=static
# address=10.0.0.5/24
# gateway=10.0.0.1
# dns=1.1.1.1
```

#### [credentials]

Sub-keyed format to support multiple credential entries:

```ini
[credentials]
ssh:authorized_key=ssh-ed25519 AAAAC3... adam@workstation
user:name=adam
user:password_hash=$6$salt$hash
```

Each `ssh:authorized_key` line appends one key to the authorized set.
`user:password_hash` is a crypt(3)-format hash; plaintext passwords are rejected.

#### [workflows]

```ini
[workflows]
autoload=provision_complete
autoload=health_monitor
```

`autoload` lists workflow OIDs or names to load from the object store at boot.
Multiple `autoload` lines are supported (each appends to the load list).

#### [drivers]

```ini
[drivers]
load=mt7925
load=xdna
load=e1000
```

Bypasses hardware discovery entirely. Each `load` line names a driver module to
initialize unconditionally. Useful for known hardware where discovery latency is
unacceptable (robots, appliances). Unknown driver names log a warning and are skipped.

### Application

If `/boot/kickstart.cfg` exists at boot, the kernel applies it during `kernel_main()` before
the first shell prompt, logging each section as it processes.

On a running system:

```
kickstart apply /path/to/kickstart.cfg
kickstart status                          # show last-applied config
```

The `kickstart` tool calls each subsystem's apply function in section order:
`[disk]` → partition, `[network]` → configure, `[ui]` → `anx_theme_apply_config()`,
`[credentials]` → credential store, `[workflows]` → workflow loader, `[drivers]` → driver init.

Sections are idempotent where possible; re-applying a kickstart file is safe.

---

## Status

Both features are in Phase 1. The theme subsystem (`kernel/core/theme.c`) and the `theme` shell
tool (`kernel/core/tools/theme.c`) are implemented. The kickstart parser and `kickstart` tool
are planned for the next sprint. The `[ui]` section is the bridge between the two: kickstart
applies theme config through `anx_theme_apply_config()`.
