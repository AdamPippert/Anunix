/*
 * test_mt7925_sta.c — MT7925 bring-up and station path against a
 * simulated firmware and access point (tests/harness/mt7925_sim.c).
 *
 * These run the driver's own code for everything above the DMA rings:
 * command transport, event handling, bring-up order, scan, join, the
 * 4-way and group key handshakes, key installation, the data path and
 * teardown.
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include "../kernel/drivers/net/wifi/mt7925_drv.h"
#include "harness/mt7925_sim.h"

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			kprintf("  FAIL: %s (line %d)\n", (msg), __LINE__); \
			return -1;					\
		}							\
	} while (0)

#define PASS	"correct horse battery"
#define SSID	"Anunix-Test"

static const uint8_t sta_mac[6] = { 0x46, 0x3b, 0x72, 0x47, 0xb2, 0x5c };
static const uint8_t bssid_5g[6]  = { 0x02, 0x00, 0x00, 0xaa, 0x00, 0x01 };
static const uint8_t bssid_2g[6]  = { 0x02, 0x00, 0x00, 0xaa, 0x00, 0x02 };
static const uint8_t bssid_pmf[6] = { 0x02, 0x00, 0x00, 0xaa, 0x00, 0x03 };

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/* Tag of the TLV at the start of a body after a header of hdr bytes. */
static uint16_t first_tag(const struct sim_cmd *c, uint32_t hdr)
{
	return c->len >= hdr + 4 ? le16(c->body + hdr) : 0xffff;
}

/* Find a TLV by tag inside a command body; NULL if absent. */
static const uint8_t *find_tlv(const struct sim_cmd *c, uint32_t hdr,
			       uint16_t tag)
{
	uint32_t pos = hdr;

	while (pos + 4 <= c->len) {
		uint16_t t = le16(c->body + pos), l = le16(c->body + pos + 2);

		if (l < 4)
			return NULL;
		if (t == tag)
			return c->body + pos;
		pos += l;
	}
	return NULL;
}

static struct mt7925_dev *fresh_dev(void)
{
	struct mt7925_dev *dev = &g_mt7925;

	anx_memset(dev, 0, sizeof(*dev));
	dev->bar0 = sim_bar();
	dev->band_idx = 0xff;
	return dev;
}

static void add_home_aps(bool with_pmf)
{
	struct sim_ap ap;

	anx_memset(&ap, 0, sizeof(ap));
	anx_memcpy(ap.bssid, bssid_5g, 6);
	ap.ssid = SSID;
	ap.band = MT_BAND_5G;
	ap.channel = 36;
	ap.rssi = -45;
	ap.passphrase = PASS;
	sim_add_ap(&ap);

	anx_memcpy(ap.bssid, bssid_2g, 6);
	ap.band = MT_BAND_2G;
	ap.channel = 6;
	ap.rssi = -70;
	sim_add_ap(&ap);

	if (with_pmf) {
		anx_memcpy(ap.bssid, bssid_pmf, 6);
		ap.band = MT_BAND_5G;
		ap.channel = 149;
		ap.rssi = -30;
		ap.pmf_required = true;
		sim_add_ap(&ap);
	}
}

static int test_bring_up(void)
{
	static const uint16_t head[] = {
		MT_UNI_CMD_CHIP_CONFIG, MT_UNI_CMD_EFUSE_CTRL,
		MT_UNI_CMD_WSYS_CONFIG, MT_UNI_CMD_EFUSE_CTRL,
		MT_UNI_CMD_CHIP_CONFIG, MT_UNI_CMD_CHIP_CONFIG,
		MT_UNI_CMD_CHIP_CONFIG, MT_UNI_CMD_SET_DOMAIN_INFO,
		MT_UNI_CMD_BAND_CONFIG, MT_UNI_CMD_SET_POWER_LIMIT,
		MT_UNI_CMD_SET_DOMAIN_INFO,
	};
	static const uint16_t tail[] = {
		MT_UNI_CMD_DEV_INFO_UPDATE, MT_UNI_CMD_BSS_INFO_UPDATE,
		MT_UNI_CMD_BAND_CONFIG, MT_UNI_CMD_EDCA_UPDATE,
		MT_UNI_CMD_BSS_INFO_UPDATE,
	};
	struct mt7925_dev *dev;
	const struct sim_cmd *c;
	uint32_t i, n;

	sim_reset();
	dev = fresh_dev();
	CHECK(mt7925_bring_up(dev) == ANX_OK, "bring-up succeeds");

	CHECK(anx_memcmp(dev->mac, sta_mac, 6) == 0, "MAC from NIC capability");
	CHECK(dev->nss == 2 && dev->has_2g && dev->has_5g, "PHY capability");
	CHECK(dev->hw_encap == 1 && dev->clc[0] && !dev->clc[1], "CLC found");

	n = sizeof(head) / sizeof(head[0]);
	for (i = 0; i < n; i++)
		CHECK(sim_cmd(i)->cid == head[i], "bring-up command order");
	CHECK(sim_cmd(0)->option == 0x06, "capability query sent as set, no ack");
	CHECK(sim_cmd(1)->option == MT_UNI_OPT_QUERY_ACK, "efuse query");
	CHECK(sim_cmd(3)->option == MT_UNI_OPT_SET_ACK, "eeprom mode set");
	CHECK(sim_cmd(4)->body[16] == 'T' && sim_cmd(6)->body[16] == 'K',
	      "thermal and deep-sleep strings");

	c = sim_cmd(9);
	CHECK(first_tag(c, 4) == 3 && le16(c->body + 10) == 6 &&
	      c->body[16] == 'U' && c->body[17] == 'S' &&
	      c->body[84] == 0x11 && c->body[89] == 0x66, "CLC segment sent");

	/* 5 batches for 14 channels at 2.4 GHz, 16 for 46 at 5 GHz */
	for (i = 11; i < 11 + 21; i++)
		CHECK(sim_cmd(i)->cid == MT_UNI_CMD_SET_POWER_LIMIT &&
		      first_tag(sim_cmd(i), 4) == 1, "power table batches");
	CHECK(sim_cmd(11)->body[12] == 3 && sim_cmd(11)->body[13] == 1 &&
	      sim_cmd(11)->body[14] == 0, "first 2.4 GHz batch");
	CHECK(sim_cmd(15)->body[12] == 2 && sim_cmd(15)->body[14] == 1,
	      "last 2.4 GHz batch");
	/* At 5 GHz the CCK slots stay 127; OFDM starts at entry 4. */
	CHECK(sim_cmd(16)->body[13] == 2 && sim_cmd(16)->body[52] == 36 &&
	      (int8_t)sim_cmd(16)->body[53] == 127 &&
	      (int8_t)sim_cmd(16)->body[53 + 4] == 46 &&
	      sim_cmd(16)->body[52 + 450] == 38 &&
	      (int8_t)sim_cmd(16)->body[53 + 450 + 4] == 127,
	      "5 GHz per-channel power");
	CHECK(sim_cmd(31)->body[12] == 1 && sim_cmd(31)->body[14] == 1,
	      "last 5 GHz batch");

	for (i = 0; i < sizeof(tail) / sizeof(tail[0]); i++)
		CHECK(sim_cmd(32 + i)->cid == tail[i], "interface commands");
	CHECK(g_sim.n_cmds == 37, "bring-up command count");
	CHECK(anx_memcmp(sim_cmd(32)->body + 10, sta_mac, 6) == 0,
	      "interface address");
	CHECK(le16(sim_cmd(33)->body + 6) == 32 && sim_cmd(33)->body[8] == 1,
	      "BSS added active");
	CHECK(le16(sim_cmd(36)->body + 4) == 23 &&
	      le16(sim_cmd(36)->body + 12) == 20, "long slot after open");
	dev->state = MT7925_STATE_FW_UP;
	return 0;
}

static int check_connected_sequence(uint32_t n0)
{
	const struct sim_cmd *c;
	const uint8_t *t;
	int i;

	i = sim_find_cmd(MT_UNI_CMD_SCAN_REQ, n0);
	CHECK(i >= 0, "scan requested");
	c = sim_cmd(i);
	t = find_tlv(c, 4, 10);
	CHECK(t && t[4] == 4 && t[5] == 1 && t[12] == 'A', "directed scan");

	/* sta_add: BSS (disabled) then STA_REC NONE */
	i = sim_find_cmd(MT_UNI_CMD_BSS_INFO_UPDATE, (uint32_t)i);
	c = sim_cmd(i);
	t = find_tlv(c, 4, 0);
	CHECK(t && t[12] == 1 && anx_memcmp(t + 14, bssid_5g, 6) == 0,
	      "BSS before auth is disconnected");
	CHECK(!find_tlv(c, 4, 2), "no RLM before association");
	CHECK(sim_cmd(i + 1)->cid == MT_UNI_CMD_STA_REC_UPDATE, "STA_REC NONE");
	t = find_tlv(sim_cmd(i + 1), 8, 7);
	CHECK(t && t[4] == 0, "state NONE");
	t = find_tlv(sim_cmd(i + 1), 8, 0x2b);
	CHECK(t && t[6] == 1, "no header translation yet");

	/* auth and assoc each bracket a channel grant */
	CHECK(sim_cmd(i + 2)->cid == MT_UNI_CMD_ROC &&
	      le16(sim_cmd(i + 2)->body + 4) == 0 &&
	      sim_cmd(i + 2)->body[10] == 36, "ROC for auth");
	CHECK(sim_cmd(i + 3)->cid == MT_UNI_CMD_ROC &&
	      le16(sim_cmd(i + 3)->body + 4) == 1, "ROC abort after auth");
	CHECK(sim_cmd(i + 4)->cid == MT_UNI_CMD_ROC &&
	      sim_cmd(i + 5)->cid == MT_UNI_CMD_ROC, "ROC around assoc");

	/* sta_assoc */
	c = sim_cmd(i + 6);
	CHECK(c->cid == MT_UNI_CMD_BSS_INFO_UPDATE && find_tlv(c, 4, 2) &&
	      find_tlv(c, 4, 0)[12] == 0, "BSS enabled with RLM");
	c = sim_cmd(i + 7);
	t = find_tlv(c, 8, 7);
	CHECK(c->cid == MT_UNI_CMD_STA_REC_UPDATE && t && t[4] == 2 &&
	      find_tlv(c, 8, 0x20), "STA_REC ASSOC with MLD");
	t = find_tlv(c, 8, 0);
	CHECK(t && le16(t + 10) == 1 && le16(t + 18) == 1, "aid, not newly");
	c = sim_cmd(i + 8);
	CHECK(c->cid == MT_UNI_CMD_STA_REC_UPDATE && c->body[1] == 19 &&
	      c->body[5] == 0x0e && c->len == 16, "own entry update");
	CHECK(sim_cmd(i + 9)->cid == MT_UNI_CMD_BSS_INFO_UPDATE &&
	      first_tag(sim_cmd(i + 9), 4) == 22, "beacon filter");
	CHECK(sim_cmd(i + 10)->cid == MT_UNI_CMD_BAND_CONFIG &&
	      sim_cmd(i + 10)->body[20] == 1, "drop other beacons");
	CHECK(le16(sim_cmd(i + 11)->body + 12) == 9, "short slot on 5 GHz");
	CHECK(sim_cmd(i + 12)->cid == MT_UNI_CMD_EDCA_UPDATE, "EDCA");
	return i + 13;
}

static int test_connect(void)
{
	struct mt7925_dev *dev = &g_mt7925;
	const struct sim_cmd *c;
	const uint8_t *t;
	uint32_t n0;
	int i, ret;

	sim_reset();
	add_home_aps(true);
	n0 = g_sim.n_cmds;
	ret = mt7925_sta_connect(dev, SSID, PASS);
	CHECK(ret == ANX_OK, "connect succeeds");
	CHECK(dev->state == MT7925_STATE_CONNECTED && dev->keyed,
	      "connected and keyed");
	CHECK(anx_memcmp(dev->bss.bssid, bssid_5g, 6) == 0,
	      "strongest usable BSS chosen over the PMF-only one");
	CHECK(dev->n_scan == 3 && dev->aid == 1, "scan table, aid");
	CHECK(g_sim.auth_seen == 1 && g_sim.assoc_seen == 1, "one of each");
	CHECK(g_sim.m2_mic_ok && g_sim.handshake_done, "4-way handshake");
	CHECK(g_sim.tx_eapol == 2 && !g_sim.last_eapol_protected,
	      "M2 and M4 in the clear");

	/* association request */
	CHECK(le16(g_sim.assoc_req + 24) & 0x0010, "privacy requested");
	{
		uint32_t pos = 28;
		bool rsn = false;

		while (pos + 2 <= g_sim.assoc_req_len) {
			if (g_sim.assoc_req[pos] == 48 &&
			    g_sim.assoc_req[pos + 7] == 4)
				rsn = true;
			pos += 2 + g_sim.assoc_req[pos + 1];
		}
		CHECK(rsn && pos == g_sim.assoc_req_len, "RSN in assoc req");
	}

	i = check_connected_sequence(n0);
	if (i < 0)
		return -1;

	/* keys */
	c = sim_cmd(i);
	t = find_tlv(c, 4, 16);
	CHECK(c->cid == MT_UNI_CMD_BSS_INFO_UPDATE && t && t[6] == 4,
	      "BSS carries the cipher before the first key");
	c = sim_cmd(i + 1);
	t = find_tlv(c, 8, 0x27);
	CHECK(c->cid == MT_UNI_CMD_STA_REC_UPDATE && t && t[4] == 1 &&
	      t[5] == 1 && t[15] == 4 && t[17] == 16 && t[18] == 1,
	      "pairwise key record");
	CHECK(anx_memcmp(t + 20, g_sim.tk, 16) == 0, "pairwise key = AP TK");
	c = sim_cmd(i + 2);
	t = find_tlv(c, 8, 0x27);
	CHECK(t && c->body[1] == 19 && t[5] == 0 && t[16] == 1 &&
	      t[18] == 19 && anx_memcmp(t + 20, g_sim.gtk, 16) == 0,
	      "group key record");
	c = sim_cmd(i + 3);
	t = find_tlv(c, 8, 0x2b);
	CHECK(c->body[1] == 1 && t && t[6] == 0 && c->len == 16,
	      "header translation after authorization");
	CHECK((uint32_t)(i + 4) == g_sim.n_cmds, "nothing after that");
	return 0;
}

static int test_data_and_rekey(void)
{
	struct mt7925_dev *dev = &g_mt7925;
	uint8_t eth[60];
	uint32_t before;
	int k;

	anx_memset(eth, 0, sizeof(eth));
	anx_memcpy(eth, bssid_5g, 6);
	anx_memcpy(eth + 6, sta_mac, 6);
	eth[12] = 0x08;
	CHECK(mt7925_tx_eth(dev, eth, sizeof(eth)) == ANX_OK, "tx data");
	CHECK(g_sim.tx_8023 == 1 && g_sim.tx_protected_8023 == 1,
	      "802.3 frame, protected");
	CHECK(((g_sim.last_txwi[0] | (g_sim.last_txwi[1] << 8)) & 0xffff) ==
	      32 + sizeof(eth), "TXD length");

	before = dev->rx_data;
	anx_memcpy(eth, sta_mac, 6);
	anx_memcpy(eth + 6, bssid_5g, 6);
	sim_inject_eth(eth, sizeof(eth));
	mt7925_service(dev);
	CHECK(dev->rx_data == before + 1, "translated frame delivered");

	before = g_sim.n_cmds;
	sim_start_rekey();
	mt7925_service(dev);
	mt7925_sta_poll(dev);
	CHECK(g_sim.rekey_done && g_sim.last_eapol_protected,
	      "group handshake answered, protected");
	k = sim_find_cmd(MT_UNI_CMD_STA_REC_UPDATE, before);
	CHECK(k >= 0 && sim_cmd(k)->body[1] == 19, "rekeyed GTK installed");
	CHECK(find_tlv(sim_cmd(k), 8, 0x27)[16] == 2 &&
	      anx_memcmp(find_tlv(sim_cmd(k), 8, 0x27) + 20, g_sim.gtk, 16)
	      == 0, "new GTK index and key");

	/* token exhaustion reclaims instead of wedging */
	g_sim.free_tokens = false;
	before = dev->tx_reclaimed;
	for (k = 0; k < 70; k++)
		mt7925_tx_eth(dev, eth, sizeof(eth));
	CHECK(dev->tx_reclaimed == before + 6, "oldest tokens reclaimed");
	g_sim.free_tokens = true;
	return 0;
}

static int test_losses(void)
{
	struct mt7925_dev *dev = &g_mt7925;
	uint32_t before;
	int i;

	/* AP deauthenticates */
	before = g_sim.n_cmds;
	sim_inject_deauth(7);
	mt7925_service(dev);
	mt7925_sta_poll(dev);
	CHECK(dev->state == MT7925_STATE_FW_UP && !dev->keyed,
	      "deauth ends the association");
	CHECK(dev->disconnect_reason == 7, "reason recorded");
	i = sim_find_cmd(MT_UNI_CMD_STA_REC_UPDATE, before);
	CHECK(i >= 0 && sim_cmd(i)->body[1] == 19, "own entry first");
	i = sim_find_cmd(MT_UNI_CMD_STA_REC_UPDATE, (uint32_t)i + 1);
	CHECK(i >= 0 && find_tlv(sim_cmd(i), 8, 0x25), "station removed");

	/* reconnect, then lose beacons */
	sim_reset();
	add_home_aps(false);
	CHECK(mt7925_sta_connect(dev, SSID, PASS) == ANX_OK, "reconnect");
	sim_inject_beacon_loss();
	mt7925_service(dev);
	mt7925_sta_poll(dev);
	CHECK(dev->state == MT7925_STATE_FW_UP, "beacon loss disconnects");

	/* reconnect, then leave */
	CHECK(mt7925_sta_connect(dev, SSID, PASS) == ANX_OK, "third connect");
	mt7925_sta_disconnect(dev, 3);
	CHECK(dev->state == MT7925_STATE_FW_UP && g_sim.deauth_seen == 1,
	      "user disconnect sends deauth");
	return 0;
}

static int test_failures(void)
{
	struct mt7925_dev *dev = &g_mt7925;
	struct sim_ap ap;
	uint32_t before;
	int ret, i;

	anx_memset(&ap, 0, sizeof(ap));
	anx_memcpy(ap.bssid, bssid_5g, 6);
	ap.ssid = SSID;
	ap.band = MT_BAND_5G;
	ap.channel = 44;
	ap.rssi = -50;
	ap.passphrase = PASS;

	/* wrong passphrase: the AP gives up after message 2 */
	sim_reset();
	ap.reject_m2 = true;
	sim_add_ap(&ap);
	before = g_sim.n_cmds;
	ret = mt7925_sta_connect(dev, SSID, PASS);
	CHECK(ret == ANX_ECONNRESET && dev->disconnect_reason == 15,
	      "rejected handshake reported");
	CHECK(dev->state == MT7925_STATE_FW_UP && !dev->keyed, "state reset");
	i = sim_find_cmd(MT_UNI_CMD_STA_REC_UPDATE, before);
	while (i >= 0 && !find_tlv(sim_cmd(i), 8, 0x25))
		i = sim_find_cmd(MT_UNI_CMD_STA_REC_UPDATE, (uint32_t)i + 1);
	CHECK(i >= 0, "station removed after failure");

	/* passphrase too short never reaches the air */
	sim_reset();
	ap.reject_m2 = false;
	sim_add_ap(&ap);
	CHECK(mt7925_sta_connect(dev, SSID, "short") == ANX_EINVAL &&
	      g_sim.auth_seen == 0, "short passphrase refused");

	/* silent AP */
	sim_reset();
	ap.ignore_auth = true;
	sim_add_ap(&ap);
	CHECK(mt7925_sta_connect(dev, SSID, PASS) == ANX_ETIMEDOUT &&
	      g_sim.auth_seen == 3, "authentication retried three times");
	CHECK(dev->state == MT7925_STATE_FW_UP, "state after timeout");

	/* association refused */
	sim_reset();
	ap.ignore_auth = false;
	ap.assoc_status = 17;
	sim_add_ap(&ap);
	CHECK(mt7925_sta_connect(dev, SSID, PASS) == ANX_EPERM &&
	      g_sim.assoc_seen == 1, "refusal reported");

	/* nothing to join */
	sim_reset();
	CHECK(mt7925_sta_connect(dev, "Elsewhere", PASS) == ANX_ENOENT,
	      "missing network");

	/* open network */
	sim_reset();
	ap.assoc_status = 0;
	ap.passphrase = NULL;
	sim_add_ap(&ap);
	before = g_sim.n_cmds;
	CHECK(mt7925_sta_connect(dev, SSID, NULL) == ANX_OK &&
	      dev->state == MT7925_STATE_CONNECTED && !dev->keyed,
	      "open network joined");
	CHECK(g_sim.tx_eapol == 0, "no handshake on an open network");
	for (i = (int)before; i < (int)g_sim.n_cmds; i++)
		CHECK(!find_tlv(sim_cmd((uint32_t)i), 8, 0x27) ||
		      sim_cmd((uint32_t)i)->cid != MT_UNI_CMD_STA_REC_UPDATE,
		      "no keys on an open network");
	CHECK(mt7925_sta_connect(dev, SSID, PASS) == ANX_ENOENT,
	      "open network is not used for a passphrase");
	CHECK(dev->state == MT7925_STATE_FW_UP, "reconnect left the old one");

	/* scan listing */
	sim_reset();
	add_home_aps(true);
	CHECK(mt7925_sta_scan_print(dev) == ANX_OK && dev->n_scan == 3,
	      "scan lists every BSS");
	CHECK(dev->scan[0].rssi == -45 && dev->scan[0].channel == 36 &&
	      dev->scan[0].band == MT_BAND_5G, "scan entry details");
	return 0;
}

int test_mt7925_sta(void)
{
	if (test_bring_up())
		return -1;
	if (test_connect())
		return -2;
	if (test_data_and_rekey())
		return -3;
	if (test_losses())
		return -4;
	if (test_failures())
		return -5;
	return 0;
}
