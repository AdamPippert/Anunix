# Configure Anunix over SSH

Use ansh commands to inspect, change, save, and verify named configuration objects.
Anunix's namespace paths identify State Objects; they are not host filesystem paths.

## Inspect before changing

Run one command per connection when using the Anunix SSH server:

```text
version
config list
config show theme
config show tiling
ls system:config
```

`config show` serializes runtime state. `cat system:config/theme` reads the last
saved object. Record both when an unsaved user change must be preserved.

## Apply and verify

For a theme adjustment:

```text
config set theme 'accent=80c0ff'
config show theme
config save theme
cat system:config/theme
```

A successful `config set` changes the running setting. A successful save writes
its named text object through the user-object journal. Treat a failed save as
unsaved state. A later `config load theme` validates the saved document before
applying it. Unknown or malformed fields must return an error.

`config apply <template>` changes theme and tiling in memory. Save both categories
explicitly when persistence is intended. See the [template guide](configuration-templates.md).

## Font selection

Inspect `theme fonts`, `theme font`, and `config show theme` before changing fonts.
Use a canonical family name: `atkinson-hyperlegible-mono`, `cascadia-mono`,
`jetbrains-mono-nerd`, or `spleen`.

```text
config set theme 'font_family=jetbrains-mono-nerd;antialiased=true'
config show theme
config save theme
cat system:config/theme
```

Check the saved `font_family` and `antialiased` fields. After restarting a test VM,
check the live values again and inspect a screenshot. `theme font <family>` is a
convenience command for the same live setting. Applying a scheme or template
selects its default font, so apply a personal font override afterward.

## Boot ordering and physical verification

The store restores named objects before subsystem configuration loads. Desktop
configuration loads after theme and hotkeys are initialized. Model and network
configuration load at their respective initialization stages. Live configuration
changes require no reboot; boot-image and driver changes still require booting
the new image.

Validate persistence by restarting a test VM and comparing `config show` with the
saved document. Validate graphical changes with screenshots and keyboard input.
Keep hardware reboot decisions with the operator: installing a Jekyll boot image
does not authorize rebooting Jekyll.

## Boundaries

Model objects hold credential references; retrieve no credential bytes for routine
configuration inspection. Use the credential subsystem for credential operations.
Do not send private images containing embedded boot credentials to public storage.

Network changes may disconnect SSH. Do not test them over the only management
connection. Use `model-init clear` to remove a stale endpoint without printing
credentials. Configuration templates do not change network settings or credentials.

The [configuration guide](configuring-anunix.md) documents object paths, command
examples, byte-editing semantics, and currently unsupported theme behavior.
