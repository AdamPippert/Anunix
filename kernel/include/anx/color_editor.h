#ifndef ANX_COLOR_EDITOR_H
#define ANX_COLOR_EDITOR_H

#include <anx/types.h>

struct anx_surface;

/* Open or focus the palette editor. */
void anx_wm_launch_color_editor(void);

/* Deliver a focused key press to the palette editor. */
void anx_wm_color_editor_key(uint32_t key, uint32_t mods, uint32_t unicode);

/* Return the editor's live surface, or NULL while closed. */
struct anx_surface *anx_wm_color_editor_surface(void);

/* Update the canvas from the live palette; the caller commits/repaints it. */
void anx_wm_color_editor_redraw(void);

#endif /* ANX_COLOR_EDITOR_H */
