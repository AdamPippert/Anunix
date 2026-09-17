/*
 * mt7925_drv.h — MT7925 internal driver state (shared across files).
 *
 * File map:
 *   mt7925.c        probe, bring-up order, TX tokens, RX dispatch, public API
 *   mt7925_fw.c     DMA rings and firmware download
 *   mt7925_mcu.c    unified-command transport, events, command wrappers
 *   mt7925_sta.c    station: scan, join, 4-way handshake, keys
 *   mt7925_uni.c    command encodings        (host-tested)
 *   mt7925_mac.c    TXD/RXD encodings        (host-tested)
 *   mt7925_ieee.c   802.11 frames            (host-tested)
 *   mt7925_eapol.c  WPA2-PSK handshakes      (host-tested)
 */

#ifndef MT7925_DRV_H
#define MT7925_DRV_H

#include <anx/types.h>
#include <anx/mt7925.h>
#include "mt7925_uni.h"
#include "mt7925_ieee.h"
#include "mt7925_eapol.h"

#define MT7925_SSID_LEN		32
#define MT7925_PSK_LEN		64

/*
 * WTBL layout (mt792x.h): index 0 is the global entry, stations are
 * allocated from 1, and each interface owns WTBL_RESERVED - bss_idx for
 * its broadcast/own entry.
 */
#define MT7925_WTBL_SIZE	20
#define MT7925_WCID_GLOBAL	0
#define MT7925_WCID_AP		1
#define MT7925_WCID_BSS		(MT7925_WTBL_SIZE - 1)
#define MT7925_BSS_IDX		0
#define MT7925_OMAC_IDX		0
#define MT7925_MUAR_BSS		0x0e

#define MT7925_SCAN_MAX		32

/* Frames the RX path hands to the station for processing outside a wait. */
#define MT7925_STA_QUEUE	8
#define MT7925_FRAME_MAX	1600

struct mt7925_frame {
	uint16_t len;
	bool     is_eapol;		/* payload starts at the EAPOL header */
	uint8_t  data[MT7925_FRAME_MAX];
};

struct mt7925_dev {
	void     *bar0;
	void     *bar2;
	uint8_t   mac[6];
	anx_mt7925_state_t state;

	/* NIC capability */
	uint8_t   nss;
	bool      has_2g;
	bool      has_5g;
	bool      has_6g;
	uint64_t  chip_cap;

	/* CLC blobs inside the embedded WM image (mt7925_load_clc) */
	const uint8_t *clc[2];
	uint8_t   hw_encap;

	/* channel grant */
	uint8_t   band_idx;		/* 0xff until a grant names one */
	uint8_t   roc_token;
	bool      roc_granted;
	uint8_t   roc_channel;

	/* scan */
	bool      scan_active;
	bool      scan_done;
	uint8_t   scan_seq;
	struct mt7925_bss scan[MT7925_SCAN_MAX];
	uint32_t  n_scan;

	/* association */
	struct mt7925_bss bss;
	struct mt7925_rate_info rates;
	uint16_t  aid;
	uint8_t   cipher;		/* pairwise, MT_CIPHER_* once keyed */
	bool      keyed;
	bool      beacon_lost;
	bool      fw_assert;
	uint16_t  disconnect_reason;
	struct mt7925_eapol eapol;
	char      ssid[MT7925_SSID_LEN + 1];
	char      psk[MT7925_PSK_LEN + 1];

	/* frames waiting for the station */
	struct mt7925_frame queue[MT7925_STA_QUEUE];
	uint32_t  q_head;
	uint32_t  q_tail;

	/* counters */
	uint32_t  rx_frames;
	uint32_t  rx_data;
	uint32_t  rx_dropped;
	uint32_t  tx_frames;
	uint32_t  tx_dropped;
	uint32_t  tx_reclaimed;
	uint32_t  tx_acked;
	uint32_t  tx_failed;
	uint32_t  rx_undecrypted;	/* protected, not decrypted */
	uint32_t  rx_port_closed;	/* data before authorization */
	uint32_t  trace_budget;		/* data frames still to log */
	uint32_t  fw_log_lines;
};

extern struct mt7925_dev g_mt7925;

/* ---- mt7925_fw.c ---------------------------------------------------- */

/* Bring up the DMA rings and download patch and WM firmware. */
int  mt7925_fw_download(struct mt7925_dev *dev);

/* Next 4-bit command sequence number (never zero). */
uint8_t mt7925_mcu_next_seq(void);

/* The DMA-reachable command buffer and its capacity. */
uint8_t *mt7925_cmd_buf(uint32_t *cap);

/* Send len bytes already framed in the command buffer on the WM ring. */
int  mt7925_wm_send(uint32_t len);

/* Queue one TXWI (TXD+TXP) on the data ring; ANX_EBUSY when full. */
int  mt7925_data_tx_push(uint32_t txwi_phys, uint32_t len);

/* Take one received buffer from the event or data ring, or NULL. */
const uint8_t *mt7925_evt_poll(uint32_t *out_len);
const uint8_t *mt7925_data_rx_poll(uint32_t *out_len);

/* ---- mt7925_mcu.c --------------------------------------------------- */

/*
 * Send a unified command whose body is in msg (built over the buffer
 * mt7925_mcu_body() returned). When rsp is non-NULL the reply body is
 * copied there. Returns 0, a negative ANX error, or the positive
 * firmware status for commands that report one.
 */
int  mt7925_mcu_send(uint16_t cid, uint8_t option, struct mt7925_msg *msg,
		     bool wait, uint8_t *rsp, uint32_t rsp_cap,
		     uint32_t *rsp_len);

/* Start a command body in the command buffer, after the descriptor. */
void mt7925_mcu_body(struct mt7925_msg *msg);

/* True while a command waits for its response. */
bool mt7925_mcu_busy(void);

/* Handle one packet of type RX_EVENT from either ring. */
void mt7925_mcu_rx_event(struct mt7925_dev *dev, const uint8_t *buf,
			 uint32_t len);

int  mt7925_mcu_run_firmware(struct mt7925_dev *dev);
int  mt7925_mcu_set_eeprom(void);
int  mt7925_mcu_chip_config(const char *cmd);
int  mt7925_mcu_set_channel_domain(struct mt7925_dev *dev);
int  mt7925_mcu_set_rts_thresh(uint32_t val);
int  mt7925_mcu_set_rate_txpower(struct mt7925_dev *dev);
int  mt7925_mcu_set_clc(struct mt7925_dev *dev, const char alpha2[2]);
int  mt7925_mcu_set_rxfilter(uint32_t fif, uint8_t bit_op, uint32_t bit_map);
int  mt7925_mcu_add_dev(struct mt7925_dev *dev, bool enable);
int  mt7925_mcu_add_bss_info(const struct mt7925_bss_params *p);
int  mt7925_mcu_sta_update(const struct mt7925_sta_params *p);
int  mt7925_mcu_sta_hdr_trans(const struct mt7925_sta_params *p);
int  mt7925_mcu_add_key(const struct mt7925_key_params *p);
int  mt7925_mcu_set_roc(struct mt7925_dev *dev, uint8_t channel,
			uint8_t band, uint32_t duration_ms);
int  mt7925_mcu_abort_roc(struct mt7925_dev *dev);
int  mt7925_mcu_hw_scan(struct mt7925_dev *dev, const uint8_t *ssid,
			uint8_t ssid_len);
int  mt7925_mcu_set_tx(const struct mt7925_edca_params ac[4]);
int  mt7925_mcu_set_timing(uint16_t slot_time);
int  mt7925_mcu_set_beacon_filter(struct mt7925_dev *dev, bool enable);

/* ---- mt7925.c ------------------------------------------------------- */

/* Drain both RX rings; frames for the station are queued, not handled. */
void mt7925_service(struct mt7925_dev *dev);

/* A timeout on the 100 Hz tick, or on counted 1 ms delays without one. */
struct mt7925_timer {
	uint64_t tick0;
	uint32_t ms;
	uint32_t iter;
};

void mt7925_timer_start(struct mt7925_timer *t, uint32_t ms);

/* Sleep about 1 ms and report whether the timeout has passed. */
bool mt7925_timer_expired(struct mt7925_timer *t);

/* Bring a probed, owned chip from firmware download to a ready interface. */
int  mt7925_bring_up(struct mt7925_dev *dev);

/* Wait up to ms milliseconds, servicing the rings, until *flag is set. */
bool mt7925_wait_flag(struct mt7925_dev *dev, const bool *flag,
		      uint32_t ms);

/* Transmit an 802.11 management frame to the AP entry. */
int  mt7925_tx_mgmt(struct mt7925_dev *dev, const uint8_t *frame,
		    uint32_t len, uint16_t wlan_idx);

/* Transmit an EAPOL payload to the AP as an 802.11 data frame. */
int  mt7925_tx_eapol(struct mt7925_dev *dev, const uint8_t *payload,
		     uint32_t len, bool protect);

/* Transmit an Ethernet II frame through the AP entry. */
int  mt7925_tx_eth(struct mt7925_dev *dev, const uint8_t *frame,
		   uint32_t len);

/* Clear or set a WTBL entry's admission counters (mt7925_mac_wtbl_update). */
void mt7925_mac_wtbl_clear(uint16_t idx);

/* Pop one queued frame for the station; false when empty. */
bool mt7925_queue_pop(struct mt7925_dev *dev, struct mt7925_frame *out);

/* ---- mt7925_sta.c --------------------------------------------------- */

/* Record a beacon or probe response seen during a scan. */
void mt7925_sta_scan_result(struct mt7925_dev *dev, const uint8_t *frame,
			    uint32_t len, const struct mt7925_bss *seen);

/* Scan, join and key a network. Blocking. */
int  mt7925_sta_connect(struct mt7925_dev *dev, const char *ssid,
			const char *psk);

/* Leave the network. */
void mt7925_sta_disconnect(struct mt7925_dev *dev, uint16_t reason);

/* Process queued frames after association (rekeys, deauth). */
void mt7925_sta_poll(struct mt7925_dev *dev);

/* Scan and print what was found. */
int  mt7925_sta_scan_print(struct mt7925_dev *dev);

/* Weak hooks: override in higher-level code to receive state notifications. */
void mt7925_on_connect(const char *ssid)    __attribute__((weak));
void mt7925_on_disconnect(void)             __attribute__((weak));

#endif /* MT7925_DRV_H */
