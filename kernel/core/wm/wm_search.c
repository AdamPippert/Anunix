/*
 * wm_search.c — Meta+Space: agent-guided workflow and command search.
 *
 * Non-empty query: anx_wf_lib_match() keyword hits + JEPA goal-alignment
 * energy bonus per workflow category + PAL cross-session prior boost.
 * The combination makes this genuinely agent-driven: the loop subsystem
 * interprets the query intent and biases results toward historically
 * successful selections.
 *
 * Empty query: PAL suggests workflows from the "workflow-search" world
 * ranked by how often they were previously selected.
 *
 * Enter on empty query → opens the full workflow directory panel (all
 * registered templates, organized by URI namespace category).
 */

#include <anx/types.h>
#include <anx/wm.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/fb.h>
#include <anx/font.h>
#include <anx/theme.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/workflow.h>
#include <anx/workflow_library.h>
#include <anx/shell.h>
#include <anx/loop.h>
#include <anx/memory.h>
#include <anx/jepa.h>

/* ------------------------------------------------------------------ */
/* Layout constants                                                    */
/* ------------------------------------------------------------------ */

#define CS_W		600
#define CS_H		420

#define FONT_W		ANX_FONT_WIDTH
#define FONT_H		ANX_FONT_HEIGHT
#define ROW_H		(FONT_H + 6)

#define TITLE_H		32
#define INPUT_H		28
#define LIST_Y		(TITLE_H + INPUT_H + 6)
#define MAX_RESULTS	12

#define DIR_W		800
#define DIR_H		560
#define DIR_TITLE_H	34
#define DIR_CAT_H	20
#define DIR_ROW_H	(FONT_H + 8)
#define DIR_MAX_ITEMS	64

/* PAL world used to record and replay search selections */
#define SEARCH_WORLD	"anx:world/workflow-search/v1"

/* ------------------------------------------------------------------ */
/* Built-in commands (always present regardless of workflow registry)  */
/* ------------------------------------------------------------------ */

static const char *const g_builtins[] = {
	"ls", "cat", "cp", "mv", "inspect", "search",
	"meta", "tensor", "model", "workflow", "kickstart",
	"fetch", "netinfo", "sysinfo", "hwd", "wifi",
	"cells", "vm", "display", "theme", "conform",
	NULL
};

/* ------------------------------------------------------------------ */
/* Local substring helper (kernel has no str_contains)                   */
/* ------------------------------------------------------------------ */

static bool str_contains(const char *hay, const char *needle)
{
	uint32_t nl;

	if (!hay || !needle || !*needle) return true;
	nl = (uint32_t)anx_strlen(needle);
	for (; *hay; hay++) {
		if (anx_strncmp(hay, needle, nl) == 0)
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Workflow-to-JEPA-action mapping for goal alignment scoring          */
/* ------------------------------------------------------------------ */

static uint32_t uri_to_action(const char *uri)
{
	if (!uri) return ANX_JEPA_ACT_IDLE;
	if (str_contains(uri, "ibal")   || str_contains(uri, "loop") ||
	    str_contains(uri, "memory") || str_contains(uri, "pal"))
		return ANX_JEPA_ACT_MEM_PROMOTE;
	if (str_contains(uri, "browser") || str_contains(uri, "fetch") ||
	    str_contains(uri, "remote"))
		return ANX_JEPA_ACT_ROUTE_REMOTE;
	if (str_contains(uri, "model")  || str_contains(uri, "infer") ||
	    str_contains(uri, "agent")  || str_contains(uri, "rag"))
		return ANX_JEPA_ACT_CAP_VALIDATE;
	if (str_contains(uri, "cell")   || str_contains(uri, "system") ||
	    str_contains(uri, "vm")     || str_contains(uri, "spawn"))
		return ANX_JEPA_ACT_CELL_SPAWN;
	if (str_contains(uri, "route")  || str_contains(uri, "net") ||
	    str_contains(uri, "local"))
		return ANX_JEPA_ACT_ROUTE_LOCAL;
	return ANX_JEPA_ACT_IDLE;
}

/* ------------------------------------------------------------------ */
/* Search state                                                        */
/* ------------------------------------------------------------------ */

struct cs_result {
	char     label[64];		/* display string */
	char     uri[128];		/* workflow URI, empty for builtins */
	uint32_t score;			/* higher = better */
	bool     is_builtin;
};

static struct {
	struct anx_surface *surf;
	uint32_t           *pixels;
	uint32_t            pos_x;
	uint32_t            pos_y;

	char     query[64];
	uint32_t query_len;

	struct cs_result results[MAX_RESULTS];
	uint32_t         result_count;
	int32_t          selected;

	bool     agent_active;		/* IBAL session running */
} g_cs;

/* ------------------------------------------------------------------ */
/* Directory panel state                                               */
/* ------------------------------------------------------------------ */

struct dir_item {
	char     display_name[64];
	char     uri[128];
	char     category[32];
};

static struct {
	struct anx_surface *surf;
	uint32_t           *pixels;

	struct dir_item items[DIR_MAX_ITEMS];
	uint32_t        item_count;
	int32_t         selected;
	uint32_t        scroll;		/* first visible item index */
} g_dir;

/* ------------------------------------------------------------------ */
/* Shared pixel helpers (used by both overlays)                        */
/* ------------------------------------------------------------------ */

static void px_fill(uint32_t *pixels, uint32_t stride,
		    uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		    uint32_t color, uint32_t clip_w, uint32_t clip_h)
{
	uint32_t r, c;

	for (r = y; r < y + h && r < clip_h; r++)
		for (c = x; c < x + w && c < clip_w; c++)
			pixels[r * stride + c] = color;
}

static void px_char(uint32_t *pixels, uint32_t stride,
		    uint32_t x, uint32_t y, char ch,
		    uint32_t fg, uint32_t bg,
		    uint32_t clip_w, uint32_t clip_h)
{
	const uint16_t *glyph = anx_font_glyph(ch);
	uint32_t r, c;

	for (r = 0; r < (uint32_t)FONT_H && y + r < clip_h; r++) {
		uint16_t bits = glyph[r];

		for (c = 0; c < (uint32_t)FONT_W && x + c < clip_w; c++)
			pixels[(y + r) * stride + (x + c)] =
				(bits & (0x800u >> c)) ? fg : bg;
	}
}

static void px_str(uint32_t *pixels, uint32_t stride,
		   uint32_t x, uint32_t y, const char *s,
		   uint32_t fg, uint32_t bg,
		   uint32_t clip_w, uint32_t clip_h)
{
	for (; *s; s++, x += FONT_W) {
		if (x + FONT_W > clip_w) break;
		px_char(pixels, stride, x, y, *s, fg, bg, clip_w, clip_h);
	}
}

/* ------------------------------------------------------------------ */
/* Category label from workflow URI                                    */
/* ------------------------------------------------------------------ */

static const char *uri_category(const char *uri)
{
	if (!uri || !*uri) return "Commands";
	if (str_contains(uri, "/ibal/")   || str_contains(uri, "/loop/"))
		return "AI Loop";
	if (str_contains(uri, "/model/")  || str_contains(uri, "/infer/") ||
	    str_contains(uri, "/agent/")  || str_contains(uri, "/rag/"))
		return "AI Models";
	if (str_contains(uri, "/browser/")|| str_contains(uri, "/fetch/"))
		return "Network";
	if (str_contains(uri, "/system/") || str_contains(uri, "/vm/"))
		return "System";
	if (str_contains(uri, "/workflow/designer") ||
	    str_contains(uri, "/workflow/"))
		return "Workflows";
	return "Other";
}

/* ------------------------------------------------------------------ */
/* Agent-driven result population                                      */
/* ------------------------------------------------------------------ */

static void cs_populate(void)
{
	const char *wf_uris[32];
	struct anx_wf_match wf_matches[MAX_RESULTS];
	uint32_t wf_count = 0, wf_matches_count = 0;
	uint32_t i;

	g_cs.result_count = 0;
	g_cs.agent_active = false;

	anx_wf_lib_list(wf_uris, 32, &wf_count);

	/* --- Empty query: PAL-biased suggestions --- */
	if (g_cs.query[0] == '\0') {
		/*
		 * Use anx_loop_select_action_by_prior() (integer result) to
		 * identify the historically preferred action category, then
		 * boost workflows in that category.  All other workflows get
		 * a baseline score.  No float arithmetic needed.
		 */
		uint32_t pal_act = anx_loop_select_action_by_prior(
					SEARCH_WORLD, ANX_JEPA_ACT_COUNT);

		for (i = 0; i < wf_count && g_cs.result_count < MAX_RESULTS; i++) {
			uint32_t act   = uri_to_action(wf_uris[i]);
			uint32_t score = 50 + (act == pal_act ? 50 : 0);
			const struct anx_wf_template *tmpl;
			const char *dname = wf_uris[i];
			const char *slash;

			tmpl = anx_wf_lib_lookup(wf_uris[i]);
			if (tmpl && tmpl->display_name[0]) {
				dname = tmpl->display_name;
			} else {
				slash = wf_uris[i];
				for (const char *p = wf_uris[i]; *p; p++)
					if (*p == '/') slash = p + 1;
				dname = slash;
			}

			anx_strlcpy(g_cs.results[g_cs.result_count].label,
				     dname, 64);
			anx_strlcpy(g_cs.results[g_cs.result_count].uri,
				     wf_uris[i], 128);
			g_cs.results[g_cs.result_count].score      = score;
			g_cs.results[g_cs.result_count].is_builtin = false;
			g_cs.result_count++;
		}

		/* Builtins always at baseline */
		for (i = 0; g_builtins[i] && g_cs.result_count < MAX_RESULTS; i++) {
			anx_strlcpy(g_cs.results[g_cs.result_count].label,
				     g_builtins[i], 64);
			g_cs.results[g_cs.result_count].uri[0]     = '\0';
			g_cs.results[g_cs.result_count].score      = 30;
			g_cs.results[g_cs.result_count].is_builtin = true;
			g_cs.result_count++;
		}

		g_cs.agent_active = (g_cs.result_count > 0);
		goto done;
	}

	/* --- Non-empty query: keyword + JEPA goal-alignment scoring --- */

	/*
	 * Step 1: keyword hits via the workflow library's built-in scorer.
	 * anx_wf_lib_match() scores against display_name, description, tags.
	 */
	wf_matches_count = anx_wf_lib_match(g_cs.query, wf_matches,
					     MAX_RESULTS);

	/*
	 * PAL: get the historically preferred action category (integer).
	 * Workflows in that category get a +50 boost on top of keyword score.
	 */
	{
		uint32_t pal_act = anx_loop_select_action_by_prior(
					SEARCH_WORLD, ANX_JEPA_ACT_COUNT);

		for (i = 0; i < wf_matches_count && g_cs.result_count < MAX_RESULTS; i++) {
			uint32_t act   = uri_to_action(wf_matches[i].uri);
			uint32_t score = wf_matches[i].score * 100
					 + (act == pal_act ? 50 : 0);
			const struct anx_wf_template *tmpl;
			const char *dname = wf_matches[i].uri;
			const char *slash;

			tmpl = anx_wf_lib_lookup(wf_matches[i].uri);
			if (tmpl && tmpl->display_name[0]) {
				dname = tmpl->display_name;
			} else {
				slash = wf_matches[i].uri;
				for (const char *p = wf_matches[i].uri; *p; p++)
					if (*p == '/') slash = p + 1;
				dname = slash;
			}

			anx_strlcpy(g_cs.results[g_cs.result_count].label,
				     dname, 64);
			anx_strlcpy(g_cs.results[g_cs.result_count].uri,
				     wf_matches[i].uri, 128);
			g_cs.results[g_cs.result_count].score      = score;
			g_cs.results[g_cs.result_count].is_builtin = false;
			g_cs.result_count++;
		}
	}

	/*
	 * Step 2: built-in commands — simple substring match, lower weight
	 * since they don't participate in JEPA scoring.
	 */
	for (i = 0; g_builtins[i] && g_cs.result_count < MAX_RESULTS; i++) {
		const char *b = g_builtins[i];
		const char *q = g_cs.query;
		bool match = false;
		const char *n;

		for (n = b; *n && !match; n++) {
			const char *nn = n;
			const char *qq = q;

			for (; *qq; qq++, nn++) {
				char nc = *nn, qc = *qq;
				if (!*nn) goto no_match;
				if (nc >= 'A' && nc <= 'Z') nc += 32;
				if (qc >= 'A' && qc <= 'Z') qc += 32;
				if (nc != qc) goto no_match;
			}
			match = true;
no_match:;
		}

		if (match) {
			g_cs.results[g_cs.result_count].score      = 20;
			g_cs.results[g_cs.result_count].is_builtin = true;
			g_cs.results[g_cs.result_count].uri[0]     = '\0';
			anx_strlcpy(g_cs.results[g_cs.result_count].label, b, 64);
			g_cs.result_count++;
		}
	}

	/*
	 * Step 3: bubble-sort descending by score (MAX_RESULTS is small).
	 */
	{
		uint32_t a, b2;
		for (a = 0; a + 1 < g_cs.result_count; a++) {
			for (b2 = a + 1; b2 < g_cs.result_count; b2++) {
				if (g_cs.results[b2].score >
				    g_cs.results[a].score) {
					struct cs_result tmp = g_cs.results[a];
					g_cs.results[a]  = g_cs.results[b2];
					g_cs.results[b2] = tmp;
				}
			}
		}
	}

	/* Mark agent as having participated so PAL update fires on selection */
	g_cs.agent_active = (g_cs.result_count > 0);

done:
	if (g_cs.selected >= (int32_t)g_cs.result_count)
		g_cs.selected = (int32_t)g_cs.result_count - 1;
	if (g_cs.selected < 0 && g_cs.result_count > 0)
		g_cs.selected = 0;
}

/* ------------------------------------------------------------------ */
/* Search overlay rendering                                            */
/* ------------------------------------------------------------------ */

static void cs_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg     = theme->palette.surface;
	uint32_t accent = theme->palette.accent;
	uint32_t text   = theme->palette.text_primary;
	uint32_t dim    = theme->palette.text_dim;
	uint32_t sel_bg = 0x00103040;
	uint32_t i;
	char     buf[72];

	if (!g_cs.surf || !g_cs.pixels) return;

#define CS_FN(x,y,w,h,c) px_fill(g_cs.pixels,CS_W,(x),(y),(w),(h),(c),CS_W,CS_H)
#define CS_STR(x,y,s,fg,bg2) px_str(g_cs.pixels,CS_W,(x),(y),(s),(fg),(bg2),CS_W,CS_H)

	CS_FN(0, 0, CS_W, CS_H, bg);

	/* Title bar */
	CS_FN(0, 0, CS_W, TITLE_H, theme->palette.background);
	CS_FN(0, TITLE_H - 1, CS_W, 1, accent);
	CS_STR(10, (TITLE_H - FONT_H) / 2,
	       g_cs.query[0] ? "Search" : "Suggestions  (Enter for directory)",
	       accent, theme->palette.background);

	/* Input field */
	CS_FN(0, TITLE_H, CS_W, INPUT_H, 0x00111111);
	CS_FN(0, TITLE_H + INPUT_H - 1, CS_W, 1, dim);
	anx_snprintf(buf, sizeof(buf), "> %s", g_cs.query);
	CS_STR(4, TITLE_H + 5, buf, text, 0x00111111);
	/* cursor */
	CS_FN(4 + (uint32_t)anx_strlen(buf) * FONT_W, TITLE_H + 5,
	      2, FONT_H, accent);

	/* Results */
	for (i = 0; i < g_cs.result_count; i++) {
		uint32_t ry  = LIST_Y + i * ROW_H;
		bool     sel = ((int32_t)i == g_cs.selected);
		uint32_t item_bg = sel ? sel_bg : bg;
		char     score_buf[8];

		if (sel) CS_FN(0, ry, CS_W, ROW_H, sel_bg);

		CS_STR(4,              ry + 3, sel ? ">" : " ",
		       accent, item_bg);
		CS_STR(4 + FONT_W,     ry + 3,
		       g_cs.results[i].is_builtin ? "[cmd] " : "      ",
		       dim, item_bg);
		CS_STR(4 + 7 * FONT_W, ry + 3, g_cs.results[i].label,
		       sel ? text : dim, item_bg);

		/* Score hint on right side (debug/transparency) */
		anx_snprintf(score_buf, sizeof(score_buf), "%u",
			     g_cs.results[i].score);
		CS_STR(CS_W - (uint32_t)anx_strlen(score_buf) * FONT_W - 4,
		       ry + 3, score_buf, 0x00303030, item_bg);
	}

	if (g_cs.result_count == 0)
		CS_STR(10, LIST_Y + 8, "no matches", dim, bg);

	/* Footer */
	{
		uint32_t fy = CS_H - FONT_H - 4;
		CS_FN(0, fy - 2, CS_W, 1, dim);
		CS_STR(4, fy,
		       g_cs.query[0]
		         ? "Up/Dn: select   Enter: run   Esc: close"
		         : "Up/Dn: select   Enter: directory   Esc: close",
		       dim, bg);
	}

#undef CS_FN
#undef CS_STR

	anx_iface_surface_commit(g_cs.surf);
}

/* ------------------------------------------------------------------ */
/* PAL update on selection                                             */
/* ------------------------------------------------------------------ */

static void cs_record_selection(uint32_t idx)
{
	struct anx_loop_memory_payload mp;
	uint32_t act;

	if (idx >= g_cs.result_count) return;
	if (!g_cs.agent_active) return;

	act = g_cs.results[idx].is_builtin
		? ANX_JEPA_ACT_IDLE
		: uri_to_action(g_cs.results[idx].uri);

	anx_memset(&mp, 0, sizeof(mp));
	anx_strlcpy(mp.world_uri, SEARCH_WORLD, sizeof(mp.world_uri));
	mp.action_stats[act].total_updates  = 1;
	mp.action_stats[act].win_rate       = 1.0f;
	mp.action_stats[act].avg_energy     = 0.1f;
	mp.action_stats[act].min_energy     = 0.05f;
	anx_pal_memory_update(SEARCH_WORLD, &mp);
}

/* ------------------------------------------------------------------ */
/* Execute selected result                                             */
/* ------------------------------------------------------------------ */

static void cs_execute_selected(void)
{
	uint32_t idx = (uint32_t)g_cs.selected;

	if (g_cs.selected < 0 || idx >= g_cs.result_count) return;

	cs_record_selection(idx);

	anx_wm_window_close(g_cs.surf);
	g_cs.surf   = NULL;
	g_cs.pixels = NULL;

	/* Workflow URI: instantiate and run */
	if (!g_cs.results[idx].is_builtin && g_cs.results[idx].uri[0]) {
		anx_oid_t wf_oid;

		if (anx_wf_lib_instantiate(g_cs.results[idx].uri,
					   g_cs.results[idx].label,
					   &wf_oid) == ANX_OK)
			anx_wf_run(&wf_oid, NULL);
		return;
	}

	/* Built-in command: type into terminal */
	{
		char     cmd[68];
		uint32_t i;

		anx_strlcpy(cmd, g_cs.results[idx].label, sizeof(cmd));
		anx_wm_terminal_open();
		for (i = 0; cmd[i]; i++)
			anx_wm_terminal_key_event(ANX_KEY_NONE, 0,
						  (uint32_t)(uint8_t)cmd[i]);
		anx_wm_terminal_key_event(ANX_KEY_ENTER, 0, '\n');
	}
}

/* ------------------------------------------------------------------ */
/* Workflow directory panel                                            */
/* ------------------------------------------------------------------ */

static void dir_populate(void)
{
	const char *uris[DIR_MAX_ITEMS];
	uint32_t    ucount = 0;
	uint32_t    i;
	const char *const *b;

	g_dir.item_count = 0;

	/* Built-in commands first */
	for (b = g_builtins; *b && g_dir.item_count < DIR_MAX_ITEMS; b++) {
		anx_strlcpy(g_dir.items[g_dir.item_count].display_name, *b, 64);
		anx_strlcpy(g_dir.items[g_dir.item_count].uri, "", 128);
		anx_strlcpy(g_dir.items[g_dir.item_count].category,
			     "Commands", 32);
		g_dir.item_count++;
	}

	/* Registered workflow templates */
	anx_wf_lib_list(uris, DIR_MAX_ITEMS, &ucount);
	for (i = 0; i < ucount && g_dir.item_count < DIR_MAX_ITEMS; i++) {
		const struct anx_wf_template *t = anx_wf_lib_lookup(uris[i]);
		const char *dname = uris[i];
		const char *slash;

		if (t && t->display_name[0])
			dname = t->display_name;
		else {
			slash = uris[i];
			for (const char *p = uris[i]; *p; p++)
				if (*p == '/') slash = p + 1;
			dname = slash;
		}

		anx_strlcpy(g_dir.items[g_dir.item_count].display_name,
			     dname, 64);
		anx_strlcpy(g_dir.items[g_dir.item_count].uri, uris[i], 128);
		anx_strlcpy(g_dir.items[g_dir.item_count].category,
			     uri_category(uris[i]), 32);
		g_dir.item_count++;
	}
}

static void dir_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg     = theme->palette.background;
	uint32_t surf   = theme->palette.surface;
	uint32_t accent = theme->palette.accent;
	uint32_t text   = theme->palette.text_primary;
	uint32_t dim    = theme->palette.text_dim;
	uint32_t y;
	uint32_t i;
	const char *cur_cat = NULL;

	if (!g_dir.surf || !g_dir.pixels) return;

#define D_FN(x,y2,w,h,c) px_fill(g_dir.pixels,DIR_W,(x),(y2),(w),(h),(c),DIR_W,DIR_H)
#define D_STR(x,y2,s,fg,bg2) px_str(g_dir.pixels,DIR_W,(x),(y2),(s),(fg),(bg2),DIR_W,DIR_H)

	D_FN(0, 0, DIR_W, DIR_H, bg);

	/* Title bar */
	D_FN(0, 0, DIR_W, DIR_TITLE_H, surf);
	D_FN(0, DIR_TITLE_H - 1, DIR_W, 1, accent);
	D_STR(10, (DIR_TITLE_H - FONT_H) / 2,
	      "Workflow Directory  (Up/Dn: select  Enter: launch  Esc: close)",
	      accent, surf);

	y = DIR_TITLE_H + 4;

	for (i = g_dir.scroll;
	     i < g_dir.item_count && y + DIR_ROW_H <= DIR_H - FONT_H - 6;
	     i++) {
		const char *cat = g_dir.items[i].category;
		bool sel = ((int32_t)i == g_dir.selected);

		/* Category header */
		if (!cur_cat || anx_strcmp(cat, cur_cat) != 0) {
			if (cur_cat) y += 4;
			D_FN(0, y, DIR_W, DIR_CAT_H, surf);
			D_FN(0, y + DIR_CAT_H - 1, DIR_W, 1, accent);
			D_STR(8, y + (DIR_CAT_H - FONT_H) / 2,
			      cat, accent, surf);
			y += DIR_CAT_H;
			cur_cat = cat;
		}

		/* Item row */
		{
			uint32_t row_bg = sel ? 0x00103040 : bg;
			if (sel) D_FN(0, y, DIR_W, DIR_ROW_H, row_bg);
			D_STR(sel ? 18 : 22, y + 4,
			      g_dir.items[i].display_name,
			      sel ? text : dim, row_bg);
			if (sel)
				D_STR(8, y + 4, ">", accent, row_bg);
		}
		y += DIR_ROW_H;
	}

	/* Footer */
	{
		uint32_t fy = DIR_H - FONT_H - 4;
		D_FN(0, fy - 2, DIR_W, 1, dim);
		D_STR(8, fy, "PgUp/PgDn: scroll", dim, bg);
	}

#undef D_FN
#undef D_STR

	anx_iface_surface_commit(g_dir.surf);
}

static void dir_execute_selected(void)
{
	uint32_t idx = (uint32_t)g_dir.selected;

	if (g_dir.selected < 0 || idx >= g_dir.item_count) return;

	anx_wm_window_close(g_dir.surf);
	g_dir.surf   = NULL;
	g_dir.pixels = NULL;

	/* Workflow URI: instantiate and run */
	if (g_dir.items[idx].uri[0]) {
		anx_oid_t wf_oid;

		if (anx_wf_lib_instantiate(g_dir.items[idx].uri,
					   g_dir.items[idx].display_name,
					   &wf_oid) == ANX_OK)
			anx_wf_run(&wf_oid, NULL);
		return;
	}

	/* Built-in command: type into terminal */
	{
		char     cmd[68];
		uint32_t i;

		anx_strlcpy(cmd, g_dir.items[idx].display_name, sizeof(cmd));
		anx_wm_terminal_open();
		for (i = 0; cmd[i]; i++)
			anx_wm_terminal_key_event(ANX_KEY_NONE, 0,
						  (uint32_t)(uint8_t)cmd[i]);
		anx_wm_terminal_key_event(ANX_KEY_ENTER, 0, '\n');
	}
}

static void dir_open(void)
{
	struct anx_content_node *cn;
	const struct anx_fb_info *fb;
	uint32_t buf_size;

	if (g_dir.surf) {
		anx_wm_window_focus(g_dir.surf);
		return;
	}

	buf_size    = DIR_W * DIR_H * 4;
	g_dir.pixels = anx_alloc(buf_size);
	if (!g_dir.pixels) return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) { anx_free(g_dir.pixels); g_dir.pixels = NULL; return; }

	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_dir.pixels;
	cn->data_len = buf_size;

	fb = anx_fb_get_info();
	{
		int32_t px = fb && fb->available ? (int32_t)(fb->width  - DIR_W) / 2 : 80;
		int32_t py = fb && fb->available ? (int32_t)(fb->height - DIR_H) / 2 : 60;

		if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
					     px, py, DIR_W, DIR_H,
					     &g_dir.surf) != ANX_OK) {
			anx_free(cn); anx_free(g_dir.pixels);
			g_dir.pixels = NULL; return;
		}
	}

	dir_populate();
	g_dir.selected = 0;
	g_dir.scroll   = 0;

	anx_iface_surface_map(g_dir.surf);
	anx_wm_window_open(g_dir.surf);
	dir_render();
}

/* ------------------------------------------------------------------ */
/* Public key routing                                                  */
/* ------------------------------------------------------------------ */

void anx_wm_search_key_event(uint32_t key, uint32_t mods, uint32_t unicode)
{
	(void)mods;

	/* --- Directory panel (takes priority when open) --- */
	if (g_dir.surf) {
		switch (key) {
		case ANX_KEY_ESC:
			anx_wm_window_close(g_dir.surf);
			g_dir.surf   = NULL;
			g_dir.pixels = NULL;
			return;
		case ANX_KEY_ENTER:
			dir_execute_selected();
			return;
		case ANX_KEY_UP:
			if (g_dir.selected > 0) {
				g_dir.selected--;
				if ((uint32_t)g_dir.selected < g_dir.scroll)
					g_dir.scroll = (uint32_t)g_dir.selected;
			}
			break;
		case ANX_KEY_DOWN:
			if ((uint32_t)g_dir.selected + 1 < g_dir.item_count) {
				g_dir.selected++;
			}
			break;
		case ANX_KEY_PAGEUP:
			if (g_dir.scroll >= 8) g_dir.scroll -= 8;
			else g_dir.scroll = 0;
			if ((uint32_t)g_dir.selected < g_dir.scroll)
				g_dir.selected = (int32_t)g_dir.scroll;
			break;
		case ANX_KEY_PAGEDOWN:
			g_dir.scroll = (g_dir.scroll + 8 < g_dir.item_count)
				       ? g_dir.scroll + 8
				       : g_dir.item_count > 8
					 ? g_dir.item_count - 8 : 0;
			break;
		default:
			return;
		}
		dir_render();
		return;
	}

	/* --- Search overlay --- */
	switch (key) {
	case ANX_KEY_ESC:
		anx_wm_window_close(g_cs.surf);
		g_cs.surf   = NULL;
		g_cs.pixels = NULL;
		return;

	case ANX_KEY_ENTER:
		if (g_cs.query[0] == '\0') {
			/* Empty query → open full directory */
			anx_wm_window_close(g_cs.surf);
			g_cs.surf   = NULL;
			g_cs.pixels = NULL;
			dir_open();
			return;
		}
		cs_execute_selected();
		return;

	case ANX_KEY_UP:
		if (g_cs.selected > 0) g_cs.selected--;
		break;

	case ANX_KEY_DOWN:
		if (g_cs.selected + 1 < (int32_t)g_cs.result_count)
			g_cs.selected++;
		break;

	case ANX_KEY_BACKSPACE:
		if (g_cs.query_len > 0) {
			g_cs.query[--g_cs.query_len] = '\0';
			cs_populate();
		}
		break;

	default:
		if (unicode >= 0x20 && unicode < 0x7F && g_cs.query_len < 63) {
			g_cs.query[g_cs.query_len++] = (char)unicode;
			g_cs.query[g_cs.query_len]   = '\0';
			cs_populate();
		}
		break;
	}

	cs_render();
}

struct anx_surface *anx_wm_search_surface(void)
{
	return g_cs.surf ? g_cs.surf : g_dir.surf;
}

bool anx_wm_search_active(void)
{
	return g_cs.surf != NULL || g_dir.surf != NULL;
}

/* ------------------------------------------------------------------ */
/* Launch (Meta+Space)                                                 */
/* ------------------------------------------------------------------ */

void anx_wm_launch_command_search(void)
{
	struct anx_content_node *cn;
	const struct anx_fb_info *fb;
	uint32_t buf_size;

	if (g_cs.surf) {
		anx_wm_window_focus(g_cs.surf);
		return;
	}

	buf_size    = CS_W * CS_H * 4;
	g_cs.pixels = anx_alloc(buf_size);
	if (!g_cs.pixels) return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) { anx_free(g_cs.pixels); g_cs.pixels = NULL; return; }

	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_cs.pixels;
	cn->data_len = buf_size;

	fb = anx_fb_get_info();
	g_cs.pos_x = fb && fb->available ? (fb->width  - CS_W) / 2 : 100;
	g_cs.pos_y = fb && fb->available ? (fb->height - CS_H) / 3 : 100;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     (int32_t)g_cs.pos_x, (int32_t)g_cs.pos_y,
				     CS_W, CS_H, &g_cs.surf) != ANX_OK) {
		anx_free(cn); anx_free(g_cs.pixels);
		g_cs.pixels = NULL; return;
	}

	g_cs.query[0]  = '\0';
	g_cs.query_len = 0;
	g_cs.selected  = 0;

	cs_populate();

	anx_iface_surface_set_title(g_cs.surf, "Search");
	anx_iface_surface_map(g_cs.surf);
	anx_wm_window_open(g_cs.surf);
	cs_render();
	kprintf("[search] opened (agent-driven)\n");
}
