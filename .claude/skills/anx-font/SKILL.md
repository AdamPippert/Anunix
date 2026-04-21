---
name: anx-font
description: Create, edit, validate, and install the ANX Schoolbook system font. Handles both the kernel bitmap font (BDF/C header) and the host TTF/OTF outline font. Use when editing glyph shapes, fixing rendering issues, or regenerating font outputs.
disable-model-invocation: false
allowed-tools: Bash(python3 *) Bash(cp *) Bash(fc-cache *) Bash(atsutil *)
when_to_use: font editing, glyph design, font rendering issues, FontBook, BDF, OTF, TTF, bitmap font, install font
---

# ANX Font Skill

The ANX Schoolbook font lives in `system-font/`. One generator produces all outputs.

## Source of truth

`system-font/gen_anx_font.py` — single Python file (~1750 lines) containing:
- All glyph pixel data in the `_G` dict (one entry per codepoint)
- BDF writer (`write_bdf`)
- OTF/TTF writer (`write_ttf`) — requires `pip install fonttools`
- C header writer (`write_c_header`) — consumed by the kernel

**Never hand-edit** `anx_schoolbook_12x24.bdf`, `anx_schoolbook_12x24.h`, or `anx_schoolbook_12x24.otf` — they are generated outputs.

## Glyph format

Each glyph is a list of 24 integers (rows 0=top … 23=bottom).
Each integer is a 12-bit value; **bit 11 (0x800) = leftmost pixel**.

Use the `_row()` helper to define rows from a visual string:
```python
_row('############')  # → 0xFFF (all 12 pixels on)
_row('##..####..##')  # → 0xC3C
_row('............')  # → 0x000 (blank row)
```

## Adding / editing a glyph

1. Find the entry in `_G` for the target codepoint (e.g. `_G[ord('A')] = [...]`)
2. Edit the 24-row list using `_row()` calls
3. Regenerate and install (see below)

## Regenerating all outputs

```bash
pip install fonttools    # one-time; already installed on Mac Studio
python3 system-font/gen_anx_font.py
```

Outputs written to `system-font/`:
- `anx_schoolbook_12x24.bdf` — X11 bitmap font (informational)
- `anx_schoolbook_12x24.otf` — macOS/host TrueType font (same tables, OTF extension)
- `anx_schoolbook_12x24.h` — C header consumed by `kernel/lib/font.c`

## Installing on macOS

```bash
cp system-font/anx_schoolbook_12x24.otf ~/Library/Fonts/
# Force font cache refresh (needed after update):
atsutil databases -remove 2>/dev/null; killall fontd 2>/dev/null || true
```

To verify in FontBook: open FontBook → File → Validate Fonts → select the OTF.

## Validating the OTF

```python
from fontTools.ttLib import TTFont
f = TTFont('system-font/anx_schoolbook_12x24.otf')

# Required name IDs (all 6 must be present)
ids = {r.nameID for r in f['name'].names}
print("nameIDs:", sorted(ids))   # must include 1,2,3,4,5,6

# All ASCII glyphs must have outlines
glyf = f['glyf']
for cp in range(0x21, 0x7F):
    g = glyf[f'uni{cp:04X}']
    g.expand(glyf)
    assert g.numberOfContours > 0, f'empty glyph U+{cp:04X}'

# cmap must cover all printable ASCII
cmap = f.getBestCmap()
missing = [cp for cp in range(0x21, 0x7F) if cp not in cmap]
print("cmap missing:", missing or "none")

# OS/2 REGULAR bit
print("fsSelection REGULAR:", bool(f['OS/2'].fsSelection & 0x40))
```

## Common OTF issues and fixes (in gen_anx_font.py)

| Symptom | Cause | Fix |
|---------|-------|-----|
| Question mark blocks in FontBook | CCW pixel contours (treated as holes by TrueType) | `draw_pixel_cw`: draw BL→BR→TR→TL (clockwise) |
| Font name not shown | Missing nameID 3/5/6 | Add `uniqueFontIdentifier`, `version`, `psName` to `setupNameTable` |
| Font not bold/regular in family picker | OS/2 fsSelection wrong | `fsSelection=0x40` for Regular, `0x20` for Bold |
| Glyphs too small/large | Wrong UPM or metrics | `UPM=2400`, `BL=18`, `PX=100` (1 pixel = 100 units) |
| Invisible at small sizes | lowestRecPPEM too low | `fb.font['head'].lowestRecPPEM = H` (24) |

## OTF winding rule (critical)

TrueType uses **non-zero winding**:
- **Clockwise contour** = outer (filled) ← what pixels must use
- **CCW contour** = inner (hole)

The `draw_pixel_cw` helper in `write_ttf` draws each pixel as:
```
(x0,y0) → (x1,y0) → (x1,y1) → (x0,y1) → close
 BL         BR         TR         TL
```
This is clockwise in y-up coordinates → filled.

## Deploying to the kernel

After editing glyphs and regenerating:

```bash
# The C header is already in the right place — just rebuild
cp system-font/anx_schoolbook_12x24.h kernel/include/anx/
make kernel ARCH=x86_64
make test
```

Then run `/anunix-deploy` to push the ISO.

## Fixed-pitch / monospace declaration

The font is declared monospace via:
- `fb.setupPost(isFixedPitch=1)` in the OTF
- `SPACING "C"` in the BDF (character cell)

Both are already set correctly.

## Font metrics (reference)

| Constant | Value | Meaning |
|----------|-------|---------|
| `W`  | 12 | glyph width in pixels |
| `H`  | 24 | glyph height in pixels |
| `BL` | 18 | baseline row (0=top) |
| `UPM` | 2400 | units per em |
| `PX` | 100 | units per pixel (UPM/H) |
| `ASCENDER` | 1800 | BL × PX |
| `DESCENDER` | -500 | -(H−BL−1) × PX |
| `ADVANCE` | 1200 | W × PX |
