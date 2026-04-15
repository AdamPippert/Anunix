/*
 * virtio.c — Virtio legacy PIO transport layer.
 *
 * Implements the virtio 0.9.5 (legacy/transitional) transport using
 * I/O ports from PCI BAR0. Handles device reset, feature negotiation,
 * virtqueue allocation, and buffer management.
 */

#include <anx/types.h>
#include <anx/virtio.h>
#include <anx/pci.h>
#include <anx/io.h>
#include <anx/page.h>
#include <anx/alloc.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/*
 * Legacy virtqueue memory layout (must be physically contiguous):
 *
 *   desc[num]         — num * 16 bytes
 *   avail header      — 4 bytes (flags + idx)
 *   avail ring[num]   — num * 2 bytes
 *   avail used_event  — 2 bytes
 *   [padding to 4096-byte boundary]
 *   used header       — 4 bytes (flags + idx)
 *   used ring[num]    — num * 8 bytes
 *   used avail_event  — 2 bytes
 */
static size_t vq_mem_size(uint16_t num)
{
	size_t desc_sz = (size_t)num * sizeof(struct anx_vq_desc);
	size_t avail_sz = sizeof(struct anx_vq_avail) +
			  (size_t)num * sizeof(uint16_t) + 2;
	size_t used_sz = sizeof(struct anx_vq_used) +
			 (size_t)num * sizeof(struct anx_vq_used_elem) + 2;
	size_t align_off;

	/* Used ring starts at next 4096-byte boundary after desc + avail */
	align_off = (desc_sz + avail_sz + 4095) & ~(size_t)4095;
	return align_off + used_sz;
}

static uint32_t pages_for(size_t bytes)
{
	return (uint32_t)((bytes + ANX_PAGE_SIZE - 1) / ANX_PAGE_SIZE);
}

static uint32_t order_for_pages(uint32_t pages)
{
	uint32_t order = 0;

	while ((1u << order) < pages)
		order++;
	return order;
}

/* --- Device init --- */

int anx_virtio_init(struct anx_virtio_dev *vdev,
		    struct anx_pci_device *pci)
{
	uint16_t io_base;

	/* BAR0 is the I/O port base; bit 0 is the I/O space indicator */
	io_base = (uint16_t)(pci->bar[0] & ~3u);
	if (io_base == 0)
		return ANX_EIO;

	vdev->pci = pci;
	vdev->io_base = io_base;

	/* Enable PCI bus mastering for DMA */
	anx_pci_enable_bus_master(pci);

	/* Reset device */
	anx_outb(0, io_base + VIRTIO_PCI_STATUS);

	/* Acknowledge the device */
	anx_outb(VIRTIO_STATUS_ACKNOWLEDGE,
		 io_base + VIRTIO_PCI_STATUS);

	/* Tell device we know how to drive it */
	anx_outb(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER,
		 io_base + VIRTIO_PCI_STATUS);

	/* Read device features */
	vdev->host_features = anx_inl(io_base + VIRTIO_PCI_HOST_FEATURES);
	vdev->guest_features = 0;

	return ANX_OK;
}

uint32_t anx_virtio_get_features(struct anx_virtio_dev *vdev)
{
	return vdev->host_features;
}

void anx_virtio_set_features(struct anx_virtio_dev *vdev, uint32_t features)
{
	vdev->guest_features = features & vdev->host_features;
	anx_outl(vdev->guest_features,
		 vdev->io_base + VIRTIO_PCI_GUEST_FEATURES);
}

void anx_virtio_driver_ok(struct anx_virtio_dev *vdev)
{
	uint8_t status;

	status = anx_inb(vdev->io_base + VIRTIO_PCI_STATUS);
	status |= VIRTIO_STATUS_DRIVER_OK;
	anx_outb(status, vdev->io_base + VIRTIO_PCI_STATUS);
}

uint8_t anx_virtio_isr(struct anx_virtio_dev *vdev)
{
	return anx_inb(vdev->io_base + VIRTIO_PCI_ISR);
}

uint8_t anx_virtio_config_read8(struct anx_virtio_dev *vdev, uint16_t off)
{
	return anx_inb(vdev->io_base + VIRTIO_PCI_CONFIG + off);
}

uint16_t anx_virtio_config_read16(struct anx_virtio_dev *vdev, uint16_t off)
{
	return anx_inw(vdev->io_base + VIRTIO_PCI_CONFIG + off);
}

uint32_t anx_virtio_config_read32(struct anx_virtio_dev *vdev, uint16_t off)
{
	return anx_inl(vdev->io_base + VIRTIO_PCI_CONFIG + off);
}

/* --- Virtqueue setup --- */

int anx_virtio_setup_vq(struct anx_virtio_dev *vdev, uint16_t index,
			 struct anx_virtqueue *vq)
{
	uint16_t num;
	size_t mem_sz;
	uint32_t npages, order;
	uintptr_t phys;
	uint8_t *base;
	size_t desc_sz, avail_sz, align_off;
	uint16_t i;

	/* Select the queue */
	anx_outw(index, vdev->io_base + VIRTIO_PCI_QUEUE_SEL);

	/* Read queue size (set by device, power of 2) */
	num = anx_inw(vdev->io_base + VIRTIO_PCI_QUEUE_SIZE);
	if (num == 0)
		return ANX_EINVAL;

	/* Allocate physically contiguous, page-aligned memory */
	mem_sz = vq_mem_size(num);
	npages = pages_for(mem_sz);
	order = order_for_pages(npages);
	phys = anx_page_alloc(order);
	if (!phys)
		return ANX_ENOMEM;

	/* Zero the memory */
	base = (uint8_t *)phys;	/* identity-mapped */
	anx_memset(base, 0, (1u << order) * ANX_PAGE_SIZE);

	/* Compute ring layout */
	desc_sz = (size_t)num * sizeof(struct anx_vq_desc);
	avail_sz = sizeof(struct anx_vq_avail) +
		   (size_t)num * sizeof(uint16_t) + 2;
	align_off = (desc_sz + avail_sz + 4095) & ~(size_t)4095;

	vq->desc = (struct anx_vq_desc *)base;
	vq->avail = (struct anx_vq_avail *)(base + desc_sz);
	vq->used = (struct anx_vq_used *)(base + align_off);
	vq->num = num;
	vq->io_base = vdev->io_base;
	vq->queue_index = index;
	vq->last_used_idx = 0;

	/* Build free descriptor chain */
	for (i = 0; i < num - 1; i++) {
		vq->desc[i].next = i + 1;
		vq->desc[i].flags = VIRTQ_DESC_F_NEXT;
	}
	vq->desc[num - 1].next = 0;
	vq->desc[num - 1].flags = 0;
	vq->free_head = 0;
	vq->num_free = num;

	/* Per-descriptor private data array */
	vq->priv = anx_zalloc((size_t)num * sizeof(void *));
	if (!vq->priv) {
		anx_page_free(phys, order);
		return ANX_ENOMEM;
	}

	anx_spin_init(&vq->lock);

	/* Tell device where the queue lives (page frame number) */
	anx_outl((uint32_t)(phys / ANX_PAGE_SIZE),
		 vdev->io_base + VIRTIO_PCI_QUEUE_PFN);

	return ANX_OK;
}

/* --- Buffer management --- */

int anx_vq_add_buf(struct anx_virtqueue *vq, void *buf, uint32_t len,
		    uint16_t flags, void *priv)
{
	uint16_t head;

	if (vq->num_free == 0)
		return ANX_ENOMEM;

	head = vq->free_head;
	vq->free_head = vq->desc[head].next;
	vq->num_free--;

	vq->desc[head].addr = (uint64_t)(uintptr_t)buf;
	vq->desc[head].len = len;
	vq->desc[head].flags = flags;
	vq->desc[head].next = 0;
	vq->priv[head] = priv;

	/* Add to available ring */
	vq->avail->ring[vq->avail->idx % vq->num] = head;
	arch_wmb();
	vq->avail->idx++;
	arch_wmb();

	return (int)head;
}

void anx_vq_kick(struct anx_virtqueue *vq)
{
	arch_mb();
	anx_outw(vq->queue_index, vq->io_base + VIRTIO_PCI_QUEUE_NOTIFY);
}

void *anx_vq_get_used(struct anx_virtqueue *vq, uint32_t *len_out)
{
	uint16_t idx;
	struct anx_vq_used_elem *elem;
	uint16_t desc_id;
	void *priv;

	arch_rmb();
	if (vq->last_used_idx == vq->used->idx)
		return NULL;

	idx = vq->last_used_idx % vq->num;
	elem = &vq->used->ring[idx];
	desc_id = (uint16_t)elem->id;

	if (len_out)
		*len_out = elem->len;

	priv = vq->priv[desc_id];
	vq->priv[desc_id] = NULL;

	/* Return descriptor to free list */
	vq->desc[desc_id].next = vq->free_head;
	vq->free_head = desc_id;
	vq->num_free++;

	vq->last_used_idx++;

	return priv;
}
