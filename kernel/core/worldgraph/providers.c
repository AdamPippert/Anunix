/*
 * providers.c — built-in world graph providers (RFC-0026 Section 7).
 *
 * The constraint validator is the first concrete provider. It plugs into the
 * commit gate and rejects any patch that attaches a numeric constraint the
 * resulting node state violates, using the provider read interface to evaluate
 * the constraint against the branch's speculative world. This is what turns
 * attach_constraint from an annotation into an enforced world rule.
 */

#include <anx/worldgraph.h>
#include <anx/string.h>

enum cmp_op {
	CMP_GT,
	CMP_LT,
	CMP_GE,
	CMP_LE,
	CMP_EQ,
	CMP_NE,
	CMP_NONE,	/* not a recognized numeric constraint */
};

/*
 * Parse a signed decimal into fixed-point milli-units: "1" -> 1000,
 * "-5" -> -5000, "0.25" -> 250. Up to three fractional digits are honored;
 * extra digits are ignored. Returns false on a malformed number.
 */
static bool parse_milli(const char *s, int64_t *out)
{
	int64_t whole = 0, frac = 0, scale = 100;
	bool neg = false, any = false;

	if (!s)
		return false;
	while (*s == ' ')
		s++;
	if (*s == '-') {
		neg = true;
		s++;
	} else if (*s == '+') {
		s++;
	}
	while (*s >= '0' && *s <= '9') {
		whole = whole * 10 + (*s - '0');
		s++;
		any = true;
	}
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9') {
			if (scale > 0) {
				frac += (int64_t)(*s - '0') * scale;
				scale /= 10;
			}
			s++;
			any = true;
		}
	}
	while (*s == ' ')
		s++;
	if (!any || *s != '\0')
		return false;
	*out = (whole * 1000 + frac) * (neg ? -1 : 1);
	return true;
}

/*
 * Split "<key><op><number>" into key, operator, and threshold. The operator is
 * the first comparison token found; two-character tokens are tried first so
 * ">=" is not read as ">". Returns CMP_NONE if no operator is present.
 */
static enum cmp_op parse_constraint(const char *expr, char *key, size_t klen,
				    int64_t *threshold)
{
	const char *p = expr;
	const char *op_at = NULL;
	enum cmp_op op = CMP_NONE;
	size_t n = 0;

	if (!expr)
		return CMP_NONE;

	for (p = expr; *p; p++) {
		if ((p[0] == '>' || p[0] == '<' || p[0] == '=' ||
		     p[0] == '!') && p[1] == '=') {
			op = p[0] == '>' ? CMP_GE : p[0] == '<' ? CMP_LE :
			     p[0] == '=' ? CMP_EQ : CMP_NE;
			op_at = p;
			n = 2;
			break;
		}
		if (p[0] == '>' || p[0] == '<') {
			op = p[0] == '>' ? CMP_GT : CMP_LT;
			op_at = p;
			n = 1;
			break;
		}
	}
	if (!op_at)
		return CMP_NONE;

	/* key is everything before the operator, trimmed of trailing spaces */
	{
		size_t klen_src = (size_t)(op_at - expr);

		while (klen_src > 0 && expr[klen_src - 1] == ' ')
			klen_src--;
		if (klen_src + 1 > klen)
			klen_src = klen - 1;
		anx_memcpy(key, expr, klen_src);
		key[klen_src] = '\0';
	}

	if (!parse_milli(op_at + n, threshold))
		return CMP_NONE;
	return op;
}

static bool satisfied(int64_t value, enum cmp_op op, int64_t threshold)
{
	switch (op) {
	case CMP_GT: return value > threshold;
	case CMP_LT: return value < threshold;
	case CMP_GE: return value >= threshold;
	case CMP_LE: return value <= threshold;
	case CMP_EQ: return value == threshold;
	case CMP_NE: return value != threshold;
	default:     return true;
	}
}

/* Read property `key` of branch snapshot node `idx` as milli. Returns false if
 * the property is absent or non-numeric. */
static bool node_prop_milli(const struct anx_world_branch *b, uint32_t idx,
			    const char *key, int64_t *out)
{
	struct anx_world_node_info info;
	uint32_t i;

	if (anx_world_branch_get_node(b, idx, &info) != ANX_OK)
		return false;
	for (i = 0; i < info.prop_count; i++) {
		char k[48], v[64];

		if (anx_world_branch_get_prop(b, idx, i, k, sizeof(k),
					      v, sizeof(v)) != ANX_OK)
			continue;
		if (anx_strcmp(k, key) == 0)
			return parse_milli(v, out);
	}
	return false;
}

static int constraint_validate(const struct anx_world_patch *patch,
			       const struct anx_world_branch *branch,
			       char *reason, size_t len, void *ctx)
{
	uint32_t i, nops = anx_world_patch_op_count(patch);

	(void)ctx;

	for (i = 0; i < nops; i++) {
		struct anx_world_op_info op;
		char key[48];
		enum cmp_op cmp;
		int64_t threshold, value;
		uint32_t idx;

		if (anx_world_patch_get_op(patch, i, &op) != ANX_OK)
			continue;
		if (op.type != ANX_WOP_ATTACH_CONSTRAINT)
			continue;

		cmp = parse_constraint(op.s2, key, sizeof(key), &threshold);
		if (cmp == CMP_NONE)
			continue;	/* not our mini-language; let it pass */

		if (anx_world_branch_resolve_ref(branch, patch, &op.a, &idx)
		    != ANX_OK) {
			anx_strlcpy(reason, "constraint targets unknown node",
				    len);
			return ANX_EINVAL;
		}
		if (!node_prop_milli(branch, idx, key, &value)) {
			anx_snprintf(reason, len,
				     "constraint '%s': property '%s' missing",
				     op.s2, key);
			return ANX_EINVAL;
		}
		if (!satisfied(value, cmp, threshold)) {
			anx_snprintf(reason, len, "constraint '%s' violated",
				     op.s2);
			return ANX_EINVAL;
		}
	}
	return ANX_OK;
}

int anx_world_constraint_validator_register(void)
{
	struct anx_world_manifest m;

	anx_memset(&m, 0, sizeof(m));
	anx_strlcpy(m.id, "constraint.validator", sizeof(m.id));
	anx_strlcpy(m.domain, "validation", sizeof(m.domain));
	/* a pure gate: reads everything, writes nothing */
	anx_strlcpy(m.reads, "*", sizeof(m.reads));
	return anx_world_provider_register(&m, constraint_validate, NULL);
}
