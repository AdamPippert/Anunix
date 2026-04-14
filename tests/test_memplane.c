/*
 * test_memplane.c — Tests for Memory Control Plane (RFC-0004).
 */

#include <anx/types.h>
#include <anx/memplane.h>
#include <anx/state_object.h>
#include <anx/string.h>

int test_memplane(void)
{
	struct anx_state_object *obj;
	struct anx_so_create_params params;
	struct anx_mem_entry *entry;
	int ret;

	anx_objstore_init();
	anx_memplane_init();

	/* Create a State Object to admit */
	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = "test data";
	params.payload_size = 9;

	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK)
		return -1;

	/* Admit into memory plane */
	ret = anx_memplane_admit(&obj->oid, ANX_ADMIT_RETRIEVAL_CANDIDATE,
				 &entry);
	if (ret != ANX_OK)
		return -2;

	/* Should be in L2 + L3 */
	if (!anx_mem_in_tier(entry, ANX_MEM_L2))
		return -3;
	if (!anx_mem_in_tier(entry, ANX_MEM_L3))
		return -4;

	/* Should not be in L4 yet */
	if (anx_mem_in_tier(entry, ANX_MEM_L4))
		return -5;

	/* Promotion to L4 should fail without validation */
	ret = anx_memplane_promote(entry, ANX_MEM_L4);
	if (ret == ANX_OK)
		return -6;	/* should have been denied */

	/* Set provisional validation, then promote */
	ret = anx_memplane_set_validation(entry, ANX_MEMVAL_PROVISIONAL);
	if (ret != ANX_OK)
		return -7;

	ret = anx_memplane_promote(entry, ANX_MEM_L4);
	if (ret != ANX_OK)
		return -8;

	if (!anx_mem_in_tier(entry, ANX_MEM_L4))
		return -9;

	/* Record access */
	anx_memplane_record_access(entry);
	if (entry->access_count != 1)
		return -10;

	/* Add contradictions */
	anx_memplane_add_contradiction(entry);
	anx_memplane_add_contradiction(entry);
	if (entry->validation != ANX_MEMVAL_CONTESTED)
		return -11;

	/* Demote from L3 */
	ret = anx_memplane_demote(entry, ANX_MEM_L3);
	if (ret != ANX_OK)
		return -12;
	if (anx_mem_in_tier(entry, ANX_MEM_L3))
		return -13;

	/* Forget with tombstone */
	ret = anx_memplane_forget(entry, ANX_FORGET_TOMBSTONE);
	if (ret != ANX_OK)
		return -14;
	if (entry->tier_mask != 0)
		return -15;

	/* Duplicate admission should fail */
	ret = anx_memplane_admit(&obj->oid, ANX_ADMIT_CACHEABLE, NULL);
	if (ret != ANX_EEXIST)
		return -16;

	anx_objstore_release(obj);
	return 0;
}
