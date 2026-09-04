/*
 * blk.c — Block device registry and dispatch.
 *
 * Every drive a storage driver finds gets a registry slot and a unique
 * name built from the driver's class name plus the next free index:
 * "nvme0", "nvme1", "ahci0", "md0". The RAID layer in kernel/core/md/
 * addresses members by that name.
 *
 * The first device registered becomes the active device, which keeps
 * single-disk boots behaving exactly as they did before the registry
 * existed. anx_md_init() moves the active pointer to an assembled array
 * when the array claims the device that held it.
 */

#include <anx/blk.h>
#include <anx/string.h>
#include <anx/kprintf.h>

static struct anx_blk_dev blk_devs[ANX_BLK_MAX_DEVS];
static struct anx_blk_dev *active_dev;

/* Fill dst with "<base><n>" for the lowest n not already registered. */
static void blk_pick_name(char *dst, uint32_t size, const char *base)
{
	uint32_t n;

	for (n = 0; n < 1000; n++) {
		char cand[ANX_BLK_NAME_MAX];
		uint32_t len;
		bool taken = false;
		uint32_t i;

		anx_strlcpy(cand, base, sizeof(cand));
		len = (uint32_t)anx_strlen(cand);
		if (len + 4 >= sizeof(cand))
			len = (uint32_t)sizeof(cand) - 5;
		/* Index stays below 1000, so three digits are enough. */
		if (n >= 100)
			cand[len++] = (char)('0' + (n / 100) % 10);
		if (n >= 10)
			cand[len++] = (char)('0' + (n / 10) % 10);
		cand[len++] = (char)('0' + n % 10);
		cand[len] = '\0';

		for (i = 0; i < ANX_BLK_MAX_DEVS; i++) {
			if (blk_devs[i].used &&
			    anx_strcmp(blk_devs[i].name, cand) == 0) {
				taken = true;
				break;
			}
		}
		if (!taken) {
			anx_strlcpy(dst, cand, size);
			return;
		}
	}
	anx_strlcpy(dst, base, size);
}

struct anx_blk_dev *anx_blk_dev_register(const struct anx_blk_ops *ops,
					 void *priv, const char *base)
{
	uint32_t i;

	if (!ops || !ops->read || !ops->write || !ops->capacity)
		return NULL;
	if (!base)
		base = ops->name ? ops->name : "blk";

	for (i = 0; i < ANX_BLK_MAX_DEVS; i++) {
		struct anx_blk_dev *dev = &blk_devs[i];

		if (dev->used)
			continue;

		dev->ops   = ops;
		dev->priv  = priv;
		dev->flags = 0;
		dev->used  = true;
		blk_pick_name(dev->name, sizeof(dev->name), base);

		kprintf("blk: %s registered (%s, %llu MiB)\n",
			dev->name, ops->name ? ops->name : "(unnamed)",
			(unsigned long long)(anx_blk_dev_capacity(dev) /
					     2048));

		if (!active_dev)
			active_dev = dev;
		return dev;
	}

	kprintf("blk: registry full, dropping %s device\n",
		ops->name ? ops->name : "(unnamed)");
	return NULL;
}

void anx_blk_dev_unregister(struct anx_blk_dev *dev)
{
	if (!dev || !dev->used)
		return;
	if (active_dev == dev)
		active_dev = NULL;
	dev->used  = false;
	dev->ops   = NULL;
	dev->priv  = NULL;
	dev->flags = 0;
	dev->name[0] = '\0';
}

uint32_t anx_blk_dev_count(void)
{
	uint32_t i, n = 0;

	for (i = 0; i < ANX_BLK_MAX_DEVS; i++)
		if (blk_devs[i].used)
			n++;
	return n;
}

struct anx_blk_dev *anx_blk_dev_at(uint32_t index)
{
	uint32_t i, n = 0;

	for (i = 0; i < ANX_BLK_MAX_DEVS; i++) {
		if (!blk_devs[i].used)
			continue;
		if (n == index)
			return &blk_devs[i];
		n++;
	}
	return NULL;
}

struct anx_blk_dev *anx_blk_dev_find(const char *name)
{
	uint32_t i;

	if (!name)
		return NULL;
	for (i = 0; i < ANX_BLK_MAX_DEVS; i++) {
		if (blk_devs[i].used &&
		    anx_strcmp(blk_devs[i].name, name) == 0)
			return &blk_devs[i];
	}
	return NULL;
}

int anx_blk_dev_read(struct anx_blk_dev *dev, uint64_t lba,
		     uint32_t count, void *buf)
{
	if (!dev || !dev->used)
		return ANX_ENODEV;
	if (count == 0)
		return ANX_OK;
	return dev->ops->read(dev, lba, count, buf);
}

int anx_blk_dev_write(struct anx_blk_dev *dev, uint64_t lba,
		      uint32_t count, const void *buf)
{
	if (!dev || !dev->used)
		return ANX_ENODEV;
	if (count == 0)
		return ANX_OK;
	return dev->ops->write(dev, lba, count, buf);
}

uint64_t anx_blk_dev_capacity(struct anx_blk_dev *dev)
{
	if (!dev || !dev->used)
		return 0;
	return dev->ops->capacity(dev);
}

void anx_blk_dev_claim(struct anx_blk_dev *dev)
{
	if (dev)
		dev->flags |= ANX_BLK_F_MEMBER;
}

void anx_blk_dev_release(struct anx_blk_dev *dev)
{
	if (dev)
		dev->flags &= ~ANX_BLK_F_MEMBER;
}

bool anx_blk_dev_is_member(const struct anx_blk_dev *dev)
{
	return dev && (dev->flags & ANX_BLK_F_MEMBER) != 0;
}

void anx_blk_set_active(struct anx_blk_dev *dev)
{
	if (dev && !dev->used)
		return;
	active_dev = dev;
	kprintf("blk: active device is %s\n", anx_blk_active_name());
}

struct anx_blk_dev *anx_blk_active(void)
{
	return active_dev;
}

const char *anx_blk_active_name(void)
{
	return active_dev ? active_dev->name : "none";
}

int anx_blk_read(uint64_t lba, uint32_t count, void *buf)
{
	if (!active_dev)
		return ANX_EIO;
	return anx_blk_dev_read(active_dev, lba, count, buf);
}

int anx_blk_write(uint64_t lba, uint32_t count, const void *buf)
{
	if (!active_dev)
		return ANX_EIO;
	return anx_blk_dev_write(active_dev, lba, count, buf);
}

uint64_t anx_blk_capacity(void)
{
	return anx_blk_dev_capacity(active_dev);
}

bool anx_blk_ready(void)
{
	return active_dev != NULL;
}
