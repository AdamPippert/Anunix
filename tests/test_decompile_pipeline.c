/*
 * test_decompile_pipeline.c — End-to-end LARQL-style decompile demo.
 *
 * Composes every primitive from this branch into one pipeline:
 *   load_model            (external-call dispatch)
 *   split_bands           (DAG child of load)
 *   extract_relations     (fan-out from split)
 *   compute_residuals     (fan-out from split, parallel to relations)
 *   seal_vindex           (fan-in on relations + residuals)
 *
 * Outputs are State Objects with tensor-codec payloads, persisted
 * via the boundary-key-ordered disk store. Verifies: DAG
 * dependency gating (fan-in blocks until both branches complete),
 * tensor codec round-trip, locality-ordered scan, and provenance
 * chains through parent_oids.
 *
 * The teacher model is a deterministic synthetic 16-layer blob
 * served by a model:// external-call handler. No real weights are
 * needed to exercise the pipeline shape.
 */

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/tensor.h>
#include <anx/external_call.h>
#include <anx/objstore_disk.h>
#include <anx/mock_blk.h>
#include <anx/meta.h>
#include <anx/string.h>

/* --- Boundary-key layout ---------------------------------------
 *
 *  63         32 31   24 23            0
 *  [  domain   ][ band ][   window     ]
 *
 * Objects in the same band land in a contiguous index slice, so
 * range_scan([band,0], [band,~0]) returns exactly that band.
 */
#define DOMAIN_SYNTHETIC	0x1ULL
#define BK(band, window) \
	((DOMAIN_SYNTHETIC << 32) | \
	 ((uint64_t)(band) << 24) | \
	 ((uint64_t)(window) & 0xFFFFFFu))

#define BAND_COUNT		3u
#define LAYERS_PER_BAND		4u		/* 3 * 4 = 12 synthetic layers */
#define DIM			8u
#define RESIDUALS_PER_BAND	2u

/* --- Synthetic teacher served by model:// handler -------------- */

static int model_handler(struct anx_external_call *call, void *ctx)
{
	struct anx_tensor_header h;
	uint8_t data[BAND_COUNT * LAYERS_PER_BAND * DIM * 2]; /* F16 */
	uint64_t written;
	uint32_t *calls = ctx;
	uint32_t i;

	(*calls)++;

	/* Deterministic filler so downstream stages can self-check. */
	for (i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)((i * 37 + 11) & 0xFF);

	anx_memset(&h, 0, sizeof(h));
	h.magic = ANX_TENSOR_MAGIC;
	h.version = ANX_TENSOR_VERSION;
	h.dtype = ANX_DTYPE_F16;
	h.ndim = 2;
	h.shape[0] = BAND_COUNT * LAYERS_PER_BAND;
	h.shape[1] = DIM;

	if (anx_tensor_encode(&h, data, sizeof(data),
			      call->response_buf,
			      sizeof(call->response_buf),
			      &written) != ANX_OK)
		return ANX_EIO;

	call->response_size = (uint32_t)written;
	call->status_code = 200;
	return ANX_OK;
}

/* --- Small helpers -------------------------------------------- */

static int make_cell(const char *name, enum anx_cell_type type,
		     struct anx_cell **out)
{
	struct anx_cell_intent intent;

	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, name, sizeof(intent.name));
	return anx_cell_create(type, &intent, out);
}

static int create_and_persist(enum anx_object_type type,
			      const anx_oid_t *parents, uint32_t parent_count,
			      anx_cid_t creator,
			      const void *payload, uint32_t payload_size,
			      uint64_t boundary_key,
			      struct anx_state_object **out)
{
	struct anx_so_create_params p;
	struct anx_state_object *obj;
	int ret;

	anx_memset(&p, 0, sizeof(p));
	p.object_type = type;
	p.payload = payload;
	p.payload_size = payload_size;
	p.parent_oids = parents;
	p.parent_count = parent_count;
	p.creator_cell = creator;

	ret = anx_so_create(&p, &obj);
	if (ret != ANX_OK)
		return ret;

	ret = anx_disk_write_obj_bk(&obj->oid, (uint32_t)type,
				    boundary_key, payload, payload_size);
	if (ret != ANX_OK)
		return ret;

	anx_meta_set_i64(obj->user_meta, "topology.bk",
			 (int64_t)boundary_key);
	*out = obj;
	return ANX_OK;
}

/* --- Collect range-scan hits ---------------------------------- */

struct hit_list {
	anx_oid_t oids[16];
	uint64_t keys[16];
	uint32_t count;
};

static int collect_hits(const anx_oid_t *oid, uint64_t bk,
			uint32_t obj_type, void *arg)
{
	struct hit_list *h = arg;

	(void)obj_type;
	if (h->count >= 16)
		return 1;
	h->oids[h->count] = *oid;
	h->keys[h->count] = bk;
	h->count++;
	return 0;
}

int test_decompile_pipeline(void)
{
	struct anx_cell *c_load, *c_split, *c_rel, *c_res, *c_seal;
	struct anx_external_call call;
	struct anx_state_object *model_blob = NULL;
	struct anx_state_object *band_obj[BAND_COUNT] = {NULL};
	struct anx_state_object *rel_obj[BAND_COUNT] = {NULL};
	struct anx_state_object *res_obj[BAND_COUNT * RESIDUALS_PER_BAND] = {NULL};
	struct anx_state_object *vindex = NULL;
	uint32_t load_invocations = 0;
	uint32_t i, j;
	int ret;

	/* --- Init all subsystems --- */
	anx_objstore_init();
	anx_cell_store_init();
	anx_external_init();
	test_mock_blk_init(2048);
	if (anx_disk_format("decompile-demo") != ANX_OK)
		return -1;

	/* --- Register the synthetic teacher handler --- */
	if (anx_external_register_handler("model", model_handler,
					  &load_invocations) != ANX_OK)
		return -2;

	/* --- Build the DAG ---------------------------------------- */
	if (make_cell("load_model", ANX_CELL_TASK_EXTERNAL_CALL,
		      &c_load) != ANX_OK)
		return -10;
	if (make_cell("split_bands", ANX_CELL_TASK_DECOMPOSITION,
		      &c_split) != ANX_OK)
		return -11;
	if (make_cell("extract_relations", ANX_CELL_TASK_GRAPH_UPDATE,
		      &c_rel) != ANX_OK)
		return -12;
	if (make_cell("compute_residuals", ANX_CELL_TASK_MEMORY_UPDATE,
		      &c_res) != ANX_OK)
		return -13;
	if (make_cell("seal_vindex", ANX_CELL_TASK_VALIDATION,
		      &c_seal) != ANX_OK)
		return -14;

	/* Edges:
	 *   load -> split -> { relations, residuals } -> seal
	 */
	if (anx_cell_add_dependency(c_split, &c_load->cid) != ANX_OK)
		return -15;
	if (anx_cell_add_dependency(c_rel, &c_split->cid) != ANX_OK)
		return -16;
	if (anx_cell_add_dependency(c_res, &c_split->cid) != ANX_OK)
		return -17;
	if (anx_cell_add_dependency(c_seal, &c_rel->cid) != ANX_OK)
		return -18;
	if (anx_cell_add_dependency(c_seal, &c_res->cid) != ANX_OK)
		return -19;

	/* seal_vindex must block until both branches complete. */
	if (anx_cell_deps_satisfied(c_seal) != 0)
		return -20;
	if (anx_cell_run(c_seal) != ANX_EBUSY)
		return -21;

	/* --- Stage L0: load_model --------------------------------- */
	anx_memset(&call, 0, sizeof(call));
	anx_strlcpy(call.endpoint, "model://teacher/synthetic",
		    sizeof(call.endpoint));
	anx_strlcpy(call.method, "GET", sizeof(call.method));
	c_load->ext_call = &call;

	if (anx_cell_run(c_load) != ANX_OK)
		return -30;
	if (c_load->status != ANX_CELL_COMPLETED)
		return -31;
	if (load_invocations != 1)
		return -32;

	/* Realize L0 output: the fetched blob as a BYTE_DATA object. */
	if (create_and_persist(ANX_OBJ_BYTE_DATA, NULL, 0, c_load->cid,
			       call.response_buf, call.response_size,
			       BK(0, 0), &model_blob) != ANX_OK)
		return -33;
	c_load->output_refs[0] = model_blob->oid;
	c_load->output_count = 1;

	/* --- Stage L1: split_bands -------------------------------- */
	if (anx_cell_run(c_split) != ANX_OK)
		return -40;
	if (c_split->status != ANX_CELL_COMPLETED)
		return -41;

	/* Decode the model blob and partition by band. Each band gets
	 * LAYERS_PER_BAND layers of DIM elements, F16. */
	{
		struct anx_tensor_header h;
		const void *data_ptr;
		uint64_t data_size;

		if (anx_tensor_decode(model_blob->payload,
				      model_blob->payload_size,
				      &h, &data_ptr, &data_size) != ANX_OK)
			return -42;
		if (h.dtype != ANX_DTYPE_F16)
			return -43;
		if (h.shape[0] != BAND_COUNT * LAYERS_PER_BAND)
			return -44;
		if (h.shape[1] != DIM)
			return -45;

		for (i = 0; i < BAND_COUNT; i++) {
			struct anx_tensor_header bh;
			uint8_t encoded[256];
			uint8_t band_data[LAYERS_PER_BAND * DIM * 2];
			const uint8_t *src;
			uint64_t written;

			src = (const uint8_t *)data_ptr +
			      i * LAYERS_PER_BAND * DIM * 2;
			anx_memcpy(band_data, src, sizeof(band_data));

			anx_memset(&bh, 0, sizeof(bh));
			bh.magic = ANX_TENSOR_MAGIC;
			bh.version = ANX_TENSOR_VERSION;
			bh.dtype = ANX_DTYPE_F16;
			bh.ndim = 2;
			bh.shape[0] = LAYERS_PER_BAND;
			bh.shape[1] = DIM;

			if (anx_tensor_encode(&bh, band_data,
					      sizeof(band_data),
					      encoded, sizeof(encoded),
					      &written) != ANX_OK)
				return -46;

			if (create_and_persist(ANX_OBJ_EMBEDDING,
					       &model_blob->oid, 1,
					       c_split->cid,
					       encoded, (uint32_t)written,
					       BK(1 + i, 0),
					       &band_obj[i]) != ANX_OK)
				return -47;
			anx_meta_set_i64(band_obj[i]->user_meta,
					 "band.id", (int64_t)i);
			anx_meta_set_i64(band_obj[i]->user_meta,
					 "band.layer_count",
					 (int64_t)LAYERS_PER_BAND);
			if (anx_so_seal(&band_obj[i]->oid) != ANX_OK)
				return -48;
		}
	}
	c_split->output_count = BAND_COUNT;
	for (i = 0; i < BAND_COUNT; i++)
		c_split->output_refs[i] = band_obj[i]->oid;

	/* --- Stage L2a: extract_relations ------------------------- */
	if (anx_cell_run(c_rel) != ANX_OK)
		return -50;
	if (c_rel->status != ANX_CELL_COMPLETED)
		return -51;

	for (i = 0; i < BAND_COUNT; i++) {
		uint8_t edge[16];

		for (j = 0; j < sizeof(edge); j++)
			edge[j] = (uint8_t)(i * 13 + j);

		if (create_and_persist(ANX_OBJ_GRAPH_NODE,
				       &band_obj[i]->oid, 1, c_rel->cid,
				       edge, sizeof(edge),
				       BK(0x20 + i, 0),
				       &rel_obj[i]) != ANX_OK)
			return -52;
	}
	c_rel->output_count = BAND_COUNT;
	for (i = 0; i < BAND_COUNT; i++)
		c_rel->output_refs[i] = rel_obj[i]->oid;

	/* --- Stage L2b: compute_residuals ------------------------- */
	if (anx_cell_run(c_res) != ANX_OK)
		return -60;
	if (c_res->status != ANX_CELL_COMPLETED)
		return -61;

	for (i = 0; i < BAND_COUNT; i++) {
		for (j = 0; j < RESIDUALS_PER_BAND; j++) {
			uint8_t trace[24];
			uint32_t k;
			uint32_t idx = i * RESIDUALS_PER_BAND + j;

			for (k = 0; k < sizeof(trace); k++)
				trace[k] = (uint8_t)(idx * 7 + k);

			if (create_and_persist(ANX_OBJ_EXECUTION_TRACE,
					       &band_obj[i]->oid, 1,
					       c_res->cid,
					       trace, sizeof(trace),
					       BK(0x40 + i, j),
					       &res_obj[idx]) != ANX_OK)
				return -62;
			anx_meta_set_i64(res_obj[idx]->user_meta,
					 "window.id", (int64_t)j);
		}
	}
	c_res->output_count = BAND_COUNT * RESIDUALS_PER_BAND;
	for (i = 0; i < BAND_COUNT * RESIDUALS_PER_BAND; i++)
		c_res->output_refs[i] = res_obj[i]->oid;

	/* --- Stage L3: seal_vindex (fan-in) ----------------------- */
	if (anx_cell_deps_satisfied(c_seal) != 1)
		return -70;
	if (anx_cell_run(c_seal) != ANX_OK)
		return -71;
	if (c_seal->status != ANX_CELL_COMPLETED)
		return -72;

	{
		anx_oid_t parents[BAND_COUNT + BAND_COUNT +
				  BAND_COUNT * RESIDUALS_PER_BAND];
		uint32_t pc = 0;
		uint8_t vindex_payload[] = "vindex:synthetic:v1";

		for (i = 0; i < BAND_COUNT; i++)
			parents[pc++] = band_obj[i]->oid;
		for (i = 0; i < BAND_COUNT; i++)
			parents[pc++] = rel_obj[i]->oid;
		for (i = 0; i < BAND_COUNT * RESIDUALS_PER_BAND; i++)
			parents[pc++] = res_obj[i]->oid;

		if (create_and_persist(ANX_OBJ_MODEL_OUTPUT,
				       parents, pc, c_seal->cid,
				       vindex_payload,
				       sizeof(vindex_payload),
				       BK(0xFF, 0),
				       &vindex) != ANX_OK)
			return -73;
		if (anx_so_seal(&vindex->oid) != ANX_OK)
			return -74;
	}
	c_seal->output_refs[0] = vindex->oid;
	c_seal->output_count = 1;

	/* --- Invariants ------------------------------------------- */

	/* Every stage completed. */
	if (c_load->status != ANX_CELL_COMPLETED ||
	    c_split->status != ANX_CELL_COMPLETED ||
	    c_rel->status != ANX_CELL_COMPLETED ||
	    c_res->status != ANX_CELL_COMPLETED ||
	    c_seal->status != ANX_CELL_COMPLETED)
		return -80;

	/* The root vindex has every intermediate as a parent. */
	if (vindex->parent_count !=
	    BAND_COUNT * 2 + BAND_COUNT * RESIDUALS_PER_BAND)
		return -81;
	if (vindex->state != ANX_OBJ_SEALED)
		return -82;

	/* Band tensors decode to the right shape. */
	for (i = 0; i < BAND_COUNT; i++) {
		struct anx_tensor_header bh;
		const void *dptr;
		uint64_t dsize;

		if (anx_tensor_decode(band_obj[i]->payload,
				      band_obj[i]->payload_size,
				      &bh, &dptr, &dsize) != ANX_OK)
			return -83;
		if (bh.dtype != ANX_DTYPE_F16 ||
		    bh.shape[0] != LAYERS_PER_BAND ||
		    bh.shape[1] != DIM)
			return -84;
		if (dsize != LAYERS_PER_BAND * DIM * 2)
			return -85;
	}

	/* Locality scan: the band-object range returns exactly the
	 * three bands, in ascending bk order. */
	{
		struct hit_list hits;

		anx_memset(&hits, 0, sizeof(hits));
		ret = anx_disk_range_scan(BK(1, 0), BK(1 + BAND_COUNT, 0),
					  collect_hits, &hits);
		if (ret != ANX_OK)
			return -86;
		if (hits.count != BAND_COUNT)
			return -87;
		for (i = 0; i < BAND_COUNT; i++) {
			if (hits.keys[i] != BK(1 + i, 0))
				return -88;
			if (hits.oids[i].hi != band_obj[i]->oid.hi ||
			    hits.oids[i].lo != band_obj[i]->oid.lo)
				return -89;
		}

		/* Residual slice returns all 6, also ordered. */
		anx_memset(&hits, 0, sizeof(hits));
		ret = anx_disk_range_scan(BK(0x40, 0), BK(0x50, 0),
					  collect_hits, &hits);
		if (ret != ANX_OK)
			return -90;
		if (hits.count != BAND_COUNT * RESIDUALS_PER_BAND)
			return -91;
		for (i = 1; i < hits.count; i++) {
			if (hits.keys[i] < hits.keys[i - 1])
				return -92;
		}
	}

	/* A failed predecessor (re-run is a no-op, but an ancillary
	 * cancel propagates) — sanity: retry gate is quiet when deps
	 * are satisfied and cells are terminal. */

	/* --- Cleanup ---------------------------------------------- */
	anx_cell_destroy(c_seal);
	anx_cell_destroy(c_res);
	anx_cell_destroy(c_rel);
	anx_cell_destroy(c_split);
	anx_cell_destroy(c_load);

	anx_external_unregister_handler("model");
	test_mock_blk_teardown();
	return 0;
}
