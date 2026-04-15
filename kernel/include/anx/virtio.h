/*
 * anx/virtio.h — Virtio transport layer (legacy PIO).
 *
 * Implements the virtio 0.9.5 legacy transport over I/O ports.
 * Provides virtqueue setup, buffer management, and device status
 * handshake for use by virtio device drivers.
 */

#ifndef ANX_VIRTIO_H
#define ANX_VIRTIO_H

#include <anx/types.h>
#include <anx/spinlock.h>

/* --- Legacy PIO register offsets (from BAR0) --- */

#define VIRTIO_PCI_HOST_FEATURES	0	/* 32-bit, R */
#define VIRTIO_PCI_GUEST_FEATURES	4	/* 32-bit, R/W */
#define VIRTIO_PCI_QUEUE_PFN		8	/* 32-bit, R/W */
#define VIRTIO_PCI_QUEUE_SIZE		12	/* 16-bit, R */
#define VIRTIO_PCI_QUEUE_SEL		14	/* 16-bit, R/W */
#define VIRTIO_PCI_QUEUE_NOTIFY		16	/* 16-bit, R/W */
#define VIRTIO_PCI_STATUS		18	/* 8-bit, R/W */
#define VIRTIO_PCI_ISR			19	/* 8-bit, R */
/* Device-specific config starts at offset 20 */
#define VIRTIO_PCI_CONFIG		20

/* --- Device status bits --- */

#define VIRTIO_STATUS_ACKNOWLEDGE	1
#define VIRTIO_STATUS_DRIVER		2
#define VIRTIO_STATUS_DRIVER_OK		4
#define VIRTIO_STATUS_FEATURES_OK	8
#define VIRTIO_STATUS_FAILED		128

/* --- Virtqueue descriptor flags --- */

#define VIRTQ_DESC_F_NEXT		1	/* buffer continues in next desc */
#define VIRTQ_DESC_F_WRITE		2	/* device writes (vs reads) */

/* --- Virtqueue structures (must match virtio spec layout) --- */

struct anx_vq_desc {
	uint64_t addr;		/* physical address of buffer */
	uint32_t len;		/* buffer length */
	uint16_t flags;		/* VIRTQ_DESC_F_* */
	uint16_t next;		/* next descriptor index (if NEXT set) */
} __attribute__((packed));

struct anx_vq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];	/* variable-length array of descriptor indices */
} __attribute__((packed));

struct anx_vq_used_elem {
	uint32_t id;		/* descriptor chain head index */
	uint32_t len;		/* bytes written by device */
} __attribute__((packed));

struct anx_vq_used {
	uint16_t flags;
	uint16_t idx;
	struct anx_vq_used_elem ring[];
} __attribute__((packed));

/* --- Virtqueue handle --- */

struct anx_virtqueue {
	uint16_t num;			/* queue size (power of 2) */
	uint16_t free_head;		/* head of free descriptor chain */
	uint16_t num_free;		/* number of free descriptors */
	uint16_t last_used_idx;		/* last consumed used ring index */
	struct anx_vq_desc *desc;	/* descriptor table */
	struct anx_vq_avail *avail;	/* available ring */
	struct anx_vq_used *used;	/* used ring */
	void **priv;			/* per-descriptor private data */
	uint16_t io_base;		/* BAR0 I/O port base */
	uint16_t queue_index;		/* which queue (0, 1, ...) */
	struct anx_spinlock lock;
};

/* --- Virtio device --- */

struct anx_virtio_dev {
	struct anx_pci_device *pci;
	uint16_t io_base;		/* BAR0 I/O port base */
	uint32_t host_features;		/* device-offered features */
	uint32_t guest_features;	/* negotiated features */
};

/* Reset the device and start the status handshake */
int anx_virtio_init(struct anx_virtio_dev *vdev,
		    struct anx_pci_device *pci);

/* Read device-offered features */
uint32_t anx_virtio_get_features(struct anx_virtio_dev *vdev);

/* Write guest (driver) features and complete negotiation */
void anx_virtio_set_features(struct anx_virtio_dev *vdev, uint32_t features);

/* Set up a virtqueue at the given index */
int anx_virtio_setup_vq(struct anx_virtio_dev *vdev, uint16_t index,
			 struct anx_virtqueue *vq);

/* Signal DRIVER_OK — device is ready for I/O */
void anx_virtio_driver_ok(struct anx_virtio_dev *vdev);

/* Read the ISR status register (clears interrupt) */
uint8_t anx_virtio_isr(struct anx_virtio_dev *vdev);

/* Read a device-config byte/word/dword at offset (relative to config base) */
uint8_t anx_virtio_config_read8(struct anx_virtio_dev *vdev, uint16_t off);
uint16_t anx_virtio_config_read16(struct anx_virtio_dev *vdev, uint16_t off);
uint32_t anx_virtio_config_read32(struct anx_virtio_dev *vdev, uint16_t off);

/* Add a single buffer to a virtqueue (returns descriptor index or < 0) */
int anx_vq_add_buf(struct anx_virtqueue *vq, void *buf, uint32_t len,
		    uint16_t flags, void *priv);

/* Notify the device that buffers have been added */
void anx_vq_kick(struct anx_virtqueue *vq);

/* Get next completed buffer from the used ring (NULL if empty) */
void *anx_vq_get_used(struct anx_virtqueue *vq, uint32_t *len_out);

#endif /* ANX_VIRTIO_H */
