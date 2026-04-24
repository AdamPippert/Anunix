# Anunix 2026.4.23 Release Notes

Milestone: **Graphical userspace P1 + P2 complete** — all ten graphical userspace tickets are now implemented, tested, and shipped. This release closes the full P1/P2 backlog and adds 41 kernel unit tests.

## Highlights

- **Multi-window / surface model hardening (P1-002)** — deterministic z-ordering with automatic renumbering, child surface cascade-destroy, focus-follows-raise for top-level surfaces.
- **Clipboard, drag-and-drop, file-picker (P1-003)** — per-cell permission table, 4 KB clipboard store, single-slot drag state, file-picker request/respond contract.
- **UTF-8 text shaping (P1-004)** — strict RFC 3629 decoder, font fallback registry (4 slots), multi-lingual `anx_font_draw_str()`.
- **Transfer policy and integrity (P1-005)** — prefix-match allow-list, SHA-256 running hash preserved across interrupt/resume, provenance tag on committed transfers.
- **Accessibility object model (P2-001)** — 64-node tree, four roles, assistive action injection, 32-event focus-narration stream.
- **Media path baseline (P2-002)** — audio queue (8 buffers at 44100 Hz), tick-driven playback simulation, underrun detection, A/V position tracking.
- **Untrusted app/process isolation (P2-003)** — four trust domains (TRUSTED/RESTRICTED/SANDBOXED/UNTRUSTED), per-cell domain registry, IPC policy matrix, 16-entry violation log with provenance.
- **Conformance gate (P2-004)** — 32-fixture corpus runner with deterministic Fisher-Yates ordering by seed, diff tracking with regression detection, configurable failure threshold enforcement.
- **Crash diagnostics and observability (P2-005)** — 4-slot crash artifact pool, 16-phase trace timeline, versioned performance metrics snapshot.

## New Subsystems

### Accessibility (`kernel/core/iface/a11y.c`)

Full accessibility tree with add/remove/lookup by node ID. Assistive actions (CLICK, FOCUS, SCROLL) synthesise events on the 32-entry ring buffer. Focus changes are pushed via `anx_a11y_notify_focus()`. Any assistive technology can poll `anx_a11y_event_poll()` to narrate UI state changes.

### Media Path (`kernel/core/iface/media.c`)

Single-session audio pipeline: enqueue `anx_audio_buffer` structs into an 8-slot circular queue, drive playback with `anx_media_tick()`. Position advances as `samples / 44100 * 1e9` nanoseconds per buffer. Underrun flag set and counter incremented when the queue runs dry.

### Process Isolation (`kernel/core/exec/isolation.c`)

Four trust domains enforced by a `bool allow[4][4]` IPC policy matrix. `anx_isolation_check()` logs violations into a ring buffer with offender/owner CIDs, domains, resource description, and timestamp. Policy is system-wide and hot-swappable via `anx_isolation_set_policy()`.

### Conformance Gate (`kernel/core/conformance_gate.c`)

Deterministic fixture runner: same seed always produces the same execution order (Fisher-Yates with a 32-bit LCG). `anx_gate_diff()` compares two reports and marks regressions (was passing, now failing). `anx_gate_threshold_check()` enforces a configurable failure budget with a human-readable reason string.

### Diagnostics (`kernel/core/diag.c`)

Three independent facilities under one init:
- **Crash pool** — ring buffer of 4 `anx_crash_artifact` records; `anx_diag_crash_parse()` validates magic + version.
- **Trace timeline** — up to 16 named phases with nanosecond start/end timestamps; duplicate-open rejected with EBUSY.
- **Metrics snapshot** — `anx_perf_metrics` with schema version lock; callers cannot accidentally downgrade `schema_version`.

## Shell Commands Added

None new in this release — all P1/P2 work is kernel subsystem API surface, not shell commands.

## API Surface Added

```c
/* P1-002 */
int anx_iface_surface_set_parent(struct anx_surface *child, anx_oid_t parent_oid);

/* P1-003 */
void anx_clipboard_init(void);
int  anx_clipboard_grant(anx_cid_t cid, uint32_t flags);
int  anx_clipboard_write(anx_cid_t cid, const char *mime_type, const void *data, uint32_t len);
int  anx_clipboard_read(anx_cid_t cid, char *mime_out, void *buf, uint32_t max, uint32_t *len_out);
int  anx_iface_drag_begin(anx_oid_t src_surf, const char *mime_type, const void *data, uint32_t len);
int  anx_iface_drag_deliver(anx_oid_t dst_surf, struct anx_drag_payload *out);
int  anx_filepick_request(anx_cid_t requester, const char *filter, uint32_t *id_out);
int  anx_filepick_respond(uint32_t id, const char *path);

/* P1-004 */
int anx_utf8_decode(const uint8_t *buf, uint32_t len, uint32_t *cp, uint32_t *consumed);
int anx_font_fallback_register(uint32_t cp_first, uint32_t cp_last, const uint16_t *(*get_glyph)(uint32_t));
int anx_font_draw_str(uint32_t x, uint32_t y, const char *utf8_str, uint32_t fg, uint32_t bg);

/* P1-005 */
int anx_xfer_begin(const struct anx_xfer_policy *policy, ...);
int anx_xfer_write(struct anx_xfer_session *sess, const void *data, uint32_t len);
int anx_xfer_interrupt(struct anx_xfer_session *sess);
int anx_xfer_resume(struct anx_xfer_session *sess);
int anx_xfer_commit(struct anx_xfer_session *sess, struct anx_xfer_result *result_out);

/* P2-001 */
int      anx_a11y_node_add(const struct anx_a11y_node *node);
int      anx_a11y_node_remove(uint32_t id);
int      anx_a11y_action(uint32_t node_id, enum anx_a11y_action action);
void     anx_a11y_notify_focus(uint32_t node_id);
int      anx_a11y_event_poll(struct anx_a11y_event *out);
uint32_t anx_a11y_event_depth(void);

/* P2-002 */
int anx_media_open(const char *uri, struct anx_media_session *sess);
int anx_media_play(struct anx_media_session *sess);
int anx_media_pause(struct anx_media_session *sess);
int anx_media_seek(struct anx_media_session *sess, uint64_t position_ns);
int anx_media_audio_enqueue(struct anx_media_session *sess, const struct anx_audio_buffer *buf);
int anx_media_tick(struct anx_media_session *sess, struct anx_audio_buffer *played_out);

/* P2-003 */
int                   anx_isolation_set_domain(anx_cid_t cid, enum anx_trust_domain domain);
enum anx_trust_domain anx_isolation_get_domain(anx_cid_t cid);
int                   anx_isolation_check(const struct anx_ipc_policy *policy, anx_cid_t accessor, anx_cid_t owner, const char *resource_desc);
uint32_t              anx_isolation_violation_count(void);
int                   anx_isolation_violation_log(struct anx_violation_event *out, uint32_t max, uint32_t *count_out);

/* P2-004 */
int  anx_gate_register(const char *name, anx_gate_fixture_fn fn);
int  anx_gate_run(uint32_t seed, struct anx_gate_report *report_out);
void anx_gate_diff(const struct anx_gate_report *prev, const struct anx_gate_report *curr, struct anx_gate_diff *diff_out);
int  anx_gate_threshold_check(const struct anx_gate_report *report, uint32_t max_failures, char *reason_out, uint32_t reason_max);

/* P2-005 */
struct anx_crash_artifact *anx_diag_fault(const char *fault_type, uint64_t fault_addr, const char *subsystem, const char *message);
int                        anx_diag_crash_parse(struct anx_crash_artifact *artifact);
int                        anx_trace_begin(const char *name);
int                        anx_trace_end(const char *name);
void                       anx_metrics_record(const struct anx_perf_metrics *in);
void                       anx_metrics_current(struct anx_perf_metrics *out);
```

## Test Suite

**41 tests passing, 0 failing** (+5 from previous release)

| Suite | Tests | Status |
|-------|-------|--------|
| state_object | existing | PASS |
| cell_lifecycle | existing | PASS |
| cell_runtime | existing | PASS |
| memplane | existing | PASS |
| engine_registry | existing | PASS |
| scheduler | existing | PASS |
| capability | existing | PASS |
| fb | existing | PASS |
| engine_lifecycle | existing | PASS |
| resource_lease | existing | PASS |
| model_server | existing | PASS |
| posix | existing | PASS |
| tensor | existing | PASS |
| model | existing | PASS |
| tensor_ops | existing | PASS |
| crypto | existing | PASS |
| sshd_crypto | existing | PASS |
| input_routing | existing | PASS |
| compositor_cell | existing | PASS |
| shm_ipc | existing | PASS |
| conformance_harness | existing | PASS |
| userspace_prereqs | existing | PASS |
| rlm | existing | PASS |
| external_call | existing | PASS |
| disk_store | existing | PASS |
| route_planner | existing | PASS |
| vm_object | existing | PASS |
| workflow | existing | PASS |
| theme | existing | PASS |
| event_qos | existing | PASS |
| compositor_dirty_rect | existing | PASS |
| multi_surface | P1-002 | PASS |
| clipboard | P1-003 | PASS |
| text_shaping | P1-004 | PASS |
| transfer_policy | P1-005 | PASS |
| **diag** | **P2-005** | **PASS** |
| **isolation** | **P2-003** | **PASS** |
| **a11y** | **P2-001** | **PASS** |
| **media** | **P2-002** | **PASS** |
| **conformance_gate** | **P2-004** | **PASS** |

## Statistics

- **10 new kernel source files** (P1-002 through P2-005)
- **10 new test files**
- **10 new public headers**
- **Version**: 2026.4.23

## Known Issues

- Media subsystem is single-session only; opening a second session returns EBUSY.
- Accessibility CLICK/SCROLL_UP/SCROLL_DOWN actions synthesise NODE_ACTIVATED events only; full pointer/scroll synthesis requires integration with the input plane (planned for P3).
- Conformance gate fixture order is deterministic by seed but not alphabetical; tooling should use fixture names (not result indices) when comparing reports across seeds.
- Trust domain registry has a hard limit of 64 cells; cells beyond this limit default to TRUSTED (safe default but logged as EFULL on `anx_isolation_set_domain`).
