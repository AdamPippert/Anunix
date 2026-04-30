/*
 * wm_terminal.c — Terminal surface (shell-in-a-window).
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
#include <anx/shell.h>
#include <anx/clipboard.h>
#include <anx/namespace.h>
#include <anx/state_object.h>

#define WM_CLIPBOARD_CID	((anx_cid_t){.hi = 0, .lo = 0xFFFF0001u})

/* ------------------------------------------------------------------ */
/* Layout constants                                                    */
/* ------------------------------------------------------------------ */

#define FONT_W		ANX_FONT_WIDTH
#define FONT_H		ANX_FONT_HEIGHT
#define LINE_H		(FONT_H + 4)

#define INPUT_H		28

#define HIST_LINES	200
#define HIST_COLS	120

#define CAPTURE_SZ	(HIST_COLS * 80)

#define CMD_HIST_COUNT	50

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct {
	struct anx_surface *surf;
	uint32_t           *pixels;

	/* Display geometry — computed from framebuffer at open time */
	uint32_t w, h;		/* terminal pixel dimensions */
	uint32_t x, y;		/* surface position on screen */
	uint32_t input_y;	/* y offset of input bar within terminal */
	uint32_t vis_lines;	/* number of history lines visible */

	/* Dirty flag: set when pixels are updated, cleared after commit */
	bool dirty;

	char     hist[HIST_LINES][HIST_COLS];
	uint32_t hist_count;
	int32_t  scroll_off;

	char     input[HIST_COLS];
	uint32_t input_len;

	char     cmd_hist[CMD_HIST_COUNT][HIST_COLS];
	uint32_t cmd_hist_count;
	int32_t  cmd_hist_pos;		/* -1 = live input */
	char     input_saved[HIST_COLS];
} g_term;

/* ------------------------------------------------------------------ */
/* Text editor state                                                   */
/* ------------------------------------------------------------------ */

#define EDIT_MAX_LINES	200
#define EDIT_STATUS_H	LINE_H		/* status bar height at top */

static struct {
	bool     active;
	char     ns[32];
	char     path[96];
	char     lines[EDIT_MAX_LINES][HIST_COLS];
	uint32_t line_count;
	uint32_t cur_line;
	uint32_t cur_col;
	uint32_t scroll;		/* first visible line */
	bool     modified;
} g_editor;

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                       */
/* ------------------------------------------------------------------ */

static void term_fill(uint32_t x, uint32_t y,
		      uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row, col;

	for (row = y; row < y + h && row < g_term.h; row++)
		for (col = x; col < x + w && col < g_term.w; col++)
			g_term.pixels[row * g_term.w + col] = color;
}

static void term_draw_str(uint32_t x, uint32_t y, const char *s,
			   uint32_t fg, uint32_t bg)
{
	anx_font_blit_str(g_term.pixels, g_term.w, g_term.h, x, y, s, fg, bg);
}

/* ------------------------------------------------------------------ */
/* History helpers                                                     */
/* ------------------------------------------------------------------ */

static void hist_append(const char *s, uint32_t len)
{
	uint32_t slot = g_term.hist_count % HIST_LINES;
	uint32_t copy = len < HIST_COLS - 1 ? len : HIST_COLS - 1;

	anx_memcpy(g_term.hist[slot], s, copy);
	g_term.hist[slot][copy] = '\0';
	g_term.hist_count++;
}

static void hist_append_str(const char *s)
{
	/* Visible columns (subtract 4px left margin) */
	uint32_t max_cols = g_term.w > (FONT_W + 4)
		? (g_term.w - 4) / FONT_W : HIST_COLS - 1;
	if (max_cols < 1) max_cols = 1;
	if (max_cols > HIST_COLS - 1) max_cols = HIST_COLS - 1;

	while (*s) {
		const char *line_start = s;
		uint32_t col = 0;

		/* Scan to next newline or visible-column boundary */
		while (*s && *s != '\n') {
			if (col >= max_cols) {
				/* Wrap: try to break at last space */
				const char *wrap = s;
				uint32_t wc = col;
				while (wrap > line_start + 1 && wrap[-1] != ' ')
					wrap--, wc--;
				if (wrap > line_start + max_cols / 2) {
					/* Clean word-break found */
					hist_append(line_start, (uint32_t)(wrap - line_start));
					s = wrap;
				} else {
					/* No good break point; hard wrap */
					hist_append(line_start, col);
					s = line_start + col;
				}
				line_start = s;
				col = 0;
				continue;
			}
			s++;
			col++;
		}
		hist_append(line_start, (uint32_t)(s - line_start));
		if (*s == '\n')
			s++;
	}
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void term_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg     = 0x00050A10;
	uint32_t fg     = theme->palette.text_primary;
	uint32_t accent = theme->palette.accent;
	uint32_t i;
	uint32_t count, scroll, vis, first_line;

	if (!g_term.surf || !g_term.pixels)
		return;

	/* Background */
	term_fill(0, 0, g_term.w, g_term.h, bg);

	/* Separator line */
	term_fill(0, g_term.input_y - 1, g_term.w, 1, accent);

	/* Output history — bottom-anchored */
	count       = g_term.hist_count;
	scroll      = (uint32_t)g_term.scroll_off;
	vis         = g_term.vis_lines;
	first_line  = (count > vis + scroll) ? count - vis - scroll : 0;

	for (i = 0; i < vis; i++) {
		uint32_t idx  = first_line + i;
		uint32_t slot = idx % HIST_LINES;
		uint32_t row_y = 8 + i * LINE_H;

		if (idx >= count)
			break;
		term_draw_str(4, row_y, g_term.hist[slot], fg, bg);
	}

	/* Input area */
	term_fill(0, g_term.input_y, g_term.w, INPUT_H, 0x00111111);

	{
		char prompt[HIST_COLS + 4];
		uint32_t pl;

		anx_snprintf(prompt, sizeof(prompt), "> %s", g_term.input);
		pl = (uint32_t)anx_strlen(prompt);

		term_draw_str(4, g_term.input_y + 4, prompt, fg, 0x00111111);

		/* Teal cursor block */
		term_fill(4 + pl * FONT_W, g_term.input_y + 4, 2, FONT_H, accent);
	}

	/* Mark dirty — actual commit deferred to main WM loop via
	 * anx_wm_terminal_flush_if_dirty() to avoid slow PCI MMIO
	 * writes in IRQ handler context. */
	g_term.dirty = true;
}

/* ------------------------------------------------------------------ */
/* Command execution                                                   */
/* ------------------------------------------------------------------ */

static void term_run_command(const char *cmd)
{
	char *capture;
	uint32_t captured;
	char echo[HIST_COLS + 4];

	anx_snprintf(echo, sizeof(echo), "> %s", cmd);
	hist_append_str(echo);

	capture = anx_alloc(CAPTURE_SZ);
	if (!capture) {
		hist_append_str("[out of memory]");
		return;
	}

	anx_kprintf_capture_start(capture, CAPTURE_SZ);
	anx_shell_execute(cmd);
	captured = anx_kprintf_capture_stop();

	if (captured > 0) {
		if (captured < CAPTURE_SZ)
			capture[captured] = '\0';
		else
			capture[CAPTURE_SZ - 1] = '\0';
		hist_append_str(capture);
	}

	anx_free(capture);
	g_term.scroll_off = 0;
}

/* ------------------------------------------------------------------ */
/* Text editor render and key handling                                 */
/* ------------------------------------------------------------------ */

static void editor_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg      = 0x00060C12u;	/* slightly darker than terminal */
	uint32_t fg      = theme->palette.text_primary;
	uint32_t cur_bg  = theme->palette.accent;
	uint32_t stat_bg = theme->palette.surface;
	uint32_t dim     = theme->palette.text_dim;
	uint32_t vis;
	uint32_t i;
	char     status[HIST_COLS];

	if (!g_term.pixels || !g_editor.active)
		return;

	/* Background */
	term_fill(0, 0, g_term.w, g_term.h, bg);

	/* Status bar at top */
	term_fill(0, 0, g_term.w, EDIT_STATUS_H, stat_bg);
	anx_snprintf(status, sizeof(status), " %s:%s%s  Ctrl+S save  Ctrl+Q quit",
		     g_editor.ns, g_editor.path,
		     g_editor.modified ? " [+]" : "");
	term_draw_str(4, 4, status, dim, stat_bg);

	/* Visible line count */
	vis = (g_term.h > EDIT_STATUS_H + 8)
	      ? (g_term.h - EDIT_STATUS_H - 8) / LINE_H : 1;

	/* File content */
	for (i = 0; i < vis; i++) {
		uint32_t lnum = g_editor.scroll + i;
		uint32_t y    = EDIT_STATUS_H + i * LINE_H;

		if (lnum >= g_editor.line_count) {
			term_draw_str(4, y + 4, "~", dim, bg);
			continue;
		}

		if (lnum == g_editor.cur_line) {
			/* Highlight current line bg */
			term_fill(0, y, g_term.w, LINE_H, 0x00101820u);

			/* Draw text, then cursor block */
			term_draw_str(4, y + 4, g_editor.lines[lnum], fg,
				      0x00101820u);

			/* Cursor block at current column */
			{
				uint32_t cx = 4 + g_editor.cur_col * FONT_W;

				if (cx + FONT_W <= g_term.w) {
					char cur_char[2] = {
						g_editor.lines[lnum][g_editor.cur_col]
						? g_editor.lines[lnum][g_editor.cur_col]
						: ' ',
						'\0'
					};
					term_fill(cx, y + 4, FONT_W, FONT_H, cur_bg);
					term_draw_str(cx, y + 4, cur_char,
						      0x00000000u, cur_bg);
				}
			}
		} else {
			term_draw_str(4, y + 4, g_editor.lines[lnum], fg, bg);
		}
	}

	g_term.dirty = true;
}

static void editor_save(void)
{
	static char buf[EDIT_MAX_LINES * HIST_COLS];
	uint32_t pos = 0;
	uint32_t i;
	anx_oid_t oid;

	for (i = 0; i < g_editor.line_count; i++) {
		uint32_t len = (uint32_t)anx_strlen(g_editor.lines[i]);

		if (pos + len + 1 >= sizeof(buf))
			break;
		anx_memcpy(buf + pos, g_editor.lines[i], len);
		pos += len;
		buf[pos++] = '\n';
	}

	if (anx_ns_resolve(g_editor.ns, g_editor.path, &oid) == ANX_OK) {
		/* Object exists — open and replace payload */
		struct anx_object_handle h;

		if (anx_so_open(&oid, ANX_OPEN_WRITE, &h) == ANX_OK) {
			anx_so_replace_payload(&h, buf, pos);
			anx_so_close(&h);
		}
	} else {
		/* New object */
		struct anx_so_create_params p;
		struct anx_state_object *obj;

		anx_memset(&p, 0, sizeof(p));
		p.object_type  = ANX_OBJ_BYTE_DATA;
		p.payload      = buf;
		p.payload_size = pos;

		if (anx_so_create(&p, &obj) == ANX_OK) {
			anx_ns_bind(g_editor.ns, g_editor.path, &obj->oid);
			anx_objstore_release(obj);
		}
	}

	g_editor.modified = false;
	hist_append_str("[editor] saved");
}

static void editor_key_event(uint32_t key, uint32_t mods, uint32_t unicode)
{
	char *line = g_editor.lines[g_editor.cur_line];
	uint32_t len = (uint32_t)anx_strlen(line);
	uint32_t vis;

	if (mods & ANX_MOD_CTRL) {
		if (key == ANX_KEY_S) {
			editor_save();
			editor_render();
			return;
		}
		if (key == ANX_KEY_Q) {
			g_editor.active = false;
			term_render();
			if (g_editor.modified)
				hist_append_str("[editor] quit without saving");
			return;
		}
	}

	vis = (g_term.h > EDIT_STATUS_H + 8)
	      ? (g_term.h - EDIT_STATUS_H - 8) / LINE_H : 1;

	switch (key) {
	case ANX_KEY_LEFT:
		if (g_editor.cur_col > 0)
			g_editor.cur_col--;
		else if (g_editor.cur_line > 0) {
			g_editor.cur_line--;
			g_editor.cur_col =
				(uint32_t)anx_strlen(
					g_editor.lines[g_editor.cur_line]);
		}
		break;

	case ANX_KEY_RIGHT:
		if (g_editor.cur_col < len)
			g_editor.cur_col++;
		else if (g_editor.cur_line + 1 < g_editor.line_count) {
			g_editor.cur_line++;
			g_editor.cur_col = 0;
		}
		break;

	case ANX_KEY_UP:
		if (g_editor.cur_line > 0) {
			g_editor.cur_line--;
			{
				uint32_t nlen = (uint32_t)anx_strlen(
					g_editor.lines[g_editor.cur_line]);
				if (g_editor.cur_col > nlen)
					g_editor.cur_col = nlen;
			}
		}
		break;

	case ANX_KEY_DOWN:
		if (g_editor.cur_line + 1 < g_editor.line_count) {
			g_editor.cur_line++;
			{
				uint32_t nlen = (uint32_t)anx_strlen(
					g_editor.lines[g_editor.cur_line]);
				if (g_editor.cur_col > nlen)
					g_editor.cur_col = nlen;
			}
		}
		break;

	case ANX_KEY_HOME:
		g_editor.cur_col = 0;
		break;

	case ANX_KEY_END:
		g_editor.cur_col = len;
		break;

	case ANX_KEY_PAGEUP:
		if (g_editor.cur_line >= vis)
			g_editor.cur_line -= vis;
		else
			g_editor.cur_line = 0;
		{
			uint32_t nlen = (uint32_t)anx_strlen(
				g_editor.lines[g_editor.cur_line]);
			if (g_editor.cur_col > nlen)
				g_editor.cur_col = nlen;
		}
		break;

	case ANX_KEY_PAGEDOWN:
		g_editor.cur_line += vis;
		if (g_editor.cur_line >= g_editor.line_count)
			g_editor.cur_line = g_editor.line_count > 0
					    ? g_editor.line_count - 1 : 0;
		{
			uint32_t nlen = (uint32_t)anx_strlen(
				g_editor.lines[g_editor.cur_line]);
			if (g_editor.cur_col > nlen)
				g_editor.cur_col = nlen;
		}
		break;

	case ANX_KEY_ENTER:
		if (g_editor.line_count >= EDIT_MAX_LINES)
			break;
		/* Split line at cursor */
		{
			uint32_t rest_len = len - g_editor.cur_col;
			uint32_t i;

			/* Shift lines down */
			for (i = g_editor.line_count;
			     i > g_editor.cur_line + 1; i--)
				anx_memcpy(g_editor.lines[i],
					   g_editor.lines[i - 1],
					   HIST_COLS);

			/* New line gets the rest */
			anx_memcpy(g_editor.lines[g_editor.cur_line + 1],
				   line + g_editor.cur_col, rest_len);
			g_editor.lines[g_editor.cur_line + 1][rest_len] = '\0';

			/* Truncate current line */
			line[g_editor.cur_col] = '\0';

			g_editor.line_count++;
			g_editor.cur_line++;
			g_editor.cur_col = 0;
			g_editor.modified = true;
		}
		break;

	case ANX_KEY_BACKSPACE:
		if (g_editor.cur_col > 0) {
			/* Delete char before cursor */
			anx_memmove(line + g_editor.cur_col - 1,
				    line + g_editor.cur_col,
				    len - g_editor.cur_col + 1);
			g_editor.cur_col--;
			g_editor.modified = true;
		} else if (g_editor.cur_line > 0) {
			/* Merge with previous line */
			uint32_t prev = g_editor.cur_line - 1;
			uint32_t prev_len = (uint32_t)anx_strlen(
					g_editor.lines[prev]);
			uint32_t i;

			if (prev_len + len < HIST_COLS) {
				anx_memcpy(g_editor.lines[prev] + prev_len,
					   line, len + 1);
				/* Shift lines up */
				for (i = g_editor.cur_line;
				     i + 1 < g_editor.line_count; i++)
					anx_memcpy(g_editor.lines[i],
						   g_editor.lines[i + 1],
						   HIST_COLS);
				g_editor.lines[g_editor.line_count - 1][0]
					= '\0';
				g_editor.line_count--;
				g_editor.cur_line = prev;
				g_editor.cur_col  = prev_len;
				g_editor.modified = true;
			}
		}
		break;

	case ANX_KEY_DELETE:
		if (g_editor.cur_col < len) {
			anx_memmove(line + g_editor.cur_col,
				    line + g_editor.cur_col + 1,
				    len - g_editor.cur_col);
			g_editor.modified = true;
		}
		break;

	default:
		if (unicode >= 0x20 && unicode < 0x7F &&
		    len < (uint32_t)(HIST_COLS - 1)) {
			anx_memmove(line + g_editor.cur_col + 1,
				    line + g_editor.cur_col,
				    len - g_editor.cur_col + 1);
			line[g_editor.cur_col] = (char)unicode;
			g_editor.cur_col++;
			g_editor.modified = true;
		}
		break;
	}

	/* Keep scroll window around cursor */
	if (g_editor.cur_line < g_editor.scroll)
		g_editor.scroll = g_editor.cur_line;
	else if (g_editor.cur_line >= g_editor.scroll + vis)
		g_editor.scroll = g_editor.cur_line - vis + 1;

	editor_render();
}

/* Public: open a state object for editing in the terminal */
void anx_wm_terminal_edit(const char *ns_name, const char *path)
{
	anx_oid_t oid;
	uint32_t  i;

	anx_memset(&g_editor, 0, sizeof(g_editor));
	anx_strlcpy(g_editor.ns,   ns_name, sizeof(g_editor.ns));
	anx_strlcpy(g_editor.path, path,    sizeof(g_editor.path));

	/* Load existing content if object exists */
	if (anx_ns_resolve(ns_name, path, &oid) == ANX_OK) {
		struct anx_state_object *obj = anx_objstore_lookup(&oid);

		if (obj && obj->payload && obj->payload_size > 0) {
			const char *src = (const char *)obj->payload;
			uint32_t src_len = (uint32_t)obj->payload_size;
			uint32_t col = 0;

			i = 0;
			while (i < src_len && g_editor.line_count < EDIT_MAX_LINES) {
				char c = src[i++];

				if (c == '\n' || col >= HIST_COLS - 1) {
					g_editor.lines[g_editor.line_count][col] = '\0';
					g_editor.line_count++;
					col = 0;
					if (c != '\n' && col < HIST_COLS - 1)
						g_editor.lines[g_editor.line_count][col++] = c;
				} else {
					g_editor.lines[g_editor.line_count][col++] = c;
				}
			}
			if (col > 0 &&
			    g_editor.line_count < EDIT_MAX_LINES) {
				g_editor.lines[g_editor.line_count][col] = '\0';
				g_editor.line_count++;
			}
			anx_objstore_release(obj);
		}
	}

	if (g_editor.line_count == 0) {
		g_editor.lines[0][0] = '\0';
		g_editor.line_count  = 1;
	}

	g_editor.active   = true;
	g_editor.cur_line = 0;
	g_editor.cur_col  = 0;
	g_editor.scroll   = 0;
	g_editor.modified = false;

	anx_wm_terminal_open();
	editor_render();
}

/* ------------------------------------------------------------------ */
/* Key event routing (called by wm.c key interceptor)                 */
/* ------------------------------------------------------------------ */

void anx_wm_terminal_key_event(uint32_t key, uint32_t mods, uint32_t unicode)
{
	/* Route to text editor when active */
	if (g_editor.active) {
		editor_key_event(key, mods, unicode);
		return;
	}

	(void)mods;

	switch (key) {
	case ANX_KEY_ESC:
		anx_wm_window_close(g_term.surf);
		g_term.surf = NULL;
		return;

	case ANX_KEY_ENTER:
		if (g_term.input_len > 0) {
			char cmd[HIST_COLS];

			anx_strlcpy(cmd, g_term.input, sizeof(cmd));
			g_term.input[0]  = '\0';
			g_term.input_len = 0;
			/* Push to command history */
			{
				uint32_t slot = g_term.cmd_hist_count % CMD_HIST_COUNT;

				anx_strlcpy(g_term.cmd_hist[slot], cmd, HIST_COLS);
				g_term.cmd_hist_count++;
			}
			g_term.cmd_hist_pos = -1;
			term_run_command(cmd);
		} else {
			hist_append_str("> ");
		}
		break;

	case ANX_KEY_BACKSPACE:
		if (g_term.input_len > 0) {
			g_term.input_len--;
			g_term.input[g_term.input_len] = '\0';
		}
		g_term.cmd_hist_pos = -1;
		break;

	case ANX_KEY_UP:
		{
			uint32_t hist_len = g_term.cmd_hist_count > CMD_HIST_COUNT
					    ? CMD_HIST_COUNT : g_term.cmd_hist_count;
			if (hist_len == 0) break;
			if (g_term.cmd_hist_pos == -1) {
				anx_strlcpy(g_term.input_saved, g_term.input,
					    sizeof(g_term.input_saved));
				g_term.cmd_hist_pos = 0;
			} else if ((uint32_t)(g_term.cmd_hist_pos + 1) < hist_len) {
				g_term.cmd_hist_pos++;
			} else {
				break;
			}
			{
				uint32_t idx = (g_term.cmd_hist_count - 1
						- (uint32_t)g_term.cmd_hist_pos)
					       % CMD_HIST_COUNT;
				anx_strlcpy(g_term.input, g_term.cmd_hist[idx],
					    sizeof(g_term.input));
				g_term.input_len = (uint32_t)anx_strlen(g_term.input);
			}
		}
		break;

	case ANX_KEY_DOWN:
		if (g_term.cmd_hist_pos < 0) break;
		g_term.cmd_hist_pos--;
		if (g_term.cmd_hist_pos < 0) {
			anx_strlcpy(g_term.input, g_term.input_saved,
				    sizeof(g_term.input));
			g_term.input_len = (uint32_t)anx_strlen(g_term.input);
		} else {
			uint32_t idx = (g_term.cmd_hist_count - 1
					- (uint32_t)g_term.cmd_hist_pos)
				       % CMD_HIST_COUNT;
			anx_strlcpy(g_term.input, g_term.cmd_hist[idx],
				    sizeof(g_term.input));
			g_term.input_len = (uint32_t)anx_strlen(g_term.input);
		}
		break;

	case ANX_KEY_TAB:
		{
			/* Complete the first word (command name) of current input */
			static const char *const cmds[] = {
				"agent", "ask", "api", "browser", "browser_init", "browser_stop",
				"cap", "cat", "cell", "cells", "clear", "compctl",
				"cp", "disk", "dns", "echo", "edit", "engine",
				"envctl", "evctl", "fetch", "fb_info", "fb_test",
				"gop_list", "grep", "halt", "head", "help",
				"history", "hwd", "hw-inventory", "http-get",
				"if", "inspect", "install", "kickstart",
				"login", "logout", "loop", "ls", "mem",
				"memplane", "meta", "mode", "model", "model-init",
				"mv", "net", "netinfo", "ntp", "pci", "perf",
				"ping", "reboot", "rlm", "rm", "sched", "search",
				"secret", "sort", "ssh-addkey", "state", "store",
				"surfctl", "sysinfo", "tail", "tensor", "theme",
				"tz", "useradd", "version", "vm", "wc", "wifi",
				"workflow", "write", "xdna", NULL
			};
			uint32_t cursor = g_term.input_len;
			uint32_t i;
			const char *best = NULL;
			uint32_t match_count = 0;
			uint32_t best_len = 0;

			/* Only complete the first token (no space yet) */
			for (i = 0; i < cursor; i++) {
				if (g_term.input[i] == ' ')
					goto tab_done;
			}

			for (i = 0; cmds[i]; i++) {
				if (anx_strncmp(cmds[i], g_term.input, cursor) == 0) {
					if (match_count == 0) {
						best = cmds[i];
						best_len = (uint32_t)anx_strlen(cmds[i]);
					} else {
						uint32_t j = cursor;
						while (j < best_len && cmds[i][j] == best[j])
							j++;
						best_len = j;
					}
					match_count++;
				}
			}

			if (match_count == 1) {
				uint32_t clen = (uint32_t)anx_strlen(best);
				if (clen + 1 < HIST_COLS - 1) {
					anx_strlcpy(g_term.input, best, HIST_COLS);
					g_term.input[clen]     = ' ';
					g_term.input[clen + 1] = '\0';
					g_term.input_len = clen + 1;
				}
			} else if (match_count > 1 && best_len > cursor) {
				anx_memcpy(g_term.input, best, best_len);
				g_term.input[best_len] = '\0';
				g_term.input_len = best_len;
			} else if (match_count > 1) {
				char line[HIST_COLS];
				uint32_t pos = 0;

				for (i = 0; cmds[i]; i++) {
					if (anx_strncmp(cmds[i], g_term.input,
							cursor) == 0) {
						uint32_t clen =
							(uint32_t)anx_strlen(cmds[i]);
						if (pos + clen + 2 < HIST_COLS) {
							anx_memcpy(line + pos,
								   cmds[i], clen);
							pos += clen;
							line[pos++] = ' ';
						}
					}
				}
				line[pos] = '\0';
				hist_append_str(line);
			}
			tab_done:;
		}
		break;

	case ANX_KEY_PAGEUP:
		{
			uint32_t vl = g_term.vis_lines;
			uint32_t max_scroll = g_term.hist_count > vl
					      ? g_term.hist_count - vl : 0;
			uint32_t step = vl / 2;

			if ((uint32_t)g_term.scroll_off + step <= max_scroll)
				g_term.scroll_off += (int32_t)step;
			else
				g_term.scroll_off = (int32_t)max_scroll;
		}
		break;

	case ANX_KEY_PAGEDOWN:
		g_term.scroll_off -= (int32_t)(g_term.vis_lines / 2);
		if (g_term.scroll_off < 0)
			g_term.scroll_off = 0;
		break;

	default:
		if (unicode >= 0x20 && unicode < 0x7F &&
		    g_term.input_len < HIST_COLS - 1) {
			g_term.cmd_hist_pos = -1;
			g_term.input[g_term.input_len++] = (char)unicode;
			g_term.input[g_term.input_len]   = '\0';
		}
		break;
	}

	term_render();
}

/* ------------------------------------------------------------------ */
/* Launch / focus                                                      */
/* ------------------------------------------------------------------ */

void anx_wm_terminal_open(void)
{
	struct anx_content_node *cn;
	uint32_t buf_size;
	const struct anx_fb_info *fb;

	if (g_term.surf) {
		anx_wm_window_focus(g_term.surf);
		return;
	}

	/* Compute dimensions from live framebuffer; fall back to 640×480. */
	fb = anx_fb_get_info();
	if (fb && fb->available && fb->width > 0 && fb->height > 0) {
		uint32_t avail_h = fb->height;

		if (avail_h > ANX_WM_MENUBAR_H + ANX_WM_TASKBAR_H + 32)
			avail_h -= ANX_WM_MENUBAR_H + ANX_WM_TASKBAR_H;
		g_term.w = fb->width;
		g_term.h = avail_h;
		g_term.x = 0;
		g_term.y = ANX_WM_MENUBAR_H;
	} else {
		g_term.w = 640;
		g_term.h = 424;
		g_term.x = 0;
		g_term.y = ANX_WM_MENUBAR_H;
	}
	g_term.input_y   = g_term.h - INPUT_H;
	g_term.vis_lines = (g_term.input_y > 8) ? (g_term.input_y - 8) / LINE_H : 1;

	buf_size      = g_term.w * g_term.h * 4;
	g_term.pixels = anx_alloc(buf_size);
	if (!g_term.pixels)
		return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_term.pixels);
		g_term.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_term.pixels;
	cn->data_len = buf_size;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     g_term.x, g_term.y, g_term.w, g_term.h,
				     &g_term.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_term.pixels);
		g_term.pixels  = NULL;
		return;
	}

	g_term.input[0]     = '\0';
	g_term.input_len    = 0;
	g_term.scroll_off   = 0;
	g_term.cmd_hist_pos = -1;
	g_term.dirty        = false;

	if (g_term.hist_count == 0)
		hist_append_str("Anunix terminal  -  type 'help' for commands");

	anx_iface_surface_set_title(g_term.surf, "Terminal");
	/* Fill pixels BEFORE mapping so no compositor tick can commit
	 * uninitialized data.  term_render() sets dirty=true; the main
	 * WM loop's flush_if_dirty() will commit on the next iteration. */
	term_render();
	anx_iface_surface_map(g_term.surf);
	anx_wm_window_open(g_term.surf);
	kprintf("[terminal] opened\n");
}

void anx_wm_terminal_flush_if_dirty(void)
{
	if (g_term.dirty && g_term.surf) {
		g_term.dirty = false;
		anx_iface_surface_commit(g_term.surf);
	}
}

struct anx_surface *anx_wm_terminal_surface(void)
{
	return g_term.surf;
}

void anx_wm_terminal_paste(const char *text, uint32_t len)
{
	uint32_t i;

	if (!text || !g_term.surf)
		return;

	for (i = 0; i < len && g_term.input_len < HIST_COLS - 1; i++) {
		char c = text[i];

		if (c < 0x20 || c >= 0x7F)
			continue;
		g_term.input[g_term.input_len++] = c;
	}
	g_term.input[g_term.input_len] = '\0';
	term_render();
}

void anx_wm_terminal_print(const char *text)
{
	if (!text)
		return;
	if (!g_term.surf)
		anx_wm_terminal_open();
	hist_append_str(text);
	term_render();
}

void anx_wm_terminal_clear_input(void)
{
	if (!g_term.surf)
		return;
	g_term.input[0]  = '\0';
	g_term.input_len = 0;
	g_term.cmd_hist_pos = -1;
	term_render();
}

void anx_wm_terminal_cut_input(void)
{
	if (!g_term.surf || g_term.input_len == 0)
		return;
	/* Copy current input to clipboard then clear */
	anx_clipboard_grant(WM_CLIPBOARD_CID, ANX_CLIPBOARD_FLAG_WRITE);
	anx_clipboard_write(WM_CLIPBOARD_CID, "text/plain",
			    g_term.input, g_term.input_len);
	anx_wm_terminal_clear_input();
}
