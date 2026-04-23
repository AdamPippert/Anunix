/*
 * jepa_tool.c — JEPA tool engine.
 *
 * Registers JEPA as an ANX_ENGINE_DETERMINISTIC_TOOL so that RLM rollouts
 * can invoke it as structured tool_use calls.  Three tools are provided:
 *
 *   jepa_world_state   — Serialise current latent state as JSON summary.
 *   jepa_predict       — Predict outcome of a named action as JSON.
 *   jepa_anomaly_score — Compute divergence between predicted and actual
 *                        latent state as JSON.
 *
 * Output is JSON-formatted text written into caller-provided buffers.
 * All outputs are bounded by buf_size; truncation returns ANX_ENOMEM.
 */

#include "jepa_internal.h"
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/engine.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/json.h>

/* ------------------------------------------------------------------ */
/* JSON helpers (minimal, no heap)                                     */
/* ------------------------------------------------------------------ */

/* Append a string to buf respecting remaining space.  Returns remaining. */
static int32_t buf_append(char *buf, uint32_t buf_size,
			  uint32_t *pos, const char *str)
{
	uint32_t len = (uint32_t)anx_strlen(str);

	if (*pos + len + 1 > buf_size)
		return -1;

	anx_memcpy(buf + *pos, str, len);
	*pos += len;
	buf[*pos] = '\0';
	return (int32_t)(buf_size - *pos);
}

/* Append a decimal integer. */
static int32_t buf_append_u32(char *buf, uint32_t buf_size,
			      uint32_t *pos, uint32_t val)
{
	char tmp[12];
	uint32_t i = 0, digits = 0;
	uint32_t v = val;

	if (v == 0) {
		tmp[i++] = '0';
	} else {
		char rev[12];
		while (v > 0) { rev[digits++] = '0' + (char)(v % 10); v /= 10; }
		while (digits > 0) tmp[i++] = rev[--digits];
	}
	tmp[i] = '\0';
	return buf_append(buf, buf_size, pos, tmp);
}

/* Append a float with 4 decimal places. */
static int32_t buf_append_f32(char *buf, uint32_t buf_size,
			      uint32_t *pos, float val)
{
	uint32_t int_part;
	uint32_t frac_part;
	char tmp[24];
	uint32_t ti = 0;

	if (val < 0.0f) {
		tmp[ti++] = '-';
		val = -val;
	}
	int_part  = (uint32_t)val;
	frac_part = (uint32_t)((val - (float)int_part) * 10000.0f + 0.5f);

	/* Integer digits */
	{
		char rev[12]; uint32_t d = 0, v = int_part;
		if (v == 0) tmp[ti++] = '0';
		else { while (v>0){rev[d++]='0'+(char)(v%10);v/=10;} while(d>0)tmp[ti++]=rev[--d]; }
	}
	tmp[ti++] = '.';
	/* 4 fractional digits */
	tmp[ti++] = '0' + (char)(frac_part / 1000);
	tmp[ti++] = '0' + (char)((frac_part / 100) % 10);
	tmp[ti++] = '0' + (char)((frac_part / 10)  % 10);
	tmp[ti++] = '0' + (char)(frac_part % 10);
	tmp[ti]   = '\0';

	return buf_append(buf, buf_size, pos, tmp);
}

/* ------------------------------------------------------------------ */
/* Tool: jepa_world_state                                              */
/* ------------------------------------------------------------------ */

int anx_jepa_tool_world_state(char *out_buf, uint32_t buf_size)
{
	struct anx_jepa_ctx *ctx = anx_jepa_ctx_get();
	const struct anx_jepa_world_profile *world;
	struct anx_jepa_obs obs;
	uint32_t pos = 0;
	int rc;

	if (!out_buf || buf_size == 0)
		return ANX_EINVAL;
	if (!anx_jepa_available()) {
		buf_append(out_buf, buf_size, &pos,
			   "{\"jepa\":\"unavailable\"}");
		return ANX_OK;
	}

	rc = anx_jepa_observe(&obs);
	if (rc != ANX_OK) {
		buf_append(out_buf, buf_size, &pos,
			   "{\"jepa\":\"observe_failed\"}");
		return ANX_OK;
	}

	world = anx_jepa_world_get_active();

#define A(s) do { if (buf_append(out_buf, buf_size, &pos, (s)) < 0) return ANX_ENOMEM; } while(0)
#define AU(v) do { if (buf_append_u32(out_buf, buf_size, &pos, (v)) < 0) return ANX_ENOMEM; } while(0)
#define AF(v) do { if (buf_append_f32(out_buf, buf_size, &pos, (v)) < 0) return ANX_ENOMEM; } while(0)

	A("{");
	A("\"world\":\"");  A(world->uri);  A("\",");
	A("\"status\":");   AU((uint32_t)ctx->status);  A(",");
	A("\"encode_count\":"); AU((uint32_t)ctx->encode_count); A(",");
	A("\"last_loss\":"); AF(ctx->last_loss); A(",");
	A("\"last_divergence\":"); AF(ctx->last_divergence); A(",");
	A("\"obs\":{");
	A("\"active_cells\":"); AU(obs.active_cell_count); A(",");
	A("\"sched_q_interactive\":"); AU(obs.sched_queue_depths[0]); A(",");
	A("\"sched_q_batch\":"); AU(obs.sched_queue_depths[3]); A(",");
	A("\"route_fallbacks\":"); AU(obs.route_fallback_count); A(",");
	A("\"route_avg_score\":"); AF(obs.route_avg_score); A(",");
	A("\"tensor_cpu_util\":"); AF(obs.tensor_cpu_util); A(",");
	A("\"tensor_npu_util\":"); AF(obs.tensor_npu_util); A(",");
	A("\"cap_validation_avg\":"); AF(obs.cap_validation_avg); A(",");
	A("\"security_events\":"); AU(obs.security_event_count);
	A("}}");

#undef A
#undef AU
#undef AF

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Tool: jepa_predict                                                  */
/* ------------------------------------------------------------------ */

int anx_jepa_tool_predict(const char *action_name,
			  char *out_buf, uint32_t buf_size)
{
	const struct anx_jepa_world_profile *world;
	struct anx_jepa_obs obs;
	anx_oid_t obs_oid, latent_oid, pred_oid;
	uint32_t action_id, i;
	uint32_t pos = 0;
	int rc;

	if (!action_name || !out_buf || buf_size == 0)
		return ANX_EINVAL;
	if (!anx_jepa_available()) {
		buf_append(out_buf, buf_size, &pos,
			   "{\"jepa\":\"unavailable\"}");
		return ANX_OK;
	}

	world = anx_jepa_world_get_active();

	/* Resolve action name to action_id */
	action_id = 0;
	for (i = 0; i < world->action_count; i++) {
		if (anx_strcmp(world->action_names[i], action_name) == 0) {
			action_id = i;
			break;
		}
	}

	rc = anx_jepa_observe(&obs);
	if (rc != ANX_OK)
		goto err;

	rc = anx_jepa_observe_store(&obs, &obs_oid);
	if (rc != ANX_OK)
		goto err;

	rc = anx_jepa_encode(&obs_oid, &latent_oid);
	if (rc != ANX_OK)
		goto err;

	rc = anx_jepa_predict(&latent_oid, action_id, &pred_oid);
	if (rc != ANX_OK)
		goto err;

	/* Divergence from current state = magnitude of predicted change */
	float divergence = anx_jepa_divergence(&latent_oid, &pred_oid);

#define A(s) do { if (buf_append(out_buf, buf_size, &pos, (s)) < 0) return ANX_ENOMEM; } while(0)
#define AF(v) do { if (buf_append_f32(out_buf, buf_size, &pos, (v)) < 0) return ANX_ENOMEM; } while(0)

	A("{\"action\":\""); A(action_name); A("\",");
	A("\"predicted_divergence\":"); AF(divergence); A(",");
	A("\"interpretation\":\"");
	if (divergence < 0.1f)       A("stable");
	else if (divergence < 0.3f)  A("minor_change");
	else if (divergence < 0.6f)  A("moderate_change");
	else                         A("significant_change");
	A("\"}");

#undef A
#undef AF

	return ANX_OK;

err:
	anx_memset(out_buf, 0, buf_size > 64 ? 64 : buf_size);
	buf_append(out_buf, buf_size, &pos, "{\"error\":\"predict_failed\"}");
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Tool: jepa_anomaly_score                                            */
/* ------------------------------------------------------------------ */

int anx_jepa_tool_anomaly_score(char *out_buf, uint32_t buf_size)
{
	struct anx_jepa_ctx *ctx = anx_jepa_ctx_get();
	uint32_t pos = 0;

	if (!out_buf || buf_size == 0)
		return ANX_EINVAL;

#define A(s) do { if (buf_append(out_buf, buf_size, &pos, (s)) < 0) return ANX_ENOMEM; } while(0)
#define AF(v) do { if (buf_append_f32(out_buf, buf_size, &pos, (v)) < 0) return ANX_ENOMEM; } while(0)

	A("{\"divergence\":"); AF(ctx->last_divergence); A(",");
	A("\"anomaly_level\":\"");
	if (!anx_jepa_available())              A("unavailable");
	else if (ctx->last_divergence < 0.0f)   A("no_data");
	else if (ctx->last_divergence < 0.15f)  A("normal");
	else if (ctx->last_divergence < 0.40f)  A("elevated");
	else if (ctx->last_divergence < 0.70f)  A("high");
	else                                    A("critical");
	A("\"}");

#undef A
#undef AF

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Engine registration                                                 */
/* ------------------------------------------------------------------ */

static struct anx_engine *g_tool_engine;

int anx_jepa_tool_register(void)
{
	int rc;

	rc = anx_engine_register("anx-jepa-tools",
				 ANX_ENGINE_DETERMINISTIC_TOOL,
				 ANX_CAP_JEPA | ANX_CAP_TOOL_EXECUTION,
				 &g_tool_engine);
	if (rc != ANX_OK) {
		kprintf("[jepa] tool engine registration failed (%d)\n", rc);
		return rc;
	}

	anx_engine_transition(g_tool_engine, ANX_ENGINE_AVAILABLE);
	return ANX_OK;
}
