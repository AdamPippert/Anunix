/*
 * jepa_internal.h — Internal declarations shared across jepa_*.c files.
 * Not part of the public kernel/include/anx/jepa.h API.
 */

#ifndef JEPA_INTERNAL_H
#define JEPA_INTERNAL_H

#include <anx/jepa.h>

/* Global context accessor (jepa.c) */
struct anx_jepa_ctx *anx_jepa_ctx_get(void);

/* os-default observation collector (jepa_obs.c) */
int anx_jepa_obs_collect_os_default(void *obs_buf, uint32_t obs_buf_size);

/*
 * Linearize an anx_jepa_obs snapshot into a flat float vector.
 * out_vec must have capacity >= world->arch.obs_dim floats.
 * Returns the number of floats written, or negative on error.
 */
int anx_jepa_obs_linearize(const struct anx_jepa_obs *obs,
			   float *out_vec, uint32_t max_dim);

/*
 * Normalize a float vector to unit L2 norm in-place.
 * No-op if the norm is zero.
 */
void anx_jepa_vec_normalize(float *vec, uint32_t dim);

/*
 * Cosine similarity between two float vectors of equal dimension.
 * Returns value in [-1.0, 1.0]; -2.0 on error.
 */
float anx_jepa_vec_cosine(const float *a, const float *b, uint32_t dim);

/* Built-in world profile registration (jepa_world.c) */
int anx_jepa_world_register_builtins(void);

/* Built-in workflow registration (jepa_workflow.c) */
int anx_jepa_workflow_register(void);

/* Tool engine registration (jepa_tool.c) */
int anx_jepa_tool_register(void);

#endif /* JEPA_INTERNAL_H */
