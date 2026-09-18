#ifndef ANX_WINDOW_CHROME_H
#define ANX_WINDOW_CHROME_H

#include <anx/theme.h>

enum anx_window_button {
	ANX_WINDOW_BUTTON_NONE = 0,
	ANX_WINDOW_BUTTON_CLOSE,
	ANX_WINDOW_BUTTON_MINIMIZE,
	ANX_WINDOW_BUTTON_MAXIMIZE,
};

struct anx_chrome_rect {
	uint32_t x, y, w, h;
};

/* Rectangles are relative to the top-left of the actual titlebar. */
struct anx_window_chrome {
	struct anx_chrome_rect draw[3]; /* indexed by button - 1 */
	struct anx_chrome_rect hit[3];
	uint32_t width, height;
	uint32_t title_x, title_width;
};

/* Compute bounded control rectangles and the remaining title span. */
void anx_window_chrome_layout(enum anx_window_controls controls,
			     uint32_t width, uint32_t height,
			     struct anx_window_chrome *chrome);

/* Resolve a title-relative pointer to the same controls the renderer draws. */
enum anx_window_button anx_window_chrome_hit(const struct anx_window_chrome *chrome,
					   int32_t x, int32_t y);

#endif /* ANX_WINDOW_CHROME_H */
