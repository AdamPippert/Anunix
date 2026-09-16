#ifndef ANX_CONTEXT_H
#define ANX_CONTEXT_H
#include <anx/state_object.h>
#define ANX_CONTEXT_MAX 32U
#define ANX_CONTEXT_PARENTS 4U
#define ANX_CONTEXT_SEGMENTS 8U
#define ANX_CONTEXT_BYTES 4096U
enum anx_context_role { ANX_CONTEXT_TOOL, ANX_CONTEXT_USER, ANX_CONTEXT_DEVELOPER, ANX_CONTEXT_SYSTEM };
enum anx_context_scope { ANX_CONTEXT_SESSION, ANX_CONTEXT_PROJECT, ANX_CONTEXT_ACCOUNT, ANX_CONTEXT_MACHINE };
enum anx_context_origin { ANX_CONTEXT_TOOL_OUTPUT, ANX_CONTEXT_USER_INPUT, ANX_CONTEXT_REPOSITORY, ANX_CONTEXT_POLICY, ANX_CONTEXT_DERIVED };
struct anx_context_view {
	uint64_t id;
	anx_cid_t owner;
	anx_oid_t source;
	uint64_t version;
	uint32_t size, sensitivity, parent_count;
	enum anx_context_origin origin;
	enum anx_context_role role_ceiling;
	enum anx_context_scope scope_ceiling;
	uint8_t digest[32];
};
struct anx_context_spec {
	uint32_t count;
	struct { uint64_t id; enum anx_context_role role; enum anx_context_scope scope; } segments[ANX_CONTEXT_SEGMENTS];
};
struct anx_context_bundle {
	uint32_t count, size;
	struct { uint64_t id; uint32_t offset, size; enum anx_context_role role; enum anx_context_scope scope; uint8_t digest[32]; } segments[ANX_CONTEXT_SEGMENTS];
	uint8_t bytes[ANX_CONTEXT_BYTES];
};
/* Only the controller assigns a root's origin; fixed ceilings follow that origin. */
int anx_context_import(const anx_cid_t *owner, const anx_oid_t *source, enum anx_context_origin origin, struct anx_context_view *out);
/* Derived records inherit the least privilege of every declared parent. */
int anx_context_derive(const anx_cid_t *owner, const anx_oid_t *source, const uint64_t *parents, uint32_t count, struct anx_context_view *out);
int anx_context_compile(const anx_cid_t *owner, const struct anx_context_spec *spec, struct anx_context_bundle *out);
int anx_context_get(uint64_t id, struct anx_context_view *out);
int anx_context_destroy(uint64_t id);
#endif
