/*
 * anx/sshd.h — SSH-2.0 server.
 *
 * Minimal SSH server supporting curve25519-sha256 key exchange,
 * ssh-ed25519 host keys, and chacha20-poly1305@openssh.com ciphers.
 * Single connection at a time. Integrates with the kernel shell.
 */

#ifndef ANX_SSHD_H
#define ANX_SSHD_H

#include <anx/types.h>

/* Initialize SSH server on the given port (default 22) */
int anx_sshd_init(uint16_t port);

/* Poll for SSH activity (call from main loop) */
void anx_sshd_poll(void);

#endif /* ANX_SSHD_H */
