#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/memory_abi.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/string.h>
int anx_research_day075(void)
{
	struct anx_cell *owner = NULL; struct anx_cell_intent intent = {0};
	struct anx_state_object *reader = NULL, *space = NULL;
	struct anx_memory_reader_format r = {1,1,ANX_MEMORY_ABI_DIMENSIONS};
	struct anx_memory_space_format s = {1,ANX_MEMORY_ABI_DIMENSIONS,1};
	struct anx_memory_contract_spec spec; struct anx_memory_contract_view view;
	struct anx_so_create_params p = {.object_type=ANX_OBJ_STRUCTURED_DATA,.schema_uri=ANX_MEMORY_READER_SCHEMA,.schema_version="1",.payload=&r,.payload_size=sizeof(r)};
	anx_strlcpy(intent.name,"research-day-075",sizeof(intent.name));
	int ret=anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL,&intent,&owner);
	if(ret==ANX_OK) ret=anx_so_create(&p,&reader);
	if(ret==ANX_OK) ret=anx_so_seal(&reader->oid);
	p.schema_uri=ANX_MEMORY_SPACE_SCHEMA;p.payload=&s;p.payload_size=sizeof(s);
	if(ret==ANX_OK) ret=anx_so_create(&p,&space);
	if(ret==ANX_OK) ret=anx_so_seal(&space->oid);
	if(ret==ANX_OK){spec=(struct anx_memory_contract_spec){reader->oid,space->oid};if(anx_memory_contract_set(&owner->cid,0,&spec,&view)!=ANX_OK)ret=-7501;}
	if(space){anx_so_delete(&space->oid,false);anx_objstore_release(space);}
	if(reader){anx_so_delete(&reader->oid,false);anx_objstore_release(reader);}
	if(owner)anx_cell_destroy(owner);
	return ret;
}
#endif
