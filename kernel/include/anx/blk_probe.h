/*
 * anx/blk_probe.h — Recognise what is already on a block device (RFC-0031 §8).
 *
 * Formatting a disk is not reversible, so the kernel reads before it writes.
 * A probe answers one question: did Anunix put this here, or did someone
 * else? Everything that is not recognisably ours is treated as theirs.
 */

#ifndef ANX_BLK_PROBE_H
#define ANX_BLK_PROBE_H

#include <anx/types.h>

struct anx_blk_dev;

enum anx_blk_content {
	ANX_CONTENT_BLANK = 0,	/* nothing recognisable; safe to format */
	ANX_CONTENT_ANUNIX,	/* an Anunix object store superblock */
	ANX_CONTENT_FOREIGN,	/* a partition table, filesystem or array */
};

#define ANX_PROBE_DESC_MAX	24

/*
 * Classify dev. `desc`, when non-NULL, receives a short name for what was
 * found ("GPT", "xfs", "Anunix object store") so a refusal can say why.
 *
 * A device that cannot be read is reported FOREIGN: an unreadable disk is
 * not an empty one, and guessing in the permissive direction costs data.
 */
enum anx_blk_content anx_blk_probe(struct anx_blk_dev *dev, char *desc,
				   uint32_t desc_len);

/* Short name for a content class, for logging. */
const char *anx_blk_content_name(enum anx_blk_content c);

#endif /* ANX_BLK_PROBE_H */
