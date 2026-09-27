/* lex.c - VXA lexer: source text -> flat token array for the parser (SPEC 1.1).
 * Global rules: a line break is a statement token that goes quiet inside [ and (,
 * and any form outside the grammar stops with an error instead of a guess
 * (SPEC axiom 1: verifiable beats tolerant). */
#include "ast.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *s; TokKind k; } kwtab[] = {
  { "def", T_KW_DEF }, { "if", T_KW_IF }, { "elif", T_KW_ELIF },
  { "else", T_KW_ELSE }, { "while", T_KW_WHILE }, { "for", T_KW_FOR },
  { "in", T_KW_IN }, { "return", T_KW_RETURN }, { "break", T_KW_BREAK },
  { "continue", T_KW_CONTINUE }, { "true", T_KW_TRUE },
  { "false", T_KW_FALSE }, { "null", T_KW_NULL }, { "and", T_KW_AND },
  { "or", T_KW_OR }, { "not", T_KW_NOT }
};

const char *tok_name(TokKind k) {
  switch (k) {
    case T_EOF: return "EOF";
    case T_NL: return "NL";
    case T_NUM: return "NUM";
    case T_STR: return "STR";
    case T_SQSTR: return "SQSTR";
    case T_TICKS: return "TICKS";
    case T_ID: return "ID";
    case T_KW_DEF: return "KW_DEF";
    case T_KW_IF: return "KW_IF";
    case T_KW_ELIF: return "KW_ELIF";
    case T_KW_ELSE: return "KW_ELSE";
    case T_KW_WHILE: return "KW_WHILE";
    case T_KW_FOR: return "KW_FOR";
    case T_KW_IN: return "KW_IN";
    case T_KW_RETURN: return "KW_RETURN";
    case T_KW_BREAK: return "KW_BREAK";
    case T_KW_CONTINUE: return "KW_CONTINUE";
    case T_KW_TRUE: return "KW_TRUE";
    case T_KW_FALSE: return "KW_FALSE";
    case T_KW_NULL: return "KW_NULL";
    case T_KW_AND: return "KW_AND";
    case T_KW_OR: return "KW_OR";
    case T_KW_NOT: return "KW_NOT";
    case T_LPAREN: return "LPAREN";
    case T_RPAREN: return "RPAREN";
    case T_LBRACK: return "LBRACK";
    case T_RBRACK: return "RBRACK";
    case T_LBRACE: return "LBRACE";
    case T_RBRACE: return "RBRACE";
    case T_COMMA: return "COMMA";
    case T_SEMI: return "SEMI";
    case T_DOT: return "DOT";
    case T_EQ: return "EQ";
    case T_ASSIGN: return "ASSIGN";
    case T_ADD: return "ADD";
    case T_SUB: return "SUB";
    case T_MUL: return "MUL";
    case T_DIV: return "DIV";
    case T_MOD: return "MOD";
    case T_LT: return "LT";
    case T_LE: return "LE";
    case T_GT: return "GT";
    case T_GE: return "GE";
    case T_EEQ: return "EQ";
    case T_NE: return "NE";
    case T_ARROW: return "ARROW";
    case T_PIPE: return "PIPE";
    case T_SIM: return "SIM";
    case T_QUEST: return "QUEST";
    case T_COLON: return "COLON";
    case T_DOTS: return "DOTS";
    case T_INVALID: return "INVALID";
  }
  return "?";
}

/* ---------- cursor ---------- */
static int at(Lexer *lx, size_t off) { /* byte at pos+off, -1 past end */
  return lx->pos + off < lx->len ? (int)(unsigned char)lx->src[lx->pos + off] : -1;
}
static void adv(Lexer *lx) {
  if (lx->pos >= lx->len) return;
  char c = lx->src[lx->pos++];
  if (c == '\n') { lx->line++; lx->col = 1; }
  else if (c == '\r') { if (at(lx, 0) != '\n') { lx->line++; } lx->col = 1; }
  else lx->col++;
}
static void eat_break(Lexer *lx) {
  char c = lx->src[lx->pos];
  adv(lx);
  if (c == '\r' && at(lx, 0) == '\n') adv(lx);
}
static int idcont(int c) { return c >= 0 && (c == '_' || isalnum(c) != 0); }
static int idstart(int c) { return c >= 0 && (c == '_' || isalpha(c) != 0); }
static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* ---------- output ---------- */
static Tok *push(Lexer *lx, TokKind k, int line, int col, int span) {
  if (lx->n == lx->cap) {
    int cap = lx->cap ? lx->cap * 2 : 64;
    Tok *t = (Tok*)arena_alloc(lx->a, (size_t)cap * sizeof(Tok));
    if (lx->n) memcpy(t, lx->toks, (size_t)lx->n * sizeof(Tok));
    lx->toks = t; lx->cap = cap;
  }
  Tok *t = &lx->toks[lx->n++];
  memset(t, 0, sizeof *t);
  t->k = k; t->line = line; t->col = col; t->len = span;
  return t;
}

static void lerr(Lexer *lx, int line, int col, const char *fmt, ...) {
  if (lx->err) return; /* first error is the one the agent must fix */
  char body[192], msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  snprintf(msg, sizeof msg, "lex:%d:%d: %s", line, col, body);
  lx->err = arena_strdup(lx->a, msg); /* never the stack buffer */
  lx->err_line = line;
  lx->err_col = col;
}

static void unexpected(Lexer *lx) {
  int line = lx->line, col = lx->col, w = 1;
  unsigned char c = (unsigned char)lx->src[lx->pos];
  if (c >= 0xF0) w = 4; else if (c >= 0xE0) w = 3; else if (c >= 0xC0) w = 2;
  if ((size_t)lx->pos + (size_t)w > lx->len) w = 1;
  /* print the whole code point, not one byte of it */
  lerr(lx, line, col, "unexpected character '%.*s'", w, lx->src + lx->pos);
}

/* ---------- counted piece list for interpolated strings ---------- */
typedef struct { Str *v; int n, cap; } StrVec;

static void sv_push(Arena *a, StrVec *v, Str s) {
  if (v->n == v->cap) {
    int cap = v->cap ? v->cap * 2 : 8;
    Str *p = (Str*)arena_alloc(a, (size_t)cap * sizeof(Str));
    if (v->n) memcpy(p, v->v, (size_t)v->n * sizeof(Str));
    v->v = p; v->cap = cap;
  }
  v->v[v->n++] = s;
}

/* ---------- "..." ---------- */
static void escape_into(Lexer *lx, Buf *b) {
  int line = lx->line, col = lx->col; /* report at the backslash */
  adv(lx);
  int c = at(lx, 0);
  if (c < 0 || c == '\n' || c == '\r') { lerr(lx, line, col, "unterminated string"); return; }
  adv(lx);
  switch (c) {
    case 'n': buf_putc(b, '\n'); return;
    case 't': buf_putc(b, '\t'); return;
    case 'r': buf_putc(b, '\r'); return;
    case '\\': buf_putc(b, '\\'); return;
    case '"': buf_putc(b, '"'); return;
    case '{': buf_putc(b, '{'); return;
    case '}': buf_putc(b, '}'); return;
    case 'x': {
      int h1 = hexval(at(lx, 0)), h2 = hexval(at(lx, 1));
      if (h1 < 0 || h2 < 0) { lerr(lx, line, col, "bad \\x escape"); return; }
      adv(lx); adv(lx);
      buf_putc(b, (char)((h1 << 4) | h2));
      return;
    }
    default: lerr(lx, line, col, "unknown escape '\\%c'", c); return;
  }
}

static int skip_quoted(Lexer *lx, int q) {
  adv(lx);
  while (lx->pos < lx->len) {
    int c = at(lx, 0);
    if (c == '\n' || c == '\r') return 0;
    adv(lx);
    if (c == '\\') { if (at(lx, 0) >= 0) adv(lx); }
    else if (c == q) return 1;
  }
  return 0;
}

/* "{...}" content is handed to the parser as raw source: the lexer does not
 * resolve it here, so nested braces and nested strings must be skipped, not parsed. */
static void interp_expr(Lexer *lx, StrVec *pv) {
  int line = lx->line, col = lx->col, depth = 1;
  adv(lx); /* '{' */
  size_t start = lx->pos;
  for (;;) {
    int c = at(lx, 0);
    if (c < 0 || c == '\n' || c == '\r') { lerr(lx, line, col, "unterminated interpolation"); return; }
    if (c == '{') depth++;
    else if (c == '}') { if (--depth == 0) break; }
    else if (c == '"' || c == '\'') { if (!skip_quoted(lx, c)) { lerr(lx, line, col, "unterminated interpolation"); return; } continue; }
    adv(lx);
  }
  sv_push(lx->a, pv, s_from(lx->a, lx->src + start, lx->pos - start));
  adv(lx); /* '}' */
}

static Str chunk_flush(Lexer *lx, Buf *lit, Buf *all, StrVec *pv) {
  Str s = buf_take(lit);
  buf_put(all, s.p, (size_t)s.len);
  buf_clear(lit);
  sv_push(lx->a, pv, s);
  return s;
}

static void str_interp(Lexer *lx) {
  int line = lx->line, col = lx->col, closed = 0;
  size_t start = lx->pos;
  adv(lx); /* opening quote */
  Buf lit, all;
  buf_init(&lit, lx->a); buf_init(&all, lx->a);
  StrVec pv; pv.v = NULL; pv.n = 0; pv.cap = 0;
  while (!lx->err) {
    int c = at(lx, 0);
    if (c < 0 || c == '\n' || c == '\r') { lerr(lx, line, col, "unterminated string"); break; }
    if (c == '"') { adv(lx); closed = 1; break; }
    if (c == '{') { chunk_flush(lx, &lit, &all, &pv); interp_expr(lx, &pv); continue; }
    if (c == '\\') { escape_into(lx, &lit); continue; }
    buf_putc(&lit, (char)c);
    adv(lx);
  }
  if (!closed) return;
  Str last = chunk_flush(lx, &lit, &all, &pv);
  Tok *t = push(lx, T_STR, line, col, (int)(lx->pos - start));
  t->s = pv.n > 1 ? buf_take(&all) : last;
  if (pv.n > 1) { t->parts = pv.v; t->nparts = pv.n; }
}

/* '...' raw: only \' and \\ mean anything */
static void str_raw(Lexer *lx) {
  int line = lx->line, col = lx->col, closed = 0;
  size_t start = lx->pos;
  adv(lx);
  Buf b; buf_init(&b, lx->a);
  while (!lx->err) {
    int c = at(lx, 0);
    if (c < 0 || c == '\n' || c == '\r') { lerr(lx, line, col, "unterminated string"); break; }
    if (c == '\'') { adv(lx); closed = 1; break; }
    if (c == '\\') {
      int d = at(lx, 1);
      if (d == '\'' || d == '\\') { buf_putc(&b, (char)d); adv(lx); adv(lx); continue; }
      buf_putc(&b, '\\'); adv(lx); continue;
    }
    buf_putc(&b, (char)c);
    adv(lx);
  }
  if (!closed) return;
  Tok *t = push(lx, T_SQSTR, line, col, (int)(lx->pos - start));
  t->s = buf_take(&b);
}

/* ```...``` multi-line raw; CRLF inside is normalized so a token never depends
 * on how the file happened to be checked out. */
static void str_ticks(Lexer *lx) {
  int line = lx->line, col = lx->col, closed = 0;
  size_t start = lx->pos;
  adv(lx); adv(lx); adv(lx);
  Buf b; buf_init(&b, lx->a);
  while (!lx->err) {
    if (at(lx, 0) < 0) break;
    if (at(lx, 0) == '`' && at(lx, 1) == '`' && at(lx, 2) == '`') {
      adv(lx); adv(lx); adv(lx); closed = 1; break;
    }
    if (at(lx, 0) == '\r') { eat_break(lx); buf_putc(&b, '\n'); continue; }
    buf_putc(&b, (char)at(lx, 0));
    adv(lx);
  }
  if (!closed) { lerr(lx, line, col, "unterminated raw string"); return; }
  Tok *t = push(lx, T_TICKS, line, col, (int)(lx->pos - start));
  t->s = buf_take(&b);
}

/* ---------- numbers ---------- */
static void lex_num(Lexer *lx) {
  int line = lx->line, col = lx->col;
  size_t start = lx->pos;
  double v = 0;
  if (at(lx, 0) == '0' && (at(lx, 1) == 'x' || at(lx, 1) == 'X')) {
    adv(lx); adv(lx);
    size_t hs = lx->pos;
    unsigned long long acc = 0;
    while (hexval(at(lx, 0)) >= 0) { acc = (acc << 4) | (unsigned long long)hexval(at(lx, 0)); adv(lx); }
    if (lx->pos == hs) { lerr(lx, line, col, "malformed number: hex digit after 0x"); return; }
    v = (double)acc;
  } else {
    while (isdigit(at(lx, 0))) adv(lx);
    if (at(lx, 0) == '.' && isdigit(at(lx, 1))) {
      adv(lx);
      while (isdigit(at(lx, 0))) adv(lx);
    }
    if (at(lx, 0) == '.' && isdigit(at(lx, 1))) { lerr(lx, line, col, "malformed number"); return; }
    if (at(lx, 0) == 'e' || at(lx, 0) == 'E') {
      adv(lx);
      if (at(lx, 0) == '+' || at(lx, 0) == '-') adv(lx);
      if (!isdigit(at(lx, 0))) { lerr(lx, line, col, "malformed number: exponent"); return; }
      while (isdigit(at(lx, 0))) adv(lx);
    }
    char *txt = arena_strndup(lx->a, lx->src + start, lx->pos - start);
    v = strtod(txt, NULL);
  }
  if (idcont(at(lx, 0))) { lerr(lx, line, col, "malformed number"); return; }
  Tok *t = push(lx, T_NUM, line, col, (int)(lx->pos - start));
  t->num = v;
}

/* ---------- identifiers and keywords ---------- */
static void lex_ident(Lexer *lx) {
  int line = lx->line, col = lx->col;
  size_t start = lx->pos;
  adv(lx);
  while (idcont(at(lx, 0))) adv(lx);
  int n = (int)(lx->pos - start);
  TokKind k = T_ID;
  for (int i = 0; i < (int)(sizeof kwtab / sizeof *kwtab); i++) {
    const char *w = kwtab[i].s;
    if ((int)strlen(w) == n && !memcmp(lx->src + start, w, (size_t)n)) { k = kwtab[i].k; break; }
  }
  push(lx, k, line, col, n)->s = s_from(lx->a, lx->src + start, (size_t)n);
}

/* ---------- operators ---------- */
/* depth counts [ ] ( ) only. A brace opens either a record or a block, and the
 * parser needs the line breaks inside blocks to separate statements, so braces
 * must not silence them; records already tolerate stray NLs (parse.c skipnl). */
static void lex_ops(Lexer *lx, int *depth) {
  int c = at(lx, 0), d = at(lx, 1);
  int line = lx->line, col = lx->col, span = 1;
  TokKind k;
  switch (c) {
    case '(': k = T_LPAREN; (*depth)++; break;
    case '[': k = T_LBRACK; (*depth)++; break;
    case ')': k = T_RPAREN; if (*depth > 0) (*depth)--; break;
    case ']': k = T_RBRACK; if (*depth > 0) (*depth)--; break;
    case '{': k = T_LBRACE; break;
    case '}': k = T_RBRACE; break;
    case ',': k = T_COMMA; break;
    case ';': k = T_SEMI; break;
    case '+': k = T_ADD; break;
    case '-': if (d == '>') { k = T_ARROW; span = 2; } else k = T_SUB; break;
    case '*': k = T_MUL; break;
    case '/': k = T_DIV; break;
    case '%': k = T_MOD; break;
    case '|': k = T_PIPE; break;
    case '?': k = T_QUEST; break;
    case ':': k = T_COLON; break;
    case '.': if (d == '.') { k = T_DOTS; span = 2; } else k = T_DOT; break;
    case '=': if (d == '=') { k = T_EEQ; span = 2; } else if (d == '>') { k = T_ARROW; span = 2; } else k = T_ASSIGN; break;
    case '<': if (d == '=') { k = T_LE; span = 2; } else k = T_LT; break;
    case '>': if (d == '=') { k = T_GE; span = 2; } else k = T_GT; break;
    case '!': if (d == '=') { k = T_NE; span = 2; } else { k = T_KW_NOT; span = 1; } break;
    case '~': if (d == '=') { k = T_SIM; span = 2; } else { unexpected(lx); return; } break;
    default: unexpected(lx); return;
  }
  for (int i = 0; i < span; i++) adv(lx);
  push(lx, k, line, col, span);
}

/* ---------- driver ---------- */
static void skip_shebang(Lexer *lx) {
  if (lx->len < 2 || lx->src[0] != '#' || lx->src[1] != '!') return;
  while (lx->pos < lx->len && lx->src[lx->pos] != '\n' && lx->src[lx->pos] != '\r') adv(lx);
  if (lx->pos < lx->len) eat_break(lx); /* the #! line leaves no NL behind */
}

void lex(Arena *a, const char *src, size_t len, Lexer *lx) {
  int depth = 0;
  lx->src = src; lx->len = len; lx->a = a;
  lx->pos = 0; lx->line = 1; lx->col = 1;
  lx->toks = NULL; lx->n = 0; lx->cap = 0;
  lx->err = NULL; lx->err_line = 0; lx->err_col = 0;
  skip_shebang(lx);
  while (!lx->err && lx->pos < lx->len) {
    int c = at(lx, 0);
    if (c == '\n' || c == '\r') {
      int line = lx->line, col = lx->col;
      size_t start = lx->pos;
      eat_break(lx);
      if (depth == 0) push(lx, T_NL, line, col, (int)(lx->pos - start));
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\f' || c == '\v') { adv(lx); continue; }
    if (c == '#') {
      while (lx->pos < lx->len && lx->src[lx->pos] != '\n' && lx->src[lx->pos] != '\r') adv(lx);
      continue;
    }
    if (c == '"') { str_interp(lx); continue; }
    if (c == '\'') { str_raw(lx); continue; }
    if (c == '`') {
      if (at(lx, 1) == '`' && at(lx, 2) == '`') str_ticks(lx); else unexpected(lx);
      continue;
    }
    if (isdigit(c)) { lex_num(lx); continue; }
    if (idstart(c)) { lex_ident(lx); continue; }
    lex_ops(lx, &depth);
  }
  push(lx, T_EOF, lx->line, lx->col, 0); /* array is always terminated, error or not */
}

/* ast.h declares both spellings; keep them the same function so either caller links. */
void lex_run(Arena *a, const char *src, size_t len, Lexer *lx) { lex(a, src, len, lx); }
