/*
 * HTTP API server — programmatic access to the Anunix runtime.
 *
 * GET  /api/v1/health                 — health check
 * GET  /api/v1/fb                     — framebuffer info
 * GET  /api/v1/display/modes          — display mode list
 * GET  /api/v1/jepa                   — JEPA world model status
 * GET  /api/v1/workflow               — list all workflows
 * GET  /api/v1/workflow/<name>        — workflow status JSON
 * GET  /api/v1/workflow/<name>/show   — workflow DSL text
 * GET  /api/v1/workflow/<name>/graph  — workflow ASCII diagram
 * POST /api/v1/exec                   — run a shell command
 * POST /api/v1/agent                  — dispatch agent goal {"goal":"..."}
 * POST /api/v1/workflow               — create workflow {"name":"...","description":"..."}
 * POST /api/v1/workflow/<name>/run    — run a workflow
 */
#ifndef ANX_HTTPD_H
#define ANX_HTTPD_H
#include <anx/types.h>

/* Start the HTTP API server on the given port. */
int anx_httpd_init(uint16_t port);

/* Poll for incoming connections and handle requests (call from main loop). */
void anx_httpd_poll(void);

#endif /* ANX_HTTPD_H */
