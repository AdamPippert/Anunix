/*
 * test_shm_ipc.c — P0-007 shared-memory IPC v0 tests.
 */

#include <anx/types.h>
#include <anx/interface_plane.h>
#include <anx/cell.h>
#include <anx/uuid.h>
#include <anx/string.h>

#define ASSERT(cond, code) do { if (!(cond)) return (code); } while (0)

static int setup_fixture(void)
{
	int rc;

	anx_cell_store_init();
	rc = anx_iface_init();
	if (rc != ANX_OK)
		return rc;
	rc = anx_iface_env_define("visual-desktop", "anx:test", ANX_ENGINE_RENDERER_HEADLESS);
	if (rc != ANX_OK)
		return rc;
	return ANX_OK;
}

/* P0-007-U01 */
static int test_shm_roundtrip(void)
{
	anx_cid_t producer;
	anx_cid_t consumer;
	anx_oid_t shm_oid;
	char payload[] = "frame:abc123";
	char out[32];
	uint32_t len;
	uint32_t seq;
	int rc;

	anx_uuid_generate(&producer);
	anx_uuid_generate(&consumer);

	rc = anx_iface_shm_create(producer, 128, &shm_oid);
	ASSERT(rc == ANX_OK, -100);
	rc = anx_iface_shm_grant(shm_oid, producer, consumer,
				 ANX_IFACE_SHM_RIGHT_CONSUME);
	ASSERT(rc == ANX_OK, -101);

	rc = anx_iface_shm_publish(shm_oid, producer, payload, sizeof(payload), &seq);
	ASSERT(rc == ANX_OK, -102);
	ASSERT(seq == 1, -103);

	rc = anx_iface_shm_consume(shm_oid, consumer, out, sizeof(out), &len, &seq);
	ASSERT(rc == ANX_OK, -104);
	ASSERT(len == sizeof(payload), -105);
	ASSERT(seq == 1, -106);
	ASSERT(anx_memcmp(out, payload, sizeof(payload)) == 0, -107);

	return 0;
}

/* P0-007-U02 */
static int test_unauthorized_map_denied(void)
{
	anx_cid_t owner;
	anx_cid_t intruder;
	anx_oid_t shm_oid;
	void *ptr;
	uint32_t size;
	uint32_t seq;
	int rc;

	anx_uuid_generate(&owner);
	anx_uuid_generate(&intruder);

	rc = anx_iface_shm_create(owner, 64, &shm_oid);
	ASSERT(rc == ANX_OK, -110);

	rc = anx_iface_shm_map(shm_oid, intruder, ANX_IFACE_SHM_RIGHT_CONSUME,
			      &ptr, &size, &seq);
	ASSERT(rc == ANX_EPERM, -111);
	return 0;
}

/* P0-007-I01 */
static int test_compositor_consume_buffer(void)
{
	anx_cid_t producer;
	anx_oid_t shm_oid;
	char payload[] = "pix:v0";
	char out[32];
	uint32_t len;
	uint32_t seq;
	uint32_t committed;
	struct anx_iface_compositor_stats stats;
	int rc;
	int tick;

	rc = setup_fixture();
	ASSERT(rc == ANX_OK, -120);
	anx_uuid_generate(&producer);

	rc = anx_iface_compositor_start("visual-desktop");
	ASSERT(rc == ANX_OK, -121);
	rc = anx_iface_compositor_stats("visual-desktop", &stats);
	ASSERT(rc == ANX_OK, -122);

	rc = anx_iface_shm_create(producer, 256, &shm_oid);
	ASSERT(rc == ANX_OK, -123);
	rc = anx_iface_shm_grant(shm_oid, producer, stats.cell_cid,
				 ANX_IFACE_SHM_RIGHT_CONSUME);
	ASSERT(rc == ANX_OK, -124);

	rc = anx_iface_shm_publish(shm_oid, producer, payload, sizeof(payload), &seq);
	ASSERT(rc == ANX_OK, -125);
	ASSERT(seq == 1, -126);

	for (tick = 0; tick < 2; tick++) {
		rc = anx_iface_compositor_tick("visual-desktop", &committed);
		ASSERT(rc == ANX_OK, -127);
		(void)committed;
		rc = anx_iface_shm_consume(shm_oid, stats.cell_cid,
					  out, sizeof(out), &len, &seq);
		if (rc == ANX_OK)
			break;
	}

	ASSERT(rc == ANX_OK, -128);
	ASSERT(seq == 1, -129);
	ASSERT(len == sizeof(payload), -130);
	ASSERT(anx_memcmp(out, payload, sizeof(payload)) == 0, -131);

	return 0;
}

int test_shm_ipc(void)
{
	int rc;

	rc = setup_fixture();
	if (rc != ANX_OK)
		return -1;
	rc = test_shm_roundtrip();
	if (rc != 0)
		return rc;
	rc = test_unauthorized_map_denied();
	if (rc != 0)
		return rc;
	rc = test_compositor_consume_buffer();
	if (rc != 0)
		return rc;
	return 0;
}
