#include <anx/window_chrome.h>
#include <anx/string.h>

#define DOT_SIZE 14u
#define DOT_LEFT 8u
#define DOT_GAP 5u
#define WINDOWS_BUTTON_WIDTH 46u
#define WINDOWS_BUTTON_MIN 12u

void anx_window_chrome_layout(enum anx_window_controls controls,
			     uint32_t width, uint32_t height,
			     struct anx_window_chrome *chrome)
{
	uint32_t i, end = 0;

	anx_memset(chrome, 0, sizeof(*chrome));
	chrome->width = width;
	chrome->height = height;
	if (!width || !height)
		return;
	if (controls == ANX_CONTROLS_WINDOWS) {
		/* Keep close first when an unusually narrow window cannot fit all three. */
		static const uint32_t order[] = { 0, 2, 1 };
		uint32_t count = width / WINDOWS_BUTTON_MIN;
		uint32_t button_w;

		if (count > 3)
			count = 3;
		button_w = count ? width / count : 0;
		if (button_w > WINDOWS_BUTTON_WIDTH)
			button_w = WINDOWS_BUTTON_WIDTH;
		end = width;
		for (i = 0; i < count; i++) {
			struct anx_chrome_rect r = { end - button_w, 0, button_w, height };

			chrome->draw[order[i]] = r;
			chrome->hit[order[i]] = r;
			end -= button_w;
		}
		chrome->title_x = width < 8 ? width : 8;
		chrome->title_width = end > 16 ? end - 16 : 0;
		return;
	}
	for (i = 0; i < 3; i++) {
		uint32_t size = height < DOT_SIZE ? height : DOT_SIZE;
		uint32_t x = DOT_LEFT + i * (DOT_SIZE + DOT_GAP);
		uint32_t hit_x = x - DOT_GAP / 2 - 1;
		uint32_t hit_end = x + size + DOT_GAP / 2;

		if (x >= width || size > width - x)
			break;
		if (hit_end > width)
			hit_end = width;
		chrome->draw[i] = (struct anx_chrome_rect){ x, (height - size) / 2,
							 size, size };
		chrome->hit[i] = (struct anx_chrome_rect){ hit_x, 0, hit_end - hit_x, height };
		end = x + size;
	}
	chrome->title_x = end + 8 < width ? end + 8 : width;
	chrome->title_width = width - chrome->title_x;
	if (chrome->title_width > 8)
		chrome->title_width -= 8;
	else
		chrome->title_width = 0;
}

enum anx_window_button anx_window_chrome_hit(const struct anx_window_chrome *chrome,
					   int32_t x, int32_t y)
{
	uint32_t i;

	if (x < 0 || y < 0 || (uint32_t)x >= chrome->width || (uint32_t)y >= chrome->height)
		return ANX_WINDOW_BUTTON_NONE;
	for (i = 0; i < 3; i++) {
		const struct anx_chrome_rect *r = &chrome->hit[i];

		if ((uint32_t)x >= r->x && (uint32_t)x - r->x < r->w &&
		    (uint32_t)y >= r->y && (uint32_t)y - r->y < r->h)
			return (enum anx_window_button)(ANX_WINDOW_BUTTON_CLOSE + i);
	}
	return ANX_WINDOW_BUTTON_NONE;
}
