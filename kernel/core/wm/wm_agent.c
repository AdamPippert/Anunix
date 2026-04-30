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

#define INPUT_H		34
#define MARGIN		12
#define LABEL_W		(FONT_W * 5)	/* "anx> " or "you> " */

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
	uint32_t hist_count;
	int32_t  scroll_off;

	/* Input */
	char     input[HIST_COLS];
	uint32_t input_len;

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
	uint32_t slot = g_agent.hist_count % HIST_LINES;
	uint32_t copy = len < HIST_COLS - 1 ? len : HIST_COLS - 1;

	anx_memcpy(g_agent.hist[slot], s, copy);
	g_agent.hist[slot][copy] = '\0';
	g_agent.hist_role[slot] = role;
	g_agent.hist_count++;
}

static void hist_append_str_role(const char *s, uint8_t role)
{
	uint32_t max_cols;

	max_cols = g_agent.w > (uint32_t)(MARGIN * 2 + LABEL_W + 4)
		? (g_agent.w - MARGIN * 2 - LABEL_W) / FONT_W
		: HIST_COLS - 1;
	if (max_cols < 1) max_cols = 1;
	if (max_cols >= HIST_COLS) max_cols = HIST_COLS - 1;

	while (*s) {
		const char *start = s;
		uint32_t col = 0;

		while (*s && *s != '\n') {
			if (col >= max_cols) {
				/* Word-wrap: back up to last space */
				const char *w = s;
				uint32_t wc = col;

				while (w > start + 1 && w[-1] != ' ')
					w--, wc--;

				if (w > start + max_cols / 2) {
					hist_append_role(start,
						(uint32_t)(w - start), role);
					s = w;
				} else {
					hist_append_role(start, col, role);
					s = start + col;
				}
				start = s;
				col = 0;
				continue;
			}
			s++; col++;
		}
		hist_append_role(start, (uint32_t)(s - start), role);
		if (*s == '\n')
			s++;
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

static void agent_redraw(void)
{
	const struct anx_theme *t = anx_theme_get();
	uint32_t bg    = t->palette.background;
	uint32_t input_bar_bg = t->palette.surface;
	uint32_t input_border = t->palette.accent;

	/* Background */
	agent_fill(0, 0, g_agent.w, g_agent.h, bg);

	/* History area */
	{
		uint32_t total = g_agent.hist_count;
		uint32_t vis   = g_agent.vis_lines;
		int32_t  start;
		uint32_t i;

		/* start = first line index to display */
		if (total > vis)
			start = (int32_t)(total - vis) + g_agent.scroll_off;
		else
			start = g_agent.scroll_off;
		if (start < 0) start = 0;

		for (i = 0; i < vis && (uint32_t)(start + (int32_t)i) < total; i++) {
			uint32_t idx  = (uint32_t)(start + (int32_t)i) % HIST_LINES;
			uint32_t y    = MARGIN + i * LINE_H;
			uint8_t  role = g_agent.hist_role[idx];
			uint32_t fg;
			const char *label;

			switch (role) {
			case ROLE_USER:
				fg    = t->palette.accent;
				label = "you> ";
				break;
			case ROLE_AGENT:
				fg    = t->palette.text_primary;
				label = "anx> ";
				break;
			case ROLE_CMD:
				fg    = 0xFF888888u;
				label = "cmd> ";
				break;
			case ROLE_OUTPUT:
				fg    = 0xFFAAAAAAu;
				label = "     ";
				break;
			default: /* ROLE_SYSTEM */
				fg    = 0xFF666666u;
				label = "     ";
				break;
			}

			agent_draw_str(MARGIN, y, label, fg, bg);
			agent_draw_str(MARGIN + LABEL_W, y,
				       g_agent.hist[idx], fg, bg);
		}
	}

	/* Input bar */
	{
		uint32_t iy = g_agent.h - INPUT_H;

		agent_fill(0, iy - 2, g_agent.w, 2, input_border);
		agent_fill(0, iy, g_agent.w, INPUT_H, input_bar_bg);

		agent_draw_str(MARGIN, iy + (INPUT_H - FONT_H) / 2,
			       "  >  ", t->palette.accent, input_bar_bg);
		agent_draw_str(MARGIN + LABEL_W,
			       iy + (INPUT_H - FONT_H) / 2,
			       g_agent.input, t->palette.text_primary, input_bar_bg);

		/* Cursor */
		{
			uint32_t cx = MARGIN + LABEL_W +
				      g_agent.input_len * FONT_W;
			uint32_t cy = iy + (INPUT_H - FONT_H) / 2;

			agent_fill(cx, cy, 2, FONT_H, t->palette.accent);
		}
	}

	/* Commit to framebuffer */
	if (g_agent.surf)
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

static void run_agent_loop(void)
{
	char  *cap;
	int    iter;

	if (!anx_model_client_ready()) {
		/* No model — route as shell command */
		char  cmdout[512];

		hist_append_str_role(g_agent.input, ROLE_USER);
		hist_append_str_role(
			"(model not configured — running as shell command; "
			"open a terminal with Meta+Enter or type 'model-init' here)",
			ROLE_SYSTEM);

		exec_cmd_capture(g_agent.input, cmdout, sizeof(cmdout));

		if (cmdout[0])
			hist_append_str_role(cmdout, ROLE_OUTPUT);

		mark_dirty();
		return;
	}

	/* Append user turn to conversation */
	conv_append("User: ");
	conv_append(g_agent.input);
	conv_append("\n");

	hist_append_str_role(g_agent.input, ROLE_USER);
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

	switch (key) {
	case ANX_KEY_ENTER:
		if (g_agent.input_len == 0)
			break;
		g_agent.input[g_agent.input_len] = '\0';
		run_agent_loop();
		g_agent.input_len  = 0;
		g_agent.input[0]   = '\0';
		g_agent.scroll_off = 0;
		mark_dirty();
		break;

	case ANX_KEY_BACKSPACE:
		if (g_agent.input_len > 0) {
			g_agent.input_len--;
			g_agent.input[g_agent.input_len] = '\0';
			mark_dirty();
		}
		break;

	case ANX_KEY_ESC:
		/* Clear input */
		g_agent.input_len = 0;
		g_agent.input[0]  = '\0';
		mark_dirty();
		break;

	case ANX_KEY_UP:
		g_agent.scroll_off--;
		mark_dirty();
		break;

	case ANX_KEY_DOWN:
		if (g_agent.scroll_off < 0) {
			g_agent.scroll_off++;
			mark_dirty();
		}
		break;

	default:
		if (unicode >= 0x20 && unicode < 0x7F &&
		    g_agent.input_len < HIST_COLS - 1) {
			g_agent.input[g_agent.input_len++] = (char)unicode;
			g_agent.input[g_agent.input_len]   = '\0';
			mark_dirty();
		}
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Flush                                                               */
/* ------------------------------------------------------------------ */

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

	g_agent.w         = fb->width;
	g_agent.h         = fb->height;
	g_agent.vis_lines = (g_agent.h > INPUT_H + MARGIN * 2)
		? (g_agent.h - INPUT_H - MARGIN * 2) / LINE_H
		: 1;

	buf_size      = g_agent.w * g_agent.h * 4;
	g_agent.pixels = anx_alloc(buf_size);
	if (!g_agent.pixels)
		return;

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
				     0, 0, g_agent.w, g_agent.h,
				     &g_agent.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_agent.pixels);
		g_agent.pixels = NULL;
		return;
	}

	/* Welcome messages */
	hist_append_str_role("Anunix — AI-Native Operating System", ROLE_SYSTEM);
	hist_append_str_role("Type anything to talk to the agent.  "
			     "Meta+Enter opens a terminal.", ROLE_SYSTEM);
	if (!anx_model_client_ready())
		hist_append_str_role(
			"No model configured.  "
			"Type 'model-init' here or open a terminal (Meta+Enter).",
			ROLE_SYSTEM);
	hist_append_str_role("", ROLE_SYSTEM);

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
