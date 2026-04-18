/*
 * xdna.c — AMD XDNA NPU driver (Ryzen AI / AI Engine).
 *
 * Supports the AI Engine NPU found in Ryzen AI SoCs:
 *   NPU1 (Phoenix/Hawk Point): 1022:1502
 *   NPU2 (Rembrandt):          1022:1638
 *   NPU4/5 (Strix Point):      1022:17f0  ← Ryzen AI HX 370
 *
 * Driver lifecycle:
 *   anx_xdna_init()           — PCI probe + MMIO map + engine register
 *   anx_xdna_load_firmware()  — PSP mailbox → firmware running
 *   anx_xdna_submit()         — DMA ring → execute inference job
 *
 * All register access uses the identity-mapped BAR0 virtual address.
 * The XDNA BAR0 size is typically 4 MiB (0x400000 bytes).
 *
 * Firmware:
 *   AMD distributes NPU firmware as "npu.sbin" (signed binary ELF) via
 *   linux-firmware.  In Anunix the payload is stored in the credential
 *   store under the name "xdna-firmware".  Load it with:
 *     anx> secret set xdna-firmware <binary-payload>
 *   or provision it via the boot command line:
 *     -append "cred:xdna-firmware=<hex-payload>"
 */

#include <anx/types.h>
#include <anx/xdna.h>
#include <anx/pci.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/credential.h>
#include <anx/engine.h>
#include <anx/irq.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Device table                                                        */
/* ------------------------------------------------------------------ */

struct xdna_device_entry {
	uint16_t    device_id;
	const char *gen_name;
	uint32_t    aie_col_count;
	uint32_t    tops_int8;
};

static const struct xdna_device_entry xdna_devices[] = {
	{ XDNA_DEV_NPU1_PHOENIX,  "NPU1 Phoenix/Hawk Point", 4,  16 },
	{ XDNA_DEV_NPU1_PHOENIX2, "NPU1 Phoenix2",           2,   8 },
	{ XDNA_DEV_NPU2_REMBRANDT,"NPU2 Rembrandt",          4,  12 },
	{ XDNA_DEV_NPU4_STRIX,    "NPU4/5 Strix Point",      8,  50 },
};

#define XDNA_DEVICE_COUNT \
	(sizeof(xdna_devices) / sizeof(xdna_devices[0]))

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

static struct anx_xdna_dev xdna_dev;
static bool xdna_found;

/* ------------------------------------------------------------------ */
/* MMIO helpers                                                        */
/* ------------------------------------------------------------------ */

static inline uint32_t xdna_read32(uint32_t offset)
{
	return *(volatile uint32_t *)(xdna_dev.bar0 + offset);
}

static inline void xdna_write32(uint32_t offset, uint32_t val)
{
	*(volatile uint32_t *)(xdna_dev.bar0 + offset) = val;
}

/* ------------------------------------------------------------------ */
/* PSP mailbox                                                         */
/* ------------------------------------------------------------------ */

/*
 * Send a command to the PSP and wait for a response.
 *
 * The mailbox protocol:
 *   1. Write command struct to XDNA_MBOX_CMD_BASE.
 *   2. Write 1 to XDNA_MBOX_IPC_TRIGGER to notify PSP.
 *   3. Poll XDNA_MBOX_IPC_STATUS until PSP clears the trigger bit.
 *   4. Read response from XDNA_MBOX_RESP_BASE.
 *
 * Timeout: 100 000 poll iterations (~1 ms at 100 MHz register clock).
 */
static int xdna_mbox_send(const void *cmd, uint32_t cmd_size,
			   void *resp, uint32_t resp_size)
{
	const uint8_t *src = cmd;
	volatile uint8_t *cmd_mem = xdna_dev.bar0 + XDNA_MBOX_CMD_BASE;
	volatile uint8_t *resp_mem = xdna_dev.bar0 + XDNA_MBOX_RESP_BASE;
	uint32_t i;

	if (cmd_size > 512 || resp_size > 512)
		return ANX_EINVAL;

	/* Copy command into mailbox command buffer */
	for (i = 0; i < cmd_size; i++)
		cmd_mem[i] = src[i];

	/* Trigger PSP (write 1 to IPC trigger register) */
	xdna_write32(XDNA_MBOX_IPC_TRIGGER, 1);

	/* Poll for completion */
	for (i = 0; i < 100000; i++) {
		uint32_t status = xdna_read32(XDNA_MBOX_IPC_STATUS);

		if (!(status & 1)) {
			/* PSP cleared the trigger — response is ready */
			if (resp && resp_size > 0) {
				uint8_t *dst = resp;
				volatile uint8_t *src_m = resp_mem;
				uint32_t j;

				for (j = 0; j < resp_size; j++)
					dst[j] = src_m[j];
			}
			return ANX_OK;
		}
		/* Lightweight spin — avoid tight loop hammering */
		__asm__ volatile("" ::: "memory");
	}

	kprintf("xdna: PSP mailbox timeout\n");
	return ANX_ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/* Firmware loading                                                    */
/* ------------------------------------------------------------------ */

int anx_xdna_load_firmware(void)
{
	uint8_t fw_buf[65536];		/* max 64 KiB firmware in credential */
	uint32_t fw_len = 0;
	struct xdna_mbox_load_fw cmd;
	struct xdna_mbox_header resp;
	int ret;

	if (!xdna_found)
		return ANX_ENODEV;
	if (xdna_dev.fw_state == XDNA_FW_READY)
		return ANX_OK;

	/* Firmware must be pre-loaded into the credential store */
	ret = anx_credential_read("xdna-firmware", fw_buf, sizeof(fw_buf),
				  &fw_len);
	if (ret != ANX_OK) {
		kprintf("xdna: firmware not found in credential store\n");
		kprintf("xdna: run: secret set xdna-firmware <npu.sbin>\n");
		xdna_dev.fw_state = XDNA_FW_ERROR;
		return ANX_ENOENT;
	}

	kprintf("xdna: loading firmware (%u bytes)...\n", fw_len);
	xdna_dev.fw_state = XDNA_FW_LOADING;

	/*
	 * The firmware binary must be mapped into physical memory before
	 * the PSP can access it via DMA.  For now we pass the virtual
	 * address directly (identity-mapped in Anunix kernel).
	 */
	anx_memset(&cmd, 0, sizeof(cmd));
	cmd.hdr.total_size       = (uint32_t)sizeof(cmd);
	cmd.hdr.protocol_version = XDNA_MBOX_PROTO_VERSION;
	cmd.hdr.opcode           = XDNA_PSP_CMD_LOAD_FIRMWARE;
	cmd.hdr.message_id       = 1;
	cmd.fw_phys_addr         = (uint64_t)(uintptr_t)fw_buf;
	cmd.fw_size_bytes        = fw_len;

	ret = xdna_mbox_send(&cmd, sizeof(cmd), &resp, sizeof(resp));
	if (ret != ANX_OK) {
		xdna_dev.fw_state = XDNA_FW_ERROR;
		return ret;
	}

	if (resp.status != XDNA_PSP_OK) {
		kprintf("xdna: PSP firmware load failed (status 0x%x)\n",
			resp.status);
		xdna_dev.fw_state = XDNA_FW_ERROR;
		return ANX_EIO;
	}

	xdna_dev.fw_state = XDNA_FW_READY;
	kprintf("xdna: firmware ready\n");

	/* Transition engine to AVAILABLE */
	if (xdna_dev.engine)
		anx_engine_transition(xdna_dev.engine, ANX_ENGINE_AVAILABLE);

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* DMA ring setup                                                      */
/* ------------------------------------------------------------------ */

static int xdna_ring_init(void)
{
	xdna_dev.cmd_ring = anx_alloc(
		sizeof(struct xdna_dma_cmd) * XDNA_DMA_RING_ENTRIES);
	xdna_dev.comp_ring = anx_alloc(
		sizeof(struct xdna_dma_comp) * XDNA_DMA_RING_ENTRIES);
	if (!xdna_dev.cmd_ring || !xdna_dev.comp_ring) {
		anx_free(xdna_dev.cmd_ring);
		anx_free(xdna_dev.comp_ring);
		return ANX_ENOMEM;
	}

	anx_memset(xdna_dev.cmd_ring, 0,
		   sizeof(struct xdna_dma_cmd) * XDNA_DMA_RING_ENTRIES);
	anx_memset(xdna_dev.comp_ring, 0,
		   sizeof(struct xdna_dma_comp) * XDNA_DMA_RING_ENTRIES);

	xdna_dev.cmd_head  = 0;
	xdna_dev.cmd_tail  = 0;
	xdna_dev.comp_head = 0;
	xdna_dev.next_job_id = 1;

	/* Program ring base addresses into DMA registers */
	{
		uint64_t cmd_phys  = (uint64_t)(uintptr_t)xdna_dev.cmd_ring;

		xdna_write32(XDNA_DMA_CMD_BASE_LO,  (uint32_t)cmd_phys);
		xdna_write32(XDNA_DMA_CMD_BASE_HI,  (uint32_t)(cmd_phys >> 32));
		xdna_write32(XDNA_DMA_CMD_SIZE, XDNA_DMA_RING_ENTRIES);
		xdna_write32(XDNA_DMA_CMD_HEAD, 0);
		xdna_write32(XDNA_DMA_CMD_TAIL, 0);
	}

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Inference submission                                                */
/* ------------------------------------------------------------------ */

int anx_xdna_submit(const void *input, uint32_t input_size,
		    void *output, uint32_t output_size,
		    uint32_t partition_id, uint32_t flags)
{
	struct xdna_dma_cmd *cmd;
	uint32_t next_tail;
	uint32_t job_id;

	if (!xdna_found || xdna_dev.fw_state != XDNA_FW_READY)
		return ANX_ENODEV;

	next_tail = (xdna_dev.cmd_tail + 1) % XDNA_DMA_RING_ENTRIES;
	if (next_tail == xdna_dev.cmd_head)
		return ANX_EBUSY;

	job_id = xdna_dev.next_job_id++;
	cmd = &xdna_dev.cmd_ring[xdna_dev.cmd_tail];

	cmd->input_phys    = (uint64_t)(uintptr_t)input;
	cmd->input_size    = input_size;
	cmd->output_phys   = (uint64_t)(uintptr_t)output;
	cmd->output_size   = output_size;
	cmd->partition_id  = partition_id;
	cmd->job_id        = job_id;
	cmd->flags         = flags;

	xdna_dev.cmd_tail = next_tail;

	/* Ring the doorbell */
	xdna_write32(XDNA_DMA_CMD_TAIL, xdna_dev.cmd_tail);
	xdna_write32(XDNA_DMA_DOORBELL, 1);

	if (flags & XDNA_JOB_FLAG_SYNC) {
		/*
		 * Poll completion ring for our job_id.
		 * Timeout: 1 000 000 iterations (~10 ms at typical NPU speed).
		 */
		uint32_t i;

		for (i = 0; i < 1000000; i++) {
			uint32_t hw_tail = xdna_read32(XDNA_DMA_COMP_TAIL);

			while (xdna_dev.comp_head != hw_tail) {
				struct xdna_dma_comp *comp =
					&xdna_dev.comp_ring[xdna_dev.comp_head];

				if (comp->job_id == job_id) {
					xdna_dev.comp_head =
						(xdna_dev.comp_head + 1) %
						XDNA_DMA_RING_ENTRIES;
					return (comp->status == 0) ?
						ANX_OK : ANX_EIO;
				}
				xdna_dev.comp_head =
					(xdna_dev.comp_head + 1) %
					XDNA_DMA_RING_ENTRIES;
			}
			__asm__ volatile("" ::: "memory");
		}
		return ANX_ETIMEDOUT;
	}

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Info / status                                                       */
/* ------------------------------------------------------------------ */

static const char *xdna_fw_state_name(enum xdna_fw_state s)
{
	switch (s) {
	case XDNA_FW_UNLOADED: return "unloaded";
	case XDNA_FW_LOADING:  return "loading";
	case XDNA_FW_READY:    return "ready";
	case XDNA_FW_ERROR:    return "error";
	}
	return "unknown";
}

void anx_xdna_info(void)
{
	uint32_t hw_ver;

	if (!xdna_found) {
		kprintf("xdna: no XDNA NPU detected\n");
		return;
	}

	hw_ver = xdna_read32(XDNA_REG_VERSION);

	kprintf("=== AMD XDNA NPU ===\n");
	kprintf("  Generation : %s\n",    xdna_dev.gen_name);
	kprintf("  PCI        : %02x:%02x.%x  [%04x:%04x]\n",
		xdna_dev.bus, xdna_dev.slot, xdna_dev.func,
		xdna_dev.vendor, xdna_dev.device);
	kprintf("  BAR0       : 0x%x (size %u KiB)\n",
		(uint32_t)xdna_dev.bar0_phys,
		xdna_dev.bar0_size / 1024);
	kprintf("  HW version : 0x%x\n",  hw_ver);
	kprintf("  AIE columns: %u\n",    xdna_dev.aie_col_count);
	kprintf("  INT8 TOPS  : ~%u\n",   xdna_dev.tops_int8);
	kprintf("  Firmware   : %s\n",    xdna_fw_state_name(xdna_dev.fw_state));

	if (xdna_dev.fw_state == XDNA_FW_READY) {
		kprintf("  DMA ring   : cmd=%u/%u  comp=%u\n",
			xdna_dev.cmd_tail, XDNA_DMA_RING_ENTRIES,
			xdna_dev.comp_head);
	} else {
		kprintf("  Hint: 'secret set xdna-firmware <npu.sbin>' then\n");
		kprintf("        'xdna load' to activate the NPU.\n");
	}
}

bool anx_xdna_present(void)
{
	return xdna_found;
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

int anx_xdna_init(void)
{
	struct anx_pci_device *pci = NULL;
	const struct xdna_device_entry *ent = NULL;
	uint32_t i;
	int ret;

	/* Probe for any supported XDNA device */
	for (i = 0; i < XDNA_DEVICE_COUNT && !pci; i++) {
		pci = anx_pci_find_device(XDNA_VENDOR,
					  xdna_devices[i].device_id);
		if (pci)
			ent = &xdna_devices[i];
	}

	if (!pci) {
		/* Not present — silent return (system may not have XDNA) */
		return ANX_ENODEV;
	}

	kprintf("xdna: found %s [%04x:%04x] at %02x:%02x.%x\n",
		ent->gen_name, XDNA_VENDOR, pci->device_id,
		pci->bus, pci->slot, pci->func);

	anx_memset(&xdna_dev, 0, sizeof(xdna_dev));
	xdna_dev.vendor       = XDNA_VENDOR;
	xdna_dev.device       = pci->device_id;
	xdna_dev.bus          = pci->bus;
	xdna_dev.slot         = pci->slot;
	xdna_dev.func         = pci->func;
	xdna_dev.revision     = pci->revision;
	xdna_dev.gen_name     = ent->gen_name;
	xdna_dev.aie_col_count = ent->aie_col_count;
	xdna_dev.tops_int8    = ent->tops_int8;
	xdna_dev.fw_state     = XDNA_FW_UNLOADED;

	/* Map BAR0 (identity-mapped in Anunix 4 GiB window) */
	xdna_dev.bar0_phys = pci->bar[0] & ~0xFULL;
	xdna_dev.bar0_size = 0x400000;		/* 4 MiB */
	xdna_dev.bar0 = (volatile uint8_t *)(uintptr_t)xdna_dev.bar0_phys;

	if (xdna_dev.bar0_phys == 0) {
		kprintf("xdna: BAR0 not mapped (BIOS resource assignment?)\n");
		return ANX_EIO;
	}

	/* Enable bus mastering for DMA */
	anx_pci_enable_bus_master(pci);

	/* Allocate and configure DMA rings */
	ret = xdna_ring_init();
	if (ret != ANX_OK) {
		kprintf("xdna: ring init failed (%d)\n", ret);
		return ret;
	}

	/* Register as a kernel engine (REGISTERED state — not AVAILABLE
	 * until firmware loads) */
	ret = anx_engine_register("amd-xdna-npu",
				  ANX_ENGINE_DEVICE_SERVICE,
				  ANX_CAP_TENSOR_NPU |
				  ANX_CAP_TENSOR_INT8 |
				  ANX_CAP_TENSOR_BF16,
				  &xdna_dev.engine);
	if (ret == ANX_OK && xdna_dev.engine) {
		xdna_dev.engine->quality_score = 90;
		xdna_dev.engine->is_local      = true;
		anx_engine_transition(xdna_dev.engine, ANX_ENGINE_REGISTERED);
	}

	xdna_found = true;
	kprintf("xdna: driver ready (%u AIE columns, %u INT8 TOPS)\n",
		xdna_dev.aie_col_count, xdna_dev.tops_int8);
	kprintf("xdna: firmware not loaded -- run 'xdna load' to activate\n");

	return ANX_OK;
}
