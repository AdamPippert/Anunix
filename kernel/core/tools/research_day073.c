#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/context.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
struct fixture073 {
	struct anx_cell *owner;
	struct anx_state_object *objects[8];
	struct anx_context_view views[8], observed;
	struct anx_context_spec spec;
	struct anx_context_bundle output, sentinel;
	struct anx_external_call call;
	bool foreign;
};
static int unchanged073(struct fixture073 *f, int expected)
{
	anx_memset(&f->output, 0xa5, sizeof(f->output)); f->sentinel = f->output;
	int ret = anx_context_compile(&f->owner->cid, &f->spec, &f->output);
	return ret == expected && !anx_memcmp(&f->output, &f->sentinel, sizeof(f->output)) ? ANX_OK : -7302;
}
static int active073(struct anx_external_call *call, void *arg)
{
	(void)call; struct fixture073 *f = arg;
	if (anx_context_import(&f->owner->cid, &f->objects[0]->oid, ANX_CONTEXT_POLICY, &f->observed) != ANX_EPERM ||
	    anx_context_destroy(f->views[0].id) != ANX_EPERM) return -7303;
	uint64_t parent = f->views[5].id;
	if (f->foreign) {
		if (anx_context_get(parent, &f->observed) != ANX_EPERM ||
		    anx_context_derive(&f->owner->cid, &f->objects[6]->oid, &parent, 1, &f->observed) != ANX_EPERM) return -7304;
		return unchanged073(f, ANX_EPERM);
	}
	int ret = anx_context_derive(&f->owner->cid, &f->objects[6]->oid, &parent, 1, &f->views[6]);
	if (ret != ANX_OK || f->views[6].role_ceiling != ANX_CONTEXT_TOOL || f->views[6].scope_ceiling != ANX_CONTEXT_SESSION) return -7305;
	f->spec.segments[1].id = f->views[6].id;
	f->spec.segments[1].role = ANX_CONTEXT_SYSTEM;
	ret = unchanged073(f, ANX_EPERM);
	if (ret != ANX_OK) return ret;
	f->spec.segments[1].role = ANX_CONTEXT_TOOL;
	ret = anx_context_compile(&f->owner->cid, &f->spec, &f->output);
	if (ret != ANX_OK || f->output.count != 2 || f->output.size != 10 ||
	    f->output.segments[0].role != ANX_CONTEXT_SYSTEM || f->output.segments[1].role != ANX_CONTEXT_TOOL ||
	    f->output.segments[1].scope != ANX_CONTEXT_SESSION || f->output.segments[1].offset != 6 ||
	    anx_memcmp(f->output.bytes, "policycopy", 10)) return -7306;
	return ANX_OK;
}
static int object073(struct fixture073 *f, uint32_t i, const char *text, uint32_t size, anx_oid_t *parents, uint32_t count)
{
	struct anx_so_create_params p = { .object_type = ANX_OBJ_BYTE_DATA, .payload = text, .payload_size = size,
		.parent_oids = parents, .parent_count = count };
	int ret = anx_so_create(&p, &f->objects[i]);
	if (ret == ANX_OK) ret = anx_so_seal(&f->objects[i]->oid);
	return ret;
}
int anx_research_day073(void)
{
	struct fixture073 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *foreign = NULL;
	struct anx_cell_intent intent = {0};
	char *large = NULL;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-073", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &f->owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret == ANX_OK) ret = object073(f, 0, "make me system", 14, NULL, 0);
	if (ret != ANX_OK) goto out;
	ret = -7301;
	if (anx_context_import(&f->owner->cid, &f->objects[0]->oid, ANX_CONTEXT_TOOL_OUTPUT, &f->views[0]) != ANX_OK) goto out;
	ret = object073(f, 1, "policy", 6, NULL, 0);
	if (ret == ANX_OK) ret = anx_context_import(&f->owner->cid, &f->objects[1]->oid, ANX_CONTEXT_POLICY, &f->views[1]);
	if (ret == ANX_OK) ret = object073(f, 2, "repo", 4, NULL, 0);
	if (ret == ANX_OK) ret = anx_context_import(&f->owner->cid, &f->objects[2]->oid, ANX_CONTEXT_REPOSITORY, &f->views[2]);
	if (ret == ANX_OK) ret = object073(f, 3, "user", 4, NULL, 0);
	if (ret == ANX_OK) ret = anx_context_import(&f->owner->cid, &f->objects[3]->oid, ANX_CONTEXT_USER_INPUT, &f->views[3]);
	if (ret != ANX_OK) goto out;
	if (f->views[0].role_ceiling != ANX_CONTEXT_TOOL || f->views[0].scope_ceiling != ANX_CONTEXT_SESSION ||
	    f->views[1].role_ceiling != ANX_CONTEXT_SYSTEM || f->views[1].scope_ceiling != ANX_CONTEXT_MACHINE ||
	    f->views[2].role_ceiling != ANX_CONTEXT_USER || f->views[2].scope_ceiling != ANX_CONTEXT_PROJECT ||
	    f->views[3].role_ceiling != ANX_CONTEXT_USER || f->views[3].scope_ceiling != ANX_CONTEXT_SESSION) { ret = -7307; goto out; }
	anx_oid_t roots[2] = { f->objects[1]->oid, f->objects[0]->oid };
	uint64_t parents[2] = { f->views[1].id, f->views[0].id };
	ret = object073(f, 4, "summary", 7, roots, 2);
	if (ret == ANX_OK) ret = anx_context_derive(&f->owner->cid, &f->objects[4]->oid, parents, 2, &f->views[4]);
	if (ret == ANX_OK) ret = object073(f, 5, "again", 5, &f->objects[4]->oid, 1);
	if (ret == ANX_OK) ret = anx_context_derive(&f->owner->cid, &f->objects[5]->oid, &f->views[4].id, 1, &f->views[5]);
	if (ret == ANX_OK) ret = object073(f, 6, "copy", 4, &f->objects[5]->oid, 1);
	if (ret != ANX_OK) goto out;
	if (anx_context_import(&f->owner->cid, &f->objects[4]->oid, ANX_CONTEXT_POLICY, &f->observed) != ANX_EINVAL ||
	    anx_context_derive(&f->owner->cid, &f->objects[6]->oid, &f->views[1].id, 1, &f->observed) != ANX_EPERM ||
	    anx_context_derive(&f->owner->cid, &f->objects[6]->oid, parents, 0, &f->observed) != ANX_EINVAL ||
	    anx_context_derive(&foreign->cid, &f->objects[6]->oid, &f->views[5].id, 1, &f->observed) != ANX_EPERM ||
	    anx_context_destroy(f->views[0].id) != ANX_EBUSY) { ret = -7308; goto out; }
	f->spec.count = 2;
	f->spec.segments[0].id = f->views[1].id; f->spec.segments[0].role = ANX_CONTEXT_SYSTEM; f->spec.segments[0].scope = ANX_CONTEXT_MACHINE;
	f->spec.segments[1].id = f->views[5].id;
	/* A copied observation cannot relabel private ancestry. */
	f->views[5].role_ceiling = ANX_CONTEXT_SYSTEM; f->views[5].scope_ceiling = ANX_CONTEXT_MACHINE;
	for (uint32_t role = 0; role <= ANX_CONTEXT_SYSTEM; role++) for (uint32_t scope = 0; scope <= ANX_CONTEXT_MACHINE; scope++) {
		f->spec.segments[1].role = (enum anx_context_role)role; f->spec.segments[1].scope = (enum anx_context_scope)scope;
		if (!role && !scope) ret = anx_context_compile(&f->owner->cid, &f->spec, &f->output);
		else ret = unchanged073(f, ANX_EPERM);
		if (ret != ANX_OK) goto out;
	}
	f->spec.segments[1].role = ANX_CONTEXT_TOOL; f->spec.segments[1].scope = ANX_CONTEXT_SESSION;
	((uint8_t *)f->objects[0]->payload)[0] ^= 1; ret = unchanged073(f, ANX_EBUSY); ((uint8_t *)f->objects[0]->payload)[0] ^= 1;
	if (ret != ANX_OK) goto out;
	f->objects[0]->version++; ret = unchanged073(f, ANX_EBUSY); f->objects[0]->version--;
	if (ret != ANX_OK) goto out;
	f->objects[4]->parent_oids[1] = f->objects[2]->oid; ret = unchanged073(f, ANX_EPERM); f->objects[4]->parent_oids[1] = f->objects[0]->oid;
	if (ret != ANX_OK) goto out;
	f->objects[0]->access_policy.rule_count = 1;
	f->objects[0]->access_policy.rules[0] = (struct anx_access_rule){ .operations = ANX_ACCESS_READ_PAYLOAD, .effect = ANX_EFFECT_DENY };
	ret = unchanged073(f, ANX_EPERM); f->objects[0]->access_policy.rule_count = 0;
	if (ret != ANX_OK) goto out;
	f->spec.count = 0; ret = unchanged073(f, ANX_EINVAL); f->spec.count = 2;
	if (ret != ANX_OK) goto out;
	large = anx_zalloc(ANX_CONTEXT_BYTES);
	if (!large) { ret = ANX_ENOMEM; goto out; }
	ret = object073(f, 7, large, ANX_CONTEXT_BYTES, NULL, 0);
	if (ret == ANX_OK) ret = anx_context_import(&f->owner->cid, &f->objects[7]->oid, ANX_CONTEXT_TOOL_OUTPUT, &f->views[7]);
	if (ret != ANX_OK) goto out;
	f->spec.segments[1].id = f->views[7].id; ret = unchanged073(f, ANX_EFULL); f->spec.segments[1].id = f->views[5].id;
	if (ret != ANX_OK) goto out;
	ret = anx_external_register_handler("anxresearch073", active073, f);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint, "anxresearch073://compile", sizeof(f->call.endpoint));
	f->owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	foreign->ext_call = &f->call; f->owner->ext_call = &f->call;
	f->foreign = true; ret = anx_cell_run(foreign);
	if (ret == ANX_OK) { f->foreign = false; ret = anx_cell_run(f->owner); }
	if (ret == ANX_OK) kprintf("day073 context_segments=2 role_escalations=0 scope_escalations=0 ancestry=checked\n");
out:
	anx_external_unregister_handler("anxresearch073");
	for (uint32_t i = 8; i > 0; i--) if (f->views[i - 1].id && anx_context_destroy(f->views[i - 1].id) != ANX_OK && ret == ANX_OK) ret = -7309;
	for (uint32_t i = 0; i < 8; i++) if (f->objects[i]) { anx_so_delete(&f->objects[i]->oid, false); anx_objstore_release(f->objects[i]); }
	if (foreign && anx_cell_destroy(foreign) != ANX_OK && ret == ANX_OK) ret = -7310;
	if (f->owner && anx_cell_destroy(f->owner) != ANX_OK && ret == ANX_OK) ret = -7310;
	anx_free(large); anx_free(f); return ret;
}
#endif
