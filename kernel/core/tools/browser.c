/*
 * browser.c — Shell commands for the Browser Renderer Cell.
 *
 *   browser_init [host [port]]   connect to anxbrowserd and start streaming
 *   browser <url>                navigate the active session to a URL
 *   browser_stop                 stop streaming and free resources
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/browser_cell.h>
#include <anx/kprintf.h>
#include <anx/string.h>

void cmd_browser_init(int argc, char **argv)
{
	const char *host = NULL;
	uint16_t    port = 0;

	if (argc >= 2)
		host = argv[1];

	if (argc >= 3) {
		const char *p = argv[2];

		while (*p >= '0' && *p <= '9')
			port = port * 10 + (uint16_t)(*p++ - '0');
	}

	kprintf("browser_init: connecting to %s:%u\n",
		host ? host : "10.0.2.2",
		(uint32_t)(port ? port : 9090));

	int ret = anx_browser_cell_init(host, port);

	if (ret != ANX_OK)
		kprintf("browser_init: failed (%d)\n", ret);
	else
		kprintf("browser_init: ready — use 'browser <url>' to navigate\n");
}

void cmd_browser(int argc, char **argv)
{
	if (argc < 2) {
		kprintf("usage: browser <url>\n");
		kprintf("       browser status\n");
		return;
	}

	if (anx_strcmp(argv[1], "status") == 0) {
		kprintf("browser: %s\n",
			anx_browser_cell_active() ? "active" : "inactive");
		return;
	}

	if (!anx_browser_cell_active()) {
		kprintf("browser: not connected — run 'browser_init' first\n");
		return;
	}

	int ret = anx_browser_cell_navigate(argv[1]);

	if (ret != ANX_OK)
		kprintf("browser: navigate failed (%d)\n", ret);
}

void cmd_browser_stop(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	anx_browser_cell_stop();
}
