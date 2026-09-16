#ifndef ANX_TOOL_NAMESPACE_H
#define ANX_TOOL_NAMESPACE_H

#include <anx/types.h>

#define ANX_TOOL_CATALOG_MAX 64U
#define ANX_TOOL_NAMESPACE_MAX 256U
#define ANX_TOOL_WORKING_SET_MAX 8U
#define ANX_TOOL_DISCOVER 1U
#define ANX_TOOL_INVOKE 2U

struct anx_tool_ref { anx_oid_t id; uint64_t generation; };
struct anx_tool_namespace_ref { anx_oid_t id; uint64_t generation; };
struct anx_tool_handle {
	struct anx_tool_namespace_ref namespace;
	struct anx_tool_ref tool;
};

struct anx_tool_descriptor {
	char name[64];
	char endpoint[256];
	char method[16];
	char schema_version[32];
	uint32_t required_authority;
};

struct anx_tool_grant { anx_oid_t tool_id; uint32_t rights; };
struct anx_tool_catalog_entry {
	struct anx_tool_descriptor descriptor;
	struct anx_tool_handle handle;
};

struct anx_cell;
struct anx_external_call;

/* Catalog and namespace mutations belong to trusted control code outside cells. */
int anx_tool_register(const struct anx_tool_descriptor *descriptor, struct anx_tool_ref *out);
int anx_tool_update(const struct anx_tool_ref *expected, const struct anx_tool_descriptor *descriptor,
		    struct anx_tool_ref *out);
int anx_tool_remove(const struct anx_tool_ref *expected);
int anx_tool_namespace_create(struct anx_cell *root, const struct anx_tool_grant *grants,
			      uint32_t count, struct anx_tool_namespace_ref *out);
int anx_tool_namespace_replace(const struct anx_tool_namespace_ref *expected,
			       const struct anx_tool_grant *grants, uint32_t count,
			       struct anx_tool_namespace_ref *out);
/* Bounded discovery filters grants and current authority before writing results. */
int anx_tool_discover(struct anx_cell *cell, struct anx_tool_catalog_entry *out,
		      uint32_t capacity, uint32_t *count_out);
/* The external-call registry checks this gate before dispatch. */
int anx_tool_authorize_call(const struct anx_cell *cell, const struct anx_external_call *call);

#endif
