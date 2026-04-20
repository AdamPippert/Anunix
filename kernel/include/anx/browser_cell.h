/*
 * anx/browser_cell.h — Browser Renderer Execution Cell.
 *
 * Connects to an anxbrowserd instance over TCP, creates a browser session,
 * and streams JPEG frames directly to the framebuffer via anx_jpeg_blit_scaled().
 *
 * Usage:
 *   anx_browser_cell_init("10.0.2.2");   // set host; default = 10.0.2.2
 *   anx_browser_cell_navigate("https://example.com");
 *   // call anx_browser_cell_tick() from the idle poll loop
 *   anx_browser_cell_stop();
 */

#ifndef ANX_BROWSER_CELL_H
#define ANX_BROWSER_CELL_H

#include <anx/types.h>

/* Initialise and connect to anxbrowserd.  host is an IPv4 dotted-quad string
 * or NULL to use the QEMU host default (10.0.2.2).  port 0 → default 9090. */
int anx_browser_cell_init(const char *host, uint16_t port);

/* Navigate the active session to url. */
int anx_browser_cell_navigate(const char *url);

/* Called from the idle poll loop; receives the next frame if available and
 * blits it to the framebuffer.  Non-blocking — returns immediately if no
 * frame is ready. */
void anx_browser_cell_tick(void);

/* Tear down the TCP connection and free resources. */
void anx_browser_cell_stop(void);

/* Return true if a session is active and streaming. */
bool anx_browser_cell_active(void);

#endif /* ANX_BROWSER_CELL_H */
