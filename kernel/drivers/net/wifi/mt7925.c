/*
 * mt7925.c — MediaTek MT7925 (RZ717) Wi-Fi 7 Ethernet driver.
 *
 * PCIe device 14C3:0717.  Found in the Framework Laptop 16 AMD Ryzen AI 300
 * (jekyll) as the sole network path for bare-metal Anunix.
 *
 * Architecture:
 *   - Polling mode (MSI-X IRQ 184 is above 8259 PIC range)
 *   - Firmware embedded as .rodata blobs in the kernel binary
 *   - After MCU boot: WiFi association via shell command
 *     "wifi connect <ssid> [<psk>]"
 *   - Data path: WFDMA TX ring 0 / RX ring 0
 *   - Layer 2: MCU decaps 802.11 → Ethernet; driver sees Ethernet frames
 */

#include <anx/types.h>
#include <anx/mt7925.h>
#include <anx/pci.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/net.h>
#include "mt7925_reg.h"
#include "mt7925_drv.h"

/* ------------------------------------------------------------------ */
/* Singleton device state                                              */
/* ------------------------------------------------------------------ */

static struct mt7925_dev g_dev;
static bool g_ready;

/* ------------------------------------------------------------------ */
/* MMIO helpers                                                        */
/* ------------------------------------------------------------------ */

static inline uint32_t nic_rd(uint32_t reg)
{
	return *(volatile uint32_t *)((uint8_t *)g_dev.bar0 + reg);
}

static inline void nic_wr(uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)((uint8_t *)g_dev.bar0 + reg) = val;
}

/* ------------------------------------------------------------------ */
/* DMA descriptor (data rings)                                        */
/* ------------------------------------------------------------------ */

struct mt7925_dma_desc {
	uint32_t buf;
	uint32_t ctrl;
	uint32_t buf1;
	uint32_t info;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* Data TX ring (ring 0)                                              */
/* ------------------------------------------------------------------ */

#define DATA_TX_RING_SIZE  64
#define DATA_TX_BUF_SIZE   2048

static struct mt7925_dma_desc *data_tx_ring;
static uint8_t *data_tx_bufs;
static uint32_t tx_cidx;
static uint32_t tx_didx_shadow; /* last observed DMA index */

static int data_tx_ring_init(void)
{
	uintptr_t ring_pa = anx_page_alloc(0);
	if (!ring_pa) return -1;

	uint32_t order = 0;
	while ((1U << order) * ANX_PAGE_SIZE <
	       DATA_TX_RING_SIZE * DATA_TX_BUF_SIZE)
		order++;
	uintptr_t buf_pa = anx_page_alloc(order);
	if (!buf_pa) return -1;

	anx_memset((void *)ring_pa, 0, ANX_PAGE_SIZE);
	anx_memset((void *)buf_pa, 0,
		   (size_t)(1U << order) * ANX_PAGE_SIZE);

	data_tx_ring = (struct mt7925_dma_desc *)ring_pa;
	data_tx_bufs = (uint8_t *)buf_pa;
	tx_cidx      = 0;
	tx_didx_shadow = 0;

	nic_wr(MT_WFDMA0_TX_RING_CNT(MT_DATA_TXRING), DATA_TX_RING_SIZE);
	nic_wr(MT_WFDMA0_TX_RING_ADDR(MT_DATA_TXRING), (uint32_t)ring_pa);
	nic_wr(MT_WFDMA0_TX_RING_CIDX(MT_DATA_TXRING), 0);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Data RX ring (ring 0)                                              */
/* ------------------------------------------------------------------ */

#define DATA_RX_RING_SIZE  64
#define DATA_RX_BUF_SIZE   2048

static struct mt7925_dma_desc *data_rx_ring;
static uint8_t *data_rx_bufs;
static uint32_t rx_cidx;

static int data_rx_ring_init(void)
{
	uintptr_t ring_pa = anx_page_alloc(0);
	if (!ring_pa) return -1;

	uint32_t order = 0;
	while ((1U << order) * ANX_PAGE_SIZE <
	       DATA_RX_RING_SIZE * DATA_RX_BUF_SIZE)
		order++;
	uintptr_t buf_pa = anx_page_alloc(order);
	if (!buf_pa) return -1;

	anx_memset((void *)ring_pa, 0, ANX_PAGE_SIZE);
	anx_memset((void *)buf_pa, 0,
		   (size_t)(1U << order) * ANX_PAGE_SIZE);

	data_rx_ring = (struct mt7925_dma_desc *)ring_pa;
	data_rx_bufs = (uint8_t *)buf_pa;
	rx_cidx      = 0;

	/* Pre-fill all RX descriptors */
	for (uint32_t i = 0; i < DATA_RX_RING_SIZE; i++) {
		data_rx_ring[i].buf  = (uint32_t)(uintptr_t)(data_rx_bufs +
							      i * DATA_RX_BUF_SIZE);
		data_rx_ring[i].ctrl = DATA_RX_BUF_SIZE | MT_DMA_CTRL_DMA_DONE;
		data_rx_ring[i].buf1 = 0;
		data_rx_ring[i].info = 0;
	}

	nic_wr(MT_WFDMA0_RX_RING_CNT(MT_DATA_RXRING), DATA_RX_RING_SIZE);
	nic_wr(MT_WFDMA0_RX_RING_ADDR(MT_DATA_RXRING), (uint32_t)ring_pa);
	nic_wr(MT_WFDMA0_RX_RING_CIDX(MT_DATA_RXRING), DATA_RX_RING_SIZE - 1);

	return 0;
}

int mt7925_data_rings_init(struct mt7925_dev *dev)
{
	(void)dev;
	if (data_tx_ring_init() != 0) return -1;
	if (data_rx_ring_init() != 0) return -1;
	kprintf("mt7925: data rings ready (TX=%u RX=%u)\n",
		DATA_TX_RING_SIZE, DATA_RX_RING_SIZE);
	return 0;
}

/* ------------------------------------------------------------------ */
/* TX path                                                             */
/* ------------------------------------------------------------------ */

int mt7925_tx_frame(struct mt7925_dev *dev, const void *frame, uint16_t len)
{
	(void)dev;

	if (!g_ready || g_dev.state < MT7925_STATE_SCANNING)
		return ANX_EIO;
	if (len > DATA_TX_BUF_SIZE)
		return ANX_EINVAL;

	/* Check ring not full */
	uint32_t didx = nic_rd(MT_WFDMA0_TX_RING_DIDX(MT_DATA_TXRING));
	uint32_t used = (tx_cidx - didx) % DATA_TX_RING_SIZE;
	if (used >= DATA_TX_RING_SIZE - 1)
		return ANX_EBUSY;

	uint32_t slot = tx_cidx % DATA_TX_RING_SIZE;
	uint8_t *buf  = data_tx_bufs + slot * DATA_TX_BUF_SIZE;

	anx_memcpy(buf, frame, len);
	data_tx_ring[slot].buf  = (uint32_t)(uintptr_t)buf;
	data_tx_ring[slot].ctrl = (len & MT_DMA_CTRL_SD_LEN0_MASK)
				| MT_DMA_CTRL_FIRST_SEC0
				| MT_DMA_CTRL_LAST_SEC0;
	data_tx_ring[slot].buf1 = 0;
	data_tx_ring[slot].info = 0;

	tx_cidx++;
	nic_wr(MT_WFDMA0_TX_RING_CIDX(MT_DATA_TXRING),
	       tx_cidx % DATA_TX_RING_SIZE);

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* RX path                                                             */
/* ------------------------------------------------------------------ */

/*
 * Consume one frame from the data RX ring and return a pointer to it.
 * The frame is valid until the next call to mt7925_data_rx_one().
 * Does NOT call anx_eth_recv — caller decides what to do with the frame.
 * Returns NULL if no frame is available.
 */
const uint8_t *mt7925_data_rx_one(struct mt7925_dev *dev, uint32_t *out_len)
{
	struct mt7925_dma_desc *desc;
	uint32_t didx, cidx, len;
	const uint8_t *buf;

	(void)dev;
	if (!g_ready) return NULL;

	didx = nic_rd(MT_WFDMA0_RX_RING_DIDX(MT_DATA_RXRING));
	cidx = rx_cidx % DATA_RX_RING_SIZE;
	if (cidx == didx) return NULL;

	desc = &data_rx_ring[cidx];
	len  = desc->ctrl & MT_DMA_CTRL_SD_LEN0_MASK;
	buf  = data_rx_bufs + cidx * DATA_RX_BUF_SIZE;

	desc->ctrl = DATA_RX_BUF_SIZE | MT_DMA_CTRL_DMA_DONE;
	rx_cidx++;
	nic_wr(MT_WFDMA0_RX_RING_CIDX(MT_DATA_RXRING),
	       rx_cidx % DATA_RX_RING_SIZE);

	if (out_len) *out_len = len;
	return buf;
}

void mt7925_rx_poll(struct mt7925_dev *dev)
{
	(void)dev;

	if (!g_ready)
		return;

	uint32_t didx = nic_rd(MT_WFDMA0_RX_RING_DIDX(MT_DATA_RXRING));
	uint32_t cidx = rx_cidx % DATA_RX_RING_SIZE;

	while (cidx != didx) {
		struct mt7925_dma_desc *desc = &data_rx_ring[cidx];
		uint32_t len = desc->ctrl & MT_DMA_CTRL_SD_LEN0_MASK;
		const uint8_t *buf = data_rx_bufs + cidx * DATA_RX_BUF_SIZE;

		if (len >= ANX_ETH_HLEN)
			anx_eth_recv(buf, len);

		/* Refill descriptor */
		desc->ctrl = DATA_RX_BUF_SIZE | MT_DMA_CTRL_DMA_DONE;
		rx_cidx++;
		cidx = rx_cidx % DATA_RX_RING_SIZE;

		/* Advance HW CPU index */
		nic_wr(MT_WFDMA0_RX_RING_CIDX(MT_DATA_RXRING), cidx);
	}
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int anx_mt7925_init(void)
{
	anx_memset(&g_dev, 0, sizeof(g_dev));
	g_ready = false;

	/* Probe PCI */
	struct anx_pci_device *pci =
		anx_pci_find_device(MT7925_VENDOR_ID, MT7925_DEVICE_ID);
	if (!pci) {
		kprintf("mt7925: device 14C3:0717 not found\n");
		return ANX_ENODEV;
	}

	kprintf("mt7925: found at %02x:%02x.%x BAR0=0x%x\n",
		pci->bus, pci->slot, pci->func,
		pci->bar[0] & ~0xf);

	/* BAR0: identity-mapped (phys == virt in Anunix) */
	g_dev.bar0 = (void *)(uintptr_t)(pci->bar[0] & ~0xf);
	if (pci->bar[2])
		g_dev.bar2 = (void *)(uintptr_t)(pci->bar[2] & ~0xf);

	anx_pci_enable_bus_master(pci);

	/* Read chip ID */
	uint32_t hw_ver = nic_rd(MT_CONN_HW_VER);
	kprintf("mt7925: chip hw_ver=0x%08x\n", hw_ver);

	/* Download firmware and boot MCU */
	int ret = mt7925_fw_download(&g_dev);
	if (ret) {
		kprintf("mt7925: firmware download failed (%d)\n", ret);
		return ret;
	}

	/* Initialize WM command ring */
	ret = mt7925_mcu_init(&g_dev);
	if (ret) {
		kprintf("mt7925: MCU init failed\n");
		return ret;
	}

	/* Initialize data TX/RX rings */
	ret = mt7925_data_rings_init(&g_dev);
	if (ret) {
		kprintf("mt7925: data rings init failed\n");
		return ret;
	}

	g_dev.state = MT7925_STATE_FW_UP;
	g_ready = true;

	kprintf("mt7925: ready — use 'wifi connect <ssid> [<psk>]'\n");
	return ANX_OK;
}

bool anx_mt7925_ready(void)
{
	return g_ready && g_dev.state >= MT7925_STATE_ASSOC;
}

int anx_mt7925_tx(const void *frame, uint16_t len)
{
	return mt7925_tx_frame(&g_dev, frame, len);
}

void anx_mt7925_poll(void)
{
	mt7925_rx_poll(&g_dev);
}

const uint8_t *anx_mt7925_mac(void)
{
	return g_dev.mac;
}

int anx_mt7925_connect(const char *ssid, const char *psk)
{
	if (!g_ready) return ANX_EIO;

	anx_strlcpy(g_dev.ssid, ssid, MT7925_SSID_LEN);
	if (psk)
		anx_strlcpy(g_dev.psk, psk, MT7925_PSK_LEN);
	else
		g_dev.psk[0] = '\0';

	g_dev.state = MT7925_STATE_SCANNING;
	return mt7925_mcu_connect(&g_dev, ssid, psk);
}

void anx_mt7925_disconnect(void)
{
	if (!g_ready) return;
	mt7925_mcu_disconnect(&g_dev);
}

anx_mt7925_state_t anx_mt7925_state(void)
{
	return g_dev.state;
}

void anx_mt7925_info(void)
{
	static const char * const state_names[] = {
		"down", "fw_up", "scanning", "assoc", "connected"
	};
	uint32_t s = (uint32_t)g_dev.state;

	kprintf("mt7925: state=%s\n",
		s < 5 ? state_names[s] : "?");

	if (g_ready) {
		kprintf("mt7925: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			g_dev.mac[0], g_dev.mac[1], g_dev.mac[2],
			g_dev.mac[3], g_dev.mac[4], g_dev.mac[5]);
		kprintf("mt7925: SSID \"%s\"\n", g_dev.ssid);
	}
}
