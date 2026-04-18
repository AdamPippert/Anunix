/*
 * search.c — Find Tensor Objects by BRIN properties.
 *
 * Iterates all objects in the store, filters for tensors, and
 * applies a BRIN-based predicate. This avoids loading payloads —
 * only metadata is checked. Supports predicates like:
 *   sparsity > 0.5
 *   mean < 0
 *   dtype == int8
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/uuid.h>

/* Maximum results from a single search */
#define SEARCH_MAX_RESULTS	64

/* Predicate operators */
enum search_op {
	SEARCH_OP_GT,
	SEARCH_OP_LT,
	SEARCH_OP_EQ,
	SEARCH_OP_GE,
	SEARCH_OP_LE,
};

/* Which BRIN field to query */
enum search_field {
	SEARCH_FIELD_MEAN,
	SEARCH_FIELD_VARIANCE,
	SEARCH_FIELD_L2_NORM,
	SEARCH_FIELD_SPARSITY,
	SEARCH_FIELD_MIN,
	SEARCH_FIELD_MAX,
	SEARCH_FIELD_DTYPE,
};

struct search_predicate {
	enum search_field field;
	enum search_op op;
	uint32_t sf_value;	/* softfloat for numeric, dtype enum for dtype */
};

struct search_ctx {
	struct search_predicate pred;
	anx_oid_t results[SEARCH_MAX_RESULTS];
	uint32_t count;
};

static uint32_t get_field(const struct anx_tensor_meta *meta,
			    enum search_field field)
{
	switch (field) {
	case SEARCH_FIELD_MEAN:		return meta->stat_mean_bits;
	case SEARCH_FIELD_VARIANCE:	return meta->stat_variance_bits;
	case SEARCH_FIELD_L2_NORM:	return meta->stat_l2_norm_bits;
	case SEARCH_FIELD_SPARSITY:	return meta->stat_sparsity_bits;
	case SEARCH_FIELD_MIN:		return meta->stat_min_bits;
	case SEARCH_FIELD_MAX:		return meta->stat_max_bits;
	case SEARCH_FIELD_DTYPE:	return (uint32_t)meta->dtype;
	default:			return 0;
	}
}

static bool eval_predicate(const struct search_predicate *pred,
			     const struct anx_tensor_meta *meta)
{
	uint32_t val = get_field(meta, pred->field);

	if (pred->field == SEARCH_FIELD_DTYPE) {
		/* Dtype comparison uses integer equality */
		if (pred->op == SEARCH_OP_EQ)
			return val == pred->sf_value;
		return false;
	}

	/* Numeric comparison uses softfloat */
	switch (pred->op) {
	case SEARCH_OP_GT:	return anx_sf_gt(val, pred->sf_value);
	case SEARCH_OP_LT:	return anx_sf_lt(val, pred->sf_value);
	case SEARCH_OP_GE:	return !anx_sf_lt(val, pred->sf_value);
	case SEARCH_OP_LE:	return !anx_sf_gt(val, pred->sf_value);
	case SEARCH_OP_EQ:
		return !anx_sf_lt(val, pred->sf_value) &&
		       !anx_sf_gt(val, pred->sf_value);
	default:		return false;
	}
}

static int search_cb(struct anx_state_object *obj, void *arg)
{
	struct search_ctx *ctx = (struct search_ctx *)arg;
	struct anx_tensor_meta meta;
	int ret;

	if (obj->object_type != ANX_OBJ_TENSOR)
		return 0;	/* skip non-tensors */

	ret = anx_tensor_meta_get(&obj->oid, &meta);
	if (ret != ANX_OK)
		return 0;

	if (eval_predicate(&ctx->pred, &meta)) {
		if (ctx->count < SEARCH_MAX_RESULTS)
			ctx->results[ctx->count++] = obj->oid;
	}

	return 0;	/* continue iteration */
}

/*
 * Parse a predicate string like "sparsity>0.5" or "dtype==int8".
 * Returns ANX_OK on success.
 */
static int parse_predicate(const char *expr, struct search_predicate *pred)
{
	const char *p = expr;
	const char *field_end;
	const char *val_start;

	/* Find operator */
	field_end = p;
	while (*field_end && *field_end != '>' && *field_end != '<' &&
	       *field_end != '=' && *field_end != '!')
		field_end++;

	if (*field_end == '\0')
		return ANX_EINVAL;

	/* Parse field name */
	{
		uint32_t flen = (uint32_t)(field_end - p);

		if (flen == 4 && anx_strncmp(p, "mean", 4) == 0)
			pred->field = SEARCH_FIELD_MEAN;
		else if (flen == 8 && anx_strncmp(p, "variance", 8) == 0)
			pred->field = SEARCH_FIELD_VARIANCE;
		else if (flen == 7 && anx_strncmp(p, "l2_norm", 7) == 0)
			pred->field = SEARCH_FIELD_L2_NORM;
		else if (flen == 8 && anx_strncmp(p, "sparsity", 8) == 0)
			pred->field = SEARCH_FIELD_SPARSITY;
		else if (flen == 3 && anx_strncmp(p, "min", 3) == 0)
			pred->field = SEARCH_FIELD_MIN;
		else if (flen == 3 && anx_strncmp(p, "max", 3) == 0)
			pred->field = SEARCH_FIELD_MAX;
		else if (flen == 5 && anx_strncmp(p, "dtype", 5) == 0)
			pred->field = SEARCH_FIELD_DTYPE;
		else
			return ANX_EINVAL;
	}

	/* Parse operator */
	p = field_end;
	if (p[0] == '>' && p[1] == '=') {
		pred->op = SEARCH_OP_GE;
		val_start = p + 2;
	} else if (p[0] == '<' && p[1] == '=') {
		pred->op = SEARCH_OP_LE;
		val_start = p + 2;
	} else if (p[0] == '=' && p[1] == '=') {
		pred->op = SEARCH_OP_EQ;
		val_start = p + 2;
	} else if (p[0] == '>') {
		pred->op = SEARCH_OP_GT;
		val_start = p + 1;
	} else if (p[0] == '<') {
		pred->op = SEARCH_OP_LT;
		val_start = p + 1;
	} else {
		return ANX_EINVAL;
	}

	/* Parse value */
	if (pred->field == SEARCH_FIELD_DTYPE) {
		/* Dtype name */
		if (anx_strcmp(val_start, "int8") == 0)
			pred->sf_value = ANX_DTYPE_INT8;
		else if (anx_strcmp(val_start, "uint8") == 0)
			pred->sf_value = ANX_DTYPE_UINT8;
		else if (anx_strcmp(val_start, "int32") == 0)
			pred->sf_value = ANX_DTYPE_INT32;
		else if (anx_strcmp(val_start, "float32") == 0)
			pred->sf_value = ANX_DTYPE_FLOAT32;
		else if (anx_strcmp(val_start, "float16") == 0)
			pred->sf_value = ANX_DTYPE_FLOAT16;
		else if (anx_strcmp(val_start, "bfloat16") == 0)
			pred->sf_value = ANX_DTYPE_BFLOAT16;
		else
			return ANX_EINVAL;
	} else {
		/* Parse as integer, convert to softfloat */
		/* Support simple integers and one-decimal values */
		int64_t integer = 0;
		bool negative = false;

		p = val_start;
		if (*p == '-') {
			negative = true;
			p++;
		}
		while (*p >= '0' && *p <= '9') {
			integer = integer * 10 + (*p - '0');
			p++;
		}
		if (negative)
			integer = -integer;

		/* Handle decimal part (e.g., "0.5") */
		if (*p == '.') {
			uint32_t frac = 0;
			uint32_t frac_div = 1;
			uint32_t sf_int, sf_frac, sf_frac_div;

			p++;
			while (*p >= '0' && *p <= '9') {
				frac = frac * 10 + (uint32_t)(*p - '0');
				frac_div *= 10;
				p++;
			}
			sf_int = anx_sf_from_int(integer);
			sf_frac = anx_sf_from_int((int64_t)frac);
			sf_frac_div = anx_sf_from_int((int64_t)frac_div);
			pred->sf_value = anx_sf_add(sf_int,
				anx_sf_div(sf_frac, sf_frac_div));
			if (negative)
				pred->sf_value |= 0x80000000U;
		} else {
			pred->sf_value = anx_sf_from_int(integer);
		}
	}

	return ANX_OK;
}

int anx_tensor_search(const char *predicate_str,
		       anx_oid_t *results, uint32_t max_results,
		       uint32_t *count_out)
{
	struct search_ctx ctx;
	int ret;

	if (!predicate_str || !results || !count_out)
		return ANX_EINVAL;

	anx_memset(&ctx, 0, sizeof(ctx));

	ret = parse_predicate(predicate_str, &ctx.pred);
	if (ret != ANX_OK)
		return ret;

	anx_objstore_iterate(search_cb, &ctx);

	{
		uint32_t copy_count = ctx.count;

		if (copy_count > max_results)
			copy_count = max_results;
		anx_memcpy(results, ctx.results,
			   copy_count * sizeof(anx_oid_t));
		*count_out = copy_count;
	}

	return ANX_OK;
}
