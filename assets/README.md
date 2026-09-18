# Default wallpaper

`wallpaper-default.png` is the 1920 × 1200 RGB PNG conversion of the
user-selected `himmelstraeume-bachalpsee-7572681_1920.jpg`. The source was
provided from the user's Downloads folder; the original was not modified.
No pixels were cropped or resized during conversion.

Source JPEG SHA-256: `b1679076457c55e4adee2aa7c0d6ea009594c543c256e33c6bb7ec6d6c7af2e6`.
PNG SHA-256: `0db9d6822407afb5c67cfbcfecd05b99162ee1a106354374a3a260d71c7972bf`.

The build decodes this PNG with `tools/embed_wallpaper.py` into an embedded
XRGB frame. The desktop preserves its aspect ratio and crops centrally to
fill the display. Run `wallpaper default` to select it, then
`config save theme` to persist that mode over an older saved background.
