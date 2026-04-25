/*
 * js_lex.c — JavaScript lexer.
 */

#include "js_lex.h"
#include <anx/string.h>

/* ── Character helpers ─────────────────────────────────────────────── */

static inline bool is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static inline bool is_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
static inline bool is_hex(char c)
{
	return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline int hex_val(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return c - 'A' + 10;
}

/* ── Keyword table ─────────────────────────────────────────────────── */

struct kw_entry { const char *word; js_tok_kind kind; };

static const struct kw_entry s_keywords[] = {
	{"break",      TK_BREAK},
	{"case",       TK_CASE},
	{"catch",      TK_CATCH},
	{"class",      TK_CLASS},
	{"const",      TK_CONST},
	{"continue",   TK_CONTINUE},
	{"debugger",   TK_DEBUGGER},
	{"default",    TK_DEFAULT},
	{"delete",     TK_DELETE},
	{"do",         TK_DO},
	{"else",       TK_ELSE},
	{"export",     TK_EXPORT},
	{"extends",    TK_EXTENDS},
	{"false",      TK_FALSE},
	{"finally",    TK_FINALLY},
	{"for",        TK_FOR},
	{"function",   TK_FUNCTION},
	{"if",         TK_IF},
	{"import",     TK_IMPORT},
	{"in",         TK_IN},
	{"instanceof", TK_INSTANCEOF},
	{"let",        TK_LET},
	{"new",        TK_NEW},
	{"null",       TK_NULL},
	{"of",         TK_OF},
	{"return",     TK_RETURN},
	{"static",     TK_STATIC},
	{"super",      TK_SUPER},
	{"switch",     TK_SWITCH},
	{"this",       TK_THIS},
	{"throw",      TK_THROW},
	{"true",       TK_TRUE},
	{"try",        TK_TRY},
	{"typeof",     TK_TYPEOF},
	{"undefined",  TK_UNDEFINED},
	{"var",        TK_VAR},
	{"void",       TK_VOID},
	{"while",      TK_WHILE},
	{"with",       TK_WITH},
	{"yield",      TK_YIELD},
	{NULL, TK_EOF}
};

static js_tok_kind lookup_keyword(const char *s, uint32_t len)
{
	uint32_t i;
	for (i = 0; s_keywords[i].word; i++) {
		const char *w = s_keywords[i].word;
		uint32_t wl = 0;
		while (w[wl]) wl++;
		if (wl != len) continue;
		if (anx_strncmp(w, s, len) == 0)
			return s_keywords[i].kind;
	}
	return TK_IDENT;
}

/* ── Number parsing ────────────────────────────────────────────────── */

static double parse_number(const char *s, uint32_t len)
{
	double v = 0.0;
	uint32_t i = 0;

	/* Hex */
	if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		for (i = 2; i < len; i++)
			v = v * 16.0 + hex_val(s[i]);
		return v;
	}
	/* Octal 0o / binary 0b */
	if (len > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
		for (i = 2; i < len; i++)
			v = v * 8.0 + (s[i] - '0');
		return v;
	}
	if (len > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
		for (i = 2; i < len; i++)
			v = v * 2.0 + (s[i] - '0');
		return v;
	}
	/* Decimal integer part */
	while (i < len && is_digit(s[i]))
		v = v * 10.0 + (s[i++] - '0');
	/* Fraction */
	if (i < len && s[i] == '.') {
		double frac = 0.1;
		i++;
		while (i < len && is_digit(s[i])) {
			v += (s[i++] - '0') * frac;
			frac *= 0.1;
		}
	}
	/* Exponent */
	if (i < len && (s[i] == 'e' || s[i] == 'E')) {
		i++;
		int sign = 1;
		if (i < len && s[i] == '+') i++;
		else if (i < len && s[i] == '-') { sign = -1; i++; }
		double exp = 0.0;
		while (i < len && is_digit(s[i]))
			exp = exp * 10.0 + (s[i++] - '0');
		/* pow without libm — use iterative multiply */
		double base = (sign > 0) ? 10.0 : 0.1;
		uint32_t e = (uint32_t)(exp < 0 ? -exp : exp);
		double mult = 1.0;
		while (e--) mult *= base;
		v *= mult;
	}
	return v;
}

/* ── String literal parsing ────────────────────────────────────────── */

/* Returns the start of the unescaped content slice and its length.
 * For simplicity we return the raw slice including escape sequences —
 * the parser/runtime resolves them.  The returned pointers are into src. */
static void scan_string(struct js_lexer *lex, char quote,
                        const char **out, uint32_t *out_len)
{
	uint32_t start = lex->pos;  /* after opening quote */
	while (lex->pos < lex->len) {
		char c = lex->src[lex->pos];
		if (c == '\\') { lex->pos += 2; continue; }
		if (c == quote) break;
		if (c == '\n') lex->line++;
		lex->pos++;
	}
	*out     = lex->src + start;
	*out_len = lex->pos - start;
	if (lex->pos < lex->len) lex->pos++; /* consume closing quote */
}

/* Template literal — treated as a plain string for now */
static void scan_template(struct js_lexer *lex,
                          const char **out, uint32_t *out_len)
{
	scan_string(lex, '`', out, out_len);
}

/* ── Main scan ─────────────────────────────────────────────────────── */

void js_lex_init(struct js_lexer *lex, const char *src, uint32_t len)
{
	lex->src    = src;
	lex->pos    = 0;
	lex->len    = len;
	lex->line   = 1;
	lex->peeked = false;
}

static struct js_token make_tok(js_tok_kind kind, uint32_t line)
{
	struct js_token t;
	t.kind    = kind;
	t.line    = line;
	t.str     = NULL;
	t.str_len = 0;
	t.num     = 0.0;
	return t;
}

struct js_token js_lex_next(struct js_lexer *lex)
{
	if (lex->peeked) {
		lex->peeked = false;
		return lex->cur;
	}

restart:
	/* Skip whitespace */
	while (lex->pos < lex->len && is_ws(lex->src[lex->pos])) {
		if (lex->src[lex->pos] == '\n') lex->line++;
		lex->pos++;
	}
	if (lex->pos >= lex->len)
		return make_tok(TK_EOF, lex->line);

	uint32_t line = lex->line;
	char c = lex->src[lex->pos++];
	struct js_token t = make_tok(TK_EOF, line);

	/* Comments */
	if (c == '/' && lex->pos < lex->len && lex->src[lex->pos] == '/') {
		lex->pos++;
		while (lex->pos < lex->len && lex->src[lex->pos] != '\n')
			lex->pos++;
		goto restart;
	}
	if (c == '/' && lex->pos < lex->len && lex->src[lex->pos] == '*') {
		lex->pos++;
		while (lex->pos + 1 < lex->len) {
			if (lex->src[lex->pos] == '\n') lex->line++;
			if (lex->src[lex->pos] == '*' && lex->src[lex->pos+1] == '/') {
				lex->pos += 2;
				break;
			}
			lex->pos++;
		}
		goto restart;
	}

	/* Identifiers / keywords */
	if (is_alpha(c) || (uint8_t)c > 127) {
		uint32_t start = lex->pos - 1;
		while (lex->pos < lex->len &&
		       (is_alpha(lex->src[lex->pos]) || is_digit(lex->src[lex->pos]) ||
		        (uint8_t)lex->src[lex->pos] > 127))
			lex->pos++;
		uint32_t len = lex->pos - start;
		t.str     = lex->src + start;
		t.str_len = len;
		t.kind    = lookup_keyword(t.str, len);
		return t;
	}

	/* Numbers */
	if (is_digit(c) || (c == '.' && lex->pos < lex->len && is_digit(lex->src[lex->pos]))) {
		uint32_t start = lex->pos - 1;
		while (lex->pos < lex->len &&
		       (is_digit(lex->src[lex->pos]) || lex->src[lex->pos] == '.' ||
		        lex->src[lex->pos] == 'e' || lex->src[lex->pos] == 'E' ||
		        lex->src[lex->pos] == 'x' || lex->src[lex->pos] == 'X' ||
		        lex->src[lex->pos] == 'o' || lex->src[lex->pos] == 'O' ||
		        lex->src[lex->pos] == 'b' || lex->src[lex->pos] == 'B' ||
		        is_hex(lex->src[lex->pos]) ||
		        ((lex->src[lex->pos] == '+' || lex->src[lex->pos] == '-') &&
		         lex->pos > 0 &&
		         (lex->src[lex->pos-1] == 'e' || lex->src[lex->pos-1] == 'E'))))
			lex->pos++;
		uint32_t nlen = lex->pos - start;
		t.kind = TK_NUMBER;
		t.num  = parse_number(lex->src + start, nlen);
		return t;
	}

	/* String literals */
	if (c == '"' || c == '\'') {
		t.kind = TK_STRING;
		scan_string(lex, c, &t.str, &t.str_len);
		return t;
	}
	if (c == '`') {
		t.kind = TK_STRING;
		scan_template(lex, &t.str, &t.str_len);
		return t;
	}

	/* Punctuation & operators */
#define PEEK (lex->pos < lex->len ? lex->src[lex->pos] : '\0')
#define EAT  (lex->pos++)

	switch (c) {
	case '{': t.kind = TK_LBRACE; break;
	case '}': t.kind = TK_RBRACE; break;
	case '(': t.kind = TK_LPAREN; break;
	case ')': t.kind = TK_RPAREN; break;
	case '[': t.kind = TK_LBRACKET; break;
	case ']': t.kind = TK_RBRACKET; break;
	case ';': t.kind = TK_SEMICOLON; break;
	case ',': t.kind = TK_COMMA; break;
	case '?': t.kind = TK_QUESTION; break;
	case ':': t.kind = TK_COLON; break;
	case '~': t.kind = TK_TILDE; break;
	case '.':
		if (PEEK == '.' && lex->pos+1 < lex->len && lex->src[lex->pos+1] == '.') {
			lex->pos += 2; t.kind = TK_DOTDOTDOT;
		} else {
			t.kind = TK_DOT;
		}
		break;
	case '=':
		if (PEEK == '>') { EAT; t.kind = TK_ARROW; }
		else if (PEEK == '=') { EAT;
			t.kind = (PEEK == '=') ? (EAT, TK_STRICT_EQ) : TK_EQ;
		} else { t.kind = TK_ASSIGN; }
		break;
	case '!':
		if (PEEK == '=') { EAT;
			t.kind = (PEEK == '=') ? (EAT, TK_STRICT_NEQ) : TK_NEQ;
		} else { t.kind = TK_BANG; }
		break;
	case '<':
		if (PEEK == '<') { EAT;
			t.kind = (PEEK == '=') ? (EAT, TK_LSHIFT_ASSIGN) : TK_LSHIFT;
		} else if (PEEK == '=') { EAT; t.kind = TK_LE; }
		else { t.kind = TK_LT; }
		break;
	case '>':
		if (PEEK == '>') { EAT;
			if (PEEK == '>') { EAT; t.kind = TK_URSHIFT; }
			else if (PEEK == '=') { EAT; t.kind = TK_RSHIFT_ASSIGN; }
			else { t.kind = TK_RSHIFT; }
		} else if (PEEK == '=') { EAT; t.kind = TK_GE; }
		else { t.kind = TK_GT; }
		break;
	case '+':
		if (PEEK == '+') { EAT; t.kind = TK_PLUSPLUS; }
		else if (PEEK == '=') { EAT; t.kind = TK_PLUS_ASSIGN; }
		else { t.kind = TK_PLUS; }
		break;
	case '-':
		if (PEEK == '-') { EAT; t.kind = TK_MINUSMINUS; }
		else if (PEEK == '=') { EAT; t.kind = TK_MINUS_ASSIGN; }
		else { t.kind = TK_MINUS; }
		break;
	case '*':
		if (PEEK == '*') { EAT; t.kind = TK_STARSTAR; }
		else if (PEEK == '=') { EAT; t.kind = TK_STAR_ASSIGN; }
		else { t.kind = TK_STAR; }
		break;
	case '/':
		if (PEEK == '=') { EAT; t.kind = TK_SLASH_ASSIGN; }
		else { t.kind = TK_SLASH; }
		break;
	case '%':
		if (PEEK == '=') { EAT; t.kind = TK_PERCENT_ASSIGN; }
		else { t.kind = TK_PERCENT; }
		break;
	case '&':
		if (PEEK == '&') { EAT; t.kind = TK_AMPAMP; }
		else if (PEEK == '=') { EAT; t.kind = TK_AMP_ASSIGN; }
		else { t.kind = TK_AMP; }
		break;
	case '|':
		if (PEEK == '|') { EAT; t.kind = TK_PIPEPIPE; }
		else if (PEEK == '=') { EAT; t.kind = TK_PIPE_ASSIGN; }
		else { t.kind = TK_PIPE; }
		break;
	case '^':
		if (PEEK == '=') { EAT; t.kind = TK_CARET_ASSIGN; }
		else { t.kind = TK_CARET; }
		break;
	default:
		t.kind = TK_ERROR;
		break;
	}
#undef PEEK
#undef EAT
	return t;
}

struct js_token js_lex_peek(struct js_lexer *lex)
{
	if (!lex->peeked) {
		lex->cur    = js_lex_next(lex);
		lex->peeked = true;
	}
	return lex->cur;
}

void js_lex_consume(struct js_lexer *lex)
{
	lex->peeked = false;
}

bool js_lex_can_regex(js_tok_kind prev)
{
	switch (prev) {
	case TK_RPAREN:
	case TK_RBRACKET:
	case TK_IDENT:
	case TK_NUMBER:
	case TK_STRING:
	case TK_PLUSPLUS:
	case TK_MINUSMINUS:
		return false;
	default:
		return true;
	}
}
