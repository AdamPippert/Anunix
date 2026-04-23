/*
 * anx/browser.h — Anunix native browser daemon.
 *
 * Speaks the ANX-Browser Protocol on port 9191.
 * The web UI and desktop UI connect to this port unchanged.
 */

#ifndef ANX_BROWSER_H
#define ANX_BROWSER_H

#include <anx/types.h>

/* Start the browser daemon on the given port (typically 9191). */
int  anx_browser_init(uint16_t port);

/*
 * Poll for incoming connections and service the streaming loop.
 * Call from the main kernel event loop alongside anx_httpd_poll().
 */
void anx_browser_poll(void);

#endif /* ANX_BROWSER_H */
