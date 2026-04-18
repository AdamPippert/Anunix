/*
 * anx/model.h — Model Namespace (RFC-0013).
 *
 * A model is a namespace of State Objects: a manifest (structured
 * data), a set of tensor layers, and optional adapters. This header
 * defines the manifest structure and API for creating, querying,
 * and importing models.
 */

#ifndef ANX_MODEL_H
#define ANX_MODEL_H

#include <anx/types.h>

/* Forward declarations */
struct anx_state_object;

#define ANX_MODEL_NAME_MAX	64
#define ANX_MODEL_ARCH_MAX	32
#define ANX_MODEL_LAYERS_MAX	512

/* Model manifest — stored as structured data in models:/<name>/manifest */
struct anx_model_manifest {
	char name[ANX_MODEL_NAME_MAX];
	char architecture[ANX_MODEL_ARCH_MAX];	/* transformer, cnn, etc */
	uint64_t parameters;			/* total parameter count */
	uint32_t layers;			/* number of layer tensors */
	uint32_t hidden_dim;
	uint32_t vocab_size;
	uint32_t dtype;				/* default dtype enum */
};

/* Layer summary returned by anx_model_list_layers */
struct anx_model_layer_info {
	char name[128];
	uint32_t ndim;
	uint64_t shape[8];
	uint32_t dtype;
	uint64_t byte_size;
};

/* Create a model namespace and manifest */
int anx_model_create(const char *name,
		      const struct anx_model_manifest *manifest);

/* Get the manifest for a model */
int anx_model_manifest_get(const char *name,
			     struct anx_model_manifest *out);

/* List all layer tensors in a model */
int anx_model_list_layers(const char *name,
			    struct anx_model_layer_info *out,
			    uint32_t max_layers, uint32_t *count);

/* Add a tensor to a model's layer namespace */
int anx_model_add_layer(const char *model_name, const char *layer_path,
			  const anx_oid_t *tensor_oid);

/* Import a safetensors-format buffer into a model namespace */
int anx_model_import_safetensors(const char *model_name,
				  const void *data, uint64_t data_size,
				  uint32_t *tensor_count_out);

/* Diff two models: list layers that differ */
int anx_model_diff(const char *name_a, const char *name_b,
		    char diff_names[][128], uint32_t max_diffs,
		    uint32_t *count);

#endif /* ANX_MODEL_H */
