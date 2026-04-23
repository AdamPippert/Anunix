/*
 * ws.h — WebSocket server (RFC 6455).
 *
 * Handles the HTTP upgrade handshake and frame encode/decode
 * for the ANX-Browser streaming protocol.
 */

#ifndef ANX_BROWSER_WS_H
#define ANX_BROWSER_WS_H

#include <anx/types.h>
#include <anx/net.h>

/* WebSocket opcodes */
#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/* Maximum incoming frame payload we accept (control messages) */
#define WS_MAX_RECV_PAYLOAD 4096

struct ws_frame {
	uint8_t  opcode;
	bool     fin;
	uint8_t *payload;    /* anx_alloc'd; caller must anx_free */
	uint32_t payload_len;
};

/*
 * Attempt WebSocket upgrade on an already-accepted TCP connection.
 * conn: connected TCP socket with the HTTP upgrade request already
 *       buffered in req_buf (len req_len).
 * Returns true on success (101 sent), false if not a WS upgrade request.
 */
bool anx_ws_upgrade(struct anx_tcp_conn *conn,
		     const char *req_buf, uint32_t req_len);

/*
 * Send a text frame.  payload must be NUL-terminated; len = strlen.
 */
int anx_ws_send_text(struct anx_tcp_conn *conn,
		      const char *payload, uint32_t len);

/*
 * Send a binary frame.
 */
int anx_ws_send_binary(struct anx_tcp_conn *conn,
			const uint8_t *payload, uint32_t len);

/*
 * Receive one frame (non-blocking; timeout_ms = 0 → poll only).
 * Caller must anx_free(frame.payload) after use.
 * Returns true if a frame was received, false on timeout/error.
 */
bool anx_ws_recv_frame(struct anx_tcp_conn *conn,
		        struct ws_frame *out, uint32_t timeout_ms);

/*
 * Send a WebSocket close frame and close the connection.
 */
void anx_ws_close(struct anx_tcp_conn *conn);

#endif /* ANX_BROWSER_WS_H */
