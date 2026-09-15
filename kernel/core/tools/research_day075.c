#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/memory_abi.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>
struct fixture075 {
	struct anx_cell *owner;
	struct anx_state_object *objects[8];
	struct anx_memory_contract_spec spec;
	struct anx_memory_contract_view contract, contract_out;
	struct anx_memory_index_view indices[3], index_out;
	struct anx_memory_match match, saved;
	struct anx_external_call call;
	anx_oid_t raw[2];
	bool foreign;
};
static int query075(struct fixture075 *f, uint32_t index, int expected)
{
	anx_memset(&f->match, 0x55, sizeof(f->match)); f->saved = f->match;
	int ret = anx_memory_index_query(f->indices[index].id, &f->objects[6]->oid, &f->match);
	if (ret != expected) return -7502;
	if (expected != ANX_OK) return anx_memcmp(&f->match, &f->saved, sizeof(f->match)) ? -7503 : ANX_OK;
	return !f->match.squared_distance && !anx_uuid_compare(&f->match.source, &f->objects[4]->oid) &&
		f->match.contract_epoch == f->contract.epoch ? ANX_OK : -7504;
}
static int active075(struct anx_external_call *call, void *arg)
{
	(void)call; struct fixture075 *f = arg;
	if (anx_memory_contract_set(&f->owner->cid, f->contract.epoch, &f->spec, &f->contract_out) != ANX_EPERM ||
	    anx_memory_contract_remove(&f->owner->cid) != ANX_EPERM ||
	    anx_memory_index_build(&f->owner->cid, f->raw, 2, &f->index_out) != ANX_EPERM ||
	    anx_memory_index_destroy(f->indices[0].id) != ANX_EPERM) return -7505;
	if (f->foreign && anx_memory_contract_get(&f->owner->cid, &f->contract_out) != ANX_EPERM) return -7506;
	return query075(f, 0, f->foreign ? ANX_EPERM : ANX_OK);
}
static int object075(struct fixture075 *f, uint32_t slot, const char *schema, const void *bytes, uint32_t size)
{
	struct anx_so_create_params p = {.object_type = schema ? ANX_OBJ_STRUCTURED_DATA : ANX_OBJ_BYTE_DATA,
		.schema_uri = schema, .schema_version = schema ? "1" : NULL, .payload = bytes, .payload_size = size};
	int ret = anx_so_create(&p, &f->objects[slot]);
	if (ret == ANX_OK) ret = anx_so_seal(&f->objects[slot]->oid);
	return ret;
}
int anx_research_day075(void)
{
	struct fixture075 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_memory_reader_format reader = {1,1,ANX_MEMORY_ABI_DIMENSIONS};
	struct anx_memory_space_format space = {1,ANX_MEMORY_ABI_DIMENSIONS,1};
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-075", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &f->owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret == ANX_OK) ret = object075(f, 0, ANX_MEMORY_READER_SCHEMA, &reader, sizeof(reader));
	if (ret == ANX_OK) ret = object075(f, 1, ANX_MEMORY_READER_SCHEMA, &reader, sizeof(reader));
	if (ret == ANX_OK) ret = object075(f, 2, ANX_MEMORY_SPACE_SCHEMA, &space, sizeof(space));
	space.transform = 2;
	if (ret == ANX_OK) ret = object075(f, 3, ANX_MEMORY_SPACE_SCHEMA, &space, sizeof(space));
	if (ret != ANX_OK) goto out;
	f->spec = (struct anx_memory_contract_spec){f->objects[0]->oid, f->objects[2]->oid};
	ret = -7501;
	if (anx_memory_contract_set(&f->owner->cid, 0, &f->spec, &f->contract) != ANX_OK) goto out;
	ret = object075(f, 4, NULL, "alpha", 5);
	if (ret == ANX_OK) ret = object075(f, 5, NULL, "beta", 4);
	if (ret == ANX_OK) ret = object075(f, 6, NULL, "alpha", 5);
	reader.semantic_schema = 2;
	if (ret == ANX_OK) ret = object075(f, 7, ANX_MEMORY_READER_SCHEMA, &reader, sizeof(reader));
	if (ret != ANX_OK) goto out;
	f->raw[0] = f->objects[4]->oid; f->raw[1] = f->objects[5]->oid;
	ret = anx_memory_index_build(&f->owner->cid, f->raw, 2, &f->indices[0]);
	if (ret == ANX_OK) ret = query075(f, 0, ANX_OK);
	if (ret != ANX_OK) goto out;
	if (anx_memory_contract_remove(&f->owner->cid) != ANX_EBUSY) { ret = -7507; goto out; }
	uint64_t previous_epoch = f->contract.epoch;
	f->spec.reader = f->objects[7]->oid;
	if (anx_memory_contract_set(&f->owner->cid, previous_epoch, &f->spec, &f->contract_out) != ANX_ENOTSUP ||
	    anx_memory_contract_get(&f->owner->cid, &f->contract_out) != ANX_OK || f->contract_out.epoch != previous_epoch) { ret = -7508; goto out; }
	f->spec.reader = f->objects[0]->oid; f->spec.space = f->objects[3]->oid;
	ret = anx_memory_contract_set(&f->owner->cid, previous_epoch, &f->spec, &f->contract);
	if (ret != ANX_OK || f->contract.dimensions != f->indices[0].dimensions || f->contract.epoch == previous_epoch) { ret = -7509; goto out; }
	/* Editing copied descriptors cannot relabel the stored projection. */
	f->indices[0].sources.space = f->objects[3]->oid;
	ret = query075(f, 0, ANX_EBUSY);
	if (ret == ANX_OK) ret = anx_memory_index_build(&f->owner->cid, f->raw, 2, &f->indices[1]);
	if (ret == ANX_OK) ret = query075(f, 1, ANX_OK);
	if (ret != ANX_OK) goto out;
	if (anx_memory_contract_set(&f->owner->cid, previous_epoch, &f->spec, &f->contract_out) != ANX_EBUSY) { ret = -7510; goto out; }
	f->spec.reader = f->objects[1]->oid;
	ret = anx_memory_contract_set(&f->owner->cid, f->contract.epoch, &f->spec, &f->contract);
	if (ret == ANX_OK) ret = query075(f, 1, ANX_EBUSY);
	if (ret == ANX_OK) ret = anx_memory_index_build(&f->owner->cid, f->raw, 2, &f->indices[2]);
	if (ret == ANX_OK) ret = query075(f, 2, ANX_OK);
	if (ret != ANX_OK) goto out;
	f->spec = (struct anx_memory_contract_spec){f->objects[0]->oid, f->objects[2]->oid};
	ret = anx_memory_contract_set(&f->owner->cid, f->contract.epoch, &f->spec, &f->contract);
	if (ret == ANX_OK) ret = query075(f, 0, ANX_OK);
	if (ret == ANX_OK) ret = query075(f, 2, ANX_EBUSY);
	if (ret != ANX_OK) goto out;
	((uint8_t *)f->objects[5]->payload)[0] ^= 1; ret = query075(f, 0, ANX_EBUSY); ((uint8_t *)f->objects[5]->payload)[0] ^= 1;
	if (ret != ANX_OK) goto out;
	f->objects[4]->version++; ret = query075(f, 0, ANX_EBUSY); f->objects[4]->version--;
	if (ret != ANX_OK) goto out;
	((uint8_t *)f->objects[2]->payload)[0] ^= 1; ret = query075(f, 0, ANX_EBUSY); ((uint8_t *)f->objects[2]->payload)[0] ^= 1;
	if (ret != ANX_OK) goto out;
	f->objects[4]->access_policy.rule_count = 1;
	f->objects[4]->access_policy.rules[0] = (struct anx_access_rule){.operations=ANX_ACCESS_READ_PAYLOAD,.effect=ANX_EFFECT_DENY};
	ret = query075(f, 0, ANX_EPERM); f->objects[4]->access_policy.rule_count = 0;
	if (ret != ANX_OK) goto out;
	anx_oid_t duplicate[2] = {f->raw[0],f->raw[0]};
	if (anx_memory_index_build(&f->owner->cid, duplicate, 2, &f->index_out) != ANX_EINVAL) { ret = -7511; goto out; }
	ret = anx_external_register_handler("anxresearch075", active075, f);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint, "anxresearch075://query", sizeof(f->call.endpoint));
	f->owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	f->owner->ext_call = foreign->ext_call = &f->call;
	f->foreign = true; ret = anx_cell_run(foreign);
	if (ret == ANX_OK) { f->foreign = false; ret = anx_cell_run(f->owner); }
	if (ret == ANX_OK) kprintf("day075 equal_dimensions=4 incompatible_reuse=rejected rebuilt_query=exact canonical_sources=unchanged\n");
out:
	anx_external_unregister_handler("anxresearch075");
	for (uint32_t i = 0; i < 3; i++) if (f->indices[i].id && anx_memory_index_destroy(f->indices[i].id) != ANX_OK && ret == ANX_OK) ret = -7512;
	if (f->contract.epoch && anx_memory_contract_remove(&f->owner->cid) != ANX_OK && ret == ANX_OK) ret = -7512;
	for (uint32_t i = 0; i < 8; i++) if (f->objects[i]) { anx_so_delete(&f->objects[i]->oid, false); anx_objstore_release(f->objects[i]); }
	if (foreign && anx_cell_destroy(foreign) != ANX_OK && ret == ANX_OK) ret = -7513;
	if (f->owner && anx_cell_destroy(f->owner) != ANX_OK && ret == ANX_OK) ret = -7513;
	if (ret != ANX_OK) kprintf("day075 native failure rc=%d\n", ret);
	anx_free(f); return ret;
}
#endif
