# Configuring Anunix

Change a running desktop with `config set`, inspect the result with `config show`,
and keep it across boots with `config save`. Changes do not require a reboot.
Saving requires a mounted object store. Check the command's result before assuming
that a setting will survive a restart.

## Configuration objects

| Object | Controls |
| --- | --- |
| `system:config/theme` | Palette, window decoration, font family, antialiasing, panel opacity, and wallpaper mode. |
| `system:config/tiling` | Tiling enabled, inner and outer gaps, border width, split ratio, and resize step. |
| `system:config/hotkeys` | Chords for the window manager's registered actions. |
| `system:config/timezone` | Signed UTC offset in whole hours. This does not implement daylight-saving rules. |
| `system:config/model` | Model endpoint host, port, enabled state, and credential name. |
| `system:config/network` | DHCP or static IPv4 configuration. |

`config list` lists supported names. Objects appear in the namespace after saving.
`config show <name>` reads live settings. `cat system:config/<name>` reads the saved
document, which can differ from the live settings. `config load <name>` validates
and applies that document. Unknown fields and malformed values produce errors.

Credentials remain in the credential store. A model configuration contains a
credential name, never the API key. Use the existing `secret` commands to manage
credentials; do not write secret values into ordinary configuration objects.

## Change and save settings

For example, change the accent color and tiling gaps:

```text
config show theme
config set theme 'accent=80c0ff'
config set tiling 'gaps_in=8;gaps_out=16'
config show tiling
config save theme
config save tiling
```

Restore the last saved settings with `config load theme` or `config load tiling`.
Each save covers one category. Saving several categories is not an atomic transaction.

Set a fixed timezone offset with:

```text
config set timezone 'offset_hours=-7'
config save timezone
```

Inspect `config show hotkeys` before editing bindings. Each action maps to a
numeric `modifiers,keycode` pair using the constants in `anx/input.h`. The complete
result must contain valid, unique chords. The graphical color editor's default
shortcut is Meta+Shift+C.

## Edit colors visually

Run `colors`, press Meta+Shift+C, or click the Anunix logo and select Open Colors in
the View menu. Navigate the menu with arrow keys and Enter.

In the editor, select a palette entry with the arrow keys and press Enter.
Type six hexadecimal digits, or use Left/Right to select a digit and Up/Down
to adjust it. Changes preview immediately across the desktop. The bottom
“Was” and “Now” swatches compare the original and edited colors.

Press Enter to accept the preview. Escape restores the original value.
Press S or Ctrl+S to accept a valid preview and save the theme. A green
“SAVED” banner confirms disk persistence; a red “SAVE FAILED” banner includes
the error and leaves the live changes available for retry.

Page Up, Page Down, Home, and End navigate the list. Escape while browsing
or Meta+Q closes and destroys the editor. Closing discards an unaccepted
preview. The permanent “Open Colors” menu item remains available to launch it.

## Select a font

Run `theme fonts` to list the embedded font families and mark the current one.
`theme font` prints the active family. To change the family and keep it across boots:

```text
theme font atkinson-hyperlegible-mono
theme status
theme save
```

| Scheme or template | Default font family |
| --- | --- |
| `default` (`aether` alias) and the other general schemes | `atkinson-hyperlegible-mono` |
| `macos`, `windows` | `cascadia-mono` |
| `omarchy` | `jetbrains-mono-nerd` |

`spleen` is also available as an explicit choice. Changing a scheme selects its
default font; select your preferred family afterward to override it. Custom
palette edits preserve that override. All families share a 12×24 text cell, so
changing families does not change the window's text capacity.

The equivalent object setting is `font_family`:

```text
config set theme 'font_family=cascadia-mono;antialiased=true'
config save theme
cat system:config/theme
```

Pretty mode, Boring mode, and all templates enable antialiasing by default.
Set `antialiased=false` to use hard glyph edges. Older saved documents that
explicitly disable antialiasing keep that setting. Documents without a font key
use their selected scheme's font; an older complete custom theme uses the mode's
Atkinson default. Unknown font names return an error without changing the theme.

## Shell input and history

The terminal and the Agent window's shell mode place the prompt below output,
starting at the top and scrolling when the viewport fills. Long input wraps;
resizing reflows visible output. Page Up and Page Down browse scrollback.

Up recalls an older command and Down moves toward newer commands, restoring
unfinished input when it reaches the current draft. The desktop and serial
shell share a 32-command history. Consecutive duplicates and lines containing
`secret` or `useradd` tokens are omitted. History persists when the object store
is available.

## Desktop wallpaper

`wallpaper default` selects the built-in photograph. It fills the display while
preserving its proportions; screen edges may crop part of the image.
`wallpaper gradient` and `wallpaper solid` select the theme's alternate backgrounds.
Use `config save theme` to preserve the mode across boots. An older saved theme
keeps its explicit wallpaper mode until you change it.

## Apply a desktop template

Run `config templates` to list the available templates. For example:

```text
config apply omarchy
config save theme
config save tiling
```

The [template guide](configuration-templates.md) describes the macOS, Windows,
and Omarchy approximations and their limitations.

## Edit an object's text

`write <ns:path> <content>` replaces the named object's content. A bounded overwrite
keeps the current object identity and length:

```text
write default:example hello world
write --at 6 default:example earth
cat default:example
```

The result is `hello earth`. Offsets count bytes. The overwrite must fit within
the existing payload, which must be no larger than 2048 bytes for this persistence
path. Sealed objects reject edits. A disk failure is reported; an in-memory edit
may already have taken effect. Reloaded ordinary objects currently receive new OIDs,
so use namespace paths across boots.

Editing a saved configuration object does not apply it automatically. Run
`config load <name>` after editing, and check its result. Prefer `config set`
for field edits because it validates before changing the live setting.

## Network and model configuration

Inspect `config show network` before changing it. Applying an address or DHCP
change can interrupt the SSH connection that issued the command. Use a local
terminal or retain another management path when changing network settings.

The static fields are `ip`, `netmask`, `gateway`, and `dns`. Set `mode=static` or
`mode=dhcp`, then save the network category when satisfied. This config does not
replace Wi-Fi credential provisioning or select an SSID.

Use `model-init clear` to disable and persistently clear a stale endpoint.
`config show model` shows the current endpoint configuration. A configured endpoint
still needs a valid credential and a reachable service to make model calls.

## Current limits

The theme stores settings that some renderers do not yet implement, including
font scaling. General UI scaling and transparent-window repaint corrections are
separate work. An image wallpaper's selected object path is not part of these
configuration documents; gradient and solid wallpaper modes are persisted.
