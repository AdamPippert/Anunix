/*
 * js_lex.h — JavaScript lexer (tokenizer).
 *
 * Single-pass, no dynamic allocation.  The lexer holds a pointer into the
 * source text and advances token-by-token.  Identifiers and string literals
 * are returned as (src, len) slices into the original buffer — no copies.
 *
 * Numeric literals are parsed eagerly to double.
 */

#ifndef ANX_JS_LEX_H
#define ANX_JS_LEX_H

#include <anx/types.h>

/* ── Token kinds ──────────────────────────────────────────────────── */

typedef enum {
	TK_EOF = 0,

	/* Literals */
	TK_NUMBER,      /* numeric literal — value in tok.num */
	TK_STRING,      /* string literal  — slice in tok.str/str_len */
	TK_IDENT,       /* identifier      — slice in tok.str/str_len */
	TK_REGEX,       /* /pattern/flags  — slice in tok.str/str_len */

	/* Keywords — must follow TK_IDENT in order (keyword check uses range) */
	TK_BREAK, TK_CASE, TK_CATCH, TK_CLASS, TK_CONST,
	TK_CONTINUE, TK_DEBUGGER, TK_DEFAULT, TK_DELETE,
	TK_DO, TK_ELSE, TK_EXPORT, TK_EXTENDS,
	TK_FALSE, TK_FINALLY, TK_FOR, TK_FUNCTION,
	TK_IF, TK_IMPORT, TK_IN, TK_INSTANCEOF,
	TK_LET, TK_NEW, TK_NULL, TK_OF,
	TK_RETURN, TK_STATIC, TK_SUPER, TK_SWITCH,
	TK_THIS, TK_THROW, TK_TRUE, TK_TRY,
	TK_TYPEOF, TK_UNDEFINED, TK_VAR, TK_VOID,
	TK_WHILE, TK_WITH, TK_YIELD,

	/* Punctuation */
	TK_LBRACE,      /* { */
	TK_RBRACE,      /* } */
	TK_LPAREN,      /* ( */
	TK_RPAREN,      /* ) */
	TK_LBRACKET,    /* [ */
	TK_RBRACKET,    /* ] */
	TK_SEMICOLON,   /* ; */
	TK_COMMA,       /* , */
	TK_DOT,         /* . */
	TK_DOTDOTDOT,   /* ... */
	TK_COLON,       /* : */
	TK_QUESTION,    /* ? */
	TK_ARROW,       /* => */

	/* Operators */
	TK_ASSIGN,      /* = */
	TK_PLUS,        /* + */
	TK_MINUS,       /* - */
	TK_STAR,        /* * */
	TK_SLASH,       /* / */
	TK_PERCENT,     /* % */
	TK_STARSTAR,    /* ** */
	TK_AMPAMP,      /* && */
	TK_PIPEPIPE,    /* || */
	TK_BANG,        /* ! */
	TK_TILDE,       /* ~ */
	TK_AMP,         /* & */
	TK_PIPE,        /* | */
	TK_CARET,       /* ^ */
	TK_LSHIFT,      /* << */
	TK_RSHIFT,      /* >> */
	TK_URSHIFT,     /* >>> */
	TK_PLUSPLUS,    /* ++ */
	TK_MINUSMINUS,  /* -- */

	/* Comparison */
	TK_EQ,          /* == */
	TK_NEQ,         /* != */
	TK_STRICT_EQ,   /* === */
	TK_STRICT_NEQ,  /* !== */
	TK_LT,          /* < */
	TK_GT,          /* > */
	TK_LE,          /* <= */
	TK_GE,          /* >= */

	/* Compound assignment */
	TK_PLUS_ASSIGN,    /* += */
	TK_MINUS_ASSIGN,   /* -= */
	TK_STAR_ASSIGN,    /* *= */
	TK_SLASH_ASSIGN,   /* /= */
	TK_PERCENT_ASSIGN, /* %= */
	TK_AMP_ASSIGN,     /* &= */
	TK_PIPE_ASSIGN,    /* |= */
	TK_CARET_ASSIGN,   /* ^= */
	TK_LSHIFT_ASSIGN,  /* <<= */
	TK_RSHIFT_ASSIGN,  /* >>= */

	TK_ERROR,       /* unrecognized character */
} js_tok_kind;

/* ── Token ────────────────────────────────────────────────────────── */

struct js_token {
	js_tok_kind  kind;
	uint32_t     line;    /* 1-based */
	const char  *str;     /* for TK_IDENT, TK_STRING, TK_REGEX */
	uint32_t     str_len; /* byte length of str slice */
	double       num;     /* for TK_NUMBER */
};

/* ── Lexer state ──────────────────────────────────────────────────── */

struct js_lexer {
	const char  *src;
	uint32_t     pos;
	uint32_t     len;
	uint32_t     line;
	struct js_token cur;   /* current (peeked) token */
	bool         peeked;
};

/* Initialize lexer over src[0..len-1].  Does not allocate. */
void js_lex_init(struct js_lexer *lex, const char *src, uint32_t len);

/* Advance and return next token.  On EOF returns TK_EOF repeatedly. */
struct js_token js_lex_next(struct js_lexer *lex);

/* Peek at next token without consuming it. */
struct js_token js_lex_peek(struct js_lexer *lex);

/* Consume peeked token; must have called js_lex_peek first. */
void js_lex_consume(struct js_lexer *lex);

/* True if current position can start a regex (context-dependent). */
bool js_lex_can_regex(js_tok_kind prev);

#endif /* ANX_JS_LEX_H */
