/* HTTP server for programmatic shell access */
#ifndef ANX_HTTPD_H
#define ANX_HTTPD_H
#include <anx/types.h>

/* Start the HTTP API server on the given port */
int anx_httpd_init(uint16_t port);

/* Poll for incoming connections and handle requests (call from main loop) */
void anx_httpd_poll(void);

#endif /* ANX_HTTPD_H */
