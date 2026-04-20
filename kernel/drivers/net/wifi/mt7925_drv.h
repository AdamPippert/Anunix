/*
 * mt7925_drv.h — MT7925 internal driver state (shared across files).
 */

#ifndef MT7925_DRV_H
#define MT7925_DRV_H

#include <anx/types.h>
#include <anx/mt7925.h>

/* Maximum 802.11 SSID length */
#define MT7925_SSID_LEN         32
#define MT7925_PSK_LEN          64

#define MT7925_TX_RING_SIZE     64
#define MT7925_RX_RING_SIZE     64
#define MT7925_RX_BUF_SIZE      2048

/* Per-ring TX buffer tracking */
struct mt7925_tx_slot {
	uint8_t  *buf;
	uint16_t  len;
};

/* Driver device state (singleton) */
struct mt7925_dev {
	void     *bar0;           /* BAR0 MMIO virtual address */
	void     *bar2;           /* BAR2 MMIO virtual address (may be NULL) */

	uint8_t   mac[6];         /* station MAC address */

	anx_mt7925_state_t state;

	/* WiFi credentials */
	char      ssid[MT7925_SSID_LEN + 1];
	char      psk[MT7925_PSK_LEN + 1];

	/* Data TX ring */
	void     *tx_ring;        /* DMA descriptor ring (physical = virtual) */
	uint8_t  *tx_bufs;        /* TX packet buffers */
	uint32_t  tx_cidx;        /* CPU write index */
	uint32_t  tx_used;        /* number of in-flight descriptors */

	/* Data RX ring */
	void     *rx_ring;        /* DMA descriptor ring */
	uint8_t  *rx_bufs;        /* RX packet buffers */
	uint32_t  rx_cidx;        /* CPU refill index */
};

/* Internal functions */
int  mt7925_fw_download(struct mt7925_dev *dev);
int  mt7925_mcu_init(struct mt7925_dev *dev);
int  mt7925_data_rings_init(struct mt7925_dev *dev);
int  mt7925_mcu_connect(struct mt7925_dev *dev,
			const char *ssid, const char *psk);
void mt7925_mcu_disconnect(struct mt7925_dev *dev);
void mt7925_rx_poll(struct mt7925_dev *dev);
int  mt7925_tx_frame(struct mt7925_dev *dev,
		     const void *frame, uint16_t len);

#endif /* MT7925_DRV_H */
