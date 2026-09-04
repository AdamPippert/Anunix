/*
 * config.c — configuration as a governed world-graph slice (RFC-0027).
 *
 * A setting lives as a node in the "config.<area>" slice of a dedicated world
 * graph: label is the key, property "value" is the value. Writes go through the
 * commit gate as patches authored by the "config.writer" provider, so every
 * change is versioned and provenance-tracked. The "config.schema" validator
 * type-checks declared keys at the gate. No new primitive — configuration is
 * just world state under governance.
 */

#include <anx/config.h>
#include <anx/worldgraph.h>
#include <anx/string.h>

#define VALUE_PROP	"value"

/* --- Schema registry --- */

struct schema_entry {
	bool used;
	char area[ANX_CONFIG_AREA_MAX];
	char key[ANX_CONFIG_KEY_MAX];
	enum anx_config_type type;
	char spec[ANX_CONFIG_SPEC_MAX];		/* enum allowed-set */
};

static struct schema_entry schema[ANX_CONFIG_MAX_SCHEMA];
static struct anx_world_graph *cfg_graph;
static bool cfg_inited;

/* Build the world-graph domain for an area: "config.<area>". */
static void make_domain(const char *area, char *buf, size_t len)
{
	anx_snprintf(buf, (uint32_t)len, "config.%s", area ? area : "");
}

struct anx_world_graph *anx_config_graph(void)
{
	if (!cfg_graph)
		cfg_graph = anx_world_graph_create("anx:world/config");
	return cfg_graph;
}

static struct schema_entry *schema_find(const char *area, const char *key)
{
	uint32_t i;

	for (i = 0; i < ANX_CONFIG_MAX_SCHEMA; i++)
		if (schema[i].used &&
		    anx_strcmp(schema[i].area, area) == 0 &&
		    anx_strcmp(schema[i].key, key) == 0)
			return &schema[i];
	return NULL;
}

int anx_config_declare(const char *area, const char *key,
		       enum anx_config_type type, const char *spec)
{
	struct schema_entry *e;
	uint32_t i;

	if (!area || !key || !area[0] || !key[0])
		return ANX_EINVAL;

	e = schema_find(area, key);
	if (!e) {
		for (i = 0; i < ANX_CONFIG_MAX_SCHEMA; i++) {
			if (!schema[i].used) {
				e = &schema[i];
				break;
			}
		}
	}
	if (!e)
		return ANX_EFULL;

	e->used = true;
	anx_strlcpy(e->area, area, sizeof(e->area));
	anx_strlcpy(e->key, key, sizeof(e->key));
	e->type = type;
	anx_strlcpy(e->spec, spec ? spec : "", sizeof(e->spec));
	return ANX_OK;
}

/* --- Value type checks --- */

static bool is_int(const char *s)
{
	if (!s || !*s)
		return false;
	if (*s == '-' || *s == '+')
		s++;
	if (!*s)
		return false;
	while (*s) {
		if (*s < '0' || *s > '9')
			return false;
		s++;
	}
	return true;
}

static bool is_bool(const char *s)
{
	static const char *ok[] = { "true", "false", "0", "1", "on", "off",
				    "yes", "no", NULL };
	uint32_t i;

	for (i = 0; ok[i]; i++)
		if (anx_strcmp(s, ok[i]) == 0)
			return true;
	return false;
}

/* True if comma-set `set` contains token `want`. */
static bool set_contains(const char *set, const char *want)
{
	size_t wlen = anx_strlen(want);
	const char *p = set;

	while (*p) {
		const char *start = p;
		size_t seg;

		while (*p && *p != ',')
			p++;
		seg = (size_t)(p - start);
		if (seg > 0 && *start == ' ') {
			start++;
			seg--;
		}
		if (seg == wlen && anx_strncmp(start, want, wlen) == 0)
			return true;
		if (*p == ',')
			p++;
	}
	return false;
}

/* Validate a value against a declared schema. NULL schema => any non-empty
 * string is fine. Returns true if acceptable. */
static bool value_ok(const struct schema_entry *e, const char *value,
		     const char **why)
{
	if (!value || !value[0]) {
		*why = "empty value";
		return false;
	}
	if (!e)
		return true;
	switch (e->type) {
	case ANX_CFG_INT:
		if (!is_int(value)) {
			*why = "not an integer";
			return false;
		}
		return true;
	case ANX_CFG_BOOL:
		if (!is_bool(value)) {
			*why = "not a boolean";
			return false;
		}
		return true;
	case ANX_CFG_ENUM:
		if (!set_contains(e->spec, value)) {
			*why = "not in allowed set";
			return false;
		}
		return true;
	case ANX_CFG_STRING:
	default:
		return true;
	}
}

/* --- Commit-gate validator --- */

static int config_validate(const struct anx_world_patch *patch,
			   const struct anx_world_branch *branch,
			   char *reason, size_t len, void *ctx)
{
	uint32_t i, nops = anx_world_patch_op_count(patch);

	(void)ctx;

	for (i = 0; i < nops; i++) {
		struct anx_world_op_info op;
		struct anx_world_node_info node;
		const struct schema_entry *e;
		const char *why = "invalid";
		const char *area;
		uint32_t idx;

		if (anx_world_patch_get_op(patch, i, &op) != ANX_OK)
			continue;
		if (op.type != ANX_WOP_UPDATE_PROPERTY)
			continue;
		if (anx_strcmp(op.s1, VALUE_PROP) != 0)
			continue;
		if (anx_world_branch_resolve_ref(branch, patch, &op.a, &idx)
		    != ANX_OK)
			continue;
		if (anx_world_branch_get_node(branch, idx, &node) != ANX_OK)
			continue;
		/* Only govern the config.* slice. */
		if (anx_strncmp(node.domain, "config.", 7) != 0)
			continue;

		area = node.domain + 7;
		e = schema_find(area, node.label);
		if (!value_ok(e, op.s2, &why)) {
			anx_snprintf(reason, len, "config %s.%s: %s",
				     area, node.label, why);
			return ANX_EINVAL;
		}
	}
	return ANX_OK;
}

void anx_config_init(void)
{
	struct anx_world_manifest m;

	if (cfg_inited)
		return;
	cfg_inited = true;

	/* Authority to write the config.* slice. */
	anx_memset(&m, 0, sizeof(m));
	anx_strlcpy(m.id, "config.writer", sizeof(m.id));
	anx_strlcpy(m.domain, "config", sizeof(m.domain));
	anx_strlcpy(m.writes, "config.*", sizeof(m.writes));
	anx_world_provider_register(&m, NULL, NULL);

	/* The RLM configurator authors under its own identity. */
	anx_strlcpy(m.id, "config.rlm", sizeof(m.id));
	anx_world_provider_register(&m, NULL, NULL);

	/* Schema type-checker on the commit gate. */
	anx_memset(&m, 0, sizeof(m));
	anx_strlcpy(m.id, "config.schema", sizeof(m.id));
	anx_strlcpy(m.domain, "validation", sizeof(m.domain));
	anx_strlcpy(m.reads, "config.*", sizeof(m.reads));
	anx_world_provider_register(&m, config_validate, NULL);
}

/* --- Lookup helpers --- */

/* Find the canonical node for (domain,key). Returns index or -1. */
static int find_node(struct anx_world_graph *g, const char *dom,
		     const char *key)
{
	uint32_t i, n = anx_world_graph_node_count(g);
	struct anx_world_node_info info;

	for (i = 0; i < n; i++) {
		if (anx_world_graph_get_node(g, i, &info) != ANX_OK)
			continue;
		if (anx_strcmp(info.domain, dom) == 0 &&
		    anx_strcmp(info.label, key) == 0)
			return (int)i;
	}
	return -1;
}

/* Read node index's "value" property into out. ANX_OK or ANX_ENOENT. */
static int read_value(struct anx_world_graph *g, uint32_t idx, char *out,
		      size_t len)
{
	struct anx_world_node_info info;
	uint32_t k;

	if (anx_world_graph_get_node(g, idx, &info) != ANX_OK)
		return ANX_ENOENT;
	for (k = 0; k < info.prop_count; k++) {
		char key[ANX_WORLD_KEY_MAX];

		if (anx_world_graph_get_prop(g, idx, k, key, sizeof(key),
					     out, len) != ANX_OK)
			continue;
		if (anx_strcmp(key, VALUE_PROP) == 0)
			return ANX_OK;
	}
	return ANX_ENOENT;
}

/*
 * Stage an add-or-update of (area,key)=value into an existing patch against the
 * config graph. Returns ANX_OK or a negative error.
 */
static int stage_set(struct anx_world_patch *p, struct anx_world_graph *g,
		     const char *area, const char *key, const char *value)
{
	char dom[ANX_WORLD_DOMAIN_MAX];
	struct anx_world_ref ref;
	int idx;

	if (!area || !key || !value || !area[0] || !key[0])
		return ANX_EINVAL;

	make_domain(area, dom, sizeof(dom));
	idx = find_node(g, dom, key);
	if (idx >= 0) {
		struct anx_world_node_info info;

		if (anx_world_graph_get_node(g, (uint32_t)idx, &info) != ANX_OK)
			return ANX_ENOENT;
		ref = anx_world_ref_oid(info.id);
	} else {
		int rc = anx_world_patch_add_node(p, dom, key, &ref);

		if (rc != ANX_OK)
			return rc;
	}
	return anx_world_patch_update_property(p, ref, VALUE_PROP, value);
}

/* Commit a single-writer patch, threading the gate report to the caller. */
static int commit_patch(struct anx_world_graph *g, struct anx_world_patch *p,
			struct anx_world_commit_report *report)
{
	struct anx_world_branch *b = anx_world_branch_fork(g);
	int rc;

	if (!b)
		return ANX_ENOMEM;
	rc = anx_world_branch_propose(b, p);
	if (rc == ANX_OK)
		rc = anx_world_branch_commit(b, report);
	anx_world_branch_abandon(b);
	return rc;
}

int anx_config_try(const char *area, const char *key, const char *value,
		   struct anx_world_commit_report *report)
{
	struct anx_world_graph *g = anx_config_graph();
	struct anx_world_patch *p;
	int rc;

	if (!g)
		return ANX_ENOMEM;
	anx_config_init();

	p = anx_world_patch_create("config.writer");
	if (!p)
		return ANX_ENOMEM;
	rc = stage_set(p, g, area, key, value);
	if (rc == ANX_OK)
		rc = commit_patch(g, p, report);
	anx_world_patch_destroy(p);
	return rc;
}

int anx_config_set(const char *area, const char *key, const char *value)
{
	return anx_config_try(area, key, value, NULL);
}

int anx_config_get(const char *area, const char *key, char *out, size_t len)
{
	struct anx_world_graph *g = anx_config_graph();
	char dom[ANX_WORLD_DOMAIN_MAX];
	int idx;

	if (!g || !area || !key || !out)
		return ANX_EINVAL;
	make_domain(area, dom, sizeof(dom));
	idx = find_node(g, dom, key);
	if (idx < 0)
		return ANX_ENOENT;
	return read_value(g, (uint32_t)idx, out, len);
}

int anx_config_list(const char *area, anx_config_iter_fn cb, void *arg)
{
	struct anx_world_graph *g = anx_config_graph();
	char dom[ANX_WORLD_DOMAIN_MAX];
	uint32_t i, n;
	int stop = 0;

	if (!cb)
		return ANX_EINVAL;
	if (!g)
		return ANX_OK;
	if (area && area[0])
		make_domain(area, dom, sizeof(dom));
	else
		dom[0] = '\0';

	n = anx_world_graph_node_count(g);
	for (i = 0; i < n; i++) {
		struct anx_world_node_info info;
		char value[ANX_CONFIG_VAL_MAX];

		if (anx_world_graph_get_node(g, i, &info) != ANX_OK)
			continue;
		if (anx_strncmp(info.domain, "config.", 7) != 0)
			continue;
		if (dom[0] && anx_strcmp(info.domain, dom) != 0)
			continue;
		if (read_value(g, i, value, sizeof(value)) != ANX_OK)
			value[0] = '\0';
		stop = cb(info.domain + 7, info.label, value, arg);
		if (stop)
			return stop;
	}
	return ANX_OK;
}

/* --- Configurator: apply a text proposal as one gated patch --- */

/* Advance past spaces. */
static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* Copy the next whitespace-delimited token into buf; return end pointer. */
static const char *next_token(const char *p, char *buf, size_t len)
{
	size_t i = 0;

	p = skip_ws(p);
	while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
		if (i + 1 < len)
			buf[i++] = *p;
		p++;
	}
	buf[i] = '\0';
	return p;
}

int anx_configurator_apply(const char *proposal,
			   struct anx_world_commit_report *report)
{
	struct anx_world_graph *g = anx_config_graph();
	struct anx_world_patch *p;
	struct anx_world_branch *b;
	const char *line = proposal;
	uint32_t staged = 0;
	int rc;

	if (!g)
		return ANX_ENOMEM;
	if (!proposal)
		return ANX_EINVAL;
	anx_config_init();

	p = anx_world_patch_create("config.rlm");
	if (!p)
		return ANX_ENOMEM;

	while (*line) {
		char verb[8], area[ANX_CONFIG_AREA_MAX], key[ANX_CONFIG_KEY_MAX];
		char value[ANX_CONFIG_VAL_MAX];
		const char *q = line;
		size_t vi = 0;

		q = next_token(q, verb, sizeof(verb));
		if (anx_strcmp(verb, "SET") == 0 ||
		    anx_strcmp(verb, "set") == 0) {
			q = next_token(q, area, sizeof(area));
			q = next_token(q, key, sizeof(key));
			q = skip_ws(q);
			/* value is the rest of the line, trimmed */
			while (*q && *q != '\n' && *q != '\r') {
				if (vi + 1 < sizeof(value))
					value[vi++] = *q;
				q++;
			}
			while (vi > 0 && value[vi - 1] == ' ')
				vi--;
			value[vi] = '\0';
			if (area[0] && key[0] && value[0]) {
				if (stage_set(p, g, area, key, value) == ANX_OK)
					staged++;
			}
		}
		/* advance to next line */
		while (*line && *line != '\n')
			line++;
		while (*line == '\n' || *line == '\r')
			line++;
	}

	if (staged == 0) {
		anx_world_patch_destroy(p);
		if (report) {
			anx_memset(report, 0, sizeof(*report));
			report->reason_code = ANX_ENOENT;
			anx_strlcpy(report->reason, "no SET directives",
				    sizeof(report->reason));
		}
		return ANX_ENOENT;
	}

	b = anx_world_branch_fork(g);
	if (!b) {
		anx_world_patch_destroy(p);
		return ANX_ENOMEM;
	}
	rc = anx_world_branch_propose(b, p);
	if (rc == ANX_OK)
		rc = anx_world_branch_commit(b, report);
	anx_world_branch_abandon(b);
	anx_world_patch_destroy(p);
	return rc;
}
