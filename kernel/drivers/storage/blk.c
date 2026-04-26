/*
 * blk.c — Block device dispatch layer.
 *
 * Maintains a single registered ops pointer. The first driver to call
 * anx_blk_register wins; subsequent calls are silently ignored so that
 * the init sequence in main.c can try virtio-blk → NVMe → AHCI without
 * coordination logic here.
 */

#include <anx/blk.h>
#include <anx/kprintf.h>

static const struct anx_blk_ops *active_ops;

void anx_blk_register(const struct anx_blk_ops *ops)
{
	if (active_ops)
		return; /* first caller wins */
	active_ops = ops;
	kprintf("blk: registered driver \"%s\"\n",
		ops->name ? ops->name : "(unnamed)");
}

int anx_blk_read(uint64_t lba, uint32_t count, void *buf)
{
	if (!active_ops)
		return ANX_EIO;
	return active_ops->read(lba, count, buf);
}

int anx_blk_write(uint64_t lba, uint32_t count, const void *buf)
{
	if (!active_ops)
		return ANX_EIO;
	return active_ops->write(lba, count, buf);
}

uint64_t anx_blk_capacity(void)
{
	if (!active_ops)
		return 0;
	return active_ops->capacity();
}

bool anx_blk_ready(void)
{
	return active_ops != NULL;
}
