/*
 * anx/virtio_net.h — Virtio network device driver.
 *
 * Provides raw Ethernet frame send/receive over a virtio-net device.
 * Uses the legacy PIO transport with RX/TX virtqueues.
 */

#ifndef ANX_VIRTIO_NET_H
#define ANX_VIRTIO_NET_H

#include <anx/types.h>

/* Probe for a virtio-net device and initialize it */
int anx_virtio_net_init(void);

/* Send a raw Ethernet frame (without virtio-net header) */
int anx_virtio_net_send(const void *frame, uint32_t len);

/* Poll for received frames, calling cb for each one */
int anx_virtio_net_poll(void (*cb)(const void *frame, uint32_t len,
				   void *arg),
			void *arg);

/* Get the device MAC address */
void anx_virtio_net_mac(uint8_t mac[6]);

/* Check if the device is initialized and ready */
bool anx_virtio_net_ready(void);

#endif /* ANX_VIRTIO_NET_H */
