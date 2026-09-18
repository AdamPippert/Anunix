/*
 * test_shell_tools.c — Helpers behind ansh commands.
 *
 * Each check is a command that printed the wrong thing on jekyll:
 * `date` froze and named the wrong weekday, `workflow show` printed raw
 * "%-2u" specifiers, `cat <oid>` could not find the OIDs `write` prints,
 * `meta show` did not exist, and `tensor fill` left float tensors zero.
 */

#include <anx/types.h>
#include <anx/civil.h>
#include <anx/string.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/meta.h>
#include <anx/tensor.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			kprintf("  FAIL: %s\n", (msg));			\
			return -1;					\
		}							\
	} while (0)

static int check_civil(void)
{
	struct anx_civil c;

	anx_civil_from_unix(0, 0, &c);
	CHECK(c.year == 1970 && c.month == 1 && c.day == 1 && c.wday == 4,
	      "epoch is Thursday 1970-01-01");
	anx_civil_from_unix(1789658050, 0, &c);
	CHECK(c.year == 2026 && c.month == 9 && c.day == 17 &&
	      c.hour == 15 && c.min == 14 && c.sec == 10 && c.wday == 4,
	      "2026-09-17 15:14:10 Thursday");
	anx_civil_from_unix(1789658050, -16, &c);
	CHECK(c.day == 16 && c.hour == 23 && c.wday == 3,
	      "offset rolls the date back");
	anx_civil_from_unix(951782400, 0, &c);
	CHECK(c.month == 2 && c.day == 29 && c.wday == 2, "2000-02-29");
	anx_civil_from_unix(-1, 0, &c);
	CHECK(c.year == 1969 && c.month == 12 && c.day == 31 &&
	      c.hour == 23 && c.wday == 3, "one second before the epoch");
	CHECK(anx_strcmp(anx_civil_day_name(4), "Thu") == 0 &&
	      anx_strcmp(anx_civil_day_name(9), "???") == 0, "day names");
	return 0;
}

static int check_snprintf(void)
{
	char b[64];
	int n;

	n = anx_snprintf(b, sizeof(b), "[%-2u|%-6s|%3d|%02u]", 7, "ab", -5, 3);
	CHECK(n == 18 && anx_strcmp(b, "[7 |ab    | -5|03]") == 0,
	      "width and flags");
	anx_snprintf(b, sizeof(b), "%x %04x %c%%", 255, 0x1a, 'z');
	CHECK(anx_strcmp(b, "ff 001a z%") == 0, "hex, char, percent");
	anx_snprintf(b, sizeof(b), "%.*s|%.2s|%llu|%lld", 3, "abcdef", "xyz",
		     18446744073709551615ULL, -9223372036854775807LL - 1);
	CHECK(anx_strcmp(b, "abc|xy|18446744073709551615|"
			    "-9223372036854775808") == 0, "precision, 64-bit");
	n = anx_snprintf(b, 6, "%s", "truncated");
	CHECK(n == 5 && anx_strcmp(b, "trunc") == 0, "truncation");
	anx_snprintf(b, sizeof(b), "%05d", -42);
	CHECK(anx_strcmp(b, "-0042") == 0, "zero pad after the sign");
	return 0;
}

struct meta_count {
	uint32_t n;
	bool saw_color;
};

static void count_entry(const struct anx_meta_entry *e, void *arg)
{
	struct meta_count *m = arg;

	m->n++;
	if (anx_strcmp(e->key, "color") == 0 &&
	    anx_strcmp(e->value.v.str.data, "blue") == 0)
		m->saw_color = true;
}

static int check_resolve_and_meta(void)
{
	struct anx_so_create_params p;
	struct anx_state_object *a, *b;
	struct anx_meta_store *ms;
	struct meta_count mc = { 0, false };
	char s[37], prefix[37];
	anx_oid_t oid;
	uint32_t i;

	anx_memset(&p, 0, sizeof(p));
	p.object_type = ANX_OBJ_BYTE_DATA;
	p.payload = "resolve me";
	p.payload_size = 10;
	CHECK(anx_so_create(&p, &a) == ANX_OK, "create a");
	CHECK(anx_so_create(&p, &b) == ANX_OK, "create b");
	CHECK(anx_ns_bind("default", "shelltest/a", &a->oid) == ANX_OK, "bind");

	CHECK(anx_so_resolve("default:shelltest/a", &oid) == ANX_OK &&
	      anx_uuid_compare(&oid, &a->oid) == 0, "ns:path");
	CHECK(anx_so_resolve("shelltest/a", &oid) == ANX_OK &&
	      anx_uuid_compare(&oid, &a->oid) == 0, "bare default path");

	anx_uuid_to_string(&b->oid, s, sizeof(s));
	CHECK(anx_so_resolve(s, &oid) == ANX_OK &&
	      anx_uuid_compare(&oid, &b->oid) == 0, "full OID");
	for (i = 0; s[i]; i++)
		prefix[i] = (s[i] >= 'a' && s[i] <= 'f') ?
			    (char)(s[i] - 'a' + 'A') : s[i];
	prefix[i] = '\0';
	CHECK(anx_so_resolve(prefix, &oid) == ANX_OK, "upper-case OID");
	CHECK(anx_so_resolve("0000", &oid) == ANX_EEXIST ||
	      anx_so_resolve("0000", &oid) == ANX_ENOENT, "short prefix");
	CHECK(anx_so_resolve("ffffffff-ffff", &oid) == ANX_ENOENT,
	      "unknown OID");
	CHECK(anx_so_resolve("nosuch/path", &oid) == ANX_ENOENT,
	      "unknown path");

	ms = anx_meta_create();
	CHECK(ms != NULL, "meta store");
	anx_meta_set_str(ms, "color", "blue");
	anx_meta_set_i64(ms, "size", 3);
	anx_meta_set_bool(ms, "ok", true);
	anx_meta_iterate(ms, count_entry, &mc);
	CHECK(mc.n == 3 && mc.saw_color, "meta iterate sees every entry");
	anx_meta_destroy(ms);
	anx_meta_iterate(NULL, count_entry, &mc);

	anx_ns_unbind("default", "shelltest/a");
	anx_objstore_release(a);
	anx_objstore_release(b);
	return 0;
}

static int check_tensor_fill(void)
{
	struct anx_tensor_meta m;
	struct anx_state_object *t;

	anx_memset(&m, 0, sizeof(m));
	m.ndim = 1;
	m.shape[0] = 4;
	m.dtype = ANX_DTYPE_FLOAT32;
	CHECK(anx_tensor_create(&m, NULL, 0, &t) == ANX_OK, "f32 tensor");
	CHECK(anx_tensor_fill(&t->oid, "ones") == ANX_OK, "f32 ones");
	CHECK(((uint32_t *)t->payload)[3] == 0x3F800000, "1.0f bits");
	CHECK(anx_tensor_fill(&t->oid, "range") == ANX_OK, "f32 range");
	CHECK(((uint32_t *)t->payload)[0] == 0 &&
	      ((uint32_t *)t->payload)[1] == 0x3F800000 &&
	      ((uint32_t *)t->payload)[2] == 0x40000000 &&
	      ((uint32_t *)t->payload)[3] == 0x40400000, "0,1,2,3 bits");
	CHECK(anx_tensor_fill(&t->oid, "1.5") == ANX_EINVAL,
	      "unknown pattern rejected");

	/* stats of 0,1,2,3: mean 1.5, variance 1.25, l2 sqrt(14) */
	CHECK(anx_tensor_meta_get(&t->oid, &m) == ANX_OK, "meta");
	CHECK(anx_tensor_compute_brin(t, &m) == ANX_OK, "stats");
	CHECK(m.stat_mean_bits == 0x3FC00000, "mean 1.5");
	CHECK(m.stat_variance_bits == 0x3FA00000, "variance 1.25");
	CHECK(m.stat_l2_norm_bits >= 0x406F7750 &&
	      m.stat_l2_norm_bits <= 0x406F7752, "l2 sqrt(14)");
	anx_objstore_release(t);

	m.dtype = ANX_DTYPE_FLOAT16;
	CHECK(anx_tensor_create(&m, NULL, 0, &t) == ANX_OK, "f16 tensor");
	CHECK(anx_tensor_fill(&t->oid, "range") == ANX_OK, "f16 range");
	CHECK(((uint16_t *)t->payload)[1] == 0x3C00 &&
	      ((uint16_t *)t->payload)[3] == 0x4200, "f16 bits");
	anx_objstore_release(t);

	m.dtype = ANX_DTYPE_FLOAT64;
	CHECK(anx_tensor_create(&m, NULL, 0, &t) == ANX_OK, "f64 tensor");
	CHECK(anx_tensor_fill(&t->oid, "range") == ANX_OK, "f64 range");
	CHECK(((uint64_t *)t->payload)[2] == 0x4000000000000000ULL,
	      "2.0 bits");
	anx_objstore_release(t);
	return 0;
}

int test_shell_tools(void)
{
	if (check_civil() || check_snprintf() || check_resolve_and_meta() ||
	    check_tensor_fill())
		return -1;
	return 0;
}
