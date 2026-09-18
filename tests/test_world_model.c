/*
 * test_world_model.c — The world model interface is model-agnostic.
 *
 * The loop and the `world` command reach the model only through
 * anx/world_model.h. These checks hold the contract a replacement
 * backend (Arboris, or whatever follows it) relies on: calls are safe
 * with no backend, go to whichever backend registered last, and the loop
 * learns through the interface without naming the model.
 */

#include <anx/types.h>
#include <anx/world_model.h>
#include <anx/jepa.h>
#include <anx/loop.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			kprintf("  FAIL: %s\n", (msg));			\
			return -1;					\
		}							\
	} while (0)

static struct {
	uint32_t observes, predicts, winners, ingests;
	uint32_t last_action;
} fake;

static bool fake_available(void) { return true; }
static enum anx_world_status fake_status(void) { return ANX_WORLD_READY; }
static uint32_t fake_steps(void) { return fake.winners; }

static int fake_observe(struct anx_world_obs *obs)
{
	anx_memset(obs, 0, sizeof(*obs));
	obs->active_cell_count = 7;
	fake.observes++;
	return ANX_OK;
}

static int fake_predict(const anx_oid_t *latent, uint32_t action,
			anx_oid_t *out)
{
	(void)latent;
	out->hi = 0xA5B0;
	out->lo = action;
	fake.predicts++;
	return ANX_OK;
}

static int fake_winner(uint32_t action)
{
	fake.winners++;
	fake.last_action = action;
	return ANX_OK;
}

static int fake_ingest(const struct anx_world_obs *obs, uint32_t action,
		       const char *uri)
{
	(void)action;
	if (obs && obs->active_cell_count == 7 && uri)
		fake.ingests++;
	return ANX_OK;
}

static const char *fake_action(uint32_t id)
{
	static const char *const names[] = { "grow", "prune", "graft" };

	return id < 3 ? names[id] : NULL;
}

static int fake_info(const char *uri, struct anx_world_info *out)
{
	if (uri && anx_strcmp(uri, "anx:world/orchard") != 0)
		return ANX_ENOENT;
	anx_memset(out, 0, sizeof(*out));
	out->uri = "anx:world/orchard";
	out->display_name = "Orchard";
	out->action_count = 3;
	out->collects_obs = true;
	return ANX_OK;
}

/* A backend with most operations missing, as a young one would be. */
static const struct anx_world_model_ops fake_ops = {
	.name          = "arboris-test",
	.available     = fake_available,
	.status        = fake_status,
	.train_steps   = fake_steps,
	.observe       = fake_observe,
	.predict       = fake_predict,
	.record_winner = fake_winner,
	.traj_ingest   = fake_ingest,
	.world_info    = fake_info,
	.action_name   = fake_action,
};

int test_world_model(void)
{
	struct anx_loop_create_params p;
	struct anx_world_info info;
	struct anx_world_obs obs;
	anx_oid_t oid = { 0, 0 }, sid;
	uint32_t entries;

	/* Whatever ran before, JEPA is registered by anx_jepa_init() */
	anx_jepa_init();
	CHECK(anx_strcmp(anx_world_model_name(), "jepa") == 0,
	      "jepa registers itself");

	/* No backend: everything answers, nothing crashes */
	anx_world_model_register(NULL);
	CHECK(anx_strcmp(anx_world_model_name(), "none") == 0, "no backend");
	CHECK(!anx_world_available(), "unavailable without backend");
	CHECK(anx_world_status_get() == ANX_WORLD_UNAVAILABLE, "status");
	CHECK(anx_world_observe(&obs) == ANX_ENOSYS, "observe ENOSYS");
	CHECK(anx_world_predict(&oid, 1, &oid) == ANX_ENOSYS, "predict");
	CHECK(anx_world_traj_count(&entries, NULL) == ANX_ENOSYS, "traj");
	CHECK(anx_world_action_count() == ANX_WORLD_ACT_COUNT,
	      "default vocabulary");
	CHECK(anx_world_action_name(0) == NULL, "no action names");
	anx_world_traj_reset();
	CHECK(anx_world_list(NULL, 0, &entries) == ANX_ENOSYS && entries == 0,
	      "empty world list");
	CHECK(anx_strcmp(anx_world_status_name(ANX_WORLD_READY), "ready") == 0 &&
	      anx_strcmp(anx_world_status_name((enum anx_world_status)99),
			 "?") == 0, "status names");

	/* A different architecture takes over */
	anx_world_model_register(&fake_ops);
	CHECK(anx_strcmp(anx_world_model_name(), "arboris-test") == 0,
	      "replacement backend");
	CHECK(anx_world_available() &&
	      anx_world_status_get() == ANX_WORLD_READY, "fake ready");
	CHECK(anx_world_encode(&oid, &oid) == ANX_ENOSYS,
	      "missing op is ENOSYS");
	CHECK(anx_world_predict(&oid, 2, &oid) == ANX_OK &&
	      oid.hi == 0xA5B0 && oid.lo == 2, "predict forwarded");
	CHECK(anx_world_info_get(NULL, &info) == ANX_OK &&
	      anx_world_action_count() == 3, "world vocabulary");
	CHECK(anx_strcmp(anx_world_action_name(2), "graft") == 0 &&
	      anx_world_action_name(3) == NULL, "action names");

	/* The loop learns through the interface, not through JEPA */
	anx_memset(&p, 0, sizeof(p));
	p.max_iterations = 1;
	p.halt_policy = ANX_LOOP_HALT_ON_BUDGET;
	anx_strlcpy(p.world_uri, "anx:world/orchard", sizeof(p.world_uri));
	CHECK(anx_loop_session_create(&p, &sid) == ANX_OK, "session");
	anx_memset(&fake, 0, sizeof(fake));
	CHECK(anx_loop_world_ingest(sid, "anx:world/orchard") == ANX_OK,
	      "ingest");
	CHECK(fake.observes == 1 && fake.winners == 1 && fake.ingests == 1,
	      "ingest used the registered model");
	CHECK(anx_world_train_steps() == 1, "train steps from backend");

	/* Put JEPA back for the suites that follow */
	anx_jepa_world_model_register();
	CHECK(anx_strcmp(anx_world_model_name(), "jepa") == 0, "restored");
	return 0;
}
