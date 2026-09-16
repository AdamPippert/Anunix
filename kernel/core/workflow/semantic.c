/* An issued semantic manifest freezes declared artifacts across suspension. */
#include <anx/workflow_semantic.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/memplane.h>

struct semantic_ref {
	enum anx_object_type type;
	enum anx_sensitivity sensitivity;
	bool memory_tracked;
	uint64_t validation_generation;
	uint8_t digest[32];
};
struct semantic_image {
	uint32_t schema;
	struct anx_semantic_spec spec;
	struct semantic_ref refs[ANX_SEMANTIC_RESOURCES_MAX];
	uint8_t graph[32];
};
struct anx_wf_semantic_guard {
	anx_oid_t oid;
	uint8_t digest[32];
	uint32_t resource_count;
	anx_oid_t resources[ANX_SEMANTIC_RESOURCES_MAX];
};

static bool valid_name(const char *name)
{
	if (!name || !name[0]) return false;
	for (uint32_t i = 0; i < ANX_SEMANTIC_NAME_MAX; i++) {
		char c = name[i];
		if (!c) return true;
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '/')) return false;
	}
	return false;
}

static int capture(const struct anx_semantic_resource *resource, struct semantic_ref *out)
{
	struct anx_object_handle h = {0};
	struct semantic_ref ref = {0};
	int ret = anx_so_open(&resource->oid, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	anx_spin_lock(&h.obj->lock);
	struct anx_state_object *obj = h.obj;
	if (obj->staged || obj->version != resource->version) ret = ANX_EBUSY;
	else if (obj->state != ANX_OBJ_ACTIVE && obj->state != ANX_OBJ_SEALED) ret = ANX_EPERM;
	else if (obj->payload_size > ANX_SEMANTIC_OBJECT_MAX || (obj->payload_size && !obj->payload) ||
		 (int)obj->sensitivity < 0 || obj->sensitivity > ANX_SENSITIVITY_RESTRICTED) ret = ANX_EINVAL;
	else {
		struct anx_sha256_ctx hash;
		ref.type = obj->object_type; ref.sensitivity = obj->sensitivity;
		anx_sha256_init(&hash);
		anx_sha256_update(&hash, obj->schema_uri, sizeof(obj->schema_uri));
		anx_sha256_update(&hash, obj->schema_version, sizeof(obj->schema_version));
		anx_sha256_update(&hash, obj->payload, (uint32_t)obj->payload_size);
		anx_sha256_final(&hash, ref.digest);
	}
	anx_spin_unlock(&obj->lock);
	anx_so_close(&h);
	if (ret == ANX_OK) {
		struct anx_mem_entry *entry = anx_memplane_lookup(&resource->oid);
		if (entry) {
			anx_spin_lock(&entry->lock);
			if (entry->validation != ANX_MEMVAL_VALIDATED || !entry->validation_generation) ret = ANX_EPERM;
			else { ref.memory_tracked = true; ref.validation_generation = entry->validation_generation; }
			anx_spin_unlock(&entry->lock);
			anx_memplane_release(entry);
		}
	}
	if (ret == ANX_OK) *out = ref;
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

static int validate_spec(const struct anx_semantic_spec *spec)
{
	if (!spec || !spec->resource_count || spec->resource_count > ANX_SEMANTIC_RESOURCES_MAX ||
	    spec->requires_count > ANX_SEMANTIC_REQUIRES_MAX) return ANX_EINVAL;
	for (uint32_t i = 0; i < spec->resource_count; i++) {
		const struct anx_semantic_resource *r = &spec->resources[i];
		if (!valid_name(r->name) || r->kind < ANX_SEMANTIC_PROMPT || r->kind > ANX_SEMANTIC_EMBEDDING ||
		    anx_uuid_is_nil(&r->oid) || !r->version) return ANX_EINVAL;
		for (uint32_t j = 0; j < i; j++)
			if (!anx_strcmp(r->name, spec->resources[j].name) ||
			    !anx_uuid_compare(&r->oid, &spec->resources[j].oid)) return ANX_EINVAL;
	}
	for (uint32_t i = 0; i < spec->requires_count; i++) {
		const struct anx_semantic_requires *r = &spec->requires[i];
		if (r->consumer >= spec->resource_count || r->provider >= spec->resource_count || r->consumer == r->provider) return ANX_EINVAL;
		const struct anx_semantic_resource *p = &spec->resources[r->provider];
		if (anx_uuid_compare(&r->expected_oid, &p->oid) || r->expected_version != p->version) return ANX_EPERM;
		for (uint32_t j = 0; j < i; j++)
			if (r->consumer == spec->requires[j].consumer && r->provider == spec->requires[j].provider) return ANX_EINVAL;
	}
	return ANX_OK;
}

int anx_wf_semantic_bind(const anx_oid_t *oid, const struct anx_semantic_spec *spec, anx_oid_t *out)
{
	struct anx_wf_object *wf;
	struct semantic_image *image = NULL;
	struct anx_wf_semantic_guard *guard = NULL;
	struct anx_state_object *obj = NULL;
	struct anx_so_create_params p = {0};
	int ret;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!oid || !out) return ANX_EINVAL;
	ret = validate_spec(spec);
	if (ret != ANX_OK) return ret;
	wf = anx_wf_object_get(oid);
	if (!wf) return ANX_ENOENT;
	if (wf->run_state == ANX_WF_RUN_RUNNING || wf->run_state == ANX_WF_RUN_SUSPENDED ||
	    wf->run_state == ANX_WF_RUN_WAITING_HUMAN) return ANX_EBUSY;
	if (!wf->nodes || !wf->edges || !wf->node_count || wf->node_count > ANX_WF_MAX_NODES) return ANX_EINVAL;
	for (uint32_t i = 0; i < ANX_WF_MAX_NODES; i++)
		if (wf->nodes[i].id && (wf->nodes[i].kind == ANX_WF_NODE_SUBFLOW ||
		    wf->nodes[i].kind == ANX_WF_NODE_AGENT_CALL)) return ANX_ENOTSUP;
	image = anx_zalloc(sizeof(*image)); guard = anx_zalloc(sizeof(*guard));
	ret = ANX_ENOMEM;
	if (!image || !guard) goto done;
	image->schema = 2; image->spec.resource_count = spec->resource_count; image->spec.requires_count = spec->requires_count;
	guard->resource_count = spec->resource_count;
	for (uint32_t i = 0; i < spec->resource_count; i++) {
		image->spec.resources[i] = spec->resources[i];
		guard->resources[i] = spec->resources[i].oid;
		ret = capture(&spec->resources[i], &image->refs[i]);
		if (ret != ANX_OK) goto done;
		if (image->refs[i].sensitivity > p.sensitivity) p.sensitivity = image->refs[i].sensitivity;
	}
	for (uint32_t i = 0; i < spec->requires_count; i++) image->spec.requires[i] = spec->requires[i];
	graph_digest(wf, image->graph);
	anx_sha256(image, sizeof(*image), guard->digest);
	p.object_type = ANX_OBJ_STRUCTURED_DATA; p.schema_uri = "anx:workflow/semantic-manifest/v2"; p.schema_version = "2";
	p.payload = image; p.payload_size = sizeof(*image);
	ret = anx_so_create(&p, &obj);
	if (ret == ANX_OK) ret = anx_so_seal(&obj->oid);
	if (ret != ANX_OK) { if (obj) anx_so_delete(&obj->oid, false); goto done; }
	guard->oid = obj->oid;
	anx_free(wf->semantic); wf->semantic = guard; guard = NULL;
	wf->run_state = ANX_WF_RUN_IDLE; wf->output_count = 0;
	anx_memset(wf->output_oids, 0, sizeof(wf->output_oids));
	wf->trace_oid = ANX_UUID_NIL; wf->topology.trace_epoch = 0; wf->checkpoint_consumed = true;
	*out = obj->oid;
done:
	if (obj) anx_objstore_release(obj);
	anx_free(image); anx_free(guard);
	return ret;
}

static int load_checked(const struct anx_wf_object *wf, struct semantic_image *image)
{
	struct anx_object_handle h = {0};
	uint8_t digest[32];
	int ret = anx_so_open(&wf->semantic->oid, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	anx_spin_lock(&h.obj->lock);
	if (h.obj->staged || h.obj->state != ANX_OBJ_SEALED || h.obj->object_type != ANX_OBJ_STRUCTURED_DATA ||
	    h.obj->payload_size != sizeof(*image) || !h.obj->payload) ret = ANX_EPERM;
	else anx_memcpy(image, h.obj->payload, sizeof(*image));
	anx_spin_unlock(&h.obj->lock);
	anx_so_close(&h);
	if (ret != ANX_OK) return ret;
	anx_sha256(image, sizeof(*image), digest);
	if (anx_memcmp(digest, wf->semantic->digest, sizeof(digest)) || image->schema != 2) return ANX_EPERM;
	ret = validate_spec(&image->spec);
	if (ret != ANX_OK) return ret;
	if (!wf->nodes || !wf->edges) return ANX_EINVAL;
	graph_digest(wf, digest);
	if (anx_memcmp(digest, image->graph, sizeof(digest))) return ANX_EBUSY;
	for (uint32_t i = 0; i < image->spec.resource_count; i++) {
		struct semantic_ref current;
		const struct semantic_ref *ref = &image->refs[i];
		ret = capture(&image->spec.resources[i], &current);
		if (ret != ANX_OK) return ret;
		if (current.type != ref->type || current.sensitivity != ref->sensitivity ||
		    current.memory_tracked != ref->memory_tracked || current.validation_generation != ref->validation_generation ||
		    anx_memcmp(current.digest, ref->digest, sizeof(ref->digest))) return ANX_EBUSY;
	}
	return ANX_OK;
}

int anx_wf_semantic_check(const struct anx_wf_object *wf)
{
	if (!wf) return ANX_EINVAL;
	if (!wf->semantic) return ANX_OK;
	struct semantic_image *image = anx_alloc(sizeof(*image));
	if (!image) return ANX_ENOMEM;
	int ret = load_checked(wf, image);
	anx_free(image);
	return ret;
}

anx_oid_t anx_wf_semantic_id(const struct anx_wf_object *wf)
{
	return wf && wf->semantic ? wf->semantic->oid : ANX_UUID_NIL;
}

bool anx_wf_semantic_needed(const struct anx_wf_object *wf, const anx_oid_t *oid)
{
	if (!wf || !wf->semantic || !oid) return false;
	const struct anx_wf_semantic_guard *guard = wf->semantic;
	if (!anx_uuid_compare(oid, &guard->oid)) return true;
	/* Keep the original private set even after public evidence becomes invalid. */
	if (guard->resource_count > ANX_SEMANTIC_RESOURCES_MAX) return true;
	for (uint32_t i = 0; i < guard->resource_count; i++)
		if (!anx_uuid_compare(oid, &guard->resources[i])) return true;
	return false;
}

int anx_wf_semantic_resolve(const anx_oid_t *oid, const char *name, anx_oid_t *out)
{
	if (!oid || !out || !valid_name(name)) return ANX_EINVAL;
	struct anx_wf_object *wf = anx_wf_object_get(oid);
	if (!wf || !wf->semantic) return ANX_ENOENT;
	struct semantic_image *image = anx_alloc(sizeof(*image));
	if (!image) return ANX_ENOMEM;
	int ret = load_checked(wf, image);
	if (ret == ANX_OK) {
		ret = ANX_ENOENT;
		for (uint32_t i = 0; i < image->spec.resource_count; i++)
			if (!anx_strcmp(name, image->spec.resources[i].name)) { *out = image->spec.resources[i].oid; ret = ANX_OK; break; }
	}
	anx_free(image);
	return ret;
}
