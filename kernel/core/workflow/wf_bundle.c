/*
 * wf_bundle.c — Pack/unpack workflow template bundles for HTTP distribution.
 */

#include <anx/types.h>
#include <anx/wf_bundle.h>
#include <anx/workflow_library.h>
#include <anx/workflow.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Convenience: size of the string fields that precede the tag array. */
#define STR_BLOCK_SIZE	(128 + 64 + 256)

int
anx_wf_bundle_pack(const struct anx_wf_template *tmpl,
		   void *buf, uint32_t buf_size, uint32_t *size_out)
{
	struct anx_wf_bundle_hdr	*hdr;
	uint8_t				*p;
	uint32_t			needed;
	uint32_t			tags_sz;
	uint32_t			nodes_sz;
	uint32_t			edges_sz;

	if (!tmpl || !buf || !size_out)
		return ANX_EINVAL;

	tags_sz  = (uint32_t)tmpl->tag_count * ANX_WF_LIB_TAG_MAX;
	nodes_sz = (uint32_t)tmpl->node_count * (uint32_t)sizeof(struct anx_wf_node);
	edges_sz = (uint32_t)tmpl->edge_count * (uint32_t)sizeof(struct anx_wf_edge);
	needed   = (uint32_t)sizeof(struct anx_wf_bundle_hdr)
		 + STR_BLOCK_SIZE
		 + tags_sz + nodes_sz + edges_sz;

	if (buf_size < needed)
		return ANX_ENOMEM;

	p = (uint8_t *)buf;

	hdr             = (struct anx_wf_bundle_hdr *)p;
	hdr->magic      = ANX_WF_BUNDLE_MAGIC;
	hdr->version    = ANX_WF_BUNDLE_VERSION;
	hdr->node_count = (uint16_t)tmpl->node_count;
	hdr->edge_count = (uint16_t)tmpl->edge_count;
	hdr->tag_count  = (uint16_t)tmpl->tag_count;
	hdr->total_size = needed;
	p += sizeof(struct anx_wf_bundle_hdr);

	anx_memcpy(p, tmpl->uri,          128); p += 128;
	anx_memcpy(p, tmpl->display_name,  64); p += 64;
	anx_memcpy(p, tmpl->description,  256); p += 256;

	if (tags_sz)
		anx_memcpy(p, tmpl->tags, tags_sz);
	p += tags_sz;

	if (nodes_sz)
		anx_memcpy(p, tmpl->nodes, nodes_sz);
	p += nodes_sz;

	if (edges_sz)
		anx_memcpy(p, tmpl->edges, edges_sz);

	*size_out = needed;
	return ANX_OK;
}

int
anx_wf_bundle_register(const void *buf, uint32_t size)
{
	const struct anx_wf_bundle_hdr	*hdr;
	const uint8_t			*p;
	struct anx_wf_template		*tmpl;
	uint32_t			tags_sz;
	uint32_t			nodes_sz;
	uint32_t			edges_sz;
	uint32_t			needed;

	if (!buf || size < sizeof(struct anx_wf_bundle_hdr))
		return ANX_EINVAL;

	hdr = (const struct anx_wf_bundle_hdr *)buf;

	if (hdr->magic != ANX_WF_BUNDLE_MAGIC) {
		kprintf("wf_bundle: bad magic 0x%x\n", hdr->magic);
		return ANX_EINVAL;
	}
	if (hdr->version != ANX_WF_BUNDLE_VERSION) {
		kprintf("wf_bundle: unsupported version %u\n", hdr->version);
		return ANX_EINVAL;
	}
	if (hdr->tag_count > ANX_WF_LIB_TAGS ||
	    hdr->node_count > ANX_WF_MAX_NODES ||
	    hdr->edge_count > ANX_WF_MAX_EDGES) {
		kprintf("wf_bundle: counts out of range\n");
		return ANX_EINVAL;
	}

	tags_sz  = (uint32_t)hdr->tag_count  * ANX_WF_LIB_TAG_MAX;
	nodes_sz = (uint32_t)hdr->node_count * (uint32_t)sizeof(struct anx_wf_node);
	edges_sz = (uint32_t)hdr->edge_count * (uint32_t)sizeof(struct anx_wf_edge);
	needed   = (uint32_t)sizeof(struct anx_wf_bundle_hdr)
		 + STR_BLOCK_SIZE
		 + tags_sz + nodes_sz + edges_sz;

	if (size < needed || hdr->total_size != needed) {
		kprintf("wf_bundle: size mismatch (got %u, need %u)\n", size, needed);
		return ANX_EINVAL;
	}

	tmpl = (struct anx_wf_template *)anx_alloc(sizeof(struct anx_wf_template));
	if (!tmpl)
		return ANX_ENOMEM;
	anx_memset(tmpl, 0, sizeof(struct anx_wf_template));

	p = (const uint8_t *)buf + sizeof(struct anx_wf_bundle_hdr);

	anx_memcpy(tmpl->uri,          p, 128); p += 128;
	anx_memcpy(tmpl->display_name, p,  64); p += 64;
	anx_memcpy(tmpl->description,  p, 256); p += 256;

	if (tags_sz) {
		anx_memcpy(tmpl->tags, p, tags_sz);
	}
	p += tags_sz;
	tmpl->tag_count = hdr->tag_count;

	if (nodes_sz) {
		anx_memcpy(tmpl->nodes, p, nodes_sz);
	}
	p += nodes_sz;
	tmpl->node_count = hdr->node_count;

	if (edges_sz) {
		anx_memcpy(tmpl->edges, p, edges_sz);
	}
	tmpl->edge_count = hdr->edge_count;

	return anx_wf_lib_register(tmpl);
}
