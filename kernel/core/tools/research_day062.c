/* Required results and competing trials obey different completion rules. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/branch_group.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
int anx_research_day062(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *prompt = NULL;
	struct anx_so_create_params params = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "~", .payload_size = 1 };
	struct anx_branch_spec *spec = anx_zalloc(sizeof(*spec));
	struct anx_branch_view group = {0};
	int ret = ANX_ENOMEM;
	if (!spec) return ret;
	anx_strlcpy(intent.name, "research-day-062", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_so_create(&params, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_recursive_cells = true;
	spec->schema = 1; spec->count = 2; spec->token_budget = 8; spec->semantics = ANX_BRANCH_TRIAL; spec->prompt = prompt->oid;
	for (uint32_t i = 0; i < 2; i++) {
		spec->candidates[i].image = (struct anx_adapter_image){ .format = 1, .count = 2,
			.deltas = {{'~','A',4096},{'A','A',4096}} };
		spec->candidates[i].maximum_tokens = spec->candidates[i].expected_size = 4;
		anx_memcpy(spec->candidates[i].expected, "AAAA", 4);
	}
	ret = -6201;
	if (anx_branch_group_create(&owner->cid, spec, &group) != ANX_OK) goto out;
	ret = ANX_OK;
out:
	if (group.id) anx_branch_group_destroy(group.id);
	if (owner) anx_cell_destroy(owner);
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	anx_free(spec);
	return ret;
}
#endif
