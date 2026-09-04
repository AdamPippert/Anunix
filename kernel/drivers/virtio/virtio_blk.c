/*
 * virtio_blk.c — Virtio block device driver.
 *
 * Drives virtio-blk PCI devices using the legacy PIO transport.
 * Provides synchronous sector-level read/write for the object store
 * and installer. Every virtio-blk function found gets its own instance,
 * so QEMU guests with several drives can build a RAID array.
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
#include <anx/list.h>

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
#define VIRTIO_BLK_DEVICE_MODERN	0x1042

#define SECTOR_SIZE		512
#define REQ_QUEUE_IDX		0

/* Maximum virtio-blk functions bound in one boot */
#define VIRTIO_BLK_MAX_DEVS	8

/* Per-device state — one instance per virtio-blk function */
struct virtio_blk_dev {
	struct anx_virtio_dev vdev;
	struct anx_virtqueue req_vq;
	uint64_t capacity;	/* total sectors */
	bool ready;
};

static struct virtio_blk_dev blkdevs[VIRTIO_BLK_MAX_DEVS];
static uint32_t blkdev_count;
static bool virtio_blk_probed;

/* Forward declarations for ops table defined at end of file */
static int virtio_blk_read(struct anx_blk_dev *dev, uint64_t sector,
			   uint32_t count, void *buf);
static int virtio_blk_write(struct anx_blk_dev *dev, uint64_t sector,
			    uint32_t count, const void *buf);
static uint64_t virtio_blk_capacity(struct anx_blk_dev *dev);

static const struct anx_blk_ops virtio_ops = {
	.read     = virtio_blk_read,
	.write    = virtio_blk_write,
	.capacity = virtio_blk_capacity,
	.name     = "virtio-blk",
};

static void virtio_blk_irq(uint32_t irq, void *arg)
{
	struct virtio_blk_dev *bd = arg;

	(void)irq;
	anx_virtio_isr(&bd->vdev);
}

/* Bring up one virtio-blk function and register it as a block device. */
static int virtio_blk_bind(struct anx_pci_device *pci)
{
	struct virtio_blk_dev *bd;
	int ret;

	if (blkdev_count >= VIRTIO_BLK_MAX_DEVS)
		return ANX_EFULL;

	bd = &blkdevs[blkdev_count];
	anx_memset(bd, 0, sizeof(*bd));

	ret = anx_virtio_init(&bd->vdev, pci);
	if (ret != ANX_OK)
		return ret;

	/* No special features needed */
	anx_virtio_set_features(&bd->vdev, 0);

	/* Read capacity from device config (offset 0, 8 bytes) */
	{
		uint32_t lo, hi;

		lo = anx_virtio_config_read32(&bd->vdev, 0);
		hi = anx_virtio_config_read32(&bd->vdev, 4);
		bd->capacity = ((uint64_t)hi << 32) | lo;
	}

	/* Set up request queue */
	ret = anx_virtio_setup_vq(&bd->vdev, REQ_QUEUE_IDX, &bd->req_vq);
	if (ret != ANX_OK)
		return ret;

	/* Register IRQ */
	if (pci->irq_line > 0 && pci->irq_line < 16) {
		anx_irq_register(pci->irq_line, virtio_blk_irq, bd);
		anx_irq_unmask(pci->irq_line);
	}

	anx_virtio_driver_ok(&bd->vdev);
	bd->ready = true;

	kprintf("virtio-blk: %u MiB on irq %u\n",
		(uint32_t)(bd->capacity * SECTOR_SIZE / (1024 * 1024)),
		(uint32_t)pci->irq_line);

	if (!anx_blk_dev_register(&virtio_ops, bd, "vblk")) {
		bd->ready = false;
		return ANX_EFULL;
	}
	blkdev_count++;
	return ANX_OK;
}

int anx_virtio_blk_init(void)
{
	struct anx_list_head *devlist;
	struct anx_list_head *pos;

	/* driver_table calls init once per matching PCI function; the whole
	 * bus is walked on the first call, so later calls are no-ops. */
	if (virtio_blk_probed)
		return blkdev_count > 0 ? ANX_OK : ANX_ENOENT;
	virtio_blk_probed = true;

	devlist = anx_pci_device_list();
	ANX_LIST_FOR_EACH(pos, devlist) {
		struct anx_pci_device *pci =
			ANX_LIST_ENTRY(pos, struct anx_pci_device, link);

		if (pci->vendor_id != VIRTIO_VENDOR)
			continue;
		if (pci->device_id != VIRTIO_BLK_DEVICE &&
		    pci->device_id != VIRTIO_BLK_DEVICE_MODERN)
			continue;

		virtio_blk_bind(pci);
	}
	return blkdev_count > 0 ? ANX_OK : ANX_ENOENT;
}

/*
 * Submit a single block request and poll for completion.
 *
 * Virtio-blk uses a 3-descriptor chain per request:
 *   desc[0]: header (device-readable)
 *   desc[1]: data buffer (readable for write, writable for read)
 *   desc[2]: status byte (device-writable)
 */
static int blk_request(struct virtio_blk_dev *bd, uint32_t type,
			uint64_t sector, uint32_t count, void *buf)
{
	struct virtio_blk_req_hdr *hdr;
	uint8_t *status_byte;
	uint32_t data_len = count * SECTOR_SIZE;
	uint16_t head, d1, d2;
	uint64_t start;

	if (!bd->ready)
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
		struct anx_virtqueue *vq = &bd->req_vq;

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

	/* Reclaim all 3 descriptors in the chain */
	{
		struct anx_virtqueue *vq = &bd->req_vq;
		uint32_t dummy_len;

		/* Consume the used ring entry */
		anx_vq_get_used(vq, &dummy_len);

		/* Return d1 and d2 to free list (get_used only freed head) */
		vq->desc[d2].next = vq->free_head;
		vq->free_head = d1;
		vq->desc[d1].next = d2;
		vq->num_free += 2;
	}

	{
		int ret = (*status_byte == VIRTIO_BLK_S_OK)
			  ? ANX_OK : ANX_EIO;

		anx_free(hdr);
		anx_free(status_byte);
		return ret;
	}
}

static int virtio_blk_read(struct anx_blk_dev *dev, uint64_t sector,
			   uint32_t count, void *buf)
{
	struct virtio_blk_dev *bd = dev->priv;

	if (sector + count > bd->capacity)
		return ANX_EINVAL;
	return blk_request(bd, VIRTIO_BLK_T_IN, sector, count, buf);
}

static int virtio_blk_write(struct anx_blk_dev *dev, uint64_t sector,
			    uint32_t count, const void *buf)
{
	struct virtio_blk_dev *bd = dev->priv;

	if (sector + count > bd->capacity)
		return ANX_EINVAL;
	return blk_request(bd, VIRTIO_BLK_T_OUT, sector, count, (void *)buf);
}

static uint64_t virtio_blk_capacity(struct anx_blk_dev *dev)
{
	struct virtio_blk_dev *bd = dev->priv;

	return bd->capacity;
}
