/*
 * wm_agent.c — Boot-time AI agent surface.
 *
 * This is the primary user interface at boot.  The agent accepts natural
 * language input, calls the configured LLM, and acts on the response.
 * Everything else in the WM (terminal, search, apps) is reachable via
 * hotkey (Meta+Enter = terminal) or by asking the agent.
 *
 * Response protocol the model must follow (one line per turn):
 *   CMD: <shell-command>    — execute and feed output back to the model
 *   OPEN: <app>             — open a WM surface (terminal|search|workflow|viewer)
 *   DONE: <text>            — end the agentic loop, display <text>
 *   <anything else>         — conversational reply, displayed directly
 *
 * If no model is configured the agent shows a welcome screen and routes
 * input directly as shell commands (power-user fallback).
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
#include <anx/model_client.h>

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define FONT_W		ANX_FONT_WIDTH
#define FONT_H		ANX_FONT_HEIGHT
#define LINE_H		(FONT_H + 4)

#define MARGIN		12

#define HIST_LINES	300
#define HIST_COLS	140

/* Number of CMD: iterations before forcing DONE */
#define AGENT_MAX_TOOL_ITERS	6
#define CMD_CAP_SZ		2048
#define CONV_HIST_SZ		6144	/* conversation history passed to model */

/* ------------------------------------------------------------------ */
/* Line roles                                                          */
/* ------------------------------------------------------------------ */

#define ROLE_SYSTEM	0	/* dim status / system messages */
#define ROLE_USER	1	/* user input */
#define ROLE_AGENT	2	/* agent response */
#define ROLE_CMD	3	/* executed command (echoed) */
#define ROLE_OUTPUT	4	/* command output */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct {
	struct anx_surface      *surf;
	struct anx_content_node *cn;
	uint32_t                *pixels;

	uint32_t w, h;
	uint32_t vis_lines;

	bool dirty;

	/* Scrollback */
	char     hist[HIST_LINES][HIST_COLS];
	uint8_t  hist_role[HIST_LINES];
	uint32_t hist_count, hist_head;
	int32_t  scroll_off;

	/* Input */
	char     input[256];
	uint32_t input_len;
	uint32_t input_pos;
	struct anx_shell_history_cursor recall;

	/* Conversation context passed to the model */
	char    *conv;		/* heap-allocated, CONV_HIST_SZ bytes */
	uint32_t conv_len;
} g_agent;

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                       */
/* ------------------------------------------------------------------ */

static void agent_fill(uint32_t x, uint32_t y,
		       uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row, col;

	for (row = y; row < y + h && row < g_agent.h; row++)
		for (col = x; col < x + w && col < g_agent.w; col++)
			g_agent.pixels[row * g_agent.w + col] = color;
}

static void agent_draw_str(uint32_t x, uint32_t y,
			   const char *s, uint32_t fg, uint32_t bg)
{
	anx_font_blit_str(g_agent.pixels, g_agent.w, g_agent.h, x, y, s, fg, bg);
}

/* ------------------------------------------------------------------ */
/* History                                                             */
/* ------------------------------------------------------------------ */

static void hist_append_role(const char *s, uint32_t len, uint8_t role)
{
	uint32_t slot = (g_agent.hist_head + g_agent.hist_count) % HIST_LINES;

	anx_memcpy(g_agent.hist[slot], s, len);
	g_agent.hist[slot][len] = '\0';
	g_agent.hist_role[slot] = role;
	if (g_agent.hist_count < HIST_LINES)
		g_agent.hist_count++;
	else
		g_agent.hist_head = (g_agent.hist_head + 1) % HIST_LINES;
}

static void hist_append_str_role(const char *s, uint8_t role)
{
	while (*s) {
		uint32_t len = 0;

		while (s[len] && s[len] != '\n' && len < HIST_COLS - 1)
			len++;
		hist_append_role(s, len, role);
		s += len;
		if (*s == '\n') s++;
	}
}

/* Append to conversation buffer (clamped) */
static void conv_append(const char *s)
{
	if (!g_agent.conv)
		return;

	uint32_t rem  = CONV_HIST_SZ - 1 - g_agent.conv_len;
	uint32_t slen = (uint32_t)anx_strlen(s);

	if (slen > rem) {
		/* Drop oldest half to make room */
		uint32_t keep = CONV_HIST_SZ / 2;
		uint32_t drop = g_agent.conv_len - keep;

		anx_memmove(g_agent.conv, g_agent.conv + drop, keep);
		g_agent.conv_len = keep;
		g_agent.conv[keep] = '\0';
		rem = CONV_HIST_SZ - 1 - g_agent.conv_len;
		if (slen > rem) slen = rem;
	}

	anx_memcpy(g_agent.conv + g_agent.conv_len, s, slen);
	g_agent.conv_len += slen;
	g_agent.conv[g_agent.conv_len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static const char *agent_label(uint8_t role)
{
	switch (role) {
	case ROLE_USER: return anx_model_client_ready() ? "you> " : "> ";
	case ROLE_AGENT: return "anx> ";
	case ROLE_CMD: return "cmd> ";
	default: return "";
	}
}

static uint32_t agent_columns(void)
{
	return g_agent.w > MARGIN * 2 + FONT_W
		? (g_agent.w - MARGIN * 2) / FONT_W : 1;
}

static void agent_draw_rows(const char *text, uint32_t cols, uint32_t first,
			    uint32_t *row, uint32_t fg, uint32_t bg)
{
	uint32_t len = (uint32_t)anx_strlen(text), pos = 0;

	do {
		uint32_t n = len - pos;
		char line[264];

		if (n > cols) n = cols;
		if (*row >= first && *row - first < g_agent.vis_lines) {
			anx_memcpy(line, text + pos, n);
			line[n] = '\0';
			agent_draw_str(MARGIN, MARGIN + (*row - first) * LINE_H,
				       line, fg, bg);
		}
		(*row)++;
		pos += n;
	} while (pos < len);
}

static void agent_redraw(void)
{
	const struct anx_theme *t = anx_theme_get();
	uint32_t bg = t->palette.background;
	uint32_t cols, output = 0, i, row = 0, first, cursor_row, prompt_len;
	char text[264];
	const char *prompt = anx_model_client_ready() ? "you> " : "> ";

	if (!g_agent.surf || !g_agent.pixels)
		return;
	agent_fill(0, 0, g_agent.w, g_agent.h, bg);
	cols = agent_columns();
	for (i = 0; i < g_agent.hist_count; i++) {
		uint32_t slot = (g_agent.hist_head + i) % HIST_LINES;
		uint32_t len = (uint32_t)anx_strlen(g_agent.hist[slot]) +
			(uint32_t)anx_strlen(agent_label(g_agent.hist_role[slot]));

		output += len ? (len + cols - 1) / cols : 1;
	}
	prompt_len = (uint32_t)anx_strlen(prompt) + g_agent.input_pos;
	cursor_row = output + prompt_len / cols;
	first = cursor_row + 1 > g_agent.vis_lines
		? cursor_row + 1 - g_agent.vis_lines : 0;
	if ((uint32_t)g_agent.scroll_off > first)
		g_agent.scroll_off = (int32_t)first;
	first -= (uint32_t)g_agent.scroll_off;
	for (i = 0; i < g_agent.hist_count; i++) {
		uint32_t slot = (g_agent.hist_head + i) % HIST_LINES;
		uint8_t role = g_agent.hist_role[slot];
		uint32_t fg = role == ROLE_SYSTEM ? t->palette.text_dim :
			role == ROLE_USER ? t->palette.accent : t->palette.text_primary;

		anx_snprintf(text, sizeof(text), "%s%s",
			     agent_label(role), g_agent.hist[slot]);
		agent_draw_rows(text, cols, first, &row, fg, bg);
	}
	anx_snprintf(text, sizeof(text), "%s%s", prompt, g_agent.input);
	agent_draw_rows(text, cols, first, &row, t->palette.text_primary, bg);
	if (cursor_row >= first && cursor_row - first < g_agent.vis_lines)
		agent_fill(MARGIN + (prompt_len % cols) * FONT_W,
			   MARGIN + (cursor_row - first) * LINE_H,
			   2, FONT_H, t->palette.accent);
	anx_iface_surface_commit(g_agent.surf);
}

static void mark_dirty(void)
{
	g_agent.dirty = true;
}

/* ------------------------------------------------------------------ */
/* Command execution (captures output for model)                      */
/* ------------------------------------------------------------------ */

static void exec_cmd_capture(const char *cmd,
			      char *out, uint32_t out_sz)
{
	struct anx_capture_state saved;

	anx_kprintf_capture_save(&saved);
	anx_kprintf_capture_start(out, out_sz);
	anx_shell_execute(cmd);
	anx_kprintf_capture_stop();
	anx_kprintf_capture_restore(&saved);
}

/* ------------------------------------------------------------------ */
/* Open an app by name                                                 */
/* ------------------------------------------------------------------ */

static void open_app(const char *name)
{
	if (anx_strcmp(name, "terminal") == 0 ||
	    anx_strcmp(name, "shell") == 0) {
		anx_wm_launch_terminal();
	} else if (anx_strcmp(name, "search") == 0) {
		anx_wm_launch_command_search();
	} else if (anx_strcmp(name, "workflow") == 0) {
		anx_wm_launch_workflow_designer();
	} else if (anx_strcmp(name, "viewer") == 0) {
		anx_wm_launch_object_viewer();
	} else {
		hist_append_str_role("(unknown app — try: terminal, search, workflow, viewer)",
				     ROLE_SYSTEM);
	}
}

/* ------------------------------------------------------------------ */
/* Agentic loop: send to model, handle CMD:/OPEN:/DONE:               */
/* ------------------------------------------------------------------ */

static const char AGENT_SYS[] =
	"You are the Anunix AI assistant — the primary interface of an "
	"AI-native operating system. Help the user accomplish tasks.\n"
	"CRITICAL: One line per response, no newlines within your response.\n"
	"Respond with exactly ONE of:\n"
	"  CMD: <command>   — run a shell command; you will see the output\n"
	"  OPEN: <app>      — open: terminal | search | workflow | viewer\n"
	"  DONE: <text>     — end this task, show <text> to the user\n"
	"  <prose>          — conversational reply (no prefix)\n"
	"Shell commands: ls, cat, write, sysinfo, netinfo, tensor, cells, "
	"loop, state, disk, model-init, theme, date";

static void run_agent_loop(const char *input)
{
	char *cap;
	int iter;

	if (!anx_model_client_ready()) {
		anx_shell_history_record(input);
		if (!anx_strcmp(input, "clear")) {
			g_agent.hist_count = 0;
			g_agent.hist_head = 0;
			mark_dirty();
			return;
		}
		if (!anx_strcmp(input, "exit") || !anx_strcmp(input, "quit")) {
			anx_wm_window_close(g_agent.surf);
			return;
		}
		hist_append_str_role(input, ROLE_USER);
		cap = anx_alloc(HIST_COLS * 80);
		if (!cap) {
			hist_append_str_role("(out of memory)", ROLE_SYSTEM);
		} else {
			exec_cmd_capture(input, cap, HIST_COLS * 80);
			if (cap[0]) hist_append_str_role(cap, ROLE_OUTPUT);
			anx_free(cap);
		}
		mark_dirty();
		return;
	}

	/* Append user turn to conversation */
	conv_append("User: ");
	conv_append(input);
	conv_append("\n");

	hist_append_str_role(input, ROLE_USER);
	mark_dirty();

	cap = anx_alloc(CMD_CAP_SZ);
	if (!cap) {
		hist_append_str_role("(out of memory)", ROLE_SYSTEM);
		mark_dirty();
		return;
	}

	for (iter = 0; iter < AGENT_MAX_TOOL_ITERS; iter++) {
		struct anx_model_request  req;
		struct anx_model_response resp;
		int ret;
		const char *content;
		char linebuf[HIST_COLS];
		char *nl;

		anx_memset(&req, 0, sizeof(req));
		req.model        = "claude-haiku-4-5-20251001";
		req.system       = AGENT_SYS;
		req.user_message = g_agent.conv;
		req.max_tokens   = 256;

		ret = anx_model_call(&req, &resp);
		if (ret != ANX_OK || !resp.content) {
			hist_append_str_role("(model error)", ROLE_SYSTEM);
			anx_model_response_free(&resp);
			break;
		}

		content = resp.content;

		/* Truncate to first line */
		anx_strlcpy(linebuf, content, sizeof(linebuf));
		nl = linebuf;
		while (*nl && *nl != '\n' && *nl != '\r') nl++;
		*nl = '\0';

		/* Append agent turn to conversation */
		conv_append("Assistant: ");
		conv_append(linebuf);
		conv_append("\n");

		anx_model_response_free(&resp);

		if (anx_strncmp(linebuf, "CMD:", 4) == 0) {
			const char *cmd = linebuf + 4;

			while (*cmd == ' ') cmd++;
			hist_append_str_role(cmd, ROLE_CMD);
			mark_dirty();
			agent_redraw();

			exec_cmd_capture(cmd, cap, CMD_CAP_SZ);

			if (cap[0]) {
				hist_append_str_role(cap, ROLE_OUTPUT);
				conv_append("Output: ");
				conv_append(cap);
				conv_append("\n");
			}
			mark_dirty();
			agent_redraw();

		} else if (anx_strncmp(linebuf, "OPEN:", 5) == 0) {
			const char *app = linebuf + 5;

			while (*app == ' ') app++;
			open_app(app);
			break;

		} else if (anx_strncmp(linebuf, "DONE:", 5) == 0) {
			const char *msg = linebuf + 5;

			while (*msg == ' ') msg++;
			hist_append_str_role(msg, ROLE_AGENT);
			mark_dirty();
			break;

		} else {
			/* Conversational response */
			hist_append_str_role(linebuf, ROLE_AGENT);
			mark_dirty();
			break;
		}
	}

	anx_free(cap);
}

/* ------------------------------------------------------------------ */
/* Key event handler                                                   */
/* ------------------------------------------------------------------ */

void anx_wm_agent_key_event(uint32_t key, uint32_t mods, uint32_t unicode)
{
	(void)mods;
	if (!g_agent.surf)
		return;
	if (key != ANX_KEY_PAGEUP && key != ANX_KEY_PAGEDOWN)
		g_agent.scroll_off = 0;
	if (anx_shell_input_key(g_agent.input, sizeof(g_agent.input),
				 &g_agent.input_len, &g_agent.input_pos,
				 &g_agent.recall, key, unicode)) {
		mark_dirty();
		return;
	}
	switch (key) {
	case ANX_KEY_ENTER: {
		char input[sizeof(g_agent.input)];

		anx_strlcpy(input, g_agent.input, sizeof(input));
		g_agent.input_len = 0;
		g_agent.input_pos = 0;
		g_agent.input[0] = '\0';
		anx_shell_history_reset(&g_agent.recall);
		if (input[0])
			run_agent_loop(input);
		else
			hist_append_role("", 0, ROLE_USER);
		break;
	}
	case ANX_KEY_ESC:
		g_agent.input_len = 0;
		g_agent.input_pos = 0;
		g_agent.input[0] = '\0';
		anx_shell_history_reset(&g_agent.recall);
		break;
	case ANX_KEY_PAGEUP:
		g_agent.scroll_off += (int32_t)g_agent.vis_lines;
		break;
	case ANX_KEY_PAGEDOWN:
		g_agent.scroll_off -= (int32_t)g_agent.vis_lines;
		if (g_agent.scroll_off < 0) g_agent.scroll_off = 0;
		break;
	default:
		break;
	}
	mark_dirty();
}

/* ------------------------------------------------------------------ */
/* Flush                                                               */
/* ------------------------------------------------------------------ */

void anx_wm_agent_redraw(void)
{
	if (g_agent.surf)
		agent_redraw();
}

void anx_wm_agent_flush_if_dirty(void)
{
	if (g_agent.dirty && g_agent.surf) {
		agent_redraw();
		g_agent.dirty = false;
	}
}

/* ------------------------------------------------------------------ */
/* Open                                                                */
/* ------------------------------------------------------------------ */

/* The window can be closed from the WM; forget it so Open starts afresh. */
static void agent_on_destroy(struct anx_surface *surf)
{
	anx_wm_canvas_free(surf);
	g_agent.surf   = NULL;
	g_agent.cn     = NULL;
	g_agent.pixels = NULL;
	anx_free(g_agent.conv);
	g_agent.conv = NULL;
	g_agent.conv_len = 0;
	g_agent.input_len = 0;
	g_agent.input_pos = 0;
	g_agent.input[0] = '\0';
	anx_shell_history_reset(&g_agent.recall);
}

/* Tiling gave the window a new size: reallocate and redraw. */
static void agent_on_resize(struct anx_surface *surf)
{
	uint32_t *px;

	px = anx_wm_canvas_realloc(surf, surf->width, surf->height);
	if (!px)
		return;	/* old buffer stays, shown unscaled */
	g_agent.pixels    = px;
	g_agent.w         = surf->width;
	g_agent.h         = surf->height;
	g_agent.vis_lines = g_agent.h >= LINE_H + MARGIN * 2
		? (g_agent.h - MARGIN * 2) / LINE_H : 1;
	agent_redraw();
}

void anx_wm_agent_open(void)
{
	struct anx_content_node *cn;
	uint32_t buf_size;
	const struct anx_fb_info *fb;

	if (g_agent.surf)
		return;

	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return;

	/*
	 * Below the menu bar, with room for the title bar, and small enough
	 * to allocate. A full-screen canvas at (0,0) covered the menu bar and
	 * failed to allocate on a 2560x1600 panel, so no window appeared.
	 */
	g_agent.w         = fb->width;
	g_agent.h = fb->height;
	if (g_agent.h > ANX_WM_MENUBAR_H + ANX_WM_DECOR_H + ANX_WM_TASKBAR_H)
		g_agent.h -= ANX_WM_MENUBAR_H + ANX_WM_DECOR_H + ANX_WM_TASKBAR_H;
	anx_wm_window_fit(&g_agent.w, &g_agent.h);
	g_agent.vis_lines = (g_agent.h >= LINE_H + MARGIN * 2)
		? (g_agent.h - MARGIN * 2) / LINE_H
		: 1;

	buf_size      = g_agent.w * g_agent.h * 4;
	g_agent.pixels = anx_alloc(buf_size);
	if (!g_agent.pixels) {
		kprintf("[agent] no memory for %ux%u window\n",
			g_agent.w, g_agent.h);
		return;
	}

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_agent.pixels);
		g_agent.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_agent.pixels;
	cn->data_len = buf_size;

	/* Allocate conversation buffer */
	if (!g_agent.conv) {
		g_agent.conv = anx_alloc(CONV_HIST_SZ);
		if (g_agent.conv) {
			g_agent.conv[0] = '\0';
			g_agent.conv_len = 0;
		}
	}

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     (int32_t)((fb->width - g_agent.w) / 2),
				     (int32_t)(ANX_WM_MENUBAR_H + ANX_WM_DECOR_H),
				     g_agent.w, g_agent.h,
				     &g_agent.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_agent.pixels);
		g_agent.pixels = NULL;
		return;
	}

	anx_shell_history_reset(&g_agent.recall);
	g_agent.scroll_off = 0;
	/* Keep scrollback across reopen without duplicating the greeting. */
	if (!g_agent.hist_count) {
		hist_append_str_role("Anunix - AI-Native Operating System", ROLE_SYSTEM);
		if (anx_model_client_ready())
			hist_append_str_role("Type anything to talk to the agent.", ROLE_SYSTEM);
		else
			hist_append_str_role("Shell mode: type commands here. Up/Down recalls history; "
				"PgUp/PgDn scrolls. Configure AI with model-init.", ROLE_SYSTEM);
		hist_append_role("", 0, ROLE_SYSTEM);
	}

	g_agent.surf->on_destroy = agent_on_destroy;
	g_agent.surf->on_resize  = agent_on_resize;
	anx_iface_surface_set_title(g_agent.surf, "Agent");
	agent_redraw();
	anx_iface_surface_map(g_agent.surf);
	anx_wm_window_open(g_agent.surf);
	kprintf("[agent] opened\n");
}

struct anx_surface *anx_wm_agent_surface(void)
{
	return g_agent.surf;
}
