/* Pin reusable workflow assumptions and demote before stale execution. */
#include <anx/workflow_reuse.h>
#include <anx/workflow_library.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct reuse_condition {
	anx_oid_t oid;
	uint64_t version;
	enum anx_object_type type;
	enum anx_sensitivity sensitivity;
	uint8_t digest[32];
};
struct anx_wf_reuse_guard {
	struct anx_wf_reuse_view view;
	uint8_t graph[32];
	uint32_t count;
	struct reuse_condition conditions[ANX_WF_REUSE_CONDITIONS_MAX];
};

static int capture_condition(const anx_oid_t *oid, struct reuse_condition *out)
{
	struct anx_object_handle handle = {0};
	struct reuse_condition c = {0};
	int ret = anx_so_open(oid, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK) return ret;
	anx_spin_lock(&handle.obj->lock);
	struct anx_state_object *obj = handle.obj;
	if (obj->staged) ret = ANX_EBUSY;
	else if (obj->state != ANX_OBJ_ACTIVE && obj->state != ANX_OBJ_SEALED) ret = ANX_EPERM;
	else if (obj->payload_size > ANX_WF_REUSE_OBJECT_MAX || (obj->payload_size && !obj->payload)) ret = ANX_EINVAL;
	else {
		c.oid = obj->oid;
		c.version = obj->version;
		c.type = obj->object_type;
		c.sensitivity = obj->sensitivity;
		anx_sha256(obj->payload, (uint32_t)obj->payload_size, c.digest);
	}
	anx_spin_unlock(&obj->lock);
	anx_so_close(&handle);
	if (ret == ANX_OK) *out = c;
	return ret;
}

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

static int validate_form(const struct anx_wf_object *wf, const struct anx_wf_reuse_guard *guard)
{
	struct anx_wf_template *t = anx_zalloc(sizeof(*t));
	int ret = ANX_EINVAL;
	if (!t) return ANX_ENOMEM;
	if (!wf->nodes || !wf->edges || !wf->node_count || wf->node_count > ANX_WF_MAX_NODES) goto out;
	anx_strlcpy(t->uri, "anx:workflow/reuse-validation", sizeof(t->uri));
	t->node_count = wf->node_count;
	anx_memcpy(t->nodes, wf->nodes, sizeof(t->nodes));
	for (uint32_t i = 0; i < ANX_WF_MAX_NODES; i++) {
		const struct anx_wf_node *node = &wf->nodes[i];
		bool pinned = false;
		if (i >= wf->node_count) { if (node->id) goto out; else continue; }
		if (node->kind == ANX_WF_NODE_STATE_REF) {
			for (uint32_t j = 0; j < guard->count; j++)
				if (!anx_uuid_compare(&node->params.state_ref.obj_oid, &guard->conditions[j].oid)) pinned = true;
			if (!pinned) goto out;
			if (node->params.state_ref.write_mode) { ret = ANX_EPERM; goto out; }
		}
		/* These nodes are placeholders or can delegate to an unbound graph. */
		if (node->kind == ANX_WF_NODE_CONDITION || node->kind == ANX_WF_NODE_TRANSFORM ||
		    node->kind == ANX_WF_NODE_SUBFLOW || node->kind == ANX_WF_NODE_AGENT_CALL) { ret = ANX_EPERM; goto out; }
		if (guard->view.form == ANX_WF_DETERMINISTIC && node->kind != ANX_WF_NODE_TRIGGER &&
		    node->kind != ANX_WF_NODE_STATE_REF && node->kind != ANX_WF_NODE_FAN_OUT &&
		    node->kind != ANX_WF_NODE_FAN_IN && node->kind != ANX_WF_NODE_OUTPUT) { ret = ANX_EPERM; goto out; }
	}
	for (uint32_t i = 0; i < ANX_WF_MAX_EDGES; i++)
		if (wf->edges[i].from_node || wf->edges[i].to_node) t->edges[t->edge_count++] = wf->edges[i];
	if (t->edge_count != wf->edge_count) goto out;
	ret = anx_wf_template_validate(t);
out:
	anx_free(t);
	return ret;
}

int anx_wf_reuse_bind(const anx_oid_t *oid, enum anx_wf_execution_form form,
		      const anx_oid_t *conditions, uint32_t count)
{
	struct anx_wf_object *wf;
	struct anx_wf_reuse_guard *guard;
	int ret;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!oid || (form != ANX_WF_HYBRID && form != ANX_WF_DETERMINISTIC) ||
	    count > ANX_WF_REUSE_CONDITIONS_MAX || (!conditions && count)) return ANX_EINVAL;
	wf = anx_wf_object_get(oid);
	if (!wf) return ANX_ENOENT;
	if (wf->run_state == ANX_WF_RUN_RUNNING || wf->run_state == ANX_WF_RUN_SUSPENDED ||
	    wf->run_state == ANX_WF_RUN_WAITING_HUMAN) return ANX_EBUSY;
	guard = anx_zalloc(sizeof(*guard));
	if (!guard) return ANX_ENOMEM;
	guard->view.bound = true;
	guard->view.form = form;
	guard->count = count;
	for (uint32_t i = 0; i < count; i++) {
		for (uint32_t j = 0; j < i; j++)
			if (!anx_uuid_compare(&conditions[i], &conditions[j])) { ret = ANX_EINVAL; goto fail; }
		ret = capture_condition(&conditions[i], &guard->conditions[i]);
		if (ret != ANX_OK) goto fail;
	}
	ret = validate_form(wf, guard);
	if (ret != ANX_OK) goto fail;
	graph_digest(wf, guard->graph);
	anx_free(wf->reuse);
	wf->reuse = guard;
	wf->run_state = ANX_WF_RUN_IDLE;
	wf->output_count = 0;
	anx_memset(wf->output_oids, 0, sizeof(wf->output_oids));
	return ANX_OK;
fail:
	anx_free(guard);
	return ret;
}

int anx_wf_reuse_status(const anx_oid_t *oid, struct anx_wf_reuse_view *out)
{
	if (!oid || !out) return ANX_EINVAL;
	struct anx_wf_object *wf = anx_wf_object_get(oid);
	if (!wf) return ANX_ENOENT;
	anx_memset(out, 0, sizeof(*out));
	if (wf->reuse) *out = wf->reuse->view;
	return ANX_OK;
}

static int demote(struct anx_wf_object *wf, int reason)
{
	wf->reuse->view.demoted = true;
	wf->reuse->view.form = ANX_WF_EXPLORATORY;
	wf->reuse->view.reason = reason;
	wf->output_count = 0;
	anx_memset(wf->output_oids, 0, sizeof(wf->output_oids));
	wf->trace_oid = ANX_UUID_NIL;
	wf->topology.trace_epoch = 0;
	wf->last_status = reason;
	if (wf->run_state != ANX_WF_RUN_SUSPENDED && wf->run_state != ANX_WF_RUN_WAITING_HUMAN)
		wf->run_state = ANX_WF_RUN_FAILED;
	return reason;
}

int anx_wf_reuse_check(struct anx_wf_object *wf)
{
	uint8_t digest[32];
	if (!wf) return ANX_EINVAL;
	if (!wf->reuse) return ANX_OK;
	struct anx_wf_reuse_guard *g = wf->reuse;
	if (g->view.demoted) return demote(wf, g->view.reason);
	if (!wf->nodes || !wf->edges) return demote(wf, ANX_EINVAL);
	graph_digest(wf, digest);
	if (anx_memcmp(digest, g->graph, sizeof(digest))) return demote(wf, ANX_EBUSY);
	for (uint32_t i = 0; i < g->count; i++) {
		struct reuse_condition current;
		const struct reuse_condition *c = &g->conditions[i];
		int ret = capture_condition(&c->oid, &current);
		if (ret != ANX_OK) return demote(wf, ret);
		if (current.version != c->version || current.type != c->type || current.sensitivity != c->sensitivity ||
		    anx_memcmp(current.digest, c->digest, sizeof(c->digest))) return demote(wf, ANX_EBUSY);
	}
	return ANX_OK;
}

void anx_wf_reuse_observe(struct anx_wf_object *wf, int result)
{
	if (!wf || !wf->reuse) return;
	if (result != ANX_OK) { demote(wf, result); return; }
	if (wf->run_state == ANX_WF_RUN_COMPLETED && wf->reuse->view.successful_runs != ~(uint64_t)0)
		wf->reuse->view.successful_runs++;
}
