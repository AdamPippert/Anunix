/*
 * anx/namespace.h — Namespace and path resolution for State Objects.
 */

#ifndef ANX_NAMESPACE_H
#define ANX_NAMESPACE_H

#include <anx/types.h>

#define ANX_NS_NAME_MAX		64
#define ANX_NS_SEGMENT_MAX	255
#define ANX_NS_PATH_MAX		1024
#define ANX_NS_MAX_DEPTH	256

/* Initialize system namespaces (posix, default, system, traces) */
void anx_ns_init(void);

/* Create a user-defined namespace */
int anx_ns_create(const char *name);

/* Resolve a path within a namespace to an OID */
int anx_ns_resolve(const char *ns_name, const char *path, anx_oid_t *out);

/* Bind an object to a path in a namespace */
int anx_ns_bind(const char *ns_name, const char *path, const anx_oid_t *oid);

/* Remove a path binding */
int anx_ns_unbind(const char *ns_name, const char *path);

/* Entry info for listing */
struct anx_ns_list_entry {
	char name[ANX_NS_SEGMENT_MAX + 1];
	anx_oid_t oid;
	bool is_directory;
};

/* List entries at a path (NULL path = root). Returns count. */
int anx_ns_list(const char *ns_name, const char *path,
		 struct anx_ns_list_entry *out, uint32_t max_entries,
		 uint32_t *count);

/* List all namespace names */
int anx_ns_list_namespaces(char names[][ANX_NS_NAME_MAX],
			    uint32_t max_names, uint32_t *count);

#endif /* ANX_NAMESPACE_H */
