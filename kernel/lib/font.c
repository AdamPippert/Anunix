/* Fixed-cell theme font rendering with grayscale coverage and bitmap fallbacks. */

#include <anx/types.h>
#include <anx/font.h>
#include <anx/utf8.h>
#include <anx/fb.h>
#include <anx/spleen_12x24.h>
#include <anx/theme.h>
#include <anx/string.h>
#include <anx/theme_font_data.h>

_Static_assert(sizeof(anx_theme_font_alpha) / sizeof(anx_theme_font_alpha[0]) ==
	       ANX_FONT_SPLEEN, "coverage atlas must match family enum order");

/* Fallback glyph for characters outside 0x20-0x7E: filled block */
static const uint16_t glyph_fallback[ANX_FONT_HEIGHT] = {
	0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF,
	0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF,
	0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF,
};

static const char *const family_names[] = {
	"atkinson-hyperlegible-mono", "cascadia-mono", "jetbrains-mono-nerd", "spleen"
};

const char *anx_font_family_name(enum anx_font_family family)
{
	return (uint32_t)family < ANX_FONT_FAMILY_COUNT ? family_names[family] : NULL;
}

int anx_font_family_parse(const char *name, enum anx_font_family *family)
{
	uint32_t i;

	if (!name || !family)
		return ANX_EINVAL;
	for (i = 0; i < ANX_FONT_FAMILY_COUNT; i++) {
		if (anx_strcmp(name, family_names[i]) == 0) {
			*family = (enum anx_font_family)i;
			return ANX_OK;
		}
	}
	return ANX_EINVAL;
}

static enum anx_font_family current_family(void)
{
	enum anx_font_family family = anx_theme_get()->font.family;

	return (uint32_t)family < ANX_FONT_FAMILY_COUNT ? family : ANX_FONT_ATKINSON;
}

const uint16_t *anx_font_glyph(char c)
{
	unsigned char uc = (unsigned char)c;
	enum anx_font_family family = current_family();

	if (uc < ANX_FONT12_FIRST || uc > ANX_FONT12_LAST)
		return glyph_fallback;
	if (family == ANX_FONT_SPLEEN)
		return anx_font12[uc - ANX_FONT12_FIRST];
	return anx_theme_font_bits[family][uc - ANX_FONT12_FIRST];
}

/* ------------------------------------------------------------------ */
/* Unicode / fallback extension                                        */
/* ------------------------------------------------------------------ */

static struct anx_font_fallback font_fallbacks[ANX_FONT_FALLBACK_MAX];
static uint32_t                 font_fallback_count;

void anx_font_init(void)
{
	uint32_t i;

	for (i = 0; i < ANX_FONT_FALLBACK_MAX; i++) {
		font_fallbacks[i].cp_first   = 0;
		font_fallbacks[i].cp_last    = 0;
		font_fallbacks[i].get_glyph  = NULL;
	}
	font_fallback_count = 0;
}

int anx_font_fallback_register(uint32_t cp_first, uint32_t cp_last,
                                 const uint16_t *(*get_glyph)(uint32_t cp))
{
	if (!get_glyph || cp_first > cp_last)
		return ANX_EINVAL;
	if (font_fallback_count >= ANX_FONT_FALLBACK_MAX)
		return ANX_EFULL;

	font_fallbacks[font_fallback_count].cp_first  = cp_first;
	font_fallbacks[font_fallback_count].cp_last   = cp_last;
	font_fallbacks[font_fallback_count].get_glyph = get_glyph;
	font_fallback_count++;
	return ANX_OK;
}

const uint16_t *anx_font_glyph_cp(uint32_t codepoint)
{
	uint32_t i;

	/* Primary font covers printable ASCII. */
	if (codepoint >= ANX_FONT12_FIRST && codepoint <= ANX_FONT12_LAST)
		return anx_font_glyph((char)codepoint);

	/* Check registered fallback fonts in registration order. */
	for (i = 0; i < font_fallback_count; i++) {
		if (codepoint >= font_fallbacks[i].cp_first &&
		    codepoint <= font_fallbacks[i].cp_last)
			return font_fallbacks[i].get_glyph(codepoint);
	}

	return glyph_fallback;
}

bool anx_font_has_glyph(uint32_t codepoint)
{
	uint32_t i;

	if (codepoint >= ANX_FONT12_FIRST && codepoint <= ANX_FONT12_LAST)
		return true;

	for (i = 0; i < font_fallback_count; i++) {
		if (codepoint >= font_fallbacks[i].cp_first &&
		    codepoint <= font_fallbacks[i].cp_last)
			return true;
	}

	return false;
}

struct font_sample {
	const uint16_t *bits;
	const uint8_t *alpha;
};

static struct font_sample font_sample(uint32_t cp)
{
	struct font_sample sample = { anx_font_glyph_cp(cp), NULL };
	enum anx_font_family family = current_family();

	if (family != ANX_FONT_SPLEEN && anx_theme_get()->font.antialiased &&
	    cp >= ANX_FONT12_FIRST && cp <= ANX_FONT12_LAST)
		sample.alpha = anx_theme_font_alpha[family][cp - ANX_FONT12_FIRST];
	return sample;
}

static uint32_t sample_alpha(const struct font_sample *sample, uint32_t row, uint32_t col)
{
	if (sample->alpha)
		return sample->alpha[row * ANX_FONT_WIDTH + col];
	return sample->bits[row] & (0x800u >> col) ? 255 : 0;
}

static uint32_t blend(uint32_t fg, uint32_t bg, uint32_t alpha)
{
	uint32_t inv = 255 - alpha;
	uint32_t r = (((fg >> 16) & 255) * alpha + ((bg >> 16) & 255) * inv + 127) / 255;
	uint32_t g = (((fg >> 8) & 255) * alpha + ((bg >> 8) & 255) * inv + 127) / 255;
	uint32_t b = ((fg & 255) * alpha + (bg & 255) * inv + 127) / 255;

	return (r << 16) | (g << 8) | b;
}

static void paint_pixel(uint32_t *dst, uint32_t fg, uint32_t bg, uint32_t alpha)
{
	if (bg == ANX_FONT_TRANSPARENT) {
		if (!alpha)
			return;
		bg = *dst;
	}
	*dst = alpha == 255 ? fg : alpha == 0 ? bg : blend(fg, bg, alpha);
}

static void draw_codepoint_scaled(uint32_t x, uint32_t y, uint32_t cp,
				  uint32_t fg, uint32_t bg, uint32_t scale)
{
	const struct anx_fb_info *fb = anx_fb_get_info();
	struct font_sample sample;
	uint32_t row, col, w, h;

	if (!fb || !fb->available || x >= fb->width || y >= fb->height ||
	    !scale || scale > 4)
		return;
	sample = font_sample(cp);
	w = ANX_FONT_WIDTH * scale;
	h = ANX_FONT_HEIGHT * scale;
	if (w > fb->width - x) w = fb->width - x;
	if (h > fb->height - y) h = fb->height - y;
	for (row = 0; row < h; row++) {
		uint32_t *dst = anx_fb_row_ptr(y + row) + x;

		for (col = 0; col < w; col++)
			paint_pixel(&dst[col], fg, bg, sample_alpha(&sample, row / scale, col / scale));
	}
}

void anx_font_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
	draw_codepoint_scaled(x, y, (uint8_t)c, fg, bg, 1);
}

void anx_font_draw_codepoint(uint32_t x, uint32_t y, uint32_t cp, uint32_t fg, uint32_t bg)
{
	draw_codepoint_scaled(x, y, cp, fg, bg, 1);
}

void anx_font_draw_char_scaled(uint32_t x, uint32_t y, char c,
			      uint32_t fg, uint32_t bg, uint32_t scale)
{
	draw_codepoint_scaled(x, y, (uint8_t)c, fg, bg, scale);
}

void anx_font_blit_char_stride(uint32_t *buf, uint32_t stride,
			      uint32_t clip_w, uint32_t clip_h,
			      uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
	struct font_sample sample;
	uint32_t row, col, w = ANX_FONT_WIDTH, h = ANX_FONT_HEIGHT;

	if (!buf || clip_w > stride || x >= clip_w || y >= clip_h)
		return;
	sample = font_sample((uint8_t)c);
	if (w > clip_w - x) w = clip_w - x;
	if (h > clip_h - y) h = clip_h - y;
	for (row = 0; row < h; row++)
		for (col = 0; col < w; col++)
			paint_pixel(&buf[(uint64_t)(y + row) * stride + x + col], fg, bg,
				    sample_alpha(&sample, row, col));
}

void anx_font_blit_char(uint32_t *buf, uint32_t buf_w, uint32_t buf_h,
		       uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
	anx_font_blit_char_stride(buf, buf_w, buf_w, buf_h, x, y, c, fg, bg);
}

void anx_font_blit_str(uint32_t *buf, uint32_t buf_w, uint32_t buf_h,
		      uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg)
{
	if (!s || x >= buf_w || y >= buf_h)
		return;
	for (; *s && ANX_FONT_WIDTH <= buf_w - x; s++, x += ANX_FONT_WIDTH)
		anx_font_blit_char(buf, buf_w, buf_h, x, y, *s, fg, bg);
}

/* Sample at destination pixel centers; interpolate coverage, never final RGB. */
static uint32_t sample_scaled(const struct font_sample *sample,
			      uint32_t row, uint32_t col, uint32_t w, uint32_t h)
{
	int32_t sx, sy;
	uint32_t x0, y0, x1, y1, fx, fy, top, bottom;

	if (!sample->alpha)
		return sample_alpha(sample, row * ANX_FONT_HEIGHT / h, col * ANX_FONT_WIDTH / w);
	sx = (int32_t)((2 * col + 1) * ANX_FONT_WIDTH * 128 / w) - 128;
	sy = (int32_t)((2 * row + 1) * ANX_FONT_HEIGHT * 128 / h) - 128;
	if (sx < 0) sx = 0;
	if (sy < 0) sy = 0;
	x0 = (uint32_t)sx / 256; y0 = (uint32_t)sy / 256;
	x1 = x0 + 1 < ANX_FONT_WIDTH ? x0 + 1 : x0;
	y1 = y0 + 1 < ANX_FONT_HEIGHT ? y0 + 1 : y0;
	fx = (uint32_t)sx % 256; fy = (uint32_t)sy % 256;
	top = sample_alpha(sample, y0, x0) * (256 - fx) + sample_alpha(sample, y0, x1) * fx;
	bottom = sample_alpha(sample, y1, x0) * (256 - fx) + sample_alpha(sample, y1, x1) * fx;
	return (top * (256 - fy) + bottom * fy + 32768) / 65536;
}

void anx_font_blit_str_scaled(uint32_t *buf, uint32_t buf_w, uint32_t buf_h,
			     uint32_t x, uint32_t y, const char *s,
			     uint32_t fg, uint32_t bg, uint32_t scale_percent)
{
	uint32_t w, h, visible_h, row, col;

	if (!buf || !s || scale_percent < 100 || scale_percent > 400 ||
	    x >= buf_w || y >= buf_h)
		return;
	w = ANX_FONT_WIDTH * scale_percent / 100;
	h = ANX_FONT_HEIGHT * scale_percent / 100;
	visible_h = h < buf_h - y ? h : buf_h - y;
	for (; *s && x < buf_w; s++, x += w) {
		struct font_sample sample = font_sample((uint8_t)*s);
		uint32_t visible_w = w < buf_w - x ? w : buf_w - x;

		for (row = 0; row < visible_h; row++)
			for (col = 0; col < visible_w; col++)
				paint_pixel(&buf[(uint64_t)(y + row) * buf_w + x + col], fg, bg,
					    sample_scaled(&sample, row, col, w, h));
		if (visible_w < w)
			break;
	}
}

int anx_font_draw_str(uint32_t x, uint32_t y,
                       const char *utf8_str, uint32_t fg, uint32_t bg)
{
	const uint8_t *p;
	uint32_t remaining, cp, consumed, glyphs;
	uint64_t cx;
	int rc;

	if (!utf8_str)
		return 0;

	p         = (const uint8_t *)utf8_str;
	remaining = 0;
	while (p[remaining])
		remaining++;

	glyphs = 0;
	cx     = x;
	while (remaining > 0) {
		rc = anx_utf8_decode(p, remaining, &cp, &consumed);
		if (rc != ANX_OK) {
			/* Skip one invalid byte and continue. */
			p++;
			remaining--;
			continue;
		}
		if (cx <= 0xFFFFFFFFu)
			anx_font_draw_codepoint((uint32_t)cx, y, cp, fg, bg);
		cx += ANX_FONT_WIDTH;
		p  += consumed;
		remaining -= consumed;
		glyphs++;
	}
	return (int)glyphs;
}
