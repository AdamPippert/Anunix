# Anunix 2026.9.18 Release Notes

Milestone: **Anunix is a networked, configurable workstation on its target
laptop, with an object-bound local model workflow.**

This release moves the Framework Laptop 16 from basic boot support to a
usable Anunix system. Its MT7925 adapter joins the local network, the desktop
has persistent themes and layouts, and the shell is practical over both the
display and SSH.

The workflow shell can also bind a State Object to a native cell call. A
small local `anxml` workflow now crosses the full
`state_ref -> cell_call -> output` path and returns a Model Output object.

## Highlights

- MT7925 Wi-Fi associates with WPA2-PSK, leases an address, and carries SSH
  traffic on the Framework Laptop 16.
- The network stack routes ARP, DHCP, ICMP, and tools through the active NIC.
- Dwindle tiling, directional focus, swaps, resize operations, floating
  windows, and configurable gaps form the new window-management baseline.
- Persistent macOS, Windows, and Omarchy templates coordinate palettes,
  fonts, controls, wallpaper, and tiling behavior.
- Pretty windows default to 80% opacity, or 20% transparency. The theme
  configuration exposes both the enable flag and the 0-to-255 opacity value.
- Atkinson Hyperlegible Mono, Cascadia Mono, and JetBrainsMono Nerd Font ship
  with reproducible atlases, source files, licenses, and provenance.
- Console, graphical, Agent, and SSH shells share bounded line editing and
  history behavior.
- Shell-created workflow nodes receive valid ports and can bind State Objects
  and local cell intents.
- 90 host-native suites pass with zero failures.

## MT7925 Wi-Fi and the network path

The station driver ports the required MT76 behavior for the Framework
Laptop 16's MT7925 adapter. It includes unified MCU messages, TX and RX
descriptors, scan and join state, beacon parsing, association, WPA2 key
exchange, key installation, rekeying, and link-loss handling.

The kernel observed a DHCP lease at `192.168.0.123`, synchronized time with
NTP, and served the built-in SSH server over the physical adapter. This is
real hardware evidence, not a simulated association result.

ARP, DHCP, ICMP, and network tools now select the active Ethernet provider.
They no longer assume that virtio-net owns the network. `net dhcp` can renew
the lease, and `wifi scan` exposes visible access points.

RFC-0034 defines how connectivity secrets remain outside the repository.
Release artifacts contain no Wi-Fi password or SSH private key.

## Memory and storage foundations

The EFI loader marks a valid firmware memory map. The x86_64 page allocator
builds its heap from conventional memory below 4 GiB, skips holes, and
reserves the execution window. The larger heap allows terminal windows to
use up to 32 MiB when memory is available.

The partition layer registers GPT partitions as bounded block devices.
Installation can target a partition or software array, disk selection uses
the Anunix superblock, and formatting refuses media that Anunix did not mark.

The boot log has a rolling on-disk ring that works before the object store.
It preserves early hardware evidence across a failed boot.

## Desktop and configuration

Configuration documents are first-class State Objects. The `config` command
can show, set, apply, save, and reset coordinated theme, window-manager, and
Agent settings.

The macOS, Windows, and Omarchy templates set window controls, colors, fonts,
wallpaper, tiling, gaps, and decoration values together. The old `aether`
theme name remains an alias for `default`.

Pretty-mode windows now default to `transparency=true` and `opacity=204`.
That is 80% opacity, or 20% transparency. Existing configurations can set a
different value or disable transparency, then persist it with
`config save theme`. Boring mode remains opaque by design. Incremental
application commits are recomposed against the desktop and lower windows, so
focusing a window or typing in a terminal no longer makes its background
opaque.

### UNIX-style command discovery

The shell's startup help is deliberately short. `cmdlist` groups commands
under objects, model, network, system, workflow, security, shell, and interface
categories; `cmdlist <category>` narrows the list. Every compiled command has a
`man <command>` page with a synopsis and category, while `help <command>` and
`help <category>` provide convenient shortcuts.

The desktop uses the selected photographic wallpaper. Its checked-in PNG and
embedded framebuffer data reproduce from the retained source conversion.

The window manager adds a Hyprland-style dwindle tree. New windows tile by
default when the active configuration enables tiling. Directional focus,
swaps, size changes, floating toggles, gaps, restacking, and repaint ordering
share one tested implementation.

## Fonts and top bar

Atkinson Hyperlegible Mono is the default family. macOS and Windows templates
use Cascadia Mono, while Omarchy uses JetBrainsMono Nerd Font. Spleen remains
available.

The checked-in font atlases cover printable ASCII. Existing Unicode fallback
continues to handle other characters. This release does not claim expanded
Nerd icon or ligature coverage.

The top bar renders at 150% of its previous scale. Its text, symbols, click
targets, workspace indicators, and responsive title and clock layout scale
together.

## Shell and application usability

The boot console, native terminal, WM terminal, Agent shell, and interactive
SSH share the same bounded editor. Left, Right, Home, End, insertion,
Backspace, Delete, history recall, and draft restoration follow one contract.
SSH also accepts escape sequences split across network packets.

Plain `help` now provides a compact orientation instead of printing the full
command catalog. `cmdlist` groups every release command by subsystem, and
`man <command>` provides its synopsis and description. A source-level test
compares both documentation paths with the compiled dispatcher, including the
conditional research build, so missing or invented entries fail validation.

The color editor shows live hex changes, original and draft swatches, save
feedback, and real close behavior. Object and workflow windows share the
updated controls and focus rules.

## Local model workflow

The workflow shell previously created nodes with no port metadata. A
`cell_call` therefore received zero inputs, and the local `anxml-generate`
intent suspended with `ANX_EINVAL`.

Shell-created nodes now receive kind-specific input and output ports.
`workflow add-edge` selects the first compatible output and input ports,
while preserving legacy zero-port workflows. A `state_ref` parameter resolves
a State Object by OID or namespace path, and a `cell_call` parameter records
its native intent.

The regression creates a prompt State Object, builds the three-node workflow,
runs it, and verifies a nonempty `ANX_OBJ_MODEL_OUTPUT`. This is a small,
deterministic local character model. It demonstrates model execution inside
Anunix without an external API.

The generic model-server interface still returns placeholder output. This
release does not claim arbitrary hosted-model inference, accelerator-backed
serving, or an OpenAI-compatible endpoint.

The graphical Workflow Atlas and showcase runner live in the separate
`Anunix-apps` repository. Application code is not bundled into the OS source.

## Validation

The final source passes:

- 90 of 90 host-native C suites;
- the page-span allocator test;
- the Python wallpaper, shell-help, font-generation, and profile suites;
- warnings-as-errors x86_64 kernel and ISO builds;
- low-memory and high-memory OVMF boot checks;
- embedded-kernel comparison between the EFI image and raw kernel;
- a no-secrets release-artifact inspection.

Hardware evidence on Jekyll covers the installed EFI boot, framebuffer,
NVMe, MT7925 association, DHCP, NTP, and SSH. Final workflow and transparency
checks use the release image before publication.

## Compatibility

- Existing zero-port workflow objects remain connectable through the legacy
  port-zero fallback.
- Existing theme documents without transparency fields retain parser defaults.
- Boring theme mode remains opaque.
- The `aether` theme name continues to select the default palette.
- Boot credentials stay optional and local. Public artifacts build with an
  empty boot-secrets configuration.

## Known limitations

- The generic model-server inference implementation remains a stub.
- `anxml` is a small deterministic local model, not a general language model.
- MT7925 hardware testing covers the target Framework Laptop 16 and its
  WPA2-PSK network. It does not establish broad adapter or enterprise Wi-Fi
  compatibility.
- USB and HID-over-I2C input were not revalidated on hardware for this release.
- Multiline SSH terminal wrapping remains less exercised than single-line
  editing.
- The new font atlases cover printable ASCII only.
- ARM64 builds retain their existing experimental status.

## Release statistics

- More than 30 commits since `2026.9.15`, including the final release notes
  and reconciliation of the Jekyll deployment history.
- 90 host-native suites, up from 64 in `2026.9.15`.
- 33 RFC documents in the repository.
- Three shipped configuration templates.
- Three reproducible theme font families, plus the existing Spleen font.
