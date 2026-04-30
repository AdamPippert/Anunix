/*
 * anx/wf_bundle.h — Binary wire format for workflow template distribution.
 *
 * Magic ANX_WF_BUNDLE_MAGIC ("ANWF") identifies the blob type.  Version
 * is checked at deserialisation time to reject stale bundles.
 *
 * Layout (little-endian, packed):
 *   anx_wf_bundle_hdr
 *   uri[128]
 *   display_name[64]
 *   description[256]
 *   tags[tag_count][32]
 *   node_count × sizeof(struct anx_wf_node)
 *   edge_count × sizeof(struct anx_wf_edge)
 */

#ifndef ANX_WF_BUNDLE_H
#define ANX_WF_BUNDLE_H

#include <anx/types.h>
#include <anx/workflow_library.h>

#define ANX_WF_BUNDLE_MAGIC	0x414E5746U	/* "ANWF" */
#define ANX_WF_BUNDLE_VERSION	1

/* Fixed-size header at offset 0 of every bundle blob. */
struct anx_wf_bundle_hdr {
	uint32_t	magic;
	uint16_t	version;
	uint16_t	node_count;
	uint16_t	edge_count;
	uint16_t	tag_count;
	uint32_t	total_size;	/* total blob bytes including this header */
};

/* Serialize template to caller-allocated buf. *size_out = bytes written. */
int anx_wf_bundle_pack(const struct anx_wf_template *tmpl,
		       void *buf, uint32_t buf_size, uint32_t *size_out);

/* Deserialise, heap-allocate an anx_wf_template, and register it.
 * The allocated template is never freed (library lifetime). */
int anx_wf_bundle_register(const void *buf, uint32_t size);

#endif /* ANX_WF_BUNDLE_H */
