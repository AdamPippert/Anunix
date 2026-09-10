/* Exercise the workflow import boundary in host tests and the live kernel. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workflow_library.h>
#include <anx/wf_bundle.h>
#include <anx/alloc.h>
#include <anx/string.h>

int anx_research_day002(void)
{
	const char *uri = "anx:workflow/research/day-002";
	struct anx_wf_template *tmpl = anx_zalloc(sizeof(*tmpl));
	uint8_t *buf = anx_alloc(sizeof(*tmpl));
	struct anx_wf_node node;
	struct anx_wf_edge edge;
	struct anx_wf_bundle_hdr hdr;
	struct anx_wf_object *wf;
	anx_oid_t oid = {0};
	uint32_t size, i, offset = sizeof(hdr) + 128 + 64 + 256;
	bool created = false;
	int rc = ANX_ENOMEM;

	if (!tmpl || !buf)
		goto out;
	anx_strlcpy(tmpl->uri, uri, sizeof(tmpl->uri));
	anx_strlcpy(tmpl->display_name, "Research import", sizeof(tmpl->display_name));
	tmpl->node_count = 2;
	tmpl->nodes[0].id = 1;
	tmpl->nodes[0].kind = ANX_WF_NODE_TRIGGER;
	tmpl->nodes[0].port_count = 2;
	tmpl->nodes[0].ports[0].dir = ANX_WF_PORT_OUT;
	tmpl->nodes[0].ports[1].dir = ANX_WF_PORT_IN;
	tmpl->nodes[1].id = 2;
	tmpl->nodes[1].kind = ANX_WF_NODE_FAN_OUT;
	tmpl->nodes[1].port_count = 2;
	tmpl->nodes[1].ports[0].dir = ANX_WF_PORT_IN;
	tmpl->nodes[1].ports[1].dir = ANX_WF_PORT_OUT;
	tmpl->edge_count = 1;
	tmpl->edges[0] = (struct anx_wf_edge){1, 2, 0, 0};

	/* Mutate a valid wire artifact, bypassing the producer's checks. */
	for (i = 0; i < 13; i++) {
		if (anx_wf_bundle_pack(tmpl, buf, sizeof(*tmpl), &size) != ANX_OK) {
			rc = -200;
			goto out;
		}
		node = tmpl->nodes[0];
		edge = tmpl->edges[0];
		switch (i) {
		case 0: node.port_count = ANX_WF_MAX_PORTS + 1; break;
		case 1: node.kind = ANX_WF_NODE_KIND_COUNT; break;
		case 2: node.id = 2; break;
		case 3: anx_memset(node.label, 'x', sizeof(node.label)); break;
		case 4: anx_memset(node.params.trigger.schedule, 'x', 64); break;
		case 5: anx_memset(node.ports[0].name, 'x', 32); break;
		case 6: node.ports[0].dir = ANX_WF_PORT_IN; break;
		case 7: edge.to_node = 3; break;
		case 8: edge.to_port = ANX_WF_MAX_PORTS; break;
		case 9: anx_memset(buf + sizeof(hdr), 'x', 128); break;
		case 10: anx_memset(buf + sizeof(hdr) + 128, 'x', 64); break;
		case 11: anx_memset(buf + sizeof(hdr) + 192, 'x', 256); break;
		case 12:
			/* A cycle with valid endpoints and port directions. */
			anx_memcpy(&hdr, buf, sizeof(hdr));
			hdr.edge_count++;
			hdr.total_size += sizeof(edge);
			anx_memcpy(buf, &hdr, sizeof(hdr));
			{
				struct anx_wf_edge back = {2, 1, 1, 1};
				anx_memcpy(buf + size, &back, sizeof(back));
			}
			size += sizeof(edge);
			break;
		}
		anx_memcpy(buf + offset, &node, sizeof(node));
		anx_memcpy(buf + offset + 2 * sizeof(node), &edge, sizeof(edge));
		if (anx_wf_bundle_register(buf, size) != ANX_EINVAL) {
			rc = -201 - (int)i;
			goto out;
		}
	}

	/* Reject invalid producer counts before computing sizes or copying. */
	tmpl->tag_count = ANX_WF_LIB_TAGS + 1;
	if (anx_wf_bundle_pack(tmpl, buf, sizeof(*tmpl), &size) != ANX_EINVAL) {
		rc = -214;
		goto out;
	}
	tmpl->tag_count = 0;
	if (anx_wf_bundle_pack(tmpl, buf, sizeof(*tmpl), &size) != ANX_OK) {
		rc = -215;
		goto out;
	}
	/* Registration owns a copy. Repeat runs reuse that immutable template. */
	rc = anx_wf_bundle_register(buf, size);
	if (rc != ANX_OK && rc != ANX_EEXIST)
		goto out;
	if (anx_wf_bundle_register(buf, size) != ANX_EEXIST) {
		rc = -216;
		goto out;
	}
	anx_memset(buf, 0xff, size);
	rc = anx_wf_lib_instantiate(uri, "research-day-002", &oid);
	if (rc != ANX_OK)
		goto out;
	created = true;
	wf = anx_wf_object_get(&oid);
	rc = -217;
	if (!wf || wf->node_count != 2 || wf->edge_count != 1 ||
	    anx_wf_run(&oid, NULL) != ANX_OK ||
	    wf->run_state != ANX_WF_RUN_COMPLETED || wf->trace_entry_count != 2)
		goto out;
	rc = ANX_OK;
out:
	if (created)
		anx_wf_destroy(&oid);
	anx_free(buf);
	anx_free(tmpl);
	return rc;
}
#endif
