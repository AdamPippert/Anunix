/*
 * anx/xdna.h — AMD XDNA NPU driver (Ryzen AI / AI Engine).
 *
 * The XDNA NPU is AMD's AI Engine (AIE) array embedded in Ryzen AI
 * SoCs.  It appears as a PCIe device with vendor 0x1022 and one of
 * several device IDs depending on the SoC generation.
 *
 * Programming model:
 *   1. Driver maps BAR0 (primary MMIO control registers).
 *   2. Firmware is uploaded to the NPU via the PSP (Platform Security
 *      Processor) mailbox.  The PSP validates, loads, and starts the
 *      XDNA firmware image ("npu.sbin").
 *   3. Once firmware is running, the host submits inference jobs via
 *      DMA descriptor rings.  Each job references input/output tensors
 *      that have already been mapped into the NPU's address space.
 *   4. Completion arrives as an MSI interrupt or via polling a doorbell
 *      register in BAR0.
 *
 * Anunix integration:
 *   - Registers as an ANX_ENGINE_DEVICE_SERVICE with ANX_CAP_TENSOR_NPU.
 *   - The tensor dispatch layer (kernel/core/tensor/tensor_ops.c) selects
 *     this engine for INT8/BF16 operations when it is AVAILABLE.
 *   - On first use, anx_xdna_load_firmware() must be called to put the
 *     engine into AVAILABLE state.
 */

#ifndef ANX_XDNA_H
#define ANX_XDNA_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Supported device IDs                                               */
/* ------------------------------------------------------------------ */

#define XDNA_VENDOR			0x1022	/* AMD */

/*
 * NPU1 — Phoenix / Hawk Point (Ryzen AI 7040 / 8040 series).
 * Fabricated on TSMC 4nm.  16 AIE tiles per column, 4 columns = 64 tiles.
 * TOPS: ~16 INT8.
 */
#define XDNA_DEV_NPU1_PHOENIX		0x1502

/*
 * NPU4/5 — Strix Point (Ryzen AI 300 series, including HX 370).
 * Fabricated on TSMC 4nm.  50 AIE tiles.  TOPS: ~50 INT8.
 * Subsystem revision 0x0000 = NPU4, 0x0010/0x0011 = NPU5.
 */
#define XDNA_DEV_NPU4_STRIX		0x17f0

/*
 * NPU2 — Rembrandt (Ryzen 6000).  AIE-ML generation, 4nm.
 * TOPS: ~12 INT8.
 */
#define XDNA_DEV_NPU2_REMBRANDT		0x1638

/*
 * Phoenix2 — Ryzen 8040U (OEM).  Smaller tile count than full Phoenix.
 */
#define XDNA_DEV_NPU1_PHOENIX2		0x17f1

/* ------------------------------------------------------------------ */
/* BAR0 register offsets                                              */
/* ------------------------------------------------------------------ */

/*
 * The BAR0 layout is divided into regions:
 *
 *  0x000000–0x07ffff   Management / PSP mailbox control
 *  0x080000–0x0fffff   AI Engine tile array registers
 *  0x100000–0x17ffff   DMA descriptor ring control
 *  0x380000–0x3fffff   Clock gating / power management
 */

/* Device version and identification */
#define XDNA_REG_VERSION		0x0000		/* device HW version */
#define XDNA_REG_STATUS			0x0004		/* device status */
#define XDNA_REG_CAPABILITY		0x0008		/* HW capability flags */

/* PSP mailbox for firmware loading */
#define XDNA_MBOX_BASE			0x3800000	/* BAR0 offset */
#define XDNA_MBOX_SIZE			0x1000
#define XDNA_MBOX_IPC_TRIGGER		(XDNA_MBOX_BASE + 0x100)
#define XDNA_MBOX_IPC_STATUS		(XDNA_MBOX_BASE + 0x104)
#define XDNA_MBOX_CMD_BASE		(XDNA_MBOX_BASE + 0x200)
#define XDNA_MBOX_RESP_BASE		(XDNA_MBOX_BASE + 0x400)

/* AI Engine tile control */
#define XDNA_AIE_COL_ENABLE		0x080000	/* column enable bitmask */
#define XDNA_AIE_CLK_GATE		0x080010	/* clock gate control */
#define XDNA_AIE_RESET			0x080020	/* tile array reset */

/* DMA ring control */
#define XDNA_DMA_CMD_HEAD		0x100000	/* command ring head */
#define XDNA_DMA_CMD_TAIL		0x100004	/* command ring tail */
#define XDNA_DMA_CMD_BASE_LO		0x100008	/* ring phys addr [31:0] */
#define XDNA_DMA_CMD_BASE_HI		0x10000C	/* ring phys addr [63:32] */
#define XDNA_DMA_CMD_SIZE		0x100010	/* ring entry count */
#define XDNA_DMA_COMP_HEAD		0x100020	/* completion ring head */
#define XDNA_DMA_COMP_TAIL		0x100024	/* completion ring tail */
#define XDNA_DMA_DOORBELL		0x100030	/* submit doorbell */

/* ------------------------------------------------------------------ */
/* PSP mailbox protocol                                               */
/* ------------------------------------------------------------------ */

/* Command opcodes (sent to PSP) */
#define XDNA_PSP_CMD_LOAD_FIRMWARE	0x01	/* load NPU firmware ELF */
#define XDNA_PSP_CMD_QUERY_VERSION	0x02	/* query firmware version */
#define XDNA_PSP_CMD_CREATE_PARTITION	0x10	/* allocate AIE partition */
#define XDNA_PSP_CMD_FREE_PARTITION	0x11
#define XDNA_PSP_CMD_EXECUTE		0x20	/* submit inference job */
#define XDNA_PSP_CMD_ABORT		0x21	/* abort in-flight job */

/* Status codes returned by PSP */
#define XDNA_PSP_OK			0x00
#define XDNA_PSP_ERR_BAD_OPCODE		0x01
#define XDNA_PSP_ERR_FW_INVALID		0x02
#define XDNA_PSP_ERR_BUSY		0x03

struct xdna_mbox_header {
	uint32_t total_size;		/* size of entire message including header */
	uint16_t protocol_version;	/* always XDNA_MBOX_PROTO_VERSION */
	uint8_t  opcode;
	uint8_t  status;		/* response status (PSP→host) */
	uint32_t message_id;		/* echoed in response */
};

#define XDNA_MBOX_PROTO_VERSION		0x0001

struct xdna_mbox_load_fw {
	struct xdna_mbox_header hdr;
	uint64_t fw_phys_addr;		/* physical address of firmware image */
	uint32_t fw_size_bytes;
	uint32_t reserved;
};

struct xdna_mbox_create_partition {
	struct xdna_mbox_header hdr;
	uint32_t col_start;		/* first AIE column (0-based) */
	uint32_t col_count;		/* number of columns */
	uint32_t priority;		/* 0=low, 1=normal, 2=realtime */
	uint32_t reserved;
};

/* ------------------------------------------------------------------ */
/* DMA descriptor ring                                                */
/* ------------------------------------------------------------------ */

#define XDNA_DMA_RING_ENTRIES		64	/* power of 2 */

struct xdna_dma_cmd {
	uint64_t input_phys;		/* physical address of input tensor */
	uint32_t input_size;
	uint64_t output_phys;		/* physical address of output tensor */
	uint32_t output_size;
	uint32_t partition_id;		/* which partition to run on */
	uint32_t job_id;		/* host-assigned; returned in completion */
	uint32_t flags;			/* XDNA_JOB_* */
};

struct xdna_dma_comp {
	uint32_t job_id;		/* matches xdna_dma_cmd.job_id */
	uint32_t status;		/* 0=ok, else error code */
	uint64_t cycles;		/* AIE cycles consumed */
};

#define XDNA_JOB_FLAG_SYNC		(1U << 0)	/* wait for completion */

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

enum xdna_fw_state {
	XDNA_FW_UNLOADED = 0,	/* hardware found, firmware not loaded */
	XDNA_FW_LOADING,	/* PSP firmware load in progress */
	XDNA_FW_READY,		/* firmware running, engine available */
	XDNA_FW_ERROR,		/* firmware load failed */
};

struct anx_xdna_dev {
	/* PCI identity */
	uint16_t vendor;
	uint16_t device;
	uint8_t  bus, slot, func;
	uint8_t  revision;

	/* Hardware generation */
	const char *gen_name;		/* "NPU1 Phoenix", "NPU4 Strix", … */
	uint32_t    aie_col_count;	/* number of AIE columns */
	uint32_t    tops_int8;		/* nominal INT8 TOPS */

	/* MMIO */
	uint64_t bar0_phys;		/* BAR0 physical address */
	uint32_t bar0_size;
	volatile uint8_t *bar0;		/* BAR0 virtual (identity-mapped) */

	/* Firmware state */
	enum xdna_fw_state fw_state;
	uint32_t fw_version;		/* filled after firmware loads */

	/* Engine registry entry */
	struct anx_engine *engine;

	/* DMA rings (allocated from kernel heap) */
	struct xdna_dma_cmd *cmd_ring;
	struct xdna_dma_comp *comp_ring;
	uint32_t cmd_head, cmd_tail;
	uint32_t comp_head;
	uint32_t next_job_id;
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Probe for XDNA device.  Returns 0 if found, ANX_ENODEV otherwise. */
int anx_xdna_init(void);

/* Load NPU firmware from a credential named 'xdna-firmware'.
 * The credential payload must be a valid AMD NPU ELF binary.
 * Returns 0 on success; call after anx_xdna_init(). */
int anx_xdna_load_firmware(void);

/* Submit one inference job.  Blocks until completion if XDNA_JOB_FLAG_SYNC
 * is set.  Returns 0 on success, ANX_EBUSY if ring is full. */
int anx_xdna_submit(const void *input, uint32_t input_size,
		    void *output, uint32_t output_size,
		    uint32_t partition_id, uint32_t flags);

/* Print current driver + hardware status to the kernel console. */
void anx_xdna_info(void);

/* True if an XDNA device was found and driver initialized. */
bool anx_xdna_present(void);

#endif /* ANX_XDNA_H */
