/* Signed authority commitments survive cell replacement and gate new admission. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/identity.h>
#include <anx/crypto.h>
#include <anx/cell_trace.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct identity_context { struct anx_cell *target; anx_oid_t identity; uint32_t calls; };

static int identity_handler(struct anx_external_call *call, void *context)
{
	struct identity_context *state = context;
	(void)call;
	state->calls++;
	if (anx_identity_bind(state->target, &state->identity) != ANX_EPERM)
		return -2210;
	return ANX_OK;
}

static int sign_commitment(const struct anx_identity_commitment *c, const uint8_t key[64],
			   uint8_t digest[32], uint8_t signature[64])
{
	int ret = anx_identity_digest(c, digest);
	if (ret == ANX_OK)
		anx_ed25519_sign(signature, digest, 32, key);
	return ret;
}

static void caller_permissions(struct anx_cell *cell)
{
	cell->execution.allow_network = true;
	cell->execution.allow_remote_models = false;
	cell->execution.allow_recursive_cells = true;
	cell->execution.allow_side_effects = true;
	cell->execution.max_recursion_depth = cell->constraints.max_recursion_depth = 4;
	cell->constraints.max_child_cells = 2;
}

static int check_record(const struct anx_identity_view *view, const anx_oid_t *parent)
{
	struct anx_object_handle handle = {0};
	uint8_t payload[ANX_IDENTITY_RECORD_BYTES], digest[32];
	int ret = anx_so_open(&view->record_oid, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK)
		return ret;
	ret = -2211;
	if (handle.obj->state != ANX_OBJ_SEALED || !handle.obj->retention.deletion_hold ||
	    handle.obj->payload_size != sizeof(payload) ||
	    anx_so_read_payload(&handle, 0, payload, sizeof(payload)) != (int)sizeof(payload))
		goto out;
	if (parent && (handle.obj->parent_count != 1 || anx_uuid_compare(&handle.obj->parent_oids[0], parent)))
		goto out;
	anx_sha256(payload, ANX_IDENTITY_WIRE_BYTES, digest);
	if (anx_memcmp(digest, view->digest, 32) ||
	    anx_ed25519_verify(payload + ANX_IDENTITY_WIRE_BYTES, digest, 32, view->commitment.operator_key))
		goto out;
	ret = ANX_OK;
out:
	anx_so_close(&handle);
	return ret;
}

static int check_identity_trace(struct anx_cell *cell, const anx_oid_t *record)
{
	struct anx_cell_trace *trace = anx_zalloc(sizeof(*trace));
	struct anx_object_handle handle = {0};
	char oid[37];
	int ret = ANX_ENOMEM;
	bool found = false;
	if (!trace)
		return ret;
	ret = anx_so_open(&cell->trace_oid, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK)
		goto out;
	ret = -2212;
	if (handle.obj->state != ANX_OBJ_SEALED ||
	    anx_so_read_payload(&handle, 0, trace, sizeof(*trace)) != (int)sizeof(*trace) ||
	    trace->denied_gate != ANX_ADMISSION_IDENTITY)
		goto out;
	anx_uuid_to_string(record, oid, sizeof(oid));
	for (uint32_t i = 0; i < trace->event_count; i++)
		if (trace->events[i].type == ANX_TRACE_IDENTITY_COMMITMENT &&
		    anx_strcmp(trace->events[i].description, oid) == 0 && trace->events[i].status_code == ANX_EPERM)
			found = true;
	ret = found ? ANX_OK : -2213;
out:
	if (handle.obj)
		anx_so_close(&handle);
	anx_free(trace);
	return ret;
}

int anx_research_day022(void)
{
	struct anx_identity_commitment root = {0}, next;
	struct anx_identity_view initial, widened, revoked, current;
	struct anx_cell *first = NULL, *replacement = NULL, *child = NULL, *fresh = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_external_call *call = NULL;
	struct identity_context context = {0};
	uint8_t seed[32] = {0x22}, key[64], other[64], other_pub[32];
	uint8_t signature[64], good_signature[64], digest[32];
	anx_cid_t first_id;
	int rc;

	root.schema = 1;
	root.generation = 1;
	root.authority = ANX_CAP_AUTH_SIDE_EFFECT | ANX_CAP_AUTH_DERIVE_CELL;
	anx_uuid_generate(&root.identity_id);
	anx_ed25519_keypair(root.operator_key, key, seed);
	seed[0] = 0x23;
	anx_ed25519_keypair(other_pub, other, seed);
	rc = sign_commitment(&root, key, digest, signature);
	if (rc != ANX_OK)
		goto out;
	rc = -2201;
	if (anx_identity_create(&root, signature) != ANX_OK ||
	    anx_identity_get(&root.identity_id, &initial) != ANX_OK)
		goto out;
	next = root;
	next.generation = 2;
	next.authority |= ANX_CAP_AUTH_NETWORK;
	anx_memcpy(next.previous_digest, initial.digest, 32);
	anx_memset(signature, 0, sizeof(signature));
	rc = -2202;
	if (anx_identity_transition(&next, signature) != ANX_EPERM)
		goto out;
	sign_commitment(&next, other, digest, signature);
	if (anx_identity_transition(&next, signature) != ANX_EPERM)
		goto out;
	sign_commitment(&next, key, digest, signature);
	anx_memcpy(good_signature, signature, 64);
	next.authority |= ANX_CAP_AUTH_REMOTE_MODEL;
	if (anx_identity_transition(&next, signature) != ANX_EPERM)
		goto out;
	next.authority &= ~ANX_CAP_AUTH_REMOTE_MODEL;
	if (anx_identity_get(&root.identity_id, &current) != ANX_OK ||
	    current.commitment.generation != 1 || anx_memcmp(current.digest, initial.digest, 32))
		goto out;
	rc = -2203;
	if (anx_identity_transition(&next, signature) != ANX_OK ||
	    anx_identity_get(&root.identity_id, &widened) != ANX_OK ||
	    widened.commitment.generation != 2 || !anx_memcmp(widened.digest, initial.digest, 32) ||
	    anx_identity_transition(&next, good_signature) != ANX_EBUSY)
		goto out;
	rc = check_record(&initial, NULL);
	if (rc != ANX_OK)
		goto out;
	rc = check_record(&widened, &initial.record_oid);
	if (rc != ANX_OK)
		goto out;
	anx_strlcpy(intent.name, "research-day-022-first", sizeof(intent.name));
	rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &first);
	if (rc != ANX_OK)
		goto out;
	first_id = first->cid;
	anx_strlcpy(intent.name, "research-day-022-replacement", sizeof(intent.name));
	rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &replacement);
	if (rc != ANX_OK)
		goto out;
	context.target = replacement;
	context.identity = root.identity_id;
	rc = anx_external_register_handler("anxresearch022", identity_handler, &context);
	if (rc != ANX_OK)
		goto out;
	call = anx_zalloc(sizeof(*call));
	rc = ANX_ENOMEM;
	if (!call)
		goto out;
	anx_strlcpy(call->endpoint, "anxresearch022://run", sizeof(call->endpoint));
	caller_permissions(first);
	first->ext_call = call;
	rc = -2204;
	if (anx_identity_bind(first, &root.identity_id) != ANX_OK || anx_cell_run(first) != ANX_OK || context.calls != 1 ||
	    !anx_uuid_is_nil(&replacement->identity_id))
		goto out;
	anx_cell_destroy(first);
	first = NULL;
	caller_permissions(replacement);
	replacement->ext_call = call;
	if (anx_identity_bind(replacement, &root.identity_id) != ANX_OK ||
	    !anx_uuid_compare(&first_id, &replacement->cid) ||
	    anx_identity_get(&root.identity_id, &current) != ANX_OK || anx_memcmp(current.digest, widened.digest, 32))
		goto out;
	rc = anx_cell_derive_child(replacement, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &child);
	if (rc != ANX_OK)
		goto out;
	child->ext_call = call;
	rc = -2205;
	if (anx_uuid_compare(&child->identity_id, &root.identity_id))
		goto out;
	next = widened.commitment;
	next.generation = 3;
	next.authority &= ~ANX_CAP_AUTH_SIDE_EFFECT;
	anx_memcpy(next.previous_digest, widened.digest, 32);
	sign_commitment(&next, key, digest, signature);
	if (anx_identity_transition(&next, signature) != ANX_OK ||
	    anx_identity_get(&root.identity_id, &revoked) != ANX_OK ||
	    anx_cell_run(child) != ANX_EPERM || anx_cell_run(replacement) != ANX_EPERM || context.calls != 1)
		goto out;
	rc = check_identity_trace(child, &revoked.record_oid);
	if (rc != ANX_OK)
		goto out;
	rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &fresh);
	if (rc != ANX_OK)
		goto out;
	caller_permissions(fresh);
	fresh->ext_call = call;
	rc = -2206;
	if (anx_identity_bind(fresh, &root.identity_id) != ANX_OK ||
	    anx_cell_run(fresh) != ANX_EPERM || context.calls != 1)
		goto out;
	anx_cell_destroy(fresh);
	fresh = NULL;
	next = widened.commitment;
	if (anx_identity_transition(&next, good_signature) != ANX_EBUSY)
		goto out;
	next.generation = 4;
	anx_memcpy(next.previous_digest, revoked.digest, 32);
	sign_commitment(&next, key, digest, signature);
	rc = -2207;
	if (anx_identity_transition(&next, signature) != ANX_OK ||
	    anx_identity_get(&root.identity_id, &current) != ANX_OK ||
	    !anx_memcmp(current.digest, widened.digest, 32))
		goto out;
	rc = check_record(&current, &revoked.record_oid);
	if (rc != ANX_OK)
		goto out;
	rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &fresh);
	if (rc != ANX_OK)
		goto out;
	caller_permissions(fresh);
	fresh->ext_call = call;
	rc = -2208;
	if (anx_identity_bind(fresh, &root.identity_id) != ANX_OK ||
	    anx_cell_run(fresh) != ANX_OK || context.calls != 2 ||
	    anx_so_delete(&initial.record_oid, false) != ANX_EPERM)
		goto out;
	rc = ANX_OK;
out:
	if (child)
		anx_cell_destroy(child);
	if (replacement)
		anx_cell_destroy(replacement);
	if (first)
		anx_cell_destroy(first);
	if (fresh)
		anx_cell_destroy(fresh);
	if (call)
		anx_free(call);
	anx_external_unregister_handler("anxresearch022");
	return rc;
}
#endif
