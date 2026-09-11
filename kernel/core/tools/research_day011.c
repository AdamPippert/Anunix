/* Bounded memory admission preserves existing object contents and charges. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/memplane.h>
#include <anx/state_object.h>
#include <anx/string.h>

struct memory_test_state {
	struct anx_cell *cell;
	struct anx_state_object *objects[4];
	struct anx_mem_entry *entries[4];
};

static int memory_handler(struct anx_external_call *call, void *context)
{
	struct memory_test_state *s = context;
	struct anx_object_handle handle = {0};
	char bytes[8];
	int rc;
	(void)call;
	rc = anx_memplane_admit(&s->objects[0]->oid, ANX_ADMIT_CACHEABLE, &s->entries[0]);
	if (rc != ANX_OK)
		return rc;
	/* Nine bytes cannot fit after eight bytes in a sixteen-byte budget. */
	if (anx_memplane_admit(&s->objects[1]->oid, ANX_ADMIT_CACHEABLE, &s->entries[1]) != ANX_ENOMEM || s->entries[1])
		return -1101;
	if (s->cell->memory_admitted_bytes != 8 ||
	    anx_memplane_lookup(&s->objects[0]->oid) != s->entries[0] ||
	    !anx_mem_in_tier(s->entries[0], ANX_MEM_L0))
		return -1102;
	rc = anx_so_open(&s->objects[0]->oid, ANX_OPEN_READ, &handle);
	if (rc != ANX_OK)
		return rc;
	rc = anx_so_read_payload(&handle, 0, bytes, sizeof(bytes));
	anx_so_close(&handle);
	if (rc != 8 || anx_memcmp(bytes, "existing", 8) != 0)
		return -1103;
	if (anx_memplane_admit(&s->objects[3]->oid, ANX_ADMIT_CACHEABLE, &s->entries[3]) != ANX_EPERM || s->entries[3])
		return -1104;
	rc = anx_memplane_admit(&s->objects[2]->oid, ANX_ADMIT_CACHEABLE, &s->entries[2]);
	if (rc != ANX_OK || s->cell->memory_admitted_bytes != 16)
		return -1105;
	anx_memplane_forget(s->entries[2], ANX_FORGET_HARD_DELETE);
	s->entries[2] = NULL;
	if (s->cell->memory_admitted_bytes != 8)
		return -1106;
	return ANX_OK;
}

int anx_research_day011(void)
{
	struct memory_test_state state = {0};
	struct anx_cell_intent intent = {0};
	struct anx_external_call call = {0};
	struct anx_so_create_params params = {0};
	uint32_t i;
	int rc;

	anx_strlcpy(intent.name, "research-day-011", sizeof(intent.name));
	anx_strlcpy(call.endpoint, "anxresearch011://memory", sizeof(call.endpoint));
	rc = anx_external_register_handler("anxresearch011", memory_handler, &state);
	if (rc != ANX_OK)
		return rc;
	for (i = 0; i < 4; i++) {
		params.object_type = ANX_OBJ_BYTE_DATA;
		params.payload = i == 0 ? "existing" : "candidate";
		params.payload_size = i == 1 ? 9 : 8;
		rc = anx_so_create(&params, &state.objects[i]);
		if (rc != ANX_OK)
			goto out;
		if (i != 3) {
			rc = anx_so_seal(&state.objects[i]->oid);
			if (rc != ANX_OK)
				goto out;
		}
	}
	rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &state.cell);
	if (rc != ANX_OK)
		goto out;
	state.cell->execution.allow_side_effects = true;
	state.cell->constraints.max_memory_admission_bytes = 16;
	state.cell->ext_call = &call;
	rc = anx_cell_run(state.cell);
	if (rc != ANX_OK)
		goto out;
	if (anx_cell_destroy(state.cell) != ANX_EBUSY) {
		state.cell = NULL;
		rc = -1107;
		goto out;
	}
	anx_memplane_forget(state.entries[0], ANX_FORGET_HARD_DELETE);
	state.entries[0] = NULL;
	if (state.cell->memory_admitted_bytes != 0) {
		rc = -1108;
		goto out;
	}
	rc = ANX_OK;
out:
	for (i = 0; i < 4; i++) {
		if (state.entries[i])
			anx_memplane_forget(state.entries[i], ANX_FORGET_HARD_DELETE);
		if (state.objects[i]) {
			anx_so_delete(&state.objects[i]->oid, false);
			anx_objstore_release(state.objects[i]);
		}
	}
	if (state.cell)
		anx_cell_destroy(state.cell);
	anx_external_unregister_handler("anxresearch011");
	return rc;
}
#endif
