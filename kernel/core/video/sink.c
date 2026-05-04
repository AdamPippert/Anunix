/*
 * sink.c — Built-in video sinks (RFC-0024).
 *
 * Three sinks ship with the player:
 *
 *   null     — discards every frame (used as the safe default).
 *   capture  — records (output_index, source_index, w, h, crc32) for
 *              up to ANX_VIDEO_CAPTURE_MAX frames so tests can verify
 *              pacing and frame-selection precisely without comparing
 *              megabyte-sized pixel buffers.
 *   surface  — copies each RGBA frame into a bound iface surface's
 *              canvas, then marks damage + commits.  Bind/unbind is
 *              explicit so a stray "surface" selection cannot panic
 *              if no surface has been provided.
 *
 * The CRC-32 used by the capture sink is the IEEE 802.3 polynomial
 * (0xEDB88320 reflected).  We hand-roll a 256-entry table that's
 * built lazily on first use — small, branch-free, and beats the
 * classical bit-by-bit version by ~8x.
 */

#include <anx/video.h>
#include <anx/types.h>
#include <anx/string.h>
#include <anx/spinlock.h>
#include <anx/alloc.h>
#include <anx/state_object.h>
#include <anx/interface_plane.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* CRC-32 / IEEE 802.3                                                 */
/* ------------------------------------------------------------------ */

static uint32_t crc32_tab[256];
static bool     crc32_tab_ready;

static void
crc32_build_table(void)
{
	uint32_t i, j, c;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
		crc32_tab[i] = c;
	}
	crc32_tab_ready = true;
}

static uint32_t
crc32_compute(const uint8_t *p, uint32_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	uint32_t i;

	if (!crc32_tab_ready)
		crc32_build_table();
	for (i = 0; i < len; i++)
		crc = crc32_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* Built-in: null sink                                                 */
/* ------------------------------------------------------------------ */

static int  null_open(const struct anx_video_format *fmt) { (void)fmt; return ANX_OK; }
static int  null_write_frame(uint64_t o, uint64_t s, const uint8_t *p,
			     uint32_t w, uint32_t h)
{
	(void)o; (void)s; (void)p; (void)w; (void)h;
	return ANX_OK;
}
static void null_close(void) {}

static const struct anx_video_sink_ops null_ops = {
	.open        = null_open,
	.write_frame = null_write_frame,
	.close       = null_close,
};

void
anx_video_sink_null_register(void)
{
	anx_video_sink_register("null", &null_ops);
}

/* ------------------------------------------------------------------ */
/* Built-in: capture sink                                              */
/* ------------------------------------------------------------------ */

#define ANX_VIDEO_CAPTURE_MAX	256

static struct anx_video_capture_record cap_records[ANX_VIDEO_CAPTURE_MAX];
static uint32_t                        cap_count;

static int
cap_open(const struct anx_video_format *fmt)
{
	(void)fmt;
	cap_count = 0;
	return ANX_OK;
}

static int
cap_write_frame(uint64_t o, uint64_t s, const uint8_t *p,
		uint32_t w, uint32_t h)
{
	if (cap_count >= ANX_VIDEO_CAPTURE_MAX)
		return ANX_OK;	/* silently drop excess; caller can resize test */

	struct anx_video_capture_record *r = &cap_records[cap_count++];
	r->output_index = o;
	r->source_index = s;
	r->width        = w;
	r->height       = h;
	r->crc32        = crc32_compute(p, w * h * 4u);
	return ANX_OK;
}

static void cap_close(void) {}

static const struct anx_video_sink_ops cap_ops = {
	.open        = cap_open,
	.write_frame = cap_write_frame,
	.close       = cap_close,
};

void
anx_video_sink_capture_register(void)
{
	anx_video_sink_register("capture", &cap_ops);
}

void
anx_video_sink_capture_reset(void)
{
	cap_count = 0;
	anx_memset(cap_records, 0, sizeof(cap_records));
}

const struct anx_video_capture_record *
anx_video_sink_capture_records(uint32_t *count_out)
{
	if (count_out)
		*count_out = cap_count;
	return cap_records;
}

/* ------------------------------------------------------------------ */
/* Built-in: surface sink                                              */
/*                                                                     */
/* Bound surface OID is global state; one playback at a time.  This    */
/* matches the "single audio sink" model and keeps the hot path free   */
/* of any registry lookups.                                            */
/* ------------------------------------------------------------------ */

static anx_oid_t           g_surf_bound_oid;
static bool                g_surf_have_binding;
static struct anx_spinlock g_surf_lock;
static bool                g_surf_lock_inited;

static void
surf_lock_init(void)
{
	if (!g_surf_lock_inited) {
		anx_spin_init(&g_surf_lock);
		g_surf_lock_inited = true;
	}
}

int
anx_video_sink_surface_bind(const anx_oid_t *surface_oid)
{
	if (!surface_oid)
		return ANX_EINVAL;
	surf_lock_init();
	anx_spin_lock(&g_surf_lock);
	g_surf_bound_oid    = *surface_oid;
	g_surf_have_binding = true;
	anx_spin_unlock(&g_surf_lock);
	return ANX_OK;
}

void
anx_video_sink_surface_unbind(void)
{
	surf_lock_init();
	anx_spin_lock(&g_surf_lock);
	g_surf_have_binding = false;
	anx_memset(&g_surf_bound_oid, 0, sizeof(g_surf_bound_oid));
	anx_spin_unlock(&g_surf_lock);
}

static int
surf_open(const struct anx_video_format *fmt)
{
	(void)fmt;
	surf_lock_init();
	return g_surf_have_binding ? ANX_OK : ANX_ENODEV;
}

/*
 * Copy 'src' (frame_w × frame_h RGBA) into a destination canvas
 * (canvas_w × canvas_h).  Clips to min on both axes — never reads
 * or writes past either buffer.  This is the entire pixel hot path.
 */
static void
blit_rgba(uint8_t *dst, uint32_t canvas_w, uint32_t canvas_h,
	  const uint8_t *src, uint32_t frame_w, uint32_t frame_h)
{
	uint32_t copy_w = frame_w < canvas_w ? frame_w : canvas_w;
	uint32_t copy_h = frame_h < canvas_h ? frame_h : canvas_h;
	uint32_t row;
	uint32_t row_bytes = copy_w * 4u;

	for (row = 0; row < copy_h; row++) {
		uint8_t       *drow = dst + (uint64_t)row * canvas_w * 4u;
		const uint8_t *srow = src + (uint64_t)row * frame_w * 4u;
		anx_memcpy(drow, srow, row_bytes);
	}
}

static int
surf_write_frame(uint64_t o, uint64_t s, const uint8_t *p,
		 uint32_t w, uint32_t h)
{
	struct anx_surface *surf = NULL;
	anx_oid_t           oid;
	bool                have;
	int                 rc;

	(void)o; (void)s;

	surf_lock_init();
	anx_spin_lock(&g_surf_lock);
	have = g_surf_have_binding;
	oid  = g_surf_bound_oid;
	anx_spin_unlock(&g_surf_lock);
	if (!have)
		return ANX_ENODEV;

	rc = anx_iface_surface_lookup(oid, &surf);
	if (rc != ANX_OK || !surf)
		return ANX_ENOENT;
	if (!surf->content_root || !surf->content_root->data ||
	    surf->content_root->data_len < (uint64_t)surf->width *
					    surf->height * 4u)
		return ANX_ENODEV;

	blit_rgba((uint8_t *)surf->content_root->data,
		  surf->width, surf->height,
		  p, w, h);

	{
		uint32_t dmg_w = w < surf->width  ? w : surf->width;
		uint32_t dmg_h = h < surf->height ? h : surf->height;
		anx_iface_surface_damage(surf, 0, 0, dmg_w, dmg_h);
	}
	anx_iface_surface_commit(surf);
	return ANX_OK;
}

static void
surf_close(void)
{
	/* Nothing to tear down — binding persists across runs unless
	 * explicitly unbound by the caller. */
}

static const struct anx_video_sink_ops surf_ops = {
	.open        = surf_open,
	.write_frame = surf_write_frame,
	.close       = surf_close,
};

void
anx_video_sink_surface_register(void)
{
	anx_video_sink_register("surface", &surf_ops);
}
