# Anunix 2026.4.24 Release Notes

Milestone: **Native kernel browser engine** — Anunix now ships a complete
HTML/CSS/JS browser engine in the kernel, speaking the ANX-Browser Protocol
natively and rendering to the framebuffer without any external process.

## Highlights

- **Full native browser driver** (`kernel/drivers/browser/`) — HTML tokenizer,
  DOM, CSS cascade + selectors, JavaScript VM (NaN-boxing, mark-sweep GC),
  JPEG/PNG/WebP decoders, PII filter, WebSocket streaming, form submission.
- **HTTPS via CONNECT proxy** — The kernel browser sends `CONNECT host:443`
  to `anxbproxy.py` at `10.0.2.2:8118`, which terminates TLS on the host and
  splices bytes bidirectionally. No TLS implementation required in the kernel.
- **Form submission** — `<form>` action/method are captured during layout;
  clicking a submit button or pressing Enter in a focused field builds the
  action URL with query params and calls `session_navigate`.
- **JS engine** — recursive-descent compiler, bytecode VM, NaN-boxing value
  representation, string interning, mark-sweep GC, DOM bindings, full
  `JSON.stringify`/`JSON.parse` for nested objects and arrays.
- **System font baseline fix** — ANX Schoolbook 12×24 ascender corrected to
  -7 (was -5), eliminating clipping on descenders at all DPI scales.
- **Session timestamps** — `created_at` now populated from `arch_time_now()`
  (nanoseconds-since-epoch) instead of being hardcoded to 0.
- **Browser shell commands** — `browser_init`, `browser <url>`,
  `browser_stop`, `browser status` available in `ansh`.
- **Window manager** + **browser engine** coexist: `anx_wm_init()` runs on
  framebuffer hardware; `anx_browser_init(9191)` starts the ANX-Browser
  Protocol listener unconditionally.

## New subsystems

### `kernel/drivers/browser/`

```
browser.c / browser.h        Main ANX-Browser Protocol HTTP server (port 9191)
session.c / session.h        Session lifecycle, framebuffer, navigate, click, scroll
ws.c / ws.h                  WebSocket upgrade, frame encode/decode (SHA-1 handshake)
jpeg_enc.c / jpeg_enc.h      JPEG encoder for frame capture

html/
  tokenizer.c / .h           HTML5 tokenizer
  dom.c / .h                 DOM tree (nodes, attributes, query selectors)
  dom_extra.c / .h           querySelector, querySelectorAll, innerHTML

css/
  css_parser.c / .h          CSS property parser
  css_selector.c / .h        Selector matching + specificity
  css_cascade.c / .h         Cascade engine with Bloom filter

layout/
  layout.c / .h              Block/inline flow layout, form action capture

paint/
  paint.c / .h               Software renderer (text, boxes, images, borders)

js/
  js_engine.c / .h           Engine init + native dispatch
  js_compile.c / .h          Recursive-descent compiler → bytecode
  js_vm.c / .h               Bytecode VM with NaN-boxing
  js_heap.c / .h             Mark-sweep garbage collector
  js_obj.c / .h              Object model with prototype chain
  js_str.c / .h              Interned string table
  js_dom.c / .h              DOM bindings (document, window, console, setTimeout)
  js_std.c / .h              Math, JSON, parseInt, parseFloat, String, Number

fetch/
  resource_loader.c / .h     Sub-resource fetch (CSS, images) with HTTPS dispatch
  https_proxy.c / .h         HTTP CONNECT tunnel through anxbproxy on 10.0.2.2:8118

image/
  jpeg.c / .h                JPEG decoder (sequential DCT)
  png.c / .h                 PNG decoder (inflate + filters)
  webp.c / .h                WebP decoder (lossy VP8L subset)

forms/
  forms.c / .h               Form field tracking, action/method storage, collect
pii/
  pii_filter.c / .h          PII detection + redaction
  pii_whitelist.c / .h       Per-domain bypass list with modal prompt
```

### `kernel/lib/crypto/`

- `sha1.c / sha1.h` — SHA-1 for WebSocket handshake (RFC 6455)
- `base64.c` extended with `anx_base64_encode`

### `tools/anxbproxy.py`

Stdlib Python (no dependencies) HTTP CONNECT proxy. Bind address:
`0.0.0.0:8118`. Terminates TLS with `ssl.create_default_context()`,
splices bytes between the kernel TCP connection and the upstream server.

```
python3 tools/anxbproxy.py &
```

## API / Protocol

The kernel browser engine listens on port 9191 and speaks a subset of the
ANX-Browser Protocol:

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/v1/health` | Daemon health + session count |
| `POST` | `/api/v1/sessions` | Create a new browser session |
| `GET` | `/api/v1/sessions` | List active sessions |
| `POST` | `/api/v1/sessions/{sid}/navigate` | Navigate to URL |
| `GET` | `/api/v1/sessions/{sid}/stream` | WebSocket frame + event stream |

## Bug fixes

- `anx_browser_init(9191)` and `anx_wm_init()` now both called at boot —
  previously only one or the other was present (merge conflict).
- `anx_mt7925_poll()`, `anx_browser_cell_tick()`, and `anx_browser_poll()`
  all called in the shell idle loop — previously merged from two branches.
