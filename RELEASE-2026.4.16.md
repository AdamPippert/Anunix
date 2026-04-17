# Anunix 2026.4.16 Release Notes

## Highlights

- **ansh (Anunix Shell)** — 12 native tools operating on State Objects
- **Interface Plane (RFC-0012)** — medium-agnostic interaction layer with surfaces, events, and renderers
- **Graphical environment** — JPEG splash, tiled GUI with live clock, PS/2 keyboard input
- **Performance profiling** — TSC-based boot instrumentation with `perf` command
- **DHCP client** — automatic network configuration at boot
- **Heteris architecture** — initial RISC-V arch stubs for AI accelerator integration

## New Subsystems

### ansh — Anunix Shell
The kernel monitor has been renamed to **ansh** and refactored with modular tools in `kernel/core/tools/*.c`:

| Tool | UNIX Equiv | Description |
|------|-----------|-------------|
| `ls` | `ls` | List namespace entries with OID/type/state details |
| `cat` | `cat` | Read State Object payload with hex dump and provenance |
| `write` | `echo >` | Create typed State Object with namespace binding |
| `cp` | `cp` | Copy with derivation provenance tracking |
| `mv` | `mv` | Rebind namespace entry (OID stays stable) |
| `rm` | `rm` | Delete with lifecycle state transition |
| `inspect` | `hexdump`+`file` | Full object internals and hex dump |
| `search` | `grep` | Search across all object payloads |
| `fetch` | `curl` | HTTP GET → State Object with URL provenance |
| `cells` | `ps` | List execution cells with lifecycle status |
| `sysinfo` | `uname`+`free` | Unified system overview |
| `netinfo` | `ifconfig` | Network interface configuration |

### Interface Plane (RFC-0012)
Medium-agnostic interaction layer with:
- Surface store (State Objects for visual/voice/tactile interfaces)
- Event queue with subscription-based delivery
- GPU and headless renderer engines
- Compositor with repaint loop and focus management
- Input layer bridging PS/2 keyboard to Interface Plane events
- CLI tools: `surfctl`, `evctl`, `compctl`, `envctl`

### Graphical Environment
- JPEG splash screen — logo decoded and displayed at native resolution for 5 seconds
- Tiled GUI — sky blue background (#87CEEB), midnight blue terminal (#191970), white text
- Live clock from CMOS RTC with timezone offset (`tz -7` for PDT)
- PS/2 keyboard driver (IRQ1, US QWERTY, shift support)
- All `kprintf` output routed through GUI terminal when framebuffer active

### Performance Profiling
- TSC-based nanosecond-precision timing (kernel/lib/perf.c)
- Boot-time instrumentation: arch_init, splash, gui_init, DHCP, net_stack
- `perf` shell command displays profile at any time
- Page allocator optimized: large allocations search from end to reduce fragmentation

## Kernel API Additions

- `anx_ns_list()` — enumerate namespace entries
- `anx_ns_unbind()` — remove namespace bindings
- `anx_ns_list_namespaces()` — list all namespace names
- `anx_ns_init()` — called during boot (was previously missing)
- `anx_objstore_iterate()` — walk all objects with callback
- `anx_cell_store_iterate()` — walk all cells with callback
- `kputc()` — single character output to all consoles (serial + framebuffer)
- `anx_gui_set_tz_offset()` — timezone offset for clock display

## Bug Fixes

- **TCP connection reuse** — connections fully zeroed on close, drain polls between sessions
- **Heap overlapping page tables** — linker script forces heap start above 0x210000
- **Kernel stack overflow** — increased from 16KB to 64KB for deep call chains
- **JPEG decode ENOMEM** — splash moved after arch_init (page allocator must be initialized first)
- **Page allocator fragmentation** — large blocks allocated from end of heap
- **Clock flicker** — redraws only when minute changes, HH:MM format
- **Keyboard echo in GUI** — shell uses kputc() for both serial and framebuffer
- **GRUB multiboot2 on UEFI** — "Anunix (UEFI native)" entry with EFI boot services tag
- **Interface Plane compilation** — fixed struct anx_list → anx_list_head, ANX_ENOTIMPL → ANX_ENOSYS

## New RFCs

- **RFC-0009: Agent Memory and Semantic Retrieval** — episodic memory, graph metadata, kernel embedding, dream consolidation
- **RFC-0010/0011: Userland and SuperRouter** (scaffolding)
- **RFC-0012: Interface Plane** — medium-agnostic interaction layer

## New Architecture

- **Heteris (RISC-V)** — initial arch stubs for AI accelerator integration

## Documentation

- Man pages for all 11 shell tools
- Comprehensive OS tools plan (4 phases, 24 tools)
- Performance profiling preference documented for all hot paths

## Known Issues

- Bare-metal UEFI boot still crashes (multiboot1 64→32 transition fails on real firmware; multiboot2 with EFI_BS not yet working)
- Subsequent `ask` calls may still fail intermittently (TCP state machine)
- JPEG splash skipped on high-resolution displays (heap too small for decode buffer)
- Shell scripting (if/for/job control) not yet implemented

## Statistics

- **~22,000 lines** of kernel C code
- **12 passing tests**
- **12 shell tools** with man pages
- **9 RFCs** (0001-0009) + 3 additional (0010-0012)
- **3 architectures**: x86_64, ARM64, Heteris (RISC-V)
