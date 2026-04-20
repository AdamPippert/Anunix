/*
 * mt7925_reg.h — MT7925 (RZ717) register map.
 *
 * Based on the MT792x / ConnAC2 architecture as documented in the
 * Linux mt76 driver (drivers/net/wireless/mediatek/mt76/mt792x/).
 * All offsets are relative to BAR0 unless noted as chip-space addresses.
 */

#ifndef MT7925_REG_H
#define MT7925_REG_H

/* ------------------------------------------------------------------ */
/* PCI device identification                                           */
/* ------------------------------------------------------------------ */

#define MT7925_VENDOR_ID        0x14C3
#define MT7925_DEVICE_ID        0x0717

/* ------------------------------------------------------------------ */
/* BAR layout                                                          */
/* BAR0 = 2 MiB MMIO (main register window)                           */
/* BAR2 = 32 KiB MMIO (HIF window, secondary)                        */
/* ------------------------------------------------------------------ */

/* Connectivity infrastructure (accessed via PCIe remap window) */
#define MT_CONN_INFRA_BASE              0x18000000 /* chip-space */
#define MT_TOP_CFG_BASE                 0x80020000 /* chip-space */

/* PCIe remap window registers (within BAR0) */
#define MT_HIF_REMAP_L1                 0x260004
#define MT_HIF_REMAP_L2                 0x260008
#define MT_PCIE_REMAP_BASE4             0x260010

/* ------------------------------------------------------------------ */
/* Connectivity subsystem control (via PCIE remap window)              */
/* ------------------------------------------------------------------ */

#define MT_CONN_INFRA_CFG_BASE          0xd000
#define MT_CONN_HW_VER                  (MT_CONN_INFRA_CFG_BASE + 0x0000)
#define MT_CONN_FW_VER                  (MT_CONN_INFRA_CFG_BASE + 0x0004)
#define MT_CONN_CHIP_ID                 (MT_CONN_INFRA_CFG_BASE + 0x0008)

/* Top-level misc registers */
#define MT_TOP_MISC2                    0xe110
#define MT_TOP_MISC2_FW_STATE           0x7           /* bits [2:0] */
#define MT_FW_STATE_IDLE                0x00
#define MT_FW_STATE_RUNNING             0x04
#define MT_FW_STATE_FW_DOWNLOAD         0x01

/* MCU power control */
#define MT_MCU_BASE                     0x7000
#define MT_MCU_PCIE_REMAP_1             (MT_MCU_BASE + 0x500)
#define MT_MCU_PCIE_REMAP_2             (MT_MCU_BASE + 0x504)

/* WM MCU control */
#define MT_MCU_WM_BASE                  0x7800
#define MT_MCU_WM_INT_STATUS            (MT_MCU_WM_BASE + 0x00)
#define MT_MCU_WM_INT_MASK              (MT_MCU_WM_BASE + 0x04)

/* ------------------------------------------------------------------ */
/* WFDMA (WiFi DMA engine)                                             */
/* MT7925 uses WFDMA0 for both data and MCU communication.            */
/* ------------------------------------------------------------------ */

#define MT_WFDMA0_BASE                  0x4000

/* Global DMA config */
#define MT_WFDMA0_GLO_CFG               (MT_WFDMA0_BASE + 0x0208)
#define MT_WFDMA0_GLO_CFG_TX_DMA_EN     (1U << 0)
#define MT_WFDMA0_GLO_CFG_TX_DMA_BUSY   (1U << 1)
#define MT_WFDMA0_GLO_CFG_RX_DMA_EN     (1U << 2)
#define MT_WFDMA0_GLO_CFG_RX_DMA_BUSY   (1U << 3)
#define MT_WFDMA0_GLO_CFG_OMIT_TX_INFO  (1U << 28)
#define MT_WFDMA0_GLO_CFG_OMIT_RX_INFO  (1U << 27)
#define MT_WFDMA0_GLO_CFG_CSR_DISP_BASE (1U << 5)

/* DMA reset */
#define MT_WFDMA0_RST_DTX_PTR           (MT_WFDMA0_BASE + 0x020c)
#define MT_WFDMA0_RST_DRX_PTR           (MT_WFDMA0_BASE + 0x0280)

/* ------------------------------------------------------------------ */
/* TX ring registers                                                   */
/* Each ring occupies 0x40 bytes starting at BASE + 0x300 + n*0x40    */
/* ------------------------------------------------------------------ */

#define MT_WFDMA0_TX_RING_BASE(n)       (MT_WFDMA0_BASE + 0x300 + (n) * 0x40)
#define MT_WFDMA0_TX_RING_CNT(n)        (MT_WFDMA0_TX_RING_BASE(n) + 0x00)
#define MT_WFDMA0_TX_RING_ADDR(n)       (MT_WFDMA0_TX_RING_BASE(n) + 0x04)
#define MT_WFDMA0_TX_RING_CIDX(n)       (MT_WFDMA0_TX_RING_BASE(n) + 0x08)
#define MT_WFDMA0_TX_RING_DIDX(n)       (MT_WFDMA0_TX_RING_BASE(n) + 0x0C)

/* TX ring indices */
#define MT_DATA_TXRING                  0   /* normal data TX */
#define MT_MCU_FWDL_TXRING             3   /* firmware download */
#define MT_MCU_WA_TXRING               15  /* WA MCU commands */
#define MT_MCU_WM_TXRING               20  /* WM MCU commands */

/* ------------------------------------------------------------------ */
/* RX ring registers                                                   */
/* Each ring occupies 0x40 bytes starting at BASE + 0x500 + n*0x40    */
/* ------------------------------------------------------------------ */

#define MT_WFDMA0_RX_RING_BASE(n)       (MT_WFDMA0_BASE + 0x500 + (n) * 0x40)
#define MT_WFDMA0_RX_RING_CNT(n)        (MT_WFDMA0_RX_RING_BASE(n) + 0x00)
#define MT_WFDMA0_RX_RING_ADDR(n)       (MT_WFDMA0_RX_RING_BASE(n) + 0x04)
#define MT_WFDMA0_RX_RING_CIDX(n)       (MT_WFDMA0_RX_RING_BASE(n) + 0x08)
#define MT_WFDMA0_RX_RING_DIDX(n)       (MT_WFDMA0_RX_RING_BASE(n) + 0x0C)

/* RX ring indices */
#define MT_DATA_RXRING                  0   /* normal data RX */
#define MT_MCU_EVENT_RXRING             4   /* MCU events / FW download ACKs */

/* ------------------------------------------------------------------ */
/* DMA descriptor format                                               */
/* 16-byte descriptor (4 × u32) used for both TX and RX rings         */
/* ------------------------------------------------------------------ */

/* ctrl field bits */
#define MT_DMA_CTRL_SD_LEN0_MASK        0x0000ffff
#define MT_DMA_CTRL_LAST_SEC0           (1U << 19)
#define MT_DMA_CTRL_FIRST_SEC0          (1U << 20)
#define MT_DMA_CTRL_DMA_DONE            (1U << 31)  /* HW sets on TX done; driver sets on RX refill */

/* info field bits (TX) */
#define MT_DMA_INFO_TOKEN_MASK          0x1fff0000
#define MT_DMA_INFO_TOKEN_SHIFT         16
#define MT_DMA_INFO_PKT_TYPE_CMD        0x00000020  /* MCU command packet */

/* ------------------------------------------------------------------ */
/* MCU TXD header (prepended to all MCU commands)                     */
/* ------------------------------------------------------------------ */

/* pkt_type values */
#define MT_MCU_PKT_TYPE_CMD             0x20

/* set_query values */
#define MT_MCU_SET                      0x01
#define MT_MCU_QUERY                    0x00

/* MCU command IDs (ext_cid extension) */
#define MCU_EXT_CMD_PATCH_SEM_CONTROL   0x10
#define MCU_EXT_CMD_FW_SCATTER          0x04
#define MCU_EXT_CMD_PATCH_FINISH_REQ    0x07
#define MCU_CMD_FW_SCATTER              0x04  /* alias */

/* Patch semaphore values */
#define MT_PATCH_SEM_GET                1
#define MT_PATCH_SEM_RELEASE            0

/* Firmware download state flags returned in ACK */
#define MT_PATCH_STATUS_ACK             0x01
#define MT_FW_DL_DONE                   0x01

/* ------------------------------------------------------------------ */
/* Interrupt registers                                                 */
/* ------------------------------------------------------------------ */

#define MT_INT_SOURCE_CSR               (MT_WFDMA0_BASE + 0x0200)
#define MT_INT_MASK_CSR                 (MT_WFDMA0_BASE + 0x0204)
#define MT_INT_RX_DONE_DATA             (1U << 16)
#define MT_INT_RX_DONE_MCU_EVT          (1U << 20)
#define MT_INT_TX_DONE_DATA             (1U << 0)
#define MT_INT_TX_DONE_MCU_WM           (1U << 4)

/* ------------------------------------------------------------------ */
/* ConnAC2 firmware control registers                                  */
/* ------------------------------------------------------------------ */

/* Patch semaphore register (CONN_INFRA space, accessed via remap) */
#define MT_PATCH_SEM_OFFSET             0x00010c28

/* Firmware status/ready check */
#define MT_FW_READY_MAGIC               0x01234567

/* Host-MCU synchronization */
#define MT_HOST_IRQ_ENABLE              (MT_WFDMA0_BASE + 0x01f4)
#define MT_MCU_CMD_REG                  0xe314   /* scratchpad for MCU sync */
#define MT_MCU_CMD_STOP_DMA             (1U << 2)
#define MT_MCU_CMD_START_DMA            (1U << 3)

/* ------------------------------------------------------------------ */
/* WiFi MAC registers (after MCU boot)                                 */
/* ------------------------------------------------------------------ */

#define MT_WMAC_BASE                    0x0000
#define MT_WMAC_CTRL                    (MT_WMAC_BASE + 0x0004)

/* Band 0 (2.4 GHz / 5 GHz / 6 GHz) */
#define MT_WF_BAND0_BASE                0x2400
#define MT_WF_RFCR                      (MT_WF_BAND0_BASE + 0x0100)
#define MT_WF_RFCR_DROP_STBC_CTRL       (1U << 0)

#endif /* MT7925_REG_H */
