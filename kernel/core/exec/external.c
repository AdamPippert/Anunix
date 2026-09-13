/*
 * external.c — External-call handler registry.
 *
 * Tiny linear registry keyed by URI scheme. Looked up on every
 * anx_external_invoke() call, so handlers stay cheap to register
 * and cheap to replace (tests register and tear down per case).
 */

#include <anx/types.h>
#include <anx/external_call.h>
#include <anx/cell.h>
#include <anx/identity.h>
#include <anx/effect_fence.h>
#include <anx/string.h>

#define ANX_EXT_MAX_HANDLERS	16

struct anx_ext_slot {
	char scheme[ANX_EXT_SCHEME_MAX];
	anx_external_handler_fn fn;
	void *ctx;
	bool active;
};

static struct anx_ext_slot handlers[ANX_EXT_MAX_HANDLERS];
static bool initialized;

void anx_external_init(void)
{
	uint32_t i;
	if (anx_cell_current_id())
		return;

	for (i = 0; i < ANX_EXT_MAX_HANDLERS; i++) {
		handlers[i].scheme[0] = '\0';
		handlers[i].fn = NULL;
		handlers[i].ctx = NULL;
		handlers[i].active = false;
	}
	initialized = true;
}

/*
 * Extract the scheme prefix — characters up to the first ':' — into
 * `out`. Returns ANX_OK on success, ANX_EINVAL if the endpoint has no
 * scheme separator or the scheme is empty/too long.
 */
static int parse_scheme(const char *endpoint, char *out, uint32_t out_size)
{
	uint32_t i;

	if (!endpoint || !out || out_size == 0)
		return ANX_EINVAL;

	for (i = 0; endpoint[i] != '\0' && endpoint[i] != ':'; i++) {
		if (i + 1 >= out_size)
			return ANX_EINVAL;
		out[i] = endpoint[i];
	}
	if (i == 0 || endpoint[i] != ':')
		return ANX_EINVAL;
	out[i] = '\0';
	return ANX_OK;
}

static struct anx_ext_slot *find_slot(const char *scheme)
{
	uint32_t i;

	for (i = 0; i < ANX_EXT_MAX_HANDLERS; i++) {
		if (!handlers[i].active)
			continue;
		if (anx_strcmp(handlers[i].scheme, scheme) == 0)
			return &handlers[i];
	}
	return NULL;
}

int anx_external_register_handler(const char *scheme,
				  anx_external_handler_fn fn,
				  void *ctx)
{
	struct anx_ext_slot *slot;
	uint32_t i;

	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!scheme || !fn)
		return ANX_EINVAL;
	if (!initialized)
		anx_external_init();

	slot = find_slot(scheme);
	if (slot) {
		slot->fn = fn;
		slot->ctx = ctx;
		return ANX_OK;
	}

	for (i = 0; i < ANX_EXT_MAX_HANDLERS; i++) {
		if (handlers[i].active)
			continue;
		anx_strlcpy(handlers[i].scheme, scheme,
			    sizeof(handlers[i].scheme));
		handlers[i].fn = fn;
		handlers[i].ctx = ctx;
		handlers[i].active = true;
		return ANX_OK;
	}
	return ANX_ENOMEM;
}

int anx_external_unregister_handler(const char *scheme)
{
	struct anx_ext_slot *slot;
	if (anx_cell_current_id())
		return ANX_EPERM;

	if (!scheme)
		return ANX_EINVAL;
	slot = find_slot(scheme);
	if (!slot)
		return ANX_ENOENT;
	slot->active = false;
	slot->fn = NULL;
	slot->ctx = NULL;
	slot->scheme[0] = '\0';
	return ANX_OK;
}

int anx_external_invoke(struct anx_external_call *call)
{
	char scheme[ANX_EXT_SCHEME_MAX];
	struct anx_ext_slot *slot;
	int ret;

	if (!call)
		return ANX_EINVAL;
	if (!initialized)
		anx_external_init();

	ret = parse_scheme(call->endpoint, scheme, sizeof(scheme));
	if (ret != ANX_OK)
		return ret;

	slot = find_slot(scheme);
	if (!slot)
		return ANX_ENOENT;

	call->response_size = 0;
	call->status_code = 0;
	if (anx_cell_current_id()) {
		struct anx_cell *caller = anx_cell_store_lookup(anx_cell_current_id());
		if (!caller)
			return ANX_EPERM;
		ret = ANX_EPERM;
		if (caller->execution.allow_side_effects && !anx_cell_status_terminal(caller->status) &&
		    anx_identity_admit(caller, NULL) == ANX_OK)
			ret = anx_effect_fence_check(caller, NULL, NULL);
		if (ret == ANX_OK)
			ret = anx_tool_authorize_call(caller, call);
		anx_cell_store_release(caller);
		if (ret != ANX_OK)
			return ret;
	}
	return slot->fn(call, slot->ctx);
}
