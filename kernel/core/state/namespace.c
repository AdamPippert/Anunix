/*
 * namespace.c — Namespace and path resolution.
 *
 * A namespace is a hierarchical directory tree that maps path
 * segments to State Object OIDs. Directory nodes are stored as
 * hash tables of name→OID pairs.
 */

#include <anx/types.h>
#include <anx/namespace.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/hashtable.h>
#include <anx/list.h>
#include <anx/uuid.h>

#define NS_HASH_BITS	4	/* 16 namespaces max initially */
#define DIR_HASH_BITS	6	/* 64 entries per directory */

/* A single directory entry */
struct anx_ns_entry {
	char name[ANX_NS_SEGMENT_MAX + 1];
	anx_oid_t oid;
	struct anx_ns_entry *children;	/* subdirectory (NULL if leaf) */
	uint32_t child_count;
	uint32_t child_cap;
	struct anx_list_head link;
};

/* A namespace */
struct anx_namespace {
	char name[ANX_NS_NAME_MAX];
	struct anx_ns_entry *root;	/* root directory entries */
	uint32_t root_count;
	uint32_t root_cap;
	struct anx_list_head link;
};

/* Global namespace registry */
static struct anx_htable ns_table;

static struct anx_namespace *ns_find(const char *name)
{
	uint64_t hash = anx_hash_bytes(name, anx_strlen(name));
	struct anx_list_head *pos;

	ANX_HTABLE_FOR_BUCKET(pos, &ns_table, hash) {
		struct anx_namespace *ns =
			ANX_LIST_ENTRY(pos, struct anx_namespace, link);
		if (anx_strcmp(ns->name, name) == 0)
			return ns;
	}
	return NULL;
}

static struct anx_namespace *ns_alloc(const char *name)
{
	struct anx_namespace *ns = anx_zalloc(sizeof(*ns));
	if (!ns)
		return NULL;

	anx_strlcpy(ns->name, name, ANX_NS_NAME_MAX);
	ns->root_cap = 16;
	ns->root = anx_zalloc(ns->root_cap * sizeof(struct anx_ns_entry));
	if (!ns->root) {
		anx_free(ns);
		return NULL;
	}
	anx_list_init(&ns->link);
	return ns;
}

void anx_ns_init(void)
{
	anx_htable_init(&ns_table, NS_HASH_BITS);

	/* Create system namespaces */
	static const char *system_ns[] = {
		"posix", "default", "system", "traces"
	};

	for (int i = 0; i < 4; i++) {
		struct anx_namespace *ns = ns_alloc(system_ns[i]);
		if (ns) {
			uint64_t hash = anx_hash_bytes(system_ns[i],
						       anx_strlen(system_ns[i]));
			anx_htable_add(&ns_table, &ns->link, hash);
		}
	}
}

int anx_ns_create(const char *name)
{
	if (!name || anx_strlen(name) == 0)
		return ANX_EINVAL;
	if (ns_find(name))
		return ANX_EEXIST;

	struct anx_namespace *ns = ns_alloc(name);
	if (!ns)
		return ANX_ENOMEM;

	uint64_t hash = anx_hash_bytes(name, anx_strlen(name));
	anx_htable_add(&ns_table, &ns->link, hash);
	return ANX_OK;
}

/* Find or create an entry in an entry array */
static struct anx_ns_entry *entry_find(struct anx_ns_entry *entries,
				       uint32_t count, const char *name)
{
	for (uint32_t i = 0; i < count; i++) {
		if (anx_strcmp(entries[i].name, name) == 0)
			return &entries[i];
	}
	return NULL;
}

int anx_ns_resolve(const char *ns_name, const char *path, anx_oid_t *out)
{
	struct anx_namespace *ns = ns_find(ns_name);
	if (!ns)
		return ANX_ENOENT;

	/* Skip leading slash */
	if (*path == '/')
		path++;
	if (*path == '\0')
		return ANX_EINVAL;

	struct anx_ns_entry *entries = ns->root;
	uint32_t count = ns->root_count;

	/* Walk path segments */
	char segment[ANX_NS_SEGMENT_MAX + 1];

	while (*path) {
		/* Extract next segment */
		const char *slash = path;
		size_t slen = 0;
		while (*slash && *slash != '/') {
			slash++;
			slen++;
		}
		if (slen > ANX_NS_SEGMENT_MAX)
			return ANX_EINVAL;

		anx_memcpy(segment, path, slen);
		segment[slen] = '\0';

		struct anx_ns_entry *entry = entry_find(entries, count, segment);
		if (!entry)
			return ANX_ENOENT;

		path = *slash ? slash + 1 : slash;

		if (*path == '\0') {
			/* Final segment — return OID */
			*out = entry->oid;
			return ANX_OK;
		}

		/* Navigate into subdirectory */
		if (!entry->children)
			return ANX_ENOENT;
		entries = entry->children;
		count = entry->child_count;
	}

	return ANX_ENOENT;
}

static int entry_add(struct anx_ns_entry **entries, uint32_t *count,
		     uint32_t *cap, const char *name, const anx_oid_t *oid)
{
	if (*count >= *cap) {
		uint32_t new_cap = *cap * 2;
		struct anx_ns_entry *new_buf;
		new_buf = anx_zalloc(new_cap * sizeof(struct anx_ns_entry));
		if (!new_buf)
			return ANX_ENOMEM;
		anx_memcpy(new_buf, *entries,
			   *count * sizeof(struct anx_ns_entry));
		anx_free(*entries);
		*entries = new_buf;
		*cap = new_cap;
	}

	struct anx_ns_entry *entry = &(*entries)[*count];
	anx_strlcpy(entry->name, name, ANX_NS_SEGMENT_MAX + 1);
	entry->oid = *oid;
	entry->children = NULL;
	entry->child_count = 0;
	entry->child_cap = 0;
	(*count)++;
	return ANX_OK;
}

int anx_ns_bind(const char *ns_name, const char *path, const anx_oid_t *oid)
{
	struct anx_namespace *ns = ns_find(ns_name);
	if (!ns)
		return ANX_ENOENT;

	if (*path == '/')
		path++;
	if (*path == '\0')
		return ANX_EINVAL;

	struct anx_ns_entry **entries = &ns->root;
	uint32_t *count = &ns->root_count;
	uint32_t *cap = &ns->root_cap;

	char segment[ANX_NS_SEGMENT_MAX + 1];

	while (*path) {
		const char *slash = path;
		size_t slen = 0;
		while (*slash && *slash != '/') {
			slash++;
			slen++;
		}
		if (slen > ANX_NS_SEGMENT_MAX)
			return ANX_EINVAL;

		anx_memcpy(segment, path, slen);
		segment[slen] = '\0';

		path = *slash ? slash + 1 : slash;

		if (*path == '\0') {
			/* Final segment — bind the OID */
			struct anx_ns_entry *existing;
			existing = entry_find(*entries, *count, segment);
			if (existing) {
				existing->oid = *oid;
				return ANX_OK;
			}
			return entry_add(entries, count, cap, segment, oid);
		}

		/* Intermediate segment — find or create directory */
		struct anx_ns_entry *entry;
		entry = entry_find(*entries, *count, segment);
		if (!entry) {
			anx_oid_t nil = ANX_UUID_NIL;
			int ret = entry_add(entries, count, cap, segment, &nil);
			if (ret != ANX_OK)
				return ret;
			entry = &(*entries)[*count - 1];
			entry->child_cap = 8;
			entry->children = anx_zalloc(
				entry->child_cap * sizeof(struct anx_ns_entry));
			if (!entry->children)
				return ANX_ENOMEM;
		} else if (!entry->children) {
			entry->child_cap = 8;
			entry->children = anx_zalloc(
				entry->child_cap * sizeof(struct anx_ns_entry));
			if (!entry->children)
				return ANX_ENOMEM;
		}

		entries = &entry->children;
		count = &entry->child_count;
		cap = &entry->child_cap;
	}

	return ANX_EINVAL;
}

int anx_ns_unbind(const char *ns_name, const char *path)
{
	struct anx_namespace *ns = ns_find(ns_name);
	struct anx_ns_entry *entries;
	uint32_t *count;
	uint32_t i;

	if (!ns)
		return ANX_ENOENT;

	if (*path == '/')
		path++;
	if (*path == '\0')
		return ANX_EINVAL;

	entries = ns->root;
	count = &ns->root_count;

	/* Walk to parent of final segment */
	char segment[ANX_NS_SEGMENT_MAX + 1];

	while (*path) {
		const char *slash = path;
		size_t slen = 0;

		while (*slash && *slash != '/') {
			slash++;
			slen++;
		}
		if (slen > ANX_NS_SEGMENT_MAX)
			return ANX_EINVAL;

		anx_memcpy(segment, path, slen);
		segment[slen] = '\0';
		path = *slash ? slash + 1 : slash;

		if (*path == '\0') {
			/* Final segment — find and remove */
			for (i = 0; i < *count; i++) {
				if (anx_strcmp(entries[i].name, segment) == 0) {
					/* Shift remaining entries down */
					for (uint32_t j = i; j + 1 < *count; j++)
						entries[j] = entries[j + 1];
					(*count)--;
					return ANX_OK;
				}
			}
			return ANX_ENOENT;
		}

		/* Navigate into subdirectory */
		struct anx_ns_entry *entry;

		entry = entry_find(entries, *count, segment);
		if (!entry || !entry->children)
			return ANX_ENOENT;
		entries = entry->children;
		count = &entry->child_count;
	}

	return ANX_ENOENT;
}

int anx_ns_list(const char *ns_name, const char *path,
		 struct anx_ns_list_entry *out, uint32_t max_entries,
		 uint32_t *count_out)
{
	struct anx_namespace *ns;
	struct anx_ns_entry *entries;
	uint32_t entry_count;
	uint32_t n = 0;
	uint32_t i;

	if (!ns_name || !out || max_entries == 0) {
		if (count_out) *count_out = 0;
		return ANX_EINVAL;
	}

	ns = ns_find(ns_name);
	if (!ns) {
		if (count_out) *count_out = 0;
		return ANX_ENOENT;
	}

	entries = ns->root;
	entry_count = ns->root_count;

	/* If path specified, navigate to that directory */
	if (path && *path) {
		if (*path == '/')
			path++;

		char segment[ANX_NS_SEGMENT_MAX + 1];

		while (*path) {
			const char *slash = path;
			size_t slen = 0;

			while (*slash && *slash != '/') {
				slash++;
				slen++;
			}
			if (slen > ANX_NS_SEGMENT_MAX)
				return ANX_EINVAL;

			anx_memcpy(segment, path, slen);
			segment[slen] = '\0';
			path = *slash ? slash + 1 : slash;

			struct anx_ns_entry *entry;

			entry = entry_find(entries, entry_count, segment);
			if (!entry)
				return ANX_ENOENT;
			if (!entry->children) {
				/* It's a leaf — can't list inside it */
				if (*path)
					return ANX_ENOENT;
				/* Single entry result */
				if (max_entries > 0) {
					anx_strlcpy(out[0].name, entry->name,
						     sizeof(out[0].name));
					out[0].oid = entry->oid;
					out[0].is_directory = false;
					n = 1;
				}
				if (count_out)
					*count_out = n;
				return ANX_OK;
			}
			entries = entry->children;
			entry_count = entry->child_count;
		}
	}

	/* Copy entries to output */
	for (i = 0; i < entry_count && n < max_entries; i++) {
		anx_strlcpy(out[n].name, entries[i].name,
			     sizeof(out[n].name));
		out[n].oid = entries[i].oid;
		out[n].is_directory = (entries[i].children != NULL);
		n++;
	}

	if (count_out)
		*count_out = n;
	return ANX_OK;
}

int anx_ns_list_namespaces(char names[][ANX_NS_NAME_MAX],
			    uint32_t max_names, uint32_t *count_out)
{
	uint32_t n = 0;
	uint32_t bucket;

	for (bucket = 0; bucket < (1u << NS_HASH_BITS); bucket++) {
		struct anx_list_head *pos;
		struct anx_list_head *head;

		head = anx_htable_bucket(&ns_table, bucket);
		ANX_LIST_FOR_EACH(pos, head) {
			struct anx_namespace *ns;

			ns = ANX_LIST_ENTRY(pos, struct anx_namespace, link);
			if (n < max_names)
				anx_strlcpy(names[n], ns->name,
					     ANX_NS_NAME_MAX);
			n++;
		}
	}

	if (count_out)
		*count_out = n;
	return ANX_OK;
}
