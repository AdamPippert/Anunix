/* The source PNG is decoded once at build time, not during desktop repaint. */
.section .rodata
.balign 4
.global anx_default_wallpaper_start
.global anx_default_wallpaper_end

anx_default_wallpaper_start:
	.incbin "build/wallpaper-default.anwp"
anx_default_wallpaper_end:

.section .note.GNU-stack,"",%progbits
