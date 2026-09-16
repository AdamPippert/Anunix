#include <anx/tool_namespace.h>
#include <anx/cell.h>
#include <anx/capability.h>
#include <anx/external_call.h>
#include <anx/identity.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct tool_slot {
	struct anx_tool_ref ref;
	struct anx_tool_descriptor descriptor;
	bool active;
};
struct namespace_slot {
	struct anx_tool_namespace_ref ref;
	struct anx_tool_grant grants[ANX_TOOL_WORKING_SET_MAX];
	uint32_t count;
};

static struct tool_slot catalog[ANX_TOOL_CATALOG_MAX];
static struct namespace_slot namespaces[ANX_TOOL_NAMESPACE_MAX];
static uint32_t namespace_count;
static struct anx_spinlock namespace_lock = ANX_SPINLOCK_INIT;

static bool bounded_text(const char *text, uint32_t size)
{
	if (!text[0])
		return false;
	for (uint32_t i = 1; i < size; i++)
		if (!text[i])
			return true;
	return false;
}

static bool valid_descriptor(const struct anx_tool_descriptor *d)
{
	const char *separator;
	if (!d || !bounded_text(d->name, sizeof(d->name)) ||
	    !bounded_text(d->endpoint, sizeof(d->endpoint)) ||
	    !bounded_text(d->method, sizeof(d->method)) ||
	    !bounded_text(d->schema_version, sizeof(d->schema_version)) ||
	    (d->required_authority & ~ANX_CAP_AUTH_ALL))
		return false;
	separator = anx_strstr(d->endpoint, "://");
	return separator && separator > d->endpoint && separator - d->endpoint < ANX_EXT_SCHEME_MAX;
}

static struct tool_slot *find_tool(const anx_oid_t *id)
{
	for (uint32_t i = 0; i < ANX_TOOL_CATALOG_MAX; i++)
		if (catalog[i].active && !anx_uuid_compare(&catalog[i].ref.id, id))
			return &catalog[i];
	return NULL;
}

static bool name_conflict(const char *name, const struct tool_slot *self)
{
	for (uint32_t i = 0; i < ANX_TOOL_CATALOG_MAX; i++)
		if (catalog[i].active && &catalog[i] != self && !anx_strcmp(catalog[i].descriptor.name, name))
			return true;
	return false;
}

static struct namespace_slot *find_namespace(const anx_oid_t *id)
{
	for (uint32_t i = 0; i < namespace_count; i++)
		if (!anx_uuid_compare(&namespaces[i].ref.id, id))
			return &namespaces[i];
	return NULL;
}

static int validate_grants(const struct anx_tool_grant *grants, uint32_t count)
{
	if (count > ANX_TOOL_WORKING_SET_MAX || (count && !grants))
		return ANX_EINVAL;
	for (uint32_t i = 0; i < count; i++) {
		if (!(grants[i].rights & ANX_TOOL_DISCOVER) ||
		    (grants[i].rights & ~(ANX_TOOL_DISCOVER | ANX_TOOL_INVOKE)) ||
		    anx_uuid_is_nil(&grants[i].tool_id))
			return ANX_EINVAL;
		if (!find_tool(&grants[i].tool_id))
			return ANX_ENOENT;
		for (uint32_t j = 0; j < i; j++)
			if (!anx_uuid_compare(&grants[i].tool_id, &grants[j].tool_id))
				return ANX_EINVAL;
	}
	return ANX_OK;
}

static uint32_t authority(const struct anx_cell *cell)
{
	uint32_t mask = 0;
	if (cell->execution.allow_network) mask |= ANX_CAP_AUTH_NETWORK;
	if (cell->execution.allow_remote_models) mask |= ANX_CAP_AUTH_REMOTE_MODEL;
	if (cell->execution.allow_recursive_cells) mask |= ANX_CAP_AUTH_DERIVE_CELL;
	if (cell->execution.allow_side_effects) mask |= ANX_CAP_AUTH_SIDE_EFFECT;
	return mask;
}

static struct anx_cell *registered_caller(const struct anx_cell *cell)
{
	const anx_cid_t *active = anx_cell_current_id();
	struct anx_cell *registered;
	if (!cell || anx_cell_status_terminal(cell->status) ||
	    (active && anx_uuid_compare(active, &cell->cid)))
		return NULL;
	registered = anx_cell_store_lookup(&cell->cid);
	if (registered && registered != cell) {
		anx_cell_store_release(registered);
		return NULL;
	}
	return registered;
}

int anx_tool_register(const struct anx_tool_descriptor *d, struct anx_tool_ref *out)
{
	anx_oid_t id;
	bool flags;
	int ret = ANX_EFULL;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!out || !valid_descriptor(d))
		return ANX_EINVAL;
	anx_uuid_generate(&id);
	anx_spin_lock_irqsave(&namespace_lock, &flags);
	if (name_conflict(d->name, NULL) || find_tool(&id)) {
		ret = ANX_EEXIST;
		goto unlock;
	}
	for (uint32_t i = 0; i < ANX_TOOL_CATALOG_MAX; i++)
		if (!catalog[i].active) {
			catalog[i].ref.id = id;
			catalog[i].ref.generation = 1;
			catalog[i].descriptor = *d;
			catalog[i].active = true;
			*out = catalog[i].ref;
			ret = ANX_OK;
			break;
		}
unlock:
	anx_spin_unlock_irqrestore(&namespace_lock, flags);
	return ret;
}

int anx_tool_update(const struct anx_tool_ref *expected, const struct anx_tool_descriptor *d,
		    struct anx_tool_ref *out)
{
	struct tool_slot *tool;
	bool flags;
	int ret = ANX_OK;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!expected || !expected->generation || !out || !valid_descriptor(d))
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&namespace_lock, &flags);
	tool = find_tool(&expected->id);
	if (!tool)
		ret = ANX_ENOENT;
	else if (tool->ref.generation != expected->generation)
		ret = ANX_EBUSY;
	else if (tool->ref.generation == ~(uint64_t)0)
		ret = ANX_EFULL;
	else if (name_conflict(d->name, tool))
		ret = ANX_EEXIST;
	else {
		tool->descriptor = *d;
		tool->ref.generation++;
		*out = tool->ref;
	}
	anx_spin_unlock_irqrestore(&namespace_lock, flags);
	return ret;
}

int anx_tool_remove(const struct anx_tool_ref *expected)
{
	struct tool_slot *tool;
	bool flags;
	int ret = ANX_OK;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!expected || !expected->generation)
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&namespace_lock, &flags);
	tool = find_tool(&expected->id);
	if (!tool)
		ret = ANX_ENOENT;
	else if (tool->ref.generation != expected->generation)
		ret = ANX_EBUSY;
	else
		tool->active = false;
	anx_spin_unlock_irqrestore(&namespace_lock, flags);
	return ret;
}

int anx_tool_namespace_create(struct anx_cell *root, const struct anx_tool_grant *grants,
			      uint32_t count, struct anx_tool_namespace_ref *out)
{
	struct anx_cell *registered;
	struct namespace_slot *ns;
	anx_oid_t id;
	bool flags;
	int ret;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!root || !out || count > ANX_TOOL_WORKING_SET_MAX || (count && !grants))
		return ANX_EINVAL;
	registered = registered_caller(root);
	if (!registered)
		return ANX_EPERM;
	anx_uuid_generate(&id);
	anx_spin_lock(&root->lock);
	if (root->runtime_active || root->status != ANX_CELL_CREATED ||
	    !anx_uuid_is_nil(&root->parent_cid) || !anx_uuid_is_nil(&root->tool_namespace_id)) {
		ret = ANX_EBUSY;
		goto release;
	}
	anx_spin_lock_irqsave(&namespace_lock, &flags);
	ret = validate_grants(grants, count);
	if (ret != ANX_OK)
		goto unlock;
	if (namespace_count == ANX_TOOL_NAMESPACE_MAX || find_namespace(&id)) {
		ret = ANX_EFULL;
		goto unlock;
	}
	ns = &namespaces[namespace_count++];
	anx_memset(ns, 0, sizeof(*ns));
	ns->ref.id = id;
	ns->ref.generation = 1;
	ns->count = count;
	if (count)
		anx_memcpy(ns->grants, grants, count * sizeof(grants[0]));
	root->tool_namespace_id = id;
	*out = ns->ref;
unlock:
	anx_spin_unlock_irqrestore(&namespace_lock, flags);
release:
	anx_spin_unlock(&root->lock);
	anx_cell_store_release(registered);
	return ret;
}

int anx_tool_namespace_replace(const struct anx_tool_namespace_ref *expected,
			       const struct anx_tool_grant *grants, uint32_t count,
			       struct anx_tool_namespace_ref *out)
{
	struct namespace_slot *ns;
	bool flags;
	int ret;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!expected || !expected->generation || !out ||
	    count > ANX_TOOL_WORKING_SET_MAX || (count && !grants))
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&namespace_lock, &flags);
	ns = find_namespace(&expected->id);
	if (!ns)
		ret = ANX_ENOENT;
	else if (ns->ref.generation != expected->generation)
		ret = ANX_EBUSY;
	else if (ns->ref.generation == ~(uint64_t)0)
		ret = ANX_EFULL;
	else {
		ret = validate_grants(grants, count);
		if (ret == ANX_OK) {
			anx_memset(ns->grants, 0, sizeof(ns->grants));
			if (count)
				anx_memcpy(ns->grants, grants, count * sizeof(grants[0]));
			ns->count = count;
			ns->ref.generation++;
			*out = ns->ref;
		}
	}
	anx_spin_unlock_irqrestore(&namespace_lock, flags);
	return ret;
}

int anx_tool_discover(struct anx_cell *cell, struct anx_tool_catalog_entry *out,
		      uint32_t capacity, uint32_t *count_out)
{
	struct anx_cell *registered;
	struct namespace_slot *ns;
	uint32_t count = 0, mask;
	bool flags;
	int ret = ANX_OK;
	if (!out || !count_out || !capacity || capacity > ANX_TOOL_WORKING_SET_MAX)
		return ANX_EINVAL;
	registered = registered_caller(cell);
	if (!registered)
		return ANX_EPERM;
	if (anx_identity_admit(cell, NULL) != ANX_OK) {
		ret = ANX_EPERM;
		goto release;
	}
	mask = authority(cell);
	anx_spin_lock_irqsave(&namespace_lock, &flags);
	ns = find_namespace(&cell->tool_namespace_id);
	if (!ns) {
		ret = ANX_ENOENT;
		goto unlock;
	}
	for (uint32_t i = 0; i < ns->count && count < capacity; i++) {
		struct tool_slot *tool = find_tool(&ns->grants[i].tool_id);
		if (!tool || !(ns->grants[i].rights & ANX_TOOL_DISCOVER) ||
		    (tool->descriptor.required_authority & ~mask))
			continue;
		anx_memset(&out[count], 0, sizeof(out[count]));
		out[count].descriptor = tool->descriptor;
		out[count].handle.namespace = ns->ref;
		out[count].handle.tool = tool->ref;
		count++;
	}
	*count_out = count;
unlock:
	anx_spin_unlock_irqrestore(&namespace_lock, flags);
release:
	anx_cell_store_release(registered);
	return ret;
}

int anx_tool_authorize_call(const struct anx_cell *cell, const struct anx_external_call *call)
{
	struct anx_cell *registered;
	struct namespace_slot *ns;
	struct tool_slot *tool;
	uint32_t rights = 0;
	bool flags;
	int ret = ANX_EPERM;
	if (!cell || !call)
		return ANX_EINVAL;
	if (anx_uuid_is_nil(&cell->tool_namespace_id))
		return anx_uuid_is_nil(&call->tool_handle.namespace.id) &&
		       anx_uuid_is_nil(&call->tool_handle.tool.id) ? ANX_OK : ANX_EPERM;
	registered = registered_caller(cell);
	if (!registered)
		return ANX_EPERM;
	if (anx_identity_admit(cell, NULL) != ANX_OK)
		goto release;
	anx_spin_lock_irqsave(&namespace_lock, &flags);
	if (anx_uuid_compare(&cell->tool_namespace_id, &call->tool_handle.namespace.id))
		goto unlock;
	ns = find_namespace(&cell->tool_namespace_id);
	if (!ns) {
		ret = ANX_ENOENT;
		goto unlock;
	}
	if (ns->ref.generation != call->tool_handle.namespace.generation) {
		ret = ANX_EBUSY;
		goto unlock;
	}
	for (uint32_t i = 0; i < ns->count; i++)
		if (!anx_uuid_compare(&ns->grants[i].tool_id, &call->tool_handle.tool.id))
			rights = ns->grants[i].rights;
	if ((rights & (ANX_TOOL_DISCOVER | ANX_TOOL_INVOKE)) != (ANX_TOOL_DISCOVER | ANX_TOOL_INVOKE))
		goto unlock;
	tool = find_tool(&call->tool_handle.tool.id);
	if (!tool) {
		ret = ANX_ENOENT;
		goto unlock;
	}
	if (tool->ref.generation != call->tool_handle.tool.generation) {
		ret = ANX_EBUSY;
		goto unlock;
	}
	if (tool->descriptor.required_authority & ~authority(cell))
		goto unlock;
	if (!bounded_text(call->endpoint, sizeof(call->endpoint)) ||
	    !bounded_text(call->method, sizeof(call->method)) || (call->request_size && !call->request_body)) {
		ret = ANX_EINVAL;
		goto unlock;
	}
	if (anx_strcmp(call->endpoint, tool->descriptor.endpoint) ||
	    anx_strcmp(call->method, tool->descriptor.method))
		goto unlock;
	ret = ANX_OK;
unlock:
	anx_spin_unlock_irqrestore(&namespace_lock, flags);
release:
	anx_cell_store_release(registered);
	return ret;
}
