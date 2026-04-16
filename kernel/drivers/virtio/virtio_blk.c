/*
 * virtio_blk.c — Virtio block device driver.
 *
 * Drives a virtio-blk PCI device using the legacy PIO transport.
 * Provides synchronous sector-level read/write for the object store
 * and installer.
 */

#include <anx/types.h>
#include <anx/virtio.h>
#include <anx/virtio_blk.h>
#include <anx/pci.h>
#include <anx/irq.h>
#include <anx/io.h>
#include <anx/page.h>
#include <anx/alloc.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Virtio-blk feature bits */
#define VIRTIO_BLK_F_SIZE_MAX	(1 << 1)
#define VIRTIO_BLK_F_SEG_MAX	(1 << 2)
#define VIRTIO_BLK_F_GEOMETRY	(1 << 4)
#define VIRTIO_BLK_F_RO		(1 << 5)
#define VIRTIO_BLK_F_BLK_SIZE	(1 << 6)

/* Virtio-blk request types */
#define VIRTIO_BLK_T_IN		0	/* read */
#define VIRTIO_BLK_T_OUT	1	/* write */

/* Virtio-blk request header (16 bytes) */
struct virtio_blk_req_hdr {
	uint32_t type;		/* VIRTIO_BLK_T_IN or _OUT */
	uint32_t reserved;
	uint64_t sector;
} __attribute__((packed));

/* Status byte written by device at end of request */
#define VIRTIO_BLK_S_OK		0
#define VIRTIO_BLK_S_IOERR	1
#define VIRTIO_BLK_S_UNSUPP	2

/* PCI vendor/device for virtio-blk (legacy) */
#define VIRTIO_VENDOR		0x1AF4
#define VIRTIO_BLK_DEVICE	0x1001

#define SECTOR_SIZE		512
#define REQ_QUEUE_IDX		0

/* Driver state */
static struct {
	struct anx_virtio_dev vdev;
	struct anx_virtqueue req_vq;
	uint64_t capacity;	/* total sectors */
	bool ready;
} blkdev;

static void virtio_blk_irq(uint32_t irq, void *arg)
{
	(void)irq;
	(void)arg;
	anx_virtio_isr(&blkdev.vdev);
}

int anx_virtio_blk_init(void)
{
	struct anx_pci_device *pci;
	int ret;

	pci = anx_pci_find_device(VIRTIO_VENDOR, VIRTIO_BLK_DEVICE);
	if (!pci)
		return ANX_ENOENT;

	ret = anx_virtio_init(&blkdev.vdev, pci);
	if (ret != ANX_OK)
		return ret;

	/* No special features needed */
	anx_virtio_set_features(&blkdev.vdev, 0);

	/* Read capacity from device config (offset 0, 8 bytes) */
	{
		uint32_t lo, hi;

		lo = anx_virtio_config_read32(&blkdev.vdev, 0);
		hi = anx_virtio_config_read32(&blkdev.vdev, 4);
		blkdev.capacity = ((uint64_t)hi << 32) | lo;
	}

	/* Set up request queue */
	ret = anx_virtio_setup_vq(&blkdev.vdev, REQ_QUEUE_IDX,
				   &blkdev.req_vq);
	if (ret != ANX_OK)
		return ret;

	/* Register IRQ */
	if (pci->irq_line > 0 && pci->irq_line < 16) {
		anx_irq_register(pci->irq_line, virtio_blk_irq, NULL);
		anx_irq_unmask(pci->irq_line);
	}

	anx_virtio_driver_ok(&blkdev.vdev);
	blkdev.ready = true;

	kprintf("virtio-blk: %u MiB on irq %u\n",
		(uint32_t)(blkdev.capacity * SECTOR_SIZE / (1024 * 1024)),
		(uint32_t)pci->irq_line);

	return ANX_OK;
}

/*
 * Submit a single block request and poll for completion.
 *
 * Virtio-blk uses a 3-descriptor chain per request:
 *   desc[0]: header (device-readable)
 *   desc[1]: data buffer (readable for write, writable for read)
 *   desc[2]: status byte (device-writable)
 */
static int blk_request(uint32_t type, uint64_t sector,
			uint32_t count, void *buf)
{
	struct virtio_blk_req_hdr *hdr;
	uint8_t *status_byte;
	uint32_t data_len = count * SECTOR_SIZE;
	uint16_t head, d1, d2;
	uint64_t start;

	if (!blkdev.ready)
		return ANX_EIO;

	/* Allocate header and status byte */
	hdr = anx_zalloc(sizeof(*hdr));
	if (!hdr)
		return ANX_ENOMEM;
	status_byte = anx_zalloc(1);
	if (!status_byte) {
		anx_free(hdr);
		return ANX_ENOMEM;
	}

	hdr->type = type;
	hdr->reserved = 0;
	hdr->sector = sector;
	*status_byte = 0xFF;	/* sentinel */

	/*
	 * Build a 3-descriptor chain manually since anx_vq_add_buf
	 * only adds single descriptors. We need to chain them.
	 */
	{
		struct anx_virtqueue *vq = &blkdev.req_vq;

		if (vq->num_free < 3) {
			anx_free(hdr);
			anx_free(status_byte);
			return ANX_EBUSY;
		}

		/* Allocate 3 descriptors from free list */
		head = vq->free_head;
		d1 = vq->desc[head].next;
		d2 = vq->desc[d1].next;
		vq->free_head = vq->desc[d2].next;
		vq->num_free -= 3;

		/* desc[0]: header (device reads) */
		vq->desc[head].addr = (uint64_t)(uintptr_t)hdr;
		vq->desc[head].len = sizeof(*hdr);
		vq->desc[head].flags = VIRTQ_DESC_F_NEXT;
		vq->desc[head].next = d1;

		/* desc[1]: data buffer */
		vq->desc[d1].addr = (uint64_t)(uintptr_t)buf;
		vq->desc[d1].len = data_len;
		vq->desc[d1].flags = VIRTQ_DESC_F_NEXT;
		if (type == VIRTIO_BLK_T_IN)
			vq->desc[d1].flags |= VIRTQ_DESC_F_WRITE;
		vq->desc[d1].next = d2;

		/* desc[2]: status byte (device writes) */
		vq->desc[d2].addr = (uint64_t)(uintptr_t)status_byte;
		vq->desc[d2].len = 1;
		vq->desc[d2].flags = VIRTQ_DESC_F_WRITE;
		vq->desc[d2].next = 0;

		/* Store private data for chain head */
		vq->priv[head] = hdr;

		/* Add to available ring */
		vq->avail->ring[vq->avail->idx % vq->num] = head;
		arch_wmb();
		vq->avail->idx++;
		arch_wmb();

		/* Kick the device */
		anx_vq_kick(vq);
	}

	/* Poll for completion (up to 5 seconds) */
	start = arch_timer_ticks();
	while (*status_byte == 0xFF) {
		if (arch_timer_ticks() - start > 500) {
			anx_free(hdr);
			anx_free(status_byte);
			return ANX_ETIMEDOUT;
		}
		arch_rmb();
	}

	/* Reclaim descriptors */
	{
		struct anx_virtqueue *vq = &blkdev.req_vq;
		uint32_t dummy_len;

		anx_vq_get_used(vq, &dummy_len);
	}

	{
		int ret = (*status_byte == VIRTIO_BLK_S_OK)
			  ? ANX_OK : ANX_EIO;

		anx_free(hdr);
		anx_free(status_byte);
		return ret;
	}
}

int anx_blk_read(uint64_t sector, uint32_t count, void *buf)
{
	if (sector + count > blkdev.capacity)
		return ANX_EINVAL;
	return blk_request(VIRTIO_BLK_T_IN, sector, count, buf);
}

int anx_blk_write(uint64_t sector, uint32_t count, const void *buf)
{
	if (sector + count > blkdev.capacity)
		return ANX_EINVAL;
	return blk_request(VIRTIO_BLK_T_OUT, sector, count, (void *)buf);
}

uint64_t anx_blk_capacity(void)
{
	return blkdev.capacity;
}

bool anx_blk_ready(void)
{
	return blkdev.ready;
}
