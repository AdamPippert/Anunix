/*
 * virtio_net.c — Virtio network device driver.
 *
 * Drives a virtio-net PCI device using the legacy PIO transport.
 * Provides raw Ethernet frame I/O via RX/TX virtqueues with
 * pre-allocated 2 KiB packet buffers.
 */

#include <anx/types.h>
#include <anx/virtio.h>
#include <anx/virtio_net.h>
#include <anx/pci.h>
#include <anx/irq.h>
#include <anx/io.h>
#include <anx/page.h>
#include <anx/alloc.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Virtio-net feature bits */
#define VIRTIO_NET_F_MAC	(1 << 5)

/* Virtio-net header prepended to every packet (10 bytes for legacy) */
struct virtio_net_hdr {
	uint8_t flags;
	uint8_t gso_type;
	uint16_t hdr_len;
	uint16_t gso_size;
	uint16_t csum_start;
	uint16_t csum_offset;
} __attribute__((packed));

#define VIRTIO_NET_HDR_SIZE	sizeof(struct virtio_net_hdr)

/* Packet buffer: virtio-net header + max Ethernet frame */
#define PKT_BUF_SIZE		2048
#define RX_QUEUE_IDX		0
#define TX_QUEUE_IDX		1
#define NUM_RX_BUFS		64

/* PCI vendor/device for virtio-net (legacy) */
#define VIRTIO_VENDOR		0x1AF4
#define VIRTIO_NET_DEVICE	0x1000

/* Driver state */
static struct {
	struct anx_virtio_dev vdev;
	struct anx_virtqueue rx_vq;
	struct anx_virtqueue tx_vq;
	uint8_t mac[6];
	bool ready;

	/* Pre-allocated RX buffers */
	uint8_t *rx_bufs[NUM_RX_BUFS];
} netdev;

/* --- RX buffer management --- */

static int rx_fill_one(uint8_t *buf)
{
	int ret;

	/* Device writes into the buffer (WRITE flag) */
	ret = anx_vq_add_buf(&netdev.rx_vq, buf, PKT_BUF_SIZE,
			      VIRTQ_DESC_F_WRITE, buf);
	return ret;
}

static int rx_fill_all(void)
{
	int i;

	for (i = 0; i < NUM_RX_BUFS; i++) {
		netdev.rx_bufs[i] = anx_alloc(PKT_BUF_SIZE);
		if (!netdev.rx_bufs[i])
			return ANX_ENOMEM;
		if (rx_fill_one(netdev.rx_bufs[i]) < 0)
			return ANX_ENOMEM;
	}

	anx_vq_kick(&netdev.rx_vq);
	return ANX_OK;
}

/* --- TX --- */

static void tx_reclaim(void)
{
	uint32_t len;
	void *buf;

	while ((buf = anx_vq_get_used(&netdev.tx_vq, &len)) != NULL)
		anx_free(buf);
}

/* --- IRQ handler --- */

static void virtio_net_irq(uint32_t irq, void *arg)
{
	(void)irq;
	(void)arg;

	/* Reading ISR clears the interrupt */
	anx_virtio_isr(&netdev.vdev);
}

/* --- Public API --- */

int anx_virtio_net_init(void)
{
	struct anx_pci_device *pci;
	uint32_t features;
	int ret;
	int i;

	pci = anx_pci_find_device(VIRTIO_VENDOR, VIRTIO_NET_DEVICE);
	if (!pci) {
		kprintf("virtio-net: no device found\n");
		return ANX_ENOENT;
	}

	ret = anx_virtio_init(&netdev.vdev, pci);
	if (ret != ANX_OK) {
		kprintf("virtio-net: init failed (%d)\n", ret);
		return ret;
	}

	/* Feature negotiation — we only need MAC */
	features = anx_virtio_get_features(&netdev.vdev);
	anx_virtio_set_features(&netdev.vdev,
				features & VIRTIO_NET_F_MAC);

	/* Read MAC address from device config */
	if (features & VIRTIO_NET_F_MAC) {
		for (i = 0; i < 6; i++)
			netdev.mac[i] = anx_virtio_config_read8(
				&netdev.vdev, (uint16_t)i);
	}

	/* Set up RX and TX queues */
	ret = anx_virtio_setup_vq(&netdev.vdev, RX_QUEUE_IDX,
				   &netdev.rx_vq);
	if (ret != ANX_OK) {
		kprintf("virtio-net: rx queue setup failed (%d)\n", ret);
		return ret;
	}

	ret = anx_virtio_setup_vq(&netdev.vdev, TX_QUEUE_IDX,
				   &netdev.tx_vq);
	if (ret != ANX_OK) {
		kprintf("virtio-net: tx queue setup failed (%d)\n", ret);
		return ret;
	}

	/* Fill RX queue with buffers */
	ret = rx_fill_all();
	if (ret != ANX_OK) {
		kprintf("virtio-net: rx buffer alloc failed (%d)\n", ret);
		return ret;
	}

	/* Register IRQ handler */
	if (pci->irq_line > 0 && pci->irq_line < 16) {
		anx_irq_register(pci->irq_line, virtio_net_irq, NULL);
		anx_irq_unmask(pci->irq_line);
	}

	/* Signal device ready */
	anx_virtio_driver_ok(&netdev.vdev);
	netdev.ready = true;

	kprintf("virtio-net: %x:%x:%x:%x:%x:%x on irq %u\n",
		(uint32_t)netdev.mac[0], (uint32_t)netdev.mac[1],
		(uint32_t)netdev.mac[2], (uint32_t)netdev.mac[3],
		(uint32_t)netdev.mac[4], (uint32_t)netdev.mac[5],
		(uint32_t)pci->irq_line);

	return ANX_OK;
}

int anx_virtio_net_send(const void *frame, uint32_t len)
{
	uint8_t *buf;
	struct virtio_net_hdr *hdr;
	int ret;

	if (!netdev.ready)
		return ANX_EIO;

	/* Reclaim completed TX buffers first */
	tx_reclaim();

	/* Allocate buffer for virtio-net header + frame */
	buf = anx_alloc(VIRTIO_NET_HDR_SIZE + len);
	if (!buf)
		return ANX_ENOMEM;

	/* Zero the virtio-net header (no offloading) */
	hdr = (struct virtio_net_hdr *)buf;
	anx_memset(hdr, 0, VIRTIO_NET_HDR_SIZE);

	/* Copy the Ethernet frame after the header */
	anx_memcpy(buf + VIRTIO_NET_HDR_SIZE, frame, len);

	/* Add to TX queue — device reads the buffer */
	ret = anx_vq_add_buf(&netdev.tx_vq, buf,
			      (uint32_t)(VIRTIO_NET_HDR_SIZE + len),
			      0, buf);
	if (ret < 0) {
		anx_free(buf);
		return ANX_EBUSY;
	}

	anx_vq_kick(&netdev.tx_vq);
	return ANX_OK;
}

int anx_virtio_net_poll(void (*cb)(const void *frame, uint32_t len,
				   void *arg),
			void *arg)
{
	uint32_t used_len;
	void *buf;
	int count = 0;

	if (!netdev.ready)
		return 0;

	/* Also reclaim TX buffers while we're here */
	tx_reclaim();

	/* Process received packets */
	while ((buf = anx_vq_get_used(&netdev.rx_vq, &used_len)) != NULL) {
		uint8_t *pkt = (uint8_t *)buf;

		if (used_len > VIRTIO_NET_HDR_SIZE && cb) {
			cb(pkt + VIRTIO_NET_HDR_SIZE,
			   used_len - (uint32_t)VIRTIO_NET_HDR_SIZE,
			   arg);
		}

		/* Re-add buffer to RX ring */
		rx_fill_one(buf);
		count++;
	}

	if (count > 0)
		anx_vq_kick(&netdev.rx_vq);

	return count;
}

void anx_virtio_net_mac(uint8_t mac[6])
{
	int i;

	for (i = 0; i < 6; i++)
		mac[i] = netdev.mac[i];
}

bool anx_virtio_net_ready(void)
{
	return netdev.ready;
}
