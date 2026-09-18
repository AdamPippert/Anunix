# Configuration templates

Anunix includes three desktop templates. Each template changes the live theme
and tiling settings. The template does not save configuration objects.

The templates approximate familiar desktops through the existing Anunix window
manager. The taskbar remains a strip of minimized windows. The templates do not
add a macOS dock or an always-visible Windows taskbar.

## Template settings

| Setting | `macos` | `windows` | `omarchy` |
| --- | --- | --- | --- |
| Palette | Light chrome and traffic lights | Light neutral chrome and blue accents | Dark violet |
| Titlebar controls | Circles on the left | Rectangles on the right: minimize, maximize, close | Circles on the left |
| Font family | Cascadia Mono | Cascadia Mono | JetBrains Mono Nerd |
| Antialiasing | Enabled | Enabled | Enabled |
| New windows | Floating | Floating | Dwindle tiling |
| Corner radius | 16 pixels, rounded | 0 pixels, square | 6 pixels, rounded |
| Inner gap | 6 pixels per side | 0 pixels | 5 pixels per side |
| Outer gap | 12 pixels | 0 pixels | 10 pixels |
| Tiled border | 1 pixel | 1 pixel | 1 pixel |
| Panel opacity | 225 / 255 | 255 / 255 | 245 / 255 |
| Window opacity | 204 / 255 | 204 / 255 | 204 / 255 |
| Wallpaper | Default photograph | Default photograph | Default photograph |

All templates select Pretty mode and reset the complete theme before applying
their settings. All templates use a split ratio of 500 permille and a resize
step of 50 permille. The settings can change at runtime.

The blue Anunix scheme is named `default`. `theme use default` selects that
palette, the signature controls, and Atkinson Hyperlegible Mono. `aether` remains a compatibility alias;
loading an older `scheme=aether` setting writes `default` on the next save.

The Windows scheme selects `controls=windows`. Custom color edits keep the
selected controls. `controls=signature` restores the original left-side controls.
The control style persists with the theme object.

`theme fonts` lists the available font families. `theme font <family>` overrides
the template's font without changing its palette. `config save theme` persists
that override, including after a custom color edit. Applying a template again
restores the template's font and enables antialiasing.

Existing windows keep their tiled or floating state. Gap changes resize tiled
windows on the active workspace. The tiling default controls newly opened windows.
The inner gap separates adjacent tiled windows by twice the listed value.
Floating windows do not use the tiling gaps.

Pretty windows default to 80% opacity. Set `transparency=false` for opaque
windows, or set `opacity` from 0 through 255 and save the theme configuration.

## Commands and persistence

| Command | Result |
| --- | --- |
| `config templates` | Lists template names and descriptions. |
| `config apply macos` | Applies the macOS approximation immediately. |
| `config apply windows` | Applies the Windows approximation immediately. |
| `config apply omarchy` | Applies the Omarchy approximation immediately. |
| `config show theme` | Prints the current theme as text. |
| `config show tiling` | Prints the current tiling settings as text. |
| `config save theme` | Saves `system:config/theme`. |
| `config save tiling` | Saves `system:config/tiling`. |
| `config load theme` | Restores the saved theme. |
| `config load tiling` | Restores the saved tiling settings. |

Saving both objects preserves the template's appearance and layout settings
across boots. The two saves are separate operations. A failed save leaves that
category unsaved; the live template remains active.

An unknown template returns an error without changing the theme or tiling
settings. Applying the same template twice produces the same settings.

Templates leave timezone, model, hotkeys, network settings, and credentials
unchanged. An active image-object override takes precedence over the embedded
photograph. `wallpaper default` clears that override and displays the photograph.
Explicit `wallpaper gradient` and `wallpaper solid` settings remain available.

## Source and validation

The build includes the settings from
[`config/templates`](../../config/templates/). Editing those source definitions
takes effect after a rebuild. Runtime edits use `config set`, `theme`, or
configuration objects.

[`tests/test_config_templates.c`](../../tests/test_config_templates.c) verifies
the distinct palettes, layout settings, tiled spacing, repeated application,
unknown-template rejection, font defaults, and theme serialization. `make test` runs that
suite with the other host tests.

Visual acceptance uses `config apply <name>` in the VM followed by a screenshot.
The macOS template should show light rounded chrome. The Windows template
should show square chrome. The Omarchy template should tile newly opened
windows with visible gaps.

[`tests/test_window_chrome.c`](../../tests/test_window_chrome.c) tests control
placement, pointer targets, glyph pixels, and clipping in narrow titlebars.
