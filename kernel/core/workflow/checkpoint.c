/* Restore only an issued, current continuation image and its exact dependencies. */
#include <anx/workflow.h>
#include <anx/workflow_reuse.h>
#include <anx/workflow_semantic.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>

#define WF_CHECKPOINT_SCHEMA 2U
#define WF_CHECKPOINT_REFS ANX_WF_MAX_EDGES
#define WF_CHECKPOINT_OBJECT_MAX (1024U * 1024U)
struct checkpoint_ref {
	anx_oid_t oid;
	uint64_t version;
	enum anx_object_type type;
	enum anx_sensitivity sensitivity;
	uint8_t digest[32];
};
struct checkpoint_image {
	uint32_t schema, trace_count, ref_count, cap;
	anx_oid_t workflow;
	anx_oid_t semantic;
	uint64_t epoch, topology_epoch;
	anx_time_t last_run;
	uint8_t graph[32];
	struct anx_wf_continuation continuation;
	struct anx_wf_trace_entry trace[ANX_WF_MAX_NODES];
	uint8_t output_count;
	anx_oid_t outputs[ANX_WF_MAX_PORTS];
	struct checkpoint_ref refs[WF_CHECKPOINT_REFS];
};

static void graph_digest(const struct anx_wf_object *wf, uint8_t out[32])
{
	struct anx_sha256_ctx hash;
	anx_sha256_init(&hash);
	anx_sha256_update(&hash, &wf->node_count, sizeof(wf->node_count));
	anx_sha256_update(&hash, &wf->edge_count, sizeof(wf->edge_count));
	anx_sha256_update(&hash, wf->nodes, ANX_WF_MAX_NODES * sizeof(*wf->nodes));
	anx_sha256_update(&hash, wf->edges, ANX_WF_MAX_EDGES * sizeof(*wf->edges));
	anx_sha256_update(&hash, &wf->policy, sizeof(wf->policy));
	anx_sha256_final(&hash, out);
}

static int capture_ref(const anx_oid_t *oid, struct checkpoint_ref *ref)
{
	struct anx_object_handle h = {0};
	int ret = anx_so_open(oid, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	anx_spin_lock(&h.obj->lock);
	struct anx_state_object *obj = h.obj;
	if (obj->staged) ret = ANX_EBUSY;
	else if (obj->state != ANX_OBJ_ACTIVE && obj->state != ANX_OBJ_SEALED) ret = ANX_EPERM;
	else if (obj->payload_size > WF_CHECKPOINT_OBJECT_MAX || (obj->payload_size && !obj->payload)) ret = ANX_EINVAL;
	else {
		ref->oid = obj->oid;
		ref->version = obj->version;
		ref->type = obj->object_type;
		ref->sensitivity = obj->sensitivity;
		anx_sha256(obj->payload, (uint32_t)obj->payload_size, ref->digest);
	}
	anx_spin_unlock(&obj->lock);
	anx_so_close(&h);
	return ret;
}

static int add_ref(struct checkpoint_image *image, const anx_oid_t *oid)
{
	if (anx_uuid_is_nil(oid)) return ANX_OK;
	for (uint32_t i = 0; i < image->ref_count; i++)
		if (!anx_uuid_compare(oid, &image->refs[i].oid)) return ANX_OK;
	if (image->ref_count == WF_CHECKPOINT_REFS) return ANX_EFULL;
	int ret = capture_ref(oid, &image->refs[image->ref_count]);
	if (ret == ANX_OK) image->ref_count++;
	return ret;
}

static int suspended(const anx_oid_t *oid, struct anx_wf_object **out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!oid || !out) return ANX_EINVAL;
	*out = anx_wf_object_get(oid);
	if (!*out) return ANX_ENOENT;
	if ((*out)->run_state != ANX_WF_RUN_SUSPENDED) return ANX_EBUSY;
	if (!(*out)->nodes || !(*out)->edges) return ANX_EINVAL;
	return ANX_OK;
}

int anx_wf_checkpoint_save(const anx_oid_t *oid, anx_oid_t *out)
{
	struct anx_wf_object *wf;
	struct checkpoint_image *image;
	struct anx_so_create_params p = {0};
	struct anx_state_object *obj = NULL;
	uint8_t digest[32];
	if (!out) return ANX_EINVAL;
	*out = ANX_UUID_NIL;
	int ret = suspended(oid, &wf);
	if (ret != ANX_OK) return ret;
	ret = anx_wf_semantic_check(wf);
	if (ret != ANX_OK) return ret;
	if (!wf->continuation || !wf->trace_entries || !wf->trace_entry_count ||
	    wf->trace_entry_count > ANX_WF_MAX_NODES || wf->output_count > ANX_WF_MAX_PORTS ||
	    !wf->computed_cap || wf->computed_cap > ANX_WF_MAX_NODES) return ANX_EINVAL;
	if (wf->checkpoint_epoch == ~(uint64_t)0) return ANX_EFULL;
	uint16_t failed = wf->continuation->failed_node_id;
	if (!failed || failed > ANX_WF_MAX_NODES || wf->nodes[failed - 1].id != failed ||
	    wf->continuation->completed[failed - 1]) return ANX_EINVAL;
	image = anx_zalloc(sizeof(*image));
	if (!image) return ANX_ENOMEM;
	image->schema = WF_CHECKPOINT_SCHEMA;
	image->workflow = wf->oid;
	image->semantic = anx_wf_semantic_id(wf);
	image->epoch = wf->checkpoint_epoch + 1;
	image->topology_epoch = wf->topology.epoch;
	image->last_run = wf->last_run;
	image->cap = wf->computed_cap;
	image->trace_count = wf->trace_entry_count;
	image->output_count = wf->output_count;
	anx_memcpy(image->outputs, wf->output_oids, wf->output_count * sizeof(anx_oid_t));
	graph_digest(wf, image->graph);
	struct anx_wf_continuation *dst = &image->continuation;
	const struct anx_wf_continuation *src = wf->continuation;
	dst->failed_node_id = src->failed_node_id;
	dst->error_code = src->error_code;
	anx_strlcpy(dst->error_msg, src->error_msg, sizeof(dst->error_msg));
	anx_memcpy(dst->in_deg, src->in_deg, sizeof(dst->in_deg));
	anx_memcpy(dst->completed, src->completed, sizeof(dst->completed));
	anx_memcpy(dst->port_oid, src->port_oid, sizeof(dst->port_oid));
	for (uint32_t i = 0; i < image->trace_count; i++) {
		struct anx_wf_trace_entry *t = &image->trace[i];
		const struct anx_wf_trace_entry *s = &wf->trace_entries[i];
		t->node_id = s->node_id; t->node_kind = s->node_kind;
		t->cell_cid = s->cell_cid; t->trace_oid = s->trace_oid;
		t->result = s->result; t->started_at = s->started_at; t->completed_at = s->completed_at;
	}
	for (uint32_t i = 0; i < ANX_WF_MAX_NODES; i++) {
		if (src->in_deg[i] > ANX_WF_MAX_EDGES || (src->completed[i] && wf->nodes[i].id != i + 1)) {
			ret = ANX_EINVAL; goto done;
		}
		for (uint32_t j = 0; j < ANX_WF_MAX_PORTS; j++) {
			ret = add_ref(image, &src->port_oid[i][j]);
			if (ret != ANX_OK) goto done;
		}
		if (wf->nodes[i].id && wf->nodes[i].kind == ANX_WF_NODE_STATE_REF) {
			ret = add_ref(image, &wf->nodes[i].params.state_ref.obj_oid);
			if (ret != ANX_OK) goto done;
		}
	}
	for (uint32_t i = 0; i < image->output_count; i++) {
		ret = add_ref(image, &image->outputs[i]);
		if (ret != ANX_OK) goto done;
	}
	for (uint32_t i = 0; i < image->trace_count; i++) {
		ret = add_ref(image, &image->trace[i].trace_oid);
		if (ret != ANX_OK) goto done;
	}
	ret = add_ref(image, &image->semantic);
	if (ret != ANX_OK) goto done;
	anx_sha256(image, sizeof(*image), digest);
	p.object_type = ANX_OBJ_STRUCTURED_DATA;
	p.schema_uri = "anx:workflow/continuation/v2";
	p.schema_version = "2";
	p.payload = image; p.payload_size = sizeof(*image);
	/* A checkpoint exposes the highest sensitivity among its dependencies. */
	for (uint32_t i = 0; i < image->ref_count; i++)
		if (image->refs[i].sensitivity > p.sensitivity) p.sensitivity = image->refs[i].sensitivity;
	ret = anx_so_create(&p, &obj);
	if (ret == ANX_OK) ret = anx_so_seal(&obj->oid);
	if (ret == ANX_OK) {
		wf->checkpoint_oid = obj->oid;
		wf->checkpoint_epoch = image->epoch;
		wf->checkpoint_consumed = false;
		anx_memcpy(wf->checkpoint_digest, digest, sizeof(digest));
		*out = obj->oid;
		anx_free(wf->continuation); wf->continuation = NULL;
		anx_free(wf->trace_entries); wf->trace_entries = NULL;
		wf->trace_entry_count = 0;
	} else if (obj) anx_so_delete(&obj->oid, false);
done:
	if (obj) anx_objstore_release(obj);
	anx_free(image);
	return ret;
}

int anx_wf_checkpoint_restore(const anx_oid_t *oid, const anx_oid_t *checkpoint)
{
	struct anx_wf_object *wf;
	struct anx_object_handle h = {0};
	struct checkpoint_image *image = NULL;
	struct anx_wf_continuation *cont = NULL;
	struct anx_wf_trace_entry *trace = NULL;
	uint8_t digest[32];
	int ret = suspended(oid, &wf);
	if (ret != ANX_OK) return ret;
	if (!checkpoint || anx_uuid_is_nil(checkpoint)) return ANX_EINVAL;
	if (wf->checkpoint_consumed) return ANX_EBUSY;
	if (anx_uuid_compare(checkpoint, &wf->checkpoint_oid)) return ANX_EPERM;
	ret = anx_so_open(checkpoint, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	image = anx_alloc(sizeof(*image));
	ret = ANX_ENOMEM;
	if (!image) goto done;
	anx_spin_lock(&h.obj->lock);
	if (h.obj->state != ANX_OBJ_SEALED || h.obj->object_type != ANX_OBJ_STRUCTURED_DATA ||
	    h.obj->payload_size != sizeof(*image) || !h.obj->payload) ret = ANX_EPERM;
	else { anx_memcpy(image, h.obj->payload, sizeof(*image)); ret = ANX_OK; }
	anx_spin_unlock(&h.obj->lock);
	if (ret != ANX_OK) goto done;
	anx_sha256(image, sizeof(*image), digest);
	if (anx_memcmp(digest, wf->checkpoint_digest, sizeof(digest)) || image->schema != WF_CHECKPOINT_SCHEMA ||
	    anx_uuid_compare(&image->workflow, oid) || image->epoch != wf->checkpoint_epoch ||
	    image->last_run != wf->last_run || image->topology_epoch != wf->topology.epoch) { ret = ANX_EPERM; goto done; }
	anx_oid_t semantic = anx_wf_semantic_id(wf);
	if (anx_uuid_compare(&image->semantic, &semantic)) { ret = ANX_EPERM; goto done; }
	ret = anx_wf_semantic_check(wf);
	if (ret != ANX_OK) goto done;
	if (!image->trace_count || image->trace_count > ANX_WF_MAX_NODES || image->ref_count > WF_CHECKPOINT_REFS ||
	    image->output_count > ANX_WF_MAX_PORTS || !image->cap || image->cap > ANX_WF_MAX_NODES) {
		ret = ANX_EINVAL; goto done;
	}
	graph_digest(wf, digest);
	if (anx_memcmp(digest, image->graph, sizeof(digest))) { ret = ANX_EBUSY; goto done; }
	for (uint32_t i = 0; i < image->ref_count; i++) {
		struct checkpoint_ref current;
		const struct checkpoint_ref *ref = &image->refs[i];
		ret = capture_ref(&ref->oid, &current);
		if (ret != ANX_OK) goto done;
		if (current.version != ref->version || current.type != ref->type || current.sensitivity != ref->sensitivity ||
		    anx_memcmp(current.digest, ref->digest, sizeof(ref->digest))) { ret = ANX_EBUSY; goto done; }
	}
	ret = anx_wf_reuse_check(wf);
	if (ret != ANX_OK) goto done;
	cont = anx_alloc(sizeof(*cont));
	trace = anx_zalloc(ANX_WF_MAX_NODES * sizeof(*trace));
	ret = ANX_ENOMEM;
	if (!cont || !trace) goto done;
	anx_memcpy(cont, &image->continuation, sizeof(*cont));
	anx_memcpy(trace, image->trace, image->trace_count * sizeof(*trace));
	anx_free(wf->continuation); wf->continuation = cont; cont = NULL;
	anx_free(wf->trace_entries); wf->trace_entries = trace; trace = NULL;
	wf->trace_entry_count = image->trace_count;
	wf->output_count = image->output_count;
	anx_memcpy(wf->output_oids, image->outputs, sizeof(image->outputs));
	wf->computed_cap = image->cap;
	wf->last_status = image->continuation.error_code;
	wf->checkpoint_consumed = true;
	ret = ANX_OK;
done:
	anx_free(cont); anx_free(trace); anx_free(image);
	anx_so_close(&h);
	return ret;
}
