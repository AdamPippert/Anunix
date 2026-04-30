/*
 * loop_goal.c — Goal alignment energy computation (RFC-0020, Phase 3).
 *
 * Translates a natural-language goal_text into an energy score for a
 * given JEPA action by keyword-to-action-category matching.
 *
 * Energy semantics: 0.0 = perfect goal alignment, 1.0 = misaligned.
 * Returns 0.5 (neutral) when goal_text is empty or no keyword matches.
 */

#include <anx/loop.h>
#include <anx/jepa.h>
#include <anx/memory.h>
#include <anx/string.h>

/* ------------------------------------------------------------------ */
/* Action category bitmasks (one bit per ANX_JEPA_ACT_*)              */
/* ------------------------------------------------------------------ */

#define ACTBIT(a)	(1u << (a))

/* Grouped action sets for scoring */
#define ACT_ROUTE   (ACTBIT(ANX_JEPA_ACT_ROUTE_LOCAL) | \
		     ACTBIT(ANX_JEPA_ACT_ROUTE_REMOTE) | \
		     ACTBIT(ANX_JEPA_ACT_ROUTE_FALLBACK))

#define ACT_MEM     (ACTBIT(ANX_JEPA_ACT_MEM_PROMOTE) | \
		     ACTBIT(ANX_JEPA_ACT_MEM_DEMOTE) | \
		     ACTBIT(ANX_JEPA_ACT_MEM_FORGET))

#define ACT_CELL    (ACTBIT(ANX_JEPA_ACT_CELL_SPAWN) | \
		     ACTBIT(ANX_JEPA_ACT_CELL_CANCEL))

#define ACT_SEC     (ACTBIT(ANX_JEPA_ACT_CAP_VALIDATE) | \
		     ACTBIT(ANX_JEPA_ACT_CAP_SUSPEND) | \
		     ACTBIT(ANX_JEPA_ACT_SECURITY_ALERT))

#define ACT_IDLE    ACTBIT(ANX_JEPA_ACT_IDLE)

/* ------------------------------------------------------------------ */
/* Keyword table                                                       */
/* ------------------------------------------------------------------ */

struct kw_entry {
	const char *keyword;
	uint32_t    preferred_mask;	/* low energy actions for this keyword */
	uint32_t    opposed_mask;	/* high energy (misaligned) actions */
};

static const struct kw_entry g_kw_table[] = {
	/* Routing keywords */
	{ "route",     ACT_ROUTE, ACT_MEM | ACT_SEC },
	{ "routing",   ACT_ROUTE, ACT_MEM | ACT_SEC },
	{ "network",   ACT_ROUTE, ACT_MEM },
	{ "forward",   ACT_ROUTE, ACT_MEM | ACT_IDLE },
	{ "deliver",   ACT_ROUTE, 0 },
	{ "send",      ACT_ROUTE, 0 },

	/* Memory keywords */
	{ "memory",    ACT_MEM, ACT_ROUTE | ACT_SEC },
	{ "mem",       ACT_MEM, ACT_ROUTE | ACT_SEC },
	{ "cache",     ACTBIT(ANX_JEPA_ACT_MEM_PROMOTE), ACTBIT(ANX_JEPA_ACT_MEM_FORGET) },
	{ "promote",   ACTBIT(ANX_JEPA_ACT_MEM_PROMOTE), ACTBIT(ANX_JEPA_ACT_MEM_DEMOTE) },
	{ "evict",     ACTBIT(ANX_JEPA_ACT_MEM_FORGET),  ACTBIT(ANX_JEPA_ACT_MEM_PROMOTE) },
	{ "forget",    ACTBIT(ANX_JEPA_ACT_MEM_FORGET),  ACTBIT(ANX_JEPA_ACT_MEM_PROMOTE) },
	{ "demote",    ACTBIT(ANX_JEPA_ACT_MEM_DEMOTE),  ACTBIT(ANX_JEPA_ACT_MEM_PROMOTE) },

	/* Cell / process keywords */
	{ "cell",      ACT_CELL,  ACT_MEM | ACT_SEC },
	{ "spawn",     ACTBIT(ANX_JEPA_ACT_CELL_SPAWN),  ACTBIT(ANX_JEPA_ACT_CELL_CANCEL) },
	{ "launch",    ACTBIT(ANX_JEPA_ACT_CELL_SPAWN),  0 },
	{ "cancel",    ACTBIT(ANX_JEPA_ACT_CELL_CANCEL), ACTBIT(ANX_JEPA_ACT_CELL_SPAWN) },
	{ "stop",      ACTBIT(ANX_JEPA_ACT_CELL_CANCEL) | ACT_IDLE, 0 },
	{ "task",      ACT_CELL,  0 },
	{ "process",   ACT_CELL,  0 },
	{ "execute",   ACT_CELL,  ACT_IDLE },
	{ "run",       ACT_CELL,  ACT_IDLE },

	/* Security / capability keywords */
	{ "security",  ACT_SEC,   ACT_MEM | ACT_ROUTE },
	{ "secure",    ACT_SEC,   0 },
	{ "validate",  ACTBIT(ANX_JEPA_ACT_CAP_VALIDATE), 0 },
	{ "capability",ACT_SEC,   ACT_MEM | ACT_ROUTE },
	{ "cap",       ACT_SEC,   0 },
	{ "suspend",   ACTBIT(ANX_JEPA_ACT_CAP_SUSPEND),  ACTBIT(ANX_JEPA_ACT_CELL_SPAWN) },
	{ "alert",     ACTBIT(ANX_JEPA_ACT_SECURITY_ALERT), ACT_IDLE },
	{ "threat",    ACTBIT(ANX_JEPA_ACT_SECURITY_ALERT), ACT_IDLE },

	/* Idle / wait keywords */
	{ "idle",      ACT_IDLE,  ACT_CELL | ACT_ROUTE },
	{ "wait",      ACT_IDLE,  ACT_CELL },
	{ "pause",     ACT_IDLE,  ACT_CELL },
	{ "monitor",   ACT_IDLE,  0 },
	{ "observe",   ACT_IDLE,  0 },
};

#define KW_TABLE_COUNT  (sizeof(g_kw_table) / sizeof(g_kw_table[0]))

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Case-insensitive substring search within a null-terminated string. */
static bool kw_match(const char *haystack, const char *needle)
{
	uint32_t nlen = (uint32_t)anx_strlen(needle);
	uint32_t hlen = (uint32_t)anx_strlen(haystack);
	uint32_t i, j;
	char hc, nc;

	if (nlen == 0 || nlen > hlen)
		return false;

	for (i = 0; i <= hlen - nlen; i++) {
		bool match = true;

		for (j = 0; j < nlen; j++) {
			hc = haystack[i + j];
			nc = needle[j];
			/* Lowercase compare */
			if (hc >= 'A' && hc <= 'Z') hc += 32;
			if (nc >= 'A' && nc <= 'Z') nc += 32;
			if (hc != nc) { match = false; break; }
		}
		if (match)
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

uint32_t anx_loop_select_action_by_prior(const char *world_uri,
					  uint32_t action_count)
{
	uint32_t best_id    = 0;
	float    best_prior = 1.0f;
	uint32_t i;

	if (!world_uri || action_count == 0)
		return 0;

	for (i = 0; i < action_count; i++) {
		float p = anx_pal_action_prior(world_uri, i);

		if (p < best_prior) {
			best_prior = p;
			best_id    = i;
		}
	}
	return best_id;
}

float anx_loop_goal_alignment_energy(const char *goal_text, uint32_t action_id)
{
	uint32_t preferred_mask = 0;
	uint32_t opposed_mask   = 0;
	uint32_t action_bit;
	uint32_t i;

	if (!goal_text || goal_text[0] == '\0')
		return 0.5f;	/* no goal → neutral */

	if (action_id >= (uint32_t)ANX_JEPA_ACT_COUNT)
		return 0.5f;

	action_bit = ACTBIT(action_id);

	/* Accumulate preferred / opposed masks from all matching keywords */
	for (i = 0; i < (uint32_t)KW_TABLE_COUNT; i++) {
		if (kw_match(goal_text, g_kw_table[i].keyword)) {
			preferred_mask |= g_kw_table[i].preferred_mask;
			opposed_mask   |= g_kw_table[i].opposed_mask;
		}
	}

	if (preferred_mask == 0 && opposed_mask == 0)
		return 0.5f;	/* no keyword matched → neutral */

	/* Preferred actions get low energy (goal-aligned) */
	if (action_bit & preferred_mask)
		return 0.1f;

	/* Opposed actions get high energy (goal-misaligned) */
	if (action_bit & opposed_mask)
		return 0.9f;

	/* Action not in either set: slightly above neutral */
	return 0.55f;
}
