/*
 * mt7925_sim.h — A simulated MT7925 firmware and access point for host
 * tests of the station path.
 *
 * mt7925_sim.c replaces mt7925_fw.c in the host build. It answers unified
 * commands the way the WM firmware does (responses carrying the sequence
 * number, unsolicited scan-done and channel-grant events, TX-free reports),
 * and it plays the access points it is given: beacons during a scan, Open
 * System authentication, association, and the authenticator side of the
 * WPA2 4-way and group key handshakes.
 */

#ifndef MT7925_SIM_H
#define MT7925_SIM_H

#include <anx/types.h>

#define SIM_MAX_CMDS		512
#define SIM_CMD_BODY		2048

struct sim_ap {
	uint8_t     bssid[6];
	const char *ssid;
	uint8_t     band;		/* MT_BAND_* */
	uint8_t     channel;
	int8_t      rssi;
	const char *passphrase;		/* NULL for an open network */
	bool        pmf_required;
	uint16_t    assoc_status;	/* 0 accepts the association */
	bool        ignore_auth;	/* never answer authentication */
	bool        reject_m2;		/* deauth (reason 15) instead of M3 */
};

struct sim_cmd {
	uint16_t cid;
	uint8_t  option;
	uint32_t len;
	uint8_t  body[SIM_CMD_BODY];
};

struct sim_state {
	uint32_t n_cmds;
	uint32_t tx_mgmt;
	uint32_t tx_eapol;
	uint32_t tx_8023;
	uint32_t tx_protected_8023;
	uint32_t auth_seen;
	uint32_t assoc_seen;
	uint32_t deauth_seen;
	bool     m2_mic_ok;
	bool     handshake_done;
	bool     rekey_done;
	bool     last_eapol_protected;
	uint8_t  tk[16];
	uint8_t  gtk[16];
	uint8_t  gtk_idx;
	uint8_t  assoc_req[512];
	uint32_t assoc_req_len;
	uint8_t  last_txwi[64];
	bool     free_tokens;
};

extern struct sim_state g_sim;

/* Forget every AP, command and frame; tokens are freed by default. */
void sim_reset(void);

/* Add an access point (at most four). */
void sim_add_ap(const struct sim_ap *ap);

/* The i-th command the driver sent. */
const struct sim_cmd *sim_cmd(uint32_t i);

/* Index of the first command with cid at or after start, or -1. */
int sim_find_cmd(uint16_t cid, uint32_t start);

/* Deliver an Ethernet II frame from the connected AP, header-translated. */
void sim_inject_eth(const uint8_t *frame, uint32_t len);

/* The connected AP deauthenticates the station. */
void sim_inject_deauth(uint16_t reason);

/* The firmware reports beacon loss. */
void sim_inject_beacon_loss(void);

/* The connected AP starts a group key handshake with a new GTK. */
void sim_start_rekey(void);

/* A fake BAR0 large enough for every fixed-map register. */
void *sim_bar(void);

#endif /* MT7925_SIM_H */
