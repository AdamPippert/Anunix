/*
 * anxml.c — Inference runtime (RFC-0021), CPU path.
 *
 * v1 ships a built-in deterministic character-LM "toy model" so that
 * the MODEL_CALL workflow path is end-to-end functional without external
 * weights.  A future phase loads GGUF / safetensors via anx_model_*.
 *
 * Toy model behavior:
 *   - Vocabulary: 7-bit ASCII (128 symbols).
 *   - Bigram table P(next | prev) seeded from a small corpus and the
 *     prompt itself.
 *   - Sampling: greedy if seed == 0; otherwise xorshift32(seed) chooses
 *     among the top-K non-zero bigram successors (K = 4).
 *   - Stops at newline once the response has at least one non-space
 *     printable, on max_tokens, or on output buffer full.
 *
 * The toy is intentionally compact and deterministic.  It is the
 * correctness oracle for the future real backends — given the same
 * (prompt, seed) the output is reproducible across runs.
 */

#include <anx/anxml.h>
#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Bigram table                                                        */
/* ------------------------------------------------------------------ */

#define V ANX_ANXML_TOY_VOCAB

static uint32_t g_bigram[V][V];
static bool     g_inited;

/*
 * A small canon of English-ish phrases used to seed the bigram table.
 * The point is not realism — it's reproducible non-trivial output.
 */
static const char *const g_corpus[] = {
	"the quick brown fox jumps over the lazy dog\n",
	"anunix is an ai native operating system\n",
	"state objects replace files. cells replace processes.\n",
	"workflows compose cells into directed graphs\n",
	"the inference runtime is anxml. the kernel is the control plane.\n",
	"to be or not to be that is the question\n",
	"hello world from the anxml toy model\n",
	"every object has an oid. every cell has a cid.\n",
	"the answer to the question is yes.\n",
	"a model is a namespace of state objects.\n",
	NULL,
};

static void seed_corpus(void)
{
	uint32_t i;
	for (i = 0; g_corpus[i]; i++) {
		const unsigned char *s = (const unsigned char *)g_corpus[i];
		uint32_t prev = ' ';
		for (; *s; s++) {
			unsigned char c = *s & 0x7f;
			g_bigram[prev][c] += 4;
			prev = c;
		}
	}
}

/* Light xorshift32 RNG */
static uint32_t rng_step(uint32_t *state)
{
	uint32_t x = *state ? *state : 0x12345678U;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

void anx_anxml_init(void)
{
	if (g_inited) return;
	anx_memset(g_bigram, 0, sizeof(g_bigram));
	seed_corpus();
	g_inited = true;
}

/* Boost bigrams from the prompt so the toy stays on-topic. */
static void seed_prompt(const char *prompt, uint32_t len)
{
	uint32_t i;
	uint32_t prev = ' ';
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)prompt[i] & 0x7f;
		g_bigram[prev][c] += 8;
		prev = c;
	}
}

/* Pick the next character given the previous one. */
static int sample_next(unsigned char prev, uint32_t *rng_state)
{
	const uint32_t *row = g_bigram[prev];
	uint32_t total = 0;
	uint32_t i;
	uint32_t r;
	uint32_t acc = 0;

	for (i = 0; i < V; i++) total += row[i];
	if (total == 0)
		return ' ';
	if (rng_state == NULL) {
		/* greedy */
		uint32_t best = 0; int besti = ' ';
		for (i = 0; i < V; i++) {
			if (row[i] > best) { best = row[i]; besti = (int)i; }
		}
		return besti;
	}
	r = rng_step(rng_state) % total;
	for (i = 0; i < V; i++) {
		acc += row[i];
		if (r < acc) return (int)i;
	}
	return ' ';
}

/* ------------------------------------------------------------------ */
/* Public generate                                                     */
/* ------------------------------------------------------------------ */

int anx_anxml_generate(const struct anx_anxml_request *req,
		       struct anx_anxml_response *resp)
{
	uint32_t      max_tokens;
	unsigned char prev;
	uint32_t      out_len = 0;
	uint32_t      tokens  = 0;
	uint32_t      i;
	uint32_t      rng_state;
	uint32_t     *rng_p;
	bool          had_printable = false;

	if (!req || !resp)
		return ANX_EINVAL;
	if (!g_inited) anx_anxml_init();

	anx_memset(resp, 0, sizeof(*resp));
	max_tokens = req->max_tokens ? req->max_tokens : ANX_ANXML_DEFAULT_MAX;
	if (max_tokens > ANX_ANXML_OUTPUT_MAX - 1)
		max_tokens = ANX_ANXML_OUTPUT_MAX - 1;

	/* Per-call boost from prompt; restored at end. */
	seed_prompt(req->prompt, req->prompt_len);

	/* Seed previous-byte from the tail of the prompt (or space). */
	prev = ' ';
	if (req->prompt_len > 0) {
		i = req->prompt_len - 1;
		prev = (unsigned char)req->prompt[i] & 0x7f;
	}

	rng_state = req->seed;
	rng_p = req->seed ? &rng_state : NULL;

	for (i = 0; i < max_tokens; i++) {
		int c = sample_next(prev, rng_p);
		if (c < 0) break;

		/* Stop conditions: newline after at least one printable */
		if (c == '\n' && had_printable) {
			resp->output[out_len++] = '\n';
			tokens++;
			break;
		}
		if (c < 0x20 && c != '\n' && c != '\t')
			c = ' ';
		if (out_len >= ANX_ANXML_OUTPUT_MAX - 1)
			break;
		resp->output[out_len++] = (char)c;
		tokens++;
		if (c > ' ') had_printable = true;
		prev = (unsigned char)c;
	}
	resp->output[out_len] = '\0';
	resp->output_len      = out_len;
	resp->tokens_generated = tokens;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Cell dispatch                                                       */
/* ------------------------------------------------------------------ */

int anx_anxml_cell_dispatch(const char *intent,
			    const anx_oid_t *in_oids, uint32_t in_count,
			    anx_oid_t *out_oid_out)
{
	struct anx_state_object    *prompt_obj;
	struct anx_anxml_request    req;
	struct anx_anxml_response  *resp;
	struct anx_so_create_params cp;
	struct anx_state_object    *out_obj;
	uint32_t                    plen;
	int                         rc;

	if (!intent || !out_oid_out)
		return ANX_EINVAL;
	anx_memset(out_oid_out, 0, sizeof(*out_oid_out));

	if (anx_strcmp(intent, "anxml-generate") != 0)
		return ANX_ENOSYS;
	if (in_count < 1)
		return ANX_EINVAL;

	prompt_obj = anx_objstore_lookup(&in_oids[0]);
	if (!prompt_obj)
		return ANX_ENOENT;
	if (!prompt_obj->payload || prompt_obj->payload_size == 0) {
		anx_objstore_release(prompt_obj);
		return ANX_EINVAL;
	}

	anx_memset(&req, 0, sizeof(req));
	plen = (uint32_t)prompt_obj->payload_size;
	if (plen >= ANX_ANXML_PROMPT_MAX)
		plen = ANX_ANXML_PROMPT_MAX - 1;
	anx_memcpy(req.prompt, prompt_obj->payload, plen);
	req.prompt[plen] = '\0';
	req.prompt_len = plen;
	anx_strlcpy(req.model_name, "anx:model/default",
		    sizeof(req.model_name));
	anx_objstore_release(prompt_obj);

	resp = (struct anx_anxml_response *)anx_zalloc(sizeof(*resp));
	if (!resp)
		return ANX_ENOMEM;

	rc = anx_anxml_generate(&req, resp);
	if (rc != ANX_OK) {
		anx_free(resp);
		return rc;
	}

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_MODEL_OUTPUT;
	cp.schema_uri     = "anx:schema/anxml/response/v1";
	cp.schema_version = "1";
	cp.payload        = resp->output;
	cp.payload_size   = resp->output_len;
	rc = anx_so_create(&cp, &out_obj);
	if (rc == ANX_OK) {
		*out_oid_out = out_obj->oid;
		anx_objstore_release(out_obj);
	}
	anx_free(resp);
	return rc;
}
