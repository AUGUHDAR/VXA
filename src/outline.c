/* outline.c - structure-only view of a source file + byte-budgeted bundles (SPEC §4.1).
 *
 * Why this module exists: reading a whole file to find one function is the single
 * biggest token cost an agent pays. outline_file() returns one line per symbol with
 * a content-hash anchor (SPEC §4.2) so the agent can go straight to tx.patch without
 * reading the file; bundle_files() packs several files' skeletons, plus grep context,
 * under a hard byte cap.
 *
 * Two axioms shape everything below.
 *  - axiom 1 (verifiable > lenient): every scanner walks a *masked* copy of the
 *    source, where comment bodies and string contents are turned into spaces, so a
 *    declaration can never be invented out of prose. Where a pattern is ambiguous
 *    the scanners emit NOTHING: a wrong symbol sends an agent off on a false
 *    premise, a missing one only costs it one more read.
 *  - axiom 4 (determinism): ordering is explicit and total (path is the last
 *    tie-break), sizes are plain integers, and nothing iterates a hash table or a
 *    directory - equal inputs give byte-identical output.
 *
 * Anchors: there is exactly one anchor rule in this program - xdiff.c's anchor_at(),
 * the one fs.read --anchors and tx.patch use. ol_anchor() calls it on the ORIGINAL
 * prev/cur/next lines of the symbol's own line, so an outline `at` and the `at`
 * printed for that same line by fs.read are the same bytes by construction. A local
 * re-derivation of the hash here was a second implementation of one rule, and the
 * two disagreed; that is why the private sha256 (and the hook that existed to swap
 * it) is gone instead of being kept in sync.
 *
 * Reads: bundle_files() receives paths only, so contents arrive through slurp_at, a
 * pointer to fsx.c's fs_slurp (declared in vxa.h). The indirection keeps this TU
 * linkable and lets tests inject file contents without fsx.c on the link line.
 */
#include "vxa.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define SIG_CAP    120    /* bytes of declaration text kept in Sym.sig            */
#define MAXSYM   4096     /* per-file symbol guard; sets note=symbols_limit       */
#define MINIFY  5000      /* one line this long => not worth outlining            */
#define JOINMAX     6     /* a logical declaration spans at most this many lines  */
#define JOINCAP   600     /* ... and at most this many bytes                       */
#define HIT_CTX     2     /* context lines around a query hit (SPEC fs.bundle)    */
#define TRUNC_TAG "##truncated="   /* marks a line-granular budget cut            */

/* ---------- cross-module indirections (see header note) ---------- */
static char *(*slurp_at)(Arena *a, const char *p, size_t *len_out, ErrCode *err) = fs_slurp;

/* ==========================================================================
 * 1. masked source model: one state machine, one tokenizer description per language
 * ========================================================================== */
typedef struct {
  const char *lc;          /* line comment start, NULL none                  */
  const char *bs, *be;     /* block comment, NULL none                       */
  unsigned char nest;      /* block comments nest (rust, kotlin)             */
  unsigned char esc;       /* backslash escapes inside quoted strings        */
  unsigned char strnl;     /* plain strings may span newlines                */
  unsigned char triple;    /* """ multi-line strings                         */
  unsigned char triple1;   /* ''' multi-line strings too (python)            */
  unsigned char strpre;    /* r/b/u/f string prefixes (python)               */
  unsigned char charlit;   /* 0: ' opens a string   1: char literal  2: rust */
  unsigned char backtick;  /* 0 none  1 go raw string  2 js template         */
  unsigned char rsraw;     /* r#"..."# (rust)                                */
} Tok;

typedef struct {
  Arena *a;
  Str    src;              /* caller's text; NOT assumed NUL-terminated      */
  char  *code;             /* same length, NUL-terminated, prose blanked     */
  int    n;                /* line count (0 for empty input)                 */
  int   *off, *len, *dep;  /* per line: offset, len without \r\n, brace depth */
  int    cpp;              /* c++ flavour of the c scanner                   */
} Doc;

typedef struct { Sym *v; int n, cap; int note; } SymList;
typedef struct { int base; char nm[96]; } Frame;

static int idch(unsigned char c) { return isalnum(c) || c=='_' || c=='$' || c>=0x80; }
static int spch(char c) { return c==' '||c=='\t'||c=='\v'||c=='\f'||c=='\r'; }
static int m_at(const char *s, int n, int i, const char *p) {
  size_t k = strlen(p);
  return (int)n - i >= (int)k && !memcmp(s + i, p, k);
}
static int till_eol(const char *s, int n, int i) { while (i < n && s[i] != '\n') i++; return i; }
static void blank_to(char *o, int i, int j) { for (; i < j; i++) if (o[i] != '\n' && o[i] != '\r') o[i] = ' '; }

static int mask_q(Doc *d, const Tok *T, int i, char term, int multi, int raw) {
  const char *s = d->src.p; int n = d->src.len; char *o = d->code;
  o[i] = ' '; i++;
  while (i < n) {
    char c = s[i];
    if (c == '\n') { if (!multi) break; i++; continue; }
    if (!raw && T->esc && c == '\\') { i += 2; continue; }
    if (c == term) { if (i < n) o[i] = ' '; i++; break; }
    o[i] = ' '; i++;
  }
  return i;
}
static int mask_triple(Doc *d, int i, char q) {
  const char *s = d->src.p; int n = d->src.len; char *o = d->code;
  for (int k = 0; k < 3 && i + k < n; k++) o[i + k] = ' ';
  i += 3;
  while (i + 2 < n) {
    if (s[i] == q && s[i+1] == q && s[i+2] == q) {
      o[i] = o[i+1] = o[i+2] = ' ';
      return i + 3;
    }
    if (s[i] == '\\') { o[i] = ' '; i++; if (i < n && s[i] != '\n') o[i] = ' '; i++; continue; }
    if (s[i] != '\n') o[i] = ' ';
    i++;
  }
  blank_to(o, i, n);
  return n;
}
static int mask_block(Doc *d, const Tok *T, int i) {
  const char *s = d->src.p; int n = d->src.len; char *o = d->code;
  size_t bl = strlen(T->bs), el = strlen(T->be);
  int depth = 1, j = i + (int)bl;
  while (j < n && depth > 0) {
    if (T->nest && m_at(s, n, j, T->bs)) { depth++; j += (int)bl; continue; }
    if (m_at(s, n, j, T->be)) { depth--; j += (int)el; continue; }
    if (s[j] != '\n' && s[j] != '\r') o[j] = ' ';
    j++;
  }
  blank_to(o, i, j < n ? j : n);
  return j;
}
/* rust: `'a` is a lifetime (stays code) while `'a'` / `'\n'` is a char literal */
static int rs_is_charlit(const char *s, int n, int i) {
  int j = i + 1;
  if (j >= n) return 0;
  if (s[j] == '\\') {
    j++;
    if (j >= n) return 0;
    if (s[j] == 'x') { j++; while (j < n && isxdigit((unsigned char)s[j])) j++; }
    else j++;
    return j < n && s[j] == '\'';
  }
  return j + 1 < n && s[j+1] == '\'';
}
static int mask_code(Doc *d, const Tok *T, int i, int lvl, int in_t);
static int mask_tmpl(Doc *d, const Tok *T, int i, int lvl) {
  const char *s = d->src.p; int n = d->src.len; char *o = d->code;
  o[i] = ' '; i++;
  while (i < n) {
    char c = s[i];
    if (c == '\\') { o[i] = ' '; i++; if (i < n && s[i] != '\n') o[i] = ' '; i++; continue; }
    if (c == '`') { o[i] = ' '; i++; return i; }
    if (c == '$' && m_at(s, n, i, "${")) {
      o[i] = ' '; o[i+1] = ' ';
      int j = mask_code(d, T, i + 2, lvl + 1, 1);   /* an interpolation is code again */
      if (j < n && s[j] == '}') { o[j] = ' '; j++; }
      i = j; continue;
    }
    if (c != '\n') o[i] = ' ';
    i++;
  }
  return n;
}
static int mask_code(Doc *d, const Tok *T, int i, int lvl, int in_t) {
  const char *s = d->src.p; int n = d->src.len; char *o = d->code;
  int bd = 0;
  while (i < n) {
    char c = s[i];
    if (c == '\n') { i++; continue; }
    if (T->lc && m_at(s, n, i, T->lc)) { int j = till_eol(s, n, i); blank_to(o, i, j); i = j; continue; }
    if (T->bs && m_at(s, n, i, T->bs)) { i = mask_block(d, T, i); continue; }
    if (T->triple && c == '"' && m_at(s, n, i, "\"\"\"")) { i = mask_triple(d, i, '"'); continue; }
    if (T->triple1 && c == '\'' && m_at(s, n, i, "'''")) { i = mask_triple(d, i, '\''); continue; }
    if (T->strpre && strchr("rRbBuUfF", c) && (i == 0 || !idch((unsigned char)s[i-1]))) {
      int k = i + 1, two = 0;
      if (k < n && !strchr("rR", s[k]) && strchr("bBfF", s[k])) { k++; two = 1; }
      if (k < n && (s[k] == '"' || s[k] == '\'')) {
        char q = s[k];
        blank_to(o, i, k);
        if (q == '"' && T->triple && m_at(s, n, k, "\"\"\"")) { i = mask_triple(d, k, '"'); continue; }
        if (q == '\'' && T->triple1 && m_at(s, n, k, "'''")) { i = mask_triple(d, k, '\''); continue; }
        i = mask_q(d, T, k, q, 0, two ? 1 : (c=='r' || c=='R'));
        continue;
      }
    }
    if (T->rsraw && c == 'r' && (i == 0 || !idch((unsigned char)s[i-1]))) {
      int k = i + 1, hashes = 0;
      while (k < n && s[k] == '#') { hashes++; k++; }
      if (k < n && s[k] == '"') {
        blank_to(o, i, k);
        if (!hashes) { i = mask_q(d, T, k, '"', 1, 1); continue; }
        int j = k + 1;
        o[k] = ' ';
        for (; j < n; j++) {
          if (s[j] != '\n') o[j] = ' ';
          if (s[j] == '"' && j + hashes < n) {
            int ok = 1;
            for (int z = 1; z <= hashes; z++) if (s[j+z] != '#') { ok = 0; break; }
            if (ok) { for (int z = 1; z <= hashes; z++) o[j+z] = ' '; j += hashes + 1; break; }
          }
        }
        i = j; continue;
      }
    }
    if (c == '"') { i = mask_q(d, T, i, '"', T->strnl, 0); continue; }
    if (c == '\'') {
      if (T->charlit == 2) {
        if (rs_is_charlit(s, n, i)) i = mask_q(d, T, i, '\'', 0, 0); else i++;
        continue;
      }
      i = mask_q(d, T, i, '\'', T->charlit == 1 ? 0 : T->strnl, 0);
      continue;
    }
    if (c == '`') {
      if (T->backtick == 1) { i = mask_q(d, T, i, '`', 1, 1); continue; }
      if (T->backtick == 2 && lvl < 8) { i = mask_tmpl(d, T, i, lvl); continue; }
      i++; continue;
    }
    if (c == '}' && in_t && bd == 0) return i;
    if (c == '{') bd++;
    else if (c == '}') { if (bd) bd--; }
    i++;
  }
  return i;
}

static const Tok TOK_C    = { "//", "/*", "*/", 0, 1, 0, 0, 0, 0, 1, 0, 0 };
static const Tok TOK_JAVA = { "//", "/*", "*/", 0, 1, 0, 1, 0, 0, 1, 0, 0 };
static const Tok TOK_KT   = { "//", "/*", "*/", 1, 1, 0, 1, 0, 0, 1, 0, 0 };
static const Tok TOK_GO   = { "//", "/*", "*/", 0, 1, 0, 0, 0, 0, 1, 1, 0 };
static const Tok TOK_PY   = { "#",  NULL, NULL, 0, 1, 0, 1, 1, 1, 0, 0, 0 };
static const Tok TOK_JS   = { "//", "/*", "*/", 0, 1, 0, 0, 0, 0, 0, 2, 0 };
static const Tok TOK_RS   = { "//", "/*", "*/", 1, 1, 0, 0, 0, 0, 2, 0, 1 };
static const Tok TOK_TEXT = { NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/* ---------- Doc ---------- */
static void doc_init(Doc *d, Arena *a, Str text, const Tok *T) {
  memset(d, 0, sizeof *d);
  d->a = a;
  d->src = text;
  int n = text.len > 0 ? text.len : 0;
  const char *p = (text.p && n) ? text.p : "";
  d->code = (char*)arena_alloc(a, (size_t)n + 1);
  if (n) memcpy(d->code, p, (size_t)n);
  d->code[n] = 0;
  int nl = 0;
  if (n) {
    for (int i = 0; i < n; i++) if (p[i] == '\n') nl++;
    if (p[n-1] != '\n') nl++;
  }
  d->n = nl;
  int cap = nl ? nl : 1;
  d->off = (int*)arena_alloc(a, sizeof(int) * (size_t)cap);
  d->len = (int*)arena_alloc(a, sizeof(int) * (size_t)cap);
  d->dep = (int*)arena_alloc(a, sizeof(int) * (size_t)cap);
  if (T && n > 0) mask_code(d, T, 0, 0, 0);   /* before the depth pass: a brace in
                                                 a string or comment is not code */
  int i = 0, ln = 0;
  while (i < n && ln < nl) {
    int st = i;
    while (i < n && p[i] != '\n') i++;
    int en = i;
    if (en > st && p[en-1] == '\r') en--;
    d->off[ln] = st; d->len[ln] = en - st; ln++;
    if (i < n) i++;
  }
  int dep = 0;
  for (ln = 0; ln < nl; ln++) {
    d->dep[ln] = dep;
    for (int k = 0; k < d->len[ln]; k++) {
      char c = d->code[d->off[ln] + k];
      if (c == '{') dep++;
      else if (c == '}') { if (dep) dep--; }
    }
  }
}
static Str doc_line(Doc *d, int i) {              /* loaned view of the ORIGINAL line */
  if (i < 0 || i >= d->n || !d->src.p) return s_null();
  Str x; x.p = d->src.p + d->off[i]; x.len = d->len[i];
  return x;
}
static int doc_maxline(Doc *d) {
  int m = 0;
  for (int i = 0; i < d->n; i++) if (d->len[i] > m) m = d->len[i];
  return m;
}
static char *doc_code_trim(Doc *d, int i) {       /* masked line, trimmed, owned */
  if (i < 0 || i >= d->n) return arena_strdup(d->a, "");
  char *t = arena_strndup(d->a, d->code + d->off[i], (size_t)d->len[i]);
  int a = 0, b = (int)strlen(t);
  while (a < b && spch(t[a])) a++;
  while (b > a && spch(t[b-1])) b--;
  t[b] = 0;
  return t + a;
}
static int doc_indent(Doc *d, int i) {            /* python block width, tab = 8 */
  if (i < 0 || i >= d->n) return 0;
  int w = 0;
  for (int k = 0; k < d->len[i]; k++) {
    char c = d->code[d->off[i] + k];
    if (c == ' ') w++;
    else if (c == '\t') w += 8;
    else break;
  }
  return w;
}

/* ==========================================================================
 * 2. logical lines, signatures, anchors, symbol sink
 * ========================================================================== */
/* Does the joined fragment still need the next physical line? An open ( or [, a
 * trailing joiner, or no terminator at all keeps it going. Braces are NOT depth:
 * a declaration header ends with '{' and its body must never be swallowed. */
static int cont_state(const char *p) {
  size_t n = strlen(p);
  while (n && spch(p[n-1])) n--;
  if (!n) return 0;
  int dp = 0;
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '(' || p[i] == '[') dp++;
    else if ((p[i] == ')' || p[i] == ']') && dp > 0) dp--;
  }
  char last = p[n-1];
  if (dp > 0) return 1;
  if (last=='\\' || last==',' || last=='=' || last=='+' || last=='&' || last=='|' ||
      last=='<' || last=='>' || last=='*' || last=='-') return 1;
  if (last==':' || last=='{' || last==';' || last=='}') return 0;
  if (!strchr(p,'(') && !strchr(p,';') && !strchr(p,'{') && !strchr(p,'=')) return 1;
  return 0;
}
static char *join_code(Doc *d, int ln, int *last_out) {
  Buf b; buf_init(&b, d->a);
  int last = ln;
  for (int k = ln; k < d->n && k <= ln + JOINMAX - 1; k++) {
    if (k > ln) buf_putc(&b, ' ');
    buf_put(&b, d->code + d->off[k], (size_t)d->len[k]);
    last = k;
    if ((int)b.len > JOINCAP) break;
    char *t = buf_str(&b).p;
    t[b.len] = 0;
    if (!cont_state(t)) break;
  }
  *last_out = last;
  return buf_take(&b).p;
}
static Str sig_of(Doc *d, int ln, int last) {     /* ORIGINAL text of the span, capped */
  Buf b; buf_init(&b, d->a);
  for (int k = ln; k <= last && k < d->n; k++) {
    Str x = doc_line(d, k);
    int a = 0, e = x.len;
    while (a < e && spch(x.p[a])) a++;
    while (e > a && spch(x.p[e-1])) e--;
    if (e > a) {
      if (b.len) buf_putc(&b, ' ');
      buf_put(&b, x.p + a, (size_t)(e - a));       /* wrapped decls: one space, no indent */
    }
    if ((int)b.len > 4 * SIG_CAP) break;
  }
  Str t = s_trim(d->a, buf_take(&b));
  if (t.len > SIG_CAP) {
    int cut = SIG_CAP - 3;
    while (cut > 0 && ((unsigned char)t.p[cut] & 0xC0) == 0x80) cut--;
    return s_concat(d->a, s_cut(d->a, t, 0, cut), s_lit(d->a, "..."));
  }
  return t;
}
/* SPEC §4.2, computed by xdiff.c's anchor_at() - the same producer fs.read
 * --anchors and tx.patch use. No line number goes into the hash: an insert above
 * must not invalidate the anchors below. */
static Str ol_anchor(Doc *d, int ln) {
  Str pv = doc_line(d, ln-1), cv = doc_line(d, ln), nx = doc_line(d, ln+1);
  return anchor_at(d->a, NULL, ln, pv, cv, nx);
}

static void sl_push(Doc *d, SymList *sl, int ln, int last, const char *kind,
                    const char *nm, int nn) {
  if (sl->n >= MAXSYM) { sl->note = 1; return; }
  if (!kind || !nm || nn <= 0) return;
  if (nn >= 160) nn = 159;
  if (sl->n == sl->cap) {
    int cap = sl->cap ? sl->cap * 2 : 16;
    Sym *nv = (Sym*)arena_alloc(d->a, sizeof(Sym) * (size_t)cap);
    if (sl->n) memcpy(nv, sl->v, sizeof(Sym) * (size_t)sl->n);
    sl->v = nv; sl->cap = cap;
  }
  Sym s;
  s.kind = kind;
  s.name = arena_strndup(d->a, nm, (size_t)nn);
  s.name_len = nn;
  s.line = ln + 1;
  s.sig = s_cut(d->a, sig_of(d, ln, last), 0, 4 * SIG_CAP).p;
  sl->v[sl->n++] = s;
}
static void sl_str(Doc *d, SymList *sl, int ln, int last, const char *kind, const char *name) {
  sl_push(d, sl, ln, last, kind, name, (int)strlen(name));
}

/* ---------- token helpers over a NUL-terminated masked logical line ---------- */
static int t_ws(const char *p, int i) { while (p[i] && spch(p[i])) i++; return i; }
static int t_word(const char *p, int i, int *e) {
  i = t_ws(p, i);
  if (!idch((unsigned char)p[i])) return -1;
  int s = i;
  while (p[i] && idch((unsigned char)p[i])) i++;
  *e = i;
  return s;
}
/* index just past `kw` when the next token is exactly kw, else -1 */
static int t_kw(const char *p, int i, const char *kw) {
  int e, s = t_word(p, i, &e);
  size_t n = strlen(kw);
  if (s < 0 || (int)n != e - s || memcmp(p + s, kw, n)) return -1;
  return e;
}
static int starts_word(const char *p, const char *w) {
  int e;
  size_t n = strlen(w);
  return t_word(p, 0, &e) == 0 && (size_t)e == n && !memcmp(p, w, n);
}
static int is_ctrl(const char *p, int s, int e) {
  static const char *const kws[] = {
    "if","else","for","while","switch","case","default","return","do","goto","break",
    "continue","throw","try","catch","sizeof","typeof","alignof","static_assert","assert",
    "new","delete","yield","await","in","of","and","or","not","elif","then","when","else",NULL
  };
  for (int i = 0; kws[i]; i++) {
    size_t n = strlen(kws[i]);
    if ((int)n == e - s && !memcmp(p + s, kws[i], n)) return 1;
  }
  return 0;
}
static int name_ok(const char *p, int s, int e) { return e > s && !is_ctrl(p, s, e); }
/* After an explicit function keyword (rust `fn`, go `func`, python `def`, kotlin
 * `fun`) the next word IS the name - only a handful of reserved words can never be
 * one. The wider is_ctrl list stays for the inferred C/Java/JS signatures, where a
 * bare `name(` is ambiguous with a call - but `fn new()` must not be dropped. */
static int fn_name_ok(const char *p, int s, int e) {
  static const char *const kws[] = {
    "if","else","for","while","switch","case","default","return","do","goto","break",
    "continue","match","try","catch","in","is","typeof","sizeof",NULL
  };
  if (e <= s) return 0;
  for (int i = 0; kws[i]; i++) {
    size_t n = strlen(kws[i]);
    if ((int)n == e - s && !memcmp(p + s, kws[i], n)) return 0;
  }
  return 1;
}
/* identifier ending just before i (whitespace allowed), else -1 */
static int ident_before(const char *p, int i, int *s_out, int *e_out) {
  int j = i - 1;
  while (j >= 0 && spch(p[j])) j--;
  if (j < 0 || !idch((unsigned char)p[j])) return -1;
  int e = j + 1;
  while (j >= 0 && idch((unsigned char)p[j])) j--;
  *s_out = j + 1; *e_out = e;
  return 0;
}
static int match_paren(const char *p, int i) {    /* p[i]=='(' -> index of its ')' */
  int d = 0;
  for (; p[i]; i++) {
    if (p[i] == '(') d++;
    else if (p[i] == ')') { d--; if (!d) return i; }
  }
  return -1;
}
/* first terminator after an arg list: '{' body, ';' decl, 0 neither */
static int decl_term(const char *p, int close) {
  for (int i = close + 1; p[i]; i++) {
    if (p[i] == '{') return '{';
    if (p[i] == ';') return ';';
    if (p[i] == '=' || p[i] == '#') return 0;
  }
  return 0;
}
static int all_caps(const char *nm, int n) {
  int letters = 0;
  if (n < 2) return 0;
  for (int i = 0; i < n; i++) {
    char c = nm[i];
    if ((c >= '0' && c <= '9') || c == '_') continue;
    if (c >= 'A' && c <= 'Z') { letters++; continue; }
    return 0;
  }
  return letters > 0;
}
static void cp_word(char *dst, int dn, const char *p, int s, int e) {
  int n = e - s;
  if (n < 0) n = 0;
  if (n >= dn) n = dn - 1;
  memcpy(dst, p + s, (size_t)n);
  dst[n] = 0;
}
static void tail_word(const char *p, char *out, int n) {   /* last identifier */
  out[0] = 0;
  int best = -1, bl = 0, e;
  for (int i = 0; p[i]; ) {
    int s = t_word(p, i, &e);
    if (s < 0) { i++; continue; }
    best = s; bl = e - s; i = e;
  }
  if (best >= 0 && bl > 0) cp_word(out, n, p, best, best + bl);
}
/* `<...>` groups removed and spaces collapsed: impl<T> Foo<T> for Bar -> Foo for Bar */
static void strip_generics(const char *p, char *out, int n) {
  int o = 0, d = 0;
  for (int i = 0; p[i] && o < n - 1; i++) {
    char c = p[i];
    if (c == '<') { d++; continue; }
    if (c == '>') { if (d) d--; continue; }
    if (d) continue;
    if (spch(c)) { if (o && out[o-1] == ' ') continue; out[o++] = ' '; continue; }
    out[o++] = c;
  }
  while (o && out[o-1] == ' ') o--;
  out[o] = 0;
}
static int has_word(const char *p, const char *w) {
  size_t n = strlen(w);
  for (size_t i = 0; p[i]; i++) {
    if (strncmp(p + i, w, n) != 0) continue;
    int left = i > 0 && idch((unsigned char)p[i-1]);
    int right = idch((unsigned char)p[i+n]);
    if (!left && !right) return 1;
  }
  return 0;
}
/* skip a run of modifier keywords (and pub(...)-style paren groups) */
static int skip_mods(const char *p, const char *const *mods, int maxpass) {
  int i = 0;
  for (int pass = 0; pass < maxpass; pass++) {
    int moved = 0;
    for (int k = 0; mods[k]; k++) {
      int q = t_kw(p, i, mods[k]);
      if (q > 0) { i = q; moved = 1; }
    }
    i = t_ws(p, i);
    if (p[i] == '(') {
      int c = match_paren(p, i);
      if (c > 0) { i = t_ws(p, c + 1); moved = 1; }
    }
    if (!moved) break;
  }
  return i;
}
/* index past a `<...>` list if one starts here (ws collapsed), else unchanged index */
static int skip_angle(const char *p, int i) {
  i = t_ws(p, i);
  if (p[i] != '<') return i;
  int dep = 0, j = i;
  for (; p[j]; j++) {
    if (p[j] == '<') dep++;
    else if (p[j] == '>') { dep--; if (!dep) { j++; break; } }
  }
  if (dep) return -1;
  return t_ws(p, j);
}
/* The shared declaration probe for the brace languages:
 *   [return type] NAME ( args ) [trailing] { | ;
 * `need_ret` demands a non-empty prefix without '=' or ',' (C and Java want a return
 * type; JS class members do not). Anything doubtful returns 0 - see axiom 1.
 * `at_end` says the joined fragment reached the last line of the document: a buffer
 * clipped mid-line (fs.read --lines, a file with no final break) then holds a whole
 * declaration header with only its terminator cut off, which is a prototype. */
static int decl_shape(Doc *d, const char *p, int need_ret, int at_end,
                      int *ns, int *ne, int *term) {
  const char *open = strchr(p, '(');
  if (!open) return 0;
  int s, e;
  if (ident_before(p, (int)(open - p), &s, &e) < 0) return 0;
  if (!name_ok(p, s, e)) return 0;
  char pre = s > 0 ? p[s-1] : 0;
  if (pre=='.' || pre=='-' || pre=='>' || pre==':' || pre=='#') return 0;
  if (need_ret) {
    if (s == 0) return 0;
    char *head = arena_strndup(d->a, p, (size_t)s);
    for (char *q = head; *q; q++) if (spch(*q)) *q = ' ';
    int he;
    if (t_word(head, 0, &he) < 0) return 0;
    if (strchr(head, '=') || strchr(head, ',') || strchr(head, '(')) return 0;
  }
  int close = match_paren(p, (int)(open - p));
  if (close < 0) return 0;
  int t = decl_term(p, close);
  if (!t && at_end) {                    /* nothing but the cut-off ';' after the ')' */
    int i = close + 1;
    while (p[i] && spch(p[i])) i++;
    if (!p[i]) t = ';';
  }
  if (!t) return 0;
  *ns = s; *ne = e; *term = t;
  return 1;
}
static void body_skip(Doc *d, int ln, int last, int *skip_to) {
  int base = d->dep[ln];
  int k = last + 1;
  while (k < d->n && d->dep[k] > base) k++;
  *skip_to = k;
}
/* Is this line worth joining and classifying at all? It must either carry an arg
 * list, start with a keyword that can open a declaration, or be a bare fragment
 * that the next line completes. Cheap filter only - the classifiers decide. */
static int worth_join(const char *t, const char *const *kws) {
  if (strchr(t, '(')) return 1;
  if (kws) for (int i = 0; kws[i]; i++) if (starts_word(t, kws[i])) return 1;
  if (!strchr(t, ';') && !strchr(t, '{') && !strchr(t, '}') && !strchr(t, ':') &&
      !strchr(t, '=') && strlen(t) < 60) return 1;
  return 0;
}

/* ==========================================================================
 * 3. C / C++
 * ========================================================================== */
/* modifiers only - deliberately no struct/enum/class here, or skip_mods would eat
 * the very keyword the type branch looks for */
static const char *const C_MODS[] = { "static","extern","inline","_Inline","register",
  "volatile","const","unsigned","signed","long","short","virtual","explicit",
  "constexpr","_Noreturn","__restrict","restrict","friend",NULL };
static const char *const C_HEADS[] = { "typedef","struct","union","enum","class",NULL };

static void scan_c(Doc *d, SymList *sl, const char *path) {
  Frame tf[12];
  int tn = 0, skip_to = 0, dead = 0, ifs = 0;
  (void)path;
  for (int ln = 0; ln < d->n; ln++) {
    if (ln < skip_to) continue;
    int base = d->dep[ln];
    while (tn > 0 && base <= tf[tn-1].base) tn--;
    char *t = doc_code_trim(d, ln);
    if (!*t) continue;
    if (*t == '#') {                    /* preprocessor: only #define is a symbol */
      if (dead) {
        if (!strncmp(t, "#if", 3)) ifs++;
        else if (!strncmp(t, "#endif", 6)) { if (--ifs <= 0) { dead = 0; ifs = 0; } }
        else if (!strncmp(t, "#else", 5) || !strncmp(t, "#elif", 5)) { dead = 0; ifs = 0; }
        continue;
      }
      if (!strncmp(t, "#if", 3) && strncmp(t, "#ifdef", 6) && strncmp(t, "#ifndef", 7)) {
        char *v = t + 3;
        while (*v && spch(*v)) v++;
        if (*v == '0' && (!v[1] || spch(v[1]))) { dead = 1; ifs = 1; }
        continue;
      }
      if (!strncmp(t, "#define", 7) && spch(t[7])) {
        int e, s = t_word(t, 7, &e);        /* the macro name is always on line 1 */
        char nm[96];
        if (s >= 0 && !is_ctrl(t, s, e)) {
          cp_word(nm, (int)sizeof nm, t, s, e);
          sl_str(d, sl, ln, ln, "macro", nm);
        }
      }
      continue;
    }
    if (dead) continue;
    if (!worth_join(t, C_HEADS)) continue;
    int last, s, e;
    char *p = join_code(d, ln, &last);
    if (!*p) continue;

    /* typedef ... NAME;      (function-pointer and initializer-ish forms skipped) */
    if (starts_word(p, "typedef")) {
      if (strchr(p, '(')) continue;
      char *semi = strrchr(p, ';');
      if (!semi || semi == p) continue;
      const char *kind = has_word(p, "enum") ? "enum"
                       : (has_word(p, "struct") || has_word(p, "union")) ? "struct" : "type";
      if (ident_before(p, (int)(semi - p), &s, &e) < 0) continue;
      if (!name_ok(p, s, e)) continue;
      sl_push(d, sl, ln, last, kind, p + s, e - s);
      continue;
    }
    if (starts_word(p, "template")) continue;   /* the declaration below speaks */
    /* struct / union / enum / class NAME */
    {
      int i = skip_mods(p, C_MODS, 3);
      const char *kind = NULL;
      int j = t_kw(p, i, "struct");
      if (j < 0) j = t_kw(p, i, "union");
      if (j >= 0) kind = "struct";
      if (j < 0) { j = t_kw(p, i, "enum"); if (j >= 0) kind = "enum"; }
      if (j < 0 && d->cpp) { j = t_kw(p, i, "class"); if (j >= 0) kind = "class"; }
      if (j >= 0) {
        i = t_word(p, j, &e);
        if (i < 0 || is_ctrl(p, i, e)) continue;
        s = i;
        char nm[96];
        cp_word(nm, (int)sizeof nm, p, s, e);
        sl_push(d, sl, ln, last, kind, nm, e - s);
        if (strchr(p + e, '{') && tn < 12) {   /* remember the body for C++ members */
          tf[tn].base = base;
          snprintf(tf[tn].nm, sizeof tf[tn].nm, "%s", nm);
          tn++;
        }
        continue;
      }
    }
    /* function definition or prototype */
    {
      int term = 0;
      if (!decl_shape(d, p, 1, last >= d->n - 1, &s, &e, &term)) continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      if (tn > 0 && d->cpp) {
        char full[256];
        int n = snprintf(full, sizeof full, "%s.%s", tf[tn-1].nm, nm);
        sl_push(d, sl, ln, last, "method", full, n);
      } else {
        sl_str(d, sl, ln, last, "fn", nm);
      }
      if (term == '{') body_skip(d, ln, last, &skip_to);
    }
  }
}

/* ==========================================================================
 * 4. Go
 * ========================================================================== */
static void scan_go(Doc *d, SymList *sl, const char *path) {
  int skip_to = 0;
  const char *blk = NULL;                          /* const/var/type block at top level */
  (void)path;
  for (int ln = 0; ln < d->n; ln++) {
    if (ln < skip_to) continue;
    int base = d->dep[ln];
    char *t = doc_code_trim(d, ln);
    if (!*t) continue;
    if (blk) {
      if (*t == ')' || base != 0) { blk = NULL; continue; }
      int e, s = t_word(t, 0, &e);
      char nm[96];
      if (s >= 0 && fn_name_ok(t, s, e)) {
        cp_word(nm, (int)sizeof nm, t, s, e);
        sl_str(d, sl, ln, ln, blk, nm);
      }
      continue;
    }
    if (base != 0) continue;                       /* top level only */
    int s, e, last;
    if (starts_word(t, "func")) {
      char *p = join_code(d, ln, &last);
      int i = t_kw(p, 0, "func");
      if (i < 0) continue;
      const char *kind = "fn";
      char recv[96];
      recv[0] = 0;
      i = t_ws(p, i);
      if (p[i] == '(') {                           /* method with a receiver */
        int c = match_paren(p, i);
        if (c < 0) continue;
        char inner[128];
        cp_word(inner, (int)sizeof inner, p, i + 1, c);
        strip_generics(inner, inner, (int)sizeof inner);
        tail_word(inner, recv, (int)sizeof recv);
        s = t_word(p, c + 1, &e);
        if (s < 0) continue;
        kind = "method";
      } else {
        s = t_word(p, i, &e);
        if (s < 0) continue;
      }
      if (!fn_name_ok(p, s, e)) continue;
      if (p[t_ws(p, e)] != '(') continue;          /* a declaration has an arg list */
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      if (!strcmp(kind, "fn") && !strncmp(nm, "Test", 4) &&
          (nm[4] == 0 || isupper((unsigned char)nm[4]))) kind = "test";
      if (kind[0] == 'm' && recv[0]) {
        char full[256];
        int n = snprintf(full, sizeof full, "%s.%s", recv, nm);
        sl_push(d, sl, ln, last, kind, full, n);
      } else {
        sl_str(d, sl, ln, last, kind, nm);
      }
      if (strchr(p + e, '{') && !strchr(p + e, ';')) body_skip(d, ln, last, &skip_to);
      continue;
    }
    if (starts_word(t, "type")) {
      char *p = join_code(d, ln, &last);
      int i = t_ws(p, t_kw(p, 0, "type"));
      if (i < 0) continue;
      if (p[i] == '(') { blk = "type"; continue; }
      s = t_word(p, i, &e);
      if (s < 0 || !fn_name_ok(p, s, e)) continue;
      const char *kind = "type";
      if (has_word(p + e, "struct")) kind = "struct";
      else if (has_word(p + e, "interface")) kind = "iface";
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      sl_str(d, sl, ln, last, kind, nm);
      continue;
    }
    if (starts_word(t, "const") || starts_word(t, "var")) {
      int isc = starts_word(t, "const");
      int i = t_ws(t, isc ? 5 : 3);
      if (t[i] == '(') { blk = isc ? "const" : "var"; continue; }
      s = t_word(t, i, &e);
      if (s < 0 || !fn_name_ok(t, s, e)) continue;
      if (t[e] == '(') continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, t, s, e);
      sl_str(d, sl, ln, ln, isc ? "const" : "var", nm);
      continue;
    }
  }
}

/* ==========================================================================
 * 5. Python - indentation gives the nesting, so names are Class.method
 * ========================================================================== */
static void scan_py(Doc *d, SymList *sl, const char *path) {
  struct { int ind, iscls; char nm[96]; } st[32];
  int sp = 0;
  const char *b = path_base(path ? path : "");
  int tfile = b && (!strncmp(b, "test_", 5) || !strncmp(b, "Test", 4) ||
                    (strlen(b) > 8 && strstr(b, "_test.py") != NULL));
  for (int ln = 0; ln < d->n; ln++) {
    char *t = doc_code_trim(d, ln);
    if (!*t || *t == '@') continue;                /* decorators carry no declaration */
    int ind = doc_indent(d, ln);
    while (sp > 0 && st[sp-1].ind >= ind) sp--;
    int ih = t_kw(t, 0, "class");
    int idf = t_kw(t, 0, "def");
    if (idf < 0) { int as = t_kw(t, 0, "async"); if (as > 0) idf = t_kw(t, as, "def"); }
    if (ih < 0 && idf < 0) {
      /* only module-level UPPER_CASE bindings are worth a line */
      int e, s = t_word(t, 0, &e);
      if (ind != 0 || s != 0 || t[e] == '(' || !all_caps(t + s, e - s)) continue;
      char *eq = strchr(t + e, '=');
      if (!eq || strchr(eq, '(')) continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, t, s, e);
      sl_str(d, sl, ln, ln, "const", nm);
      continue;
    }
    int last, is_class = ih > 0;
    char *p = join_code(d, ln, &last);
    int i = t_kw(p, 0, "class");
    if (!is_class) {
      i = t_kw(p, 0, "def");
      if (i < 0) { int as = t_kw(p, 0, "async"); if (as > 0) i = t_kw(p, as, "def"); }
    }
    if (i < 0) continue;
    int e, s = t_word(p, i, &e);
    if (s < 0 || !fn_name_ok(p, s, e)) continue;
    char nm[96];
    cp_word(nm, (int)sizeof nm, p, s, e);
    if (!is_class && p[t_ws(p, e)] != '(') continue;
    char full[4 * 192];
    size_t o = 0;
    for (int z = 0; z < sp && o + 1 < sizeof full; z++)
      o += (size_t)snprintf(full + o, sizeof full - o, "%s%s", z ? "." : "", st[z].nm);
    if (sp && o + 1 < sizeof full) full[o++] = '.';
    full[o] = 0;
    strncat(full, nm, sizeof full - strlen(full) - 1);
    const char *kind;
    if (is_class) kind = "class";
    else if (sp > 0 && st[sp-1].iscls) kind = "method";
    else if (!strncmp(nm, "test_", 5) || (tfile && !strncmp(nm, "test", 4))) kind = "test";
    else kind = "fn";
    sl_str(d, sl, ln, last, kind, full);
    if (sp < 32) {
      st[sp].ind = ind;
      st[sp].iscls = is_class;
      snprintf(st[sp].nm, sizeof st[sp].nm, "%s", nm);
      sp++;
    }
  }
}

/* ==========================================================================
 * 6. JavaScript / TypeScript
 * ========================================================================== */
static const char *const JS_MODS[] = { "export","default","declare","static","async",
  "public","private","protected","readonly","abstract","override","get","set",NULL };
static const char *const JS_HEADS[] = { "class","interface","enum","type","function",
  "const","let","var","abstract","export","declare","async",NULL };

static void scan_js(Doc *d, SymList *sl, const char *path) {
  Frame tf[12];
  int tn = 0, skip_to = 0;
  (void)path;
  for (int ln = 0; ln < d->n; ln++) {
    if (ln < skip_to) continue;
    int base = d->dep[ln];
    while (tn > 0 && base <= tf[tn-1].base) tn--;
    char *t = doc_code_trim(d, ln);
    if (!*t) continue;
    if (!worth_join(t, JS_HEADS)) continue;
    int last, s, e;
    char *p = join_code(d, ln, &last);
    if (!*p) continue;
    int i = skip_mods(p, JS_MODS, 4);
    if (starts_word(p + i, "namespace") || starts_word(p + i, "module")) continue;
    if ((s = t_kw(p, i, "function")) > 0) {
      int j = t_ws(p, s);
      if (p[j] == '*') j = t_ws(p, j + 1);
      s = t_word(p, j, &e);
      if (s < 0 || !fn_name_ok(p, s, e)) continue;
      if (p[t_ws(p, e)] != '(') continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      sl_str(d, sl, ln, last, "fn", nm);
      if (strchr(p + e, '{')) body_skip(d, ln, last, &skip_to);
      continue;
    }
    const char *kind = NULL;
    if ((s = t_kw(p, i, "class")) > 0) kind = "class";
    else if ((s = t_kw(p, i, "interface")) > 0) kind = "iface";
    else if ((s = t_kw(p, i, "enum")) > 0) kind = "enum";
    if (kind) {
      s = t_word(p, s, &e);
      if (s < 0 || !name_ok(p, s, e)) continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      sl_str(d, sl, ln, last, kind, nm);
      if (strchr(p + e, '{') && tn < 12) {
        tf[tn].base = base;
        snprintf(tf[tn].nm, sizeof tf[tn].nm, "%s", nm);
        tn++;
      }
      continue;
    }
    if ((s = t_kw(p, i, "type")) > 0) {             /* TS alias */
      s = t_word(p, s, &e);
      if (s < 0 || is_ctrl(p, s, e)) continue;
      int k = skip_angle(p, e);
      if (k < 0 || p[k] != '=') continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      sl_str(d, sl, ln, last, "type", nm);
      continue;
    }
    if ((i = t_kw(p, i, "const")) > 0 || (i = t_kw(p, i, "let")) > 0 || (i = t_kw(p, i, "var")) > 0) {
      s = t_word(p, i, &e);
      if (s < 0) continue;
      char *eq = strchr(p + e, '=');
      const char *rhs = eq ? eq + 1 : NULL;
      while (rhs && spch(*rhs)) rhs++;
      int isfn = rhs && (rhs[0] == '(' || starts_word(rhs, "function") ||
                         starts_word(rhs, "async") || starts_word(rhs, "Function"));
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      if (isfn && p[e] != '(') {
        if (tn > 0) {
          char full[256];
          int n = snprintf(full, sizeof full, "%s.%s", tf[tn-1].nm, nm);
          sl_push(d, sl, ln, last, "method", full, n);
        } else sl_str(d, sl, ln, last, "fn", nm);
        if (strchr(rhs, '{')) body_skip(d, ln, last, &skip_to);
      } else if (tn == 0 && rhs && all_caps(p + s, e - s) && !strchr(rhs, '(')) {
        sl_str(d, sl, ln, last, "const", nm);      /* SCREAMING_CASE module constant */
      }
      continue;
    }
    if (tn > 0) {                                   /* class member shorthand */
      int term = 0;
      if (!decl_shape(d, p, 0, last >= d->n - 1, &s, &e, &term)) {
        /* `name: (…) => {` and `name = (…) => {` class fields */
        int k = -1;
        for (int q = 0; p[q]; q++) if (p[q] == ':' || p[q] == '=') { k = q; break; }
        if (k <= 0 || ident_before(p, k, &s, &e) < 0) continue;
        const char *rhs = p + k + 1;
        while (*rhs && spch(*rhs)) rhs++;
        if (*rhs != '(' && !starts_word(rhs, "function") && !starts_word(rhs, "async")) continue;
        const char *open = strchr(p + k, '(');
        if (!open) continue;
        int close = match_paren(p, (int)(open - p));
        if (close < 0) continue;
        term = decl_term(p, close);
        if (!term) continue;
      }
      if (!fn_name_ok(p, s, e)) continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      char full[256];
      int n = snprintf(full, sizeof full, "%s.%s", tf[tn-1].nm, nm);
      sl_push(d, sl, ln, last, "method", full, n);
      if (term == '{') body_skip(d, ln, last, &skip_to);
      continue;
    }
  }
}

/* ==========================================================================
 * 7. Java / Kotlin
 * ========================================================================== */
static const char *const JV_MODS[] = { "public","private","protected","internal","static",
  "final","abstract","native","synchronized","strictfp","default","open","sealed","data",
  "inner","companion","inline","external","suspend","lateinit","override","const",NULL };
static const char *const JV_HEADS[] = { "class","interface","enum","record","object","fun",
  "trait","public","private","protected","static","final","abstract","internal","data",
  "open","sealed","companion","inline","suspend","void","fun",NULL };

static void scan_java(Doc *d, SymList *sl, const char *path) {
  Frame tf[16];
  int tn = 0, skip_to = 0, pend = 0;
  (void)path;
  for (int ln = 0; ln < d->n; ln++) {
    if (ln < skip_to) continue;
    int base = d->dep[ln];
    while (tn > 0 && base <= tf[tn-1].base) tn--;
    char *t = doc_code_trim(d, ln);
    if (!*t) continue;
    if (*t == '@') {                              /* annotations: only @Test matters */
      int e, s = t_word(t, 1, &e);
      pend = (s >= 0 && e - s == 4 && !memcmp(t + s, "Test", 4)) ? 1 : 0;
      continue;
    }
    if (!worth_join(t, JV_HEADS)) continue;
    int last, s, e;
    char *p = join_code(d, ln, &last);
    if (!*p) continue;
    int i = skip_mods(p, JV_MODS, 4);
    const char *kind = NULL;
    int j = -1;
    if ((j = t_kw(p, i, "class")) > 0 || (j = t_kw(p, i, "record")) > 0) kind = "class";
    else if ((j = t_kw(p, i, "interface")) > 0) kind = "iface";
    else if ((j = t_kw(p, i, "trait")) > 0) kind = "trait";
    else if ((j = t_kw(p, i, "enum")) > 0) {
      int q = t_kw(p, j, "class");
      kind = "enum";
      if (q > 0) j = q;
    } else if ((j = t_kw(p, i, "object")) > 0) kind = "class";
    if (kind) {
      s = t_word(p, j, &e);
      if (s < 0 || !name_ok(p, s, e)) continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      sl_str(d, sl, ln, last, kind, nm);
      if (strchr(p + e, '{') && tn < 16) {
        tf[tn].base = base;
        snprintf(tf[tn].nm, sizeof tf[tn].nm, "%s", nm);
        tn++;
      }
      continue;
    }
    /* kotlin `fun name(...)`, java `Type name(...)`: both need an arg list + body */
    int isfun = t_kw(p, i, "fun");
    int term = 0;
    if (isfun > 0) {
      s = t_word(p, isfun, &e);
      if (s < 0) continue;
      int k = skip_angle(p, e);
      if (k < 0) continue;
      if (p[k] == '.') { s = t_word(p, k + 1, &e); if (s < 0) continue; }
      k = t_ws(p, e);
      if (p[k] != '(') continue;
      int close = match_paren(p, k);
      if (close < 0) continue;
      term = decl_term(p, close);
      if (!term) continue;
    } else {
      if (!decl_shape(d, p, 1, last >= d->n - 1, &s, &e, &term)) continue;
    }
    if (!fn_name_ok(p, s, e)) continue;
    char nm[96];
    cp_word(nm, (int)sizeof nm, p, s, e);
    const char *mk = tn > 0 ? "method" : "fn";
    if (tn > 0) {
      char full[256];
      int n = !strcmp(nm, tf[tn-1].nm)
                ? snprintf(full, sizeof full, "%s.new", tf[tn-1].nm)
                : snprintf(full, sizeof full, "%s.%s", tf[tn-1].nm, nm);
      sl_push(d, sl, ln, last, pend ? "test" : mk, full, n);
    } else {
      sl_str(d, sl, ln, last, pend ? "test" : mk, nm);
    }
    pend = 0;
    if (term == '{') body_skip(d, ln, last, &skip_to);
  }
}

/* ==========================================================================
 * 8. Rust
 * ========================================================================== */
static const char *const RS_MODS[] = { "pub","crate","unsafe","async","extern","default",
  "gen","dyn","ref","move","box",NULL };
static const char *const RS_HEADS[] = { "struct","enum","trait","impl","mod","type",
  "macro_rules","const","static","use","fn","pub","unsafe","async",NULL };

static void scan_rs(Doc *d, SymList *sl, const char *path) {
  int skip_to = 0, pend = 0, tr_base = -1;
  Frame impl;
  impl.base = -1;
  impl.nm[0] = 0;
  (void)path;
  for (int ln = 0; ln < d->n; ln++) {
    if (ln < skip_to) continue;
    int base = d->dep[ln];
    if (impl.base >= 0 && base <= impl.base) { impl.base = -1; impl.nm[0] = 0; }
    if (tr_base >= 0 && base <= tr_base) tr_base = -1;
    char *t = doc_code_trim(d, ln);
    if (!*t) continue;
    if (*t == '#') {                              /* attributes: #[test] marks the next fn */
      int i = t_ws(t, 1);
      if (t[i] == '[') i = t_ws(t, i + 1);
      if (t[i] == '!') i = t_ws(t, i + 1);
      int e, s = t_word(t, i, &e);
      char an[32];
      pend = 0;
      if (s >= 0) {
        cp_word(an, (int)sizeof an, t, s, e);
        if (!strncmp(an, "test", 4)) pend = 1;    /* test, test_case, tokio::test-ish */
      }
      continue;
    }
    if (!worth_join(t, RS_HEADS)) continue;
    int last, s, e;
    char *p = join_code(d, ln, &last);
    if (!*p) continue;
    int i = skip_mods(p, RS_MODS, 4);
    int ic = t_kw(p, i, "const");
    int ifn = t_kw(p, ic > 0 ? ic : i, "fn");
    if ((s = t_kw(p, i, "macro_rules")) > 0) {
      int j = t_ws(p, s);
      if (p[j] == '!') j = t_ws(p, j + 1);
      s = t_word(p, j, &e);
      if (s >= 0) {
        char nm[96];
        cp_word(nm, (int)sizeof nm, p, s, e);
        sl_str(d, sl, ln, last, "macro", nm);
      }
      continue;
    }
    if (ifn > 0) {
      /* a trait's `fn f(&self) -> T;` is a required signature, not a definition:
       * there is no body there for an agent to aim an edit at, so it is not a
       * symbol (same call go makes for an interface body). */
      if (tr_base >= 0 && base > tr_base) continue;
      s = t_word(p, ifn, &e);
      if (s < 0 || !fn_name_ok(p, s, e)) continue;
      int k = skip_angle(p, e);
      if (k < 0 || p[k] != '(') continue;
      int close = match_paren(p, k);
      if (close < 0) continue;
      int term = decl_term(p, close);
      if (!term) continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      const char *kind = pend ? "test" : (impl.base >= 0 ? "method" : "fn");
      if (impl.base >= 0 && !pend) {
        char full[256];
        int n = snprintf(full, sizeof full, "%s::%s", impl.nm, nm);
        sl_push(d, sl, ln, last, kind, full, n);
      } else {
        sl_str(d, sl, ln, last, kind, nm);
      }
      pend = 0;
      if (term == '{') body_skip(d, ln, last, &skip_to);
      continue;
    }
    pend = 0;
    if ((s = t_kw(p, i, "struct")) > 0 || (s = t_kw(p, i, "enum")) > 0 ||
        (s = t_kw(p, i, "trait")) > 0 || (s = t_kw(p, i, "union")) > 0) {
      const char *kind = t_kw(p, i, "trait") > 0 ? "trait"
                       : t_kw(p, i, "enum") > 0 ? "enum" : "struct";
      s = t_word(p, s, &e);
      if (s < 0 || !fn_name_ok(p, s, e)) continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      sl_str(d, sl, ln, last, kind, nm);
      if (kind[0] == 't') tr_base = base;        /* its body is signatures, see above */
      continue;
    }
    if ((s = t_kw(p, i, "mod")) > 0) {
      s = t_word(p, s, &e);
      if (s < 0 || !fn_name_ok(p, s, e)) continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      sl_str(d, sl, ln, last, "mod", nm);
      continue;
    }
    if ((s = t_kw(p, i, "impl")) > 0) {
      const char *brace = strchr(p + s, '{');
      if (!brace) continue;
      char seg[192];
      cp_word(seg, (int)sizeof seg, p, s, (int)(brace - p));
      strip_generics(seg, seg, (int)sizeof seg);
      int l = 0;
      while (seg[l] && spch(seg[l])) l++;
      if (!seg[l]) continue;
      char ty[96];
      tail_word(seg + l, ty, (int)sizeof ty);
      sl_str(d, sl, ln, last, "impl", seg + l);
      impl.base = base;
      snprintf(impl.nm, sizeof impl.nm, "%s", ty[0] ? ty : seg + l);
      continue;
    }
    if ((s = t_kw(p, i, "type")) > 0) {
      s = t_word(p, s, &e);
      if (s < 0 || is_ctrl(p, s, e)) continue;
      int k = skip_angle(p, e);
      if (k < 0 || p[k] != '=') continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      sl_str(d, sl, ln, last, "type", nm);
      continue;
    }
    if (ic > 0 || (s = t_kw(p, i, "static")) > 0) {
      int isc = ic > 0;
      s = t_word(p, isc ? ic : s, &e);
      if (s < 0) continue;
      if (e - s == 3 && !memcmp(p + s, "mut", 3)) { s = t_word(p, e, &e); if (s < 0) continue; }
      if (base != 0) continue;                    /* module level only */
      int k = t_ws(p, e);
      if (p[k] != ':' && p[k] != '=') continue;
      char nm[96];
      cp_word(nm, (int)sizeof nm, p, s, e);
      sl_str(d, sl, ln, last, isc ? "const" : "var", nm);
      continue;
    }
  }
}

/* ==========================================================================
 * 9. language table + detection
 * ========================================================================== */
typedef void (*ScanFn)(Doc *, SymList *, const char *);
typedef struct { const char *lang; const Tok *tok; ScanFn scan; int cpp; } LangDef;

static const LangDef LANGS[] = {
  { "c",      &TOK_C,    scan_c,    0 },
  { "cpp",    &TOK_C,    scan_c,    1 },
  { "go",     &TOK_GO,   scan_go,   0 },
  { "java",   &TOK_JAVA, scan_java, 0 },
  { "kotlin", &TOK_KT,   scan_java, 0 },
  { "js",     &TOK_JS,   scan_js,   0 },
  { "ts",     &TOK_JS,   scan_js,   0 },
  { "python", &TOK_PY,   scan_py,   0 },
  { "rust",   &TOK_RS,   scan_rs,   0 },
  { "text",   &TOK_TEXT, NULL,      0 },
};
#define N_LANGS ((int)(sizeof LANGS / sizeof LANGS[0]))
static const LangDef *lang_text(void) { return &LANGS[N_LANGS - 1]; }

static const struct { const char *ext, *lang; } EXTAB[] = {
  { "c", "c" }, { "h", "c" },
  { "cc", "cpp" }, { "cpp", "cpp" }, { "cxx", "cpp" }, { "hpp", "cpp" }, { "hh", "cpp" },
  { "hxx", "cpp" }, { "inl", "cpp" }, { "ipp", "cpp" },
  { "go", "go" }, { "java", "java" }, { "kt", "kotlin" }, { "kts", "kotlin" },
  { "js", "js" }, { "jsx", "js" }, { "mjs", "js" }, { "cjs", "js" },
  { "ts", "ts" }, { "tsx", "ts" }, { "mts", "ts" }, { "cts", "ts" },
  { "py", "python" }, { "pyw", "python" }, { "pyi", "python" },
  { "rs", "rust" },
  { NULL, NULL }
};
/* Extension-less basenames that name the language outright: a file called exactly
 * `go` is Go's module/main file (cmd/go resolves `go` before any extension), and no
 * other language in this table uses that bare name. */
static const struct { const char *base, *lang; } BASEAB[] = {
  { "go", "go" },
  { NULL, NULL }
};
/* accepts a canonical name, an extension, or the usual alias */
static const LangDef *lang_by_name(const char *l) {
  if (!l || !*l) return NULL;
  while (*l == '.') l++;
  for (size_t i = 0; EXTAB[i].ext; i++)
    if (!strcasecmp(l, EXTAB[i].ext)) l = EXTAB[i].lang;
  if (!strcasecmp(l, "c++") || !strcasecmp(l, "h++")) l = "cpp";
  if (!strcasecmp(l, "golang")) l = "go";
  if (!strcasecmp(l, "javascript")) l = "js";
  if (!strcasecmp(l, "typescript")) l = "ts";
  for (int i = 0; i < N_LANGS; i++) if (!strcasecmp(l, LANGS[i].lang)) return &LANGS[i];
  return NULL;
}
/* used only when the extension said nothing. Markers are unmistakable; a language
 * we cannot scan is never guessed at (ruby, php, ... stay text). */
static const LangDef *sniff(Arena *a, Str text) {
  int n = text.len > 4000 ? 4000 : text.len;
  if (n <= 0 || !text.p) return NULL;
  char *h = (char*)arena_alloc(a, (size_t)n + 1);
  for (int i = 0; i < n; i++) h[i] = (char)tolower((unsigned char)text.p[i]);
  h[n] = 0;
  if (!strncmp(h, "#!", 2)) {
    int e = 0;
    while (e < n && h[e] != '\n') e++;
    if (e > 190) e = 190;
    char sh[192];
    memcpy(sh, h, (size_t)e);
    sh[e] = 0;
    if (strstr(sh, "python")) return lang_by_name("python");
    if (strstr(sh, "deno") || strstr(sh, "node") || strstr(sh, "bun")) return lang_by_name("js");
  }
  if (strstr(h, "#[derive") || strstr(h, "println!(") || strstr(h, "fn main("))
    return lang_by_name("rust");
  if (strstr(h, "func main(") || !strncmp(h, "package main", 12) ||
      (strstr(h, "\nimport (") != NULL && strstr(h, ":=") != NULL))
    return lang_by_name("go");
  if (strstr(h, "public class ") || strstr(h, "import java.") ||
      strstr(h, "system.out.println"))
    return lang_by_name("java");
  if (strstr(h, "data class ") || strstr(h, "companion object"))
    return lang_by_name("kotlin");
  if (strstr(h, "def main(") || strstr(h, "__name__") || strstr(h, "import os"))
    return lang_by_name("python");
  if (strstr(h, "console.log(") || strstr(h, "module.exports"))
    return lang_by_name("js");
  if (strstr(h, "export interface ") || strstr(h, "export type "))
    return lang_by_name("ts");
  return NULL;
}
static const LangDef *lang_for(Arena *a, const char *path, Str text) {
  char ext[16];
  ext[0] = 0;
  if (path) path_ext(path, ext, sizeof ext);
  if (ext[0])
    for (size_t i = 0; EXTAB[i].ext; i++)
      if (!strcasecmp(ext + 1, EXTAB[i].ext)) return lang_by_name(EXTAB[i].lang);
  const char *b = path_base(path);                 /* nothing to extend: the name speaks */
  for (size_t i = 0; BASEAB[i].base; i++)
    if (!strcasecmp(b, BASEAB[i].base)) return lang_by_name(BASEAB[i].lang);
  const LangDef *s = sniff(a, text);
  return s ? s : lang_text();
}

/* ==========================================================================
 * 10. outline_file
 * ========================================================================== */
static V list_v(List *l) { V v; v.t = V_LIST; v.u.l = l; return v; }

static V sym_to_v(Doc *d, Sym *s) {
  Arena *a = d->a;
  Str at = ol_anchor(d, s->line - 1);
  return v_rec(a, 5,
    "kind", v_str(s_lit(a, s->kind)),
    "name", v_str(s_from(a, s->name, (size_t)s->name_len)),
    "line", v_num((double)s->line),
    "at",   v_str(at),
    "sig",  v_str(s_lit(a, s->sig ? s->sig : "")));
}
static List *syms_to_list(Doc *d, SymList *sl) {
  List *l = list_new(d->a);
  for (int i = 0; i < sl->n; i++) list_push(d->a, l, sym_to_v(d, &sl->v[i]));
  return l;
}

V outline_file(Ctx *c, const char *path, Str text, const char *lang) {
  if (!c) return v_err_hint(E_BAD_INPUT, s_wrap("outline_file: no context"),
                            s_wrap("call it as fs.outline(path) from a script"));
  Arena *a = ctx_arena(c);
  if (!a) return v_err_hint(E_INTERNAL, s_wrap("outline_file: context has no arena"),
                            s_wrap(err_hint(E_INTERNAL)));
  if (text.len < 0 || !text.p) text.len = 0;
  const LangDef *L;
  if (lang && *lang) { L = lang_by_name(lang); if (!L) L = lang_text(); }
  else L = lang_for(a, path, text);
  Doc d;
  doc_init(&d, a, text, L->tok);
  d.cpp = L->cpp;
  int nb = text.len > 0 ? text.len : 0;
  Str h = hash12(a, nb ? text.p : "", (size_t)nb);
  if (doc_maxline(&d) > MINIFY) {                 /* minified: no skeleton to give */
    return v_rec(a, 6,
      "path", v_str(s_lit(a, path ? path : "")),
      "lang", v_str(s_lit(a, "text")),
      "bytes", v_num((double)nb),
      "lines", v_num((double)d.n),
      "hash", v_str(h),
      "note", v_str(s_lit(a, "minified?")));
  }
  SymList sl;
  memset(&sl, 0, sizeof sl);
  if (L->scan) L->scan(&d, &sl, path ? path : "");
  V out = v_rec(a, 7,
    "path", v_str(s_lit(a, path ? path : "")),
    "lang", v_str(s_lit(a, L->lang)),
    "bytes", v_num((double)nb),
    "lines", v_num((double)d.n),
    "hash", v_str(h),
    "symbols", list_v(syms_to_list(&d, &sl)),
    "tok", v_num(0));
  if (sl.note) rec_setz(a, out.u.r, "note", v_str(s_lit(a, "symbols_limit")));
  rec_setz(a, out.u.r, "tok", v_num((double)v_tok_est(out)));
  return out;
}

/* Writes the detected language into out[16] and returns how many characters it
 * wrote (always > 0: the fallback is "text", which is never an error). */
int outline_lang_detect(const char *path, Str text, char *out) {
  if (!out) return 0;
  out[0] = 0;
  Str t = text;
  if (t.len < 0 || !t.p) { t.len = 0; t.p = NULL; }
  static Arena scratch;                            /* sniffing only; kept bounded */
  if (!scratch.cur) arena_init(&scratch, 1u << 20);
  const LangDef *L = lang_for(&scratch, path, t);
  snprintf(out, 16, "%s", L ? L->lang : "text");
  if (scratch.total > (1u << 19)) { arena_free(&scratch); arena_init(&scratch, 1u << 20); }
  return (int)strlen(out);
}

/* ==========================================================================
 * 11. bundle_files
 * ========================================================================== */
typedef struct {
  Arena *a;
  char  *path;
  const LangDef *L;
  Doc    d;
  SymList sl;
  Str    text, hash, body;
  int    bytes, lines, nsym, hits;
} Bi;

static void linef(Buf *b, const char *fmt, ...) {
  char tmp[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n >= (int)sizeof tmp) n = (int)sizeof tmp - 1;
  if (b->len) buf_putc(b, '\n');
  buf_put(b, tmp, (size_t)n);
}
static int line_hit(Doc *d, int ln, const char *q, int ql) {
  Str x = doc_line(d, ln);
  if (ql <= 0 || x.len < ql) return 0;
  for (int i = 0; i + ql <= x.len; i++) {
    int ok = 1;
    for (int j = 0; j < ql; j++) {
      if (tolower((unsigned char)x.p[i+j]) != tolower((unsigned char)q[j])) { ok = 0; break; }
    }
    if (ok) return 1;
  }
  return 0;
}
static int count_hits(Doc *d, const char *q) {
  if (!q || !*q) return 0;
  int ql = (int)strlen(q), n = 0;
  for (int i = 0; i < d->n; i++) if (line_hit(d, i, q, ql)) n++;
  return n;
}
/* the body: symbol lines, then (query mode) matched lines with 2 of context.
 * Every line carries L<no>:<at>| so an anchor pastes straight into tx.patch. */
static Str bundle_body(Arena *a, Doc *d, SymList *sl, const char *query, int emit_hits) {
  Buf b; buf_init(&b, a);
  for (int i = 0; i < sl->n; i++) {
    Sym *s = &sl->v[i];
    Str at = ol_anchor(d, s->line - 1);
    linef(&b, "L%d:%.*s| %s %s", s->line, at.len, at.p ? at.p : "-", s->kind, s->name);
  }
  if (!emit_hits) return buf_take(&b);
  int ql = query ? (int)strlen(query) : 0;
  if (ql <= 0 || d->n <= 0) return buf_take(&b);
  int *hit = (int*)arena_alloc(a, sizeof(int) * (size_t)d->n);
  int nh = 0;
  for (int i = 0; i < d->n; i++) if (line_hit(d, i, query, ql)) hit[nh++] = i;
  if (!nh) return buf_take(&b);
  linef(&b, "##hits=%d", nh);
  int prev_hi = -1;
  for (int k = 0; k < nh; k++) {
    int lo = hit[k] - HIT_CTX, hi = hit[k] + HIT_CTX;
    if (lo < 0) lo = 0;
    if (hi > d->n - 1) hi = d->n - 1;
    if (lo <= prev_hi) lo = prev_hi + 1;           /* merge overlapping windows */
    prev_hi = hi;
    for (int i = lo; i <= hi; i++) {
      Str x = doc_line(d, i);
      Str at = ol_anchor(d, i);
      linef(&b, "L%d:%.*s| %.*s", i + 1, at.len, at.p ? at.p : "-",
            x.len, x.len > 0 ? x.p : "");
    }
  }
  return buf_take(&b);
}
/* line-granular fit into `avail` bytes: whole lines only, cut marked, -1 if even
 * the first line will not fit (the caller then drops the file with a reason). */
static int fit_body(Bi *it, int avail) {
  Arena *a = it->a;
  Str body = it->body;
  if (body.len + 1 <= avail) return body.len + 1;
  int nl = 1;
  for (int i = 0; i < body.len; i++) if (body.p[i] == '\n') nl++;
  int *lo = (int*)arena_alloc(a, sizeof(int) * (size_t)nl);
  int *lnlen = (int*)arena_alloc(a, sizeof(int) * (size_t)nl);
  int k = 0, o = 0;
  lo[0] = 0;
  for (int i = 0; i < body.len; i++) {
    if (body.p[i] == '\n') { lnlen[k++] = i - o; o = i + 1; lo[k] = o; }
  }
  lnlen[k++] = body.len - o;
  char mk[48];
  for (int keep = nl - 1; keep > 0; keep--) {
    int used = 0;
    for (int i = 0; i < keep; i++) used += lnlen[i] + 1;
    snprintf(mk, sizeof mk, "%s%d", TRUNC_TAG, nl - keep);
    int cost = used + (int)strlen(mk) + 1;
    if (cost > avail) continue;
    Buf b; buf_init(&b, a);
    for (int i = 0; i < keep; i++) {
      if (i) buf_putc(&b, '\n');
      buf_put(&b, body.p + lo[i], (size_t)lnlen[i]);
    }
    buf_putc(&b, '\n');
    buf_puts(&b, mk);
    it->body = buf_take(&b);
    return it->body.len + 1;
  }
  return -1;
}
/* relevance first: query hits, then how much structure a file has, then path so
 * the order is total and never depends on the caller's list order or the fs. */
static int bi_cmp(const void *x, const void *y) {
  const Bi *a = (const Bi*)x, *b = (const Bi*)y;
  if (a->hits != b->hits) return a->hits > b->hits ? -1 : 1;
  if (a->nsym != b->nsym) return a->nsym > b->nsym ? -1 : 1;
  return strcmp(a->path ? a->path : "", b->path ? b->path : "");
}

V bundle_files(Ctx *c, V paths, const char *query, int max_bytes, bool only_symbols) {
  if (!c) return v_err_hint(E_BAD_INPUT, s_wrap("bundle_files: no context"),
                            s_wrap("fs.bundle([paths], {max_bytes, query, only}) takes the paths list first"));
  Arena *a = ctx_arena(c);
  if (!a) return v_err_hint(E_INTERNAL, s_wrap("bundle_files: context has no arena"),
                            s_wrap(err_hint(E_INTERNAL)));
  if (paths.t != V_LIST || !paths.u.l)
    return v_err_hint(E_TYPE, s_wrap("bundle_files: paths must be a list of strings"),
                      s_wrap("pass [path, ...] as the first argument"));
  List *pl = paths.u.l;
  if (query && !*query) query = NULL;
  /* cap is a HARD byte budget over the assembled bodies (+1 byte per entry for the
   * newline that joins them); the emitted `bytes` never exceeds `cap`. */
  int cap = max_bytes > 0 ? max_bytes : 0;
  int budget = cap > 0 ? cap - 1 : 0;

  Bi *items = (Bi*)arena_zalloc(a, sizeof(Bi) * (size_t)(pl->len > 0 ? pl->len : 1));
  int ni = 0;
  Buf dl; buf_init(&dl, a);                       /* dropped rows: path \t reason */
  for (int i = 0; i < pl->len; i++) {
    V pv = pl->v[i];
    if (pv.t != V_STR || !pv.u.s.p) { linef(&dl, "-\tnot-a-path"); continue; }
    Str ps = s_trim(a, pv.u.s);
    char *path = arena_strndup(a, ps.p, (size_t)ps.len);
    int dup = 0;
    for (int z = 0; z < ni; z++) if (!strcmp(items[z].path, path)) { dup = 1; break; }
    if (dup) { linef(&dl, "%s\tduplicate", path); continue; }
    size_t n = 0;
    ErrCode se = E_NONE;
    char *data = slurp_at ? slurp_at(a, path, &n, &se) : NULL;
    if (!data) { linef(&dl, "%s\tread:%s", path, err_name(se == E_NONE ? E_IO : se)); continue; }
    Bi *it = &items[ni++];
    it->a = a;
    it->path = path;
    it->text = s_from(a, data, n);
    it->L = lang_for(a, path, it->text);
    doc_init(&it->d, a, it->text, it->L->tok);
    it->d.cpp = it->L->cpp;
    it->bytes = (int)n;
    it->lines = it->d.n;
    it->hash = hash12(a, data, n);
    if (doc_maxline(&it->d) <= MINIFY && it->L->scan) it->L->scan(&it->d, &it->sl, path);
    it->nsym = it->sl.n;
    it->hits = query ? count_hits(&it->d, query) : 0;
    it->body = bundle_body(a, &it->d, &it->sl, query, query != NULL && !only_symbols);
  }
  if (ni > 1) qsort(items, (size_t)ni, sizeof(Bi), bi_cmp);

  List *bundle = list_new(a);
  int used = 0;
  for (int i = 0; i < ni; i++) {
    Bi *it = &items[i];
    int need = it->body.len + 1;
    if (used + need > budget) {
      need = fit_body(it, budget - used);
      if (need < 0) { linef(&dl, "%s\tbudget", it->path); continue; }
    }
    used += need;
    Rec *r = rec_new(a);
    rec_setz(a, r, "path", v_str(s_lit(a, it->path)));
    rec_setz(a, r, "lang", v_str(s_lit(a, it->L->lang)));
    rec_setz(a, r, "bytes", v_num((double)it->bytes));
    rec_setz(a, r, "hash", v_str(it->hash));
    rec_setz(a, r, "lines", v_num((double)it->lines));
    rec_setz(a, r, "hits", v_num((double)it->hits));
    rec_setz(a, r, "text", v_str(it->body));
    rec_setz(a, r, "symbols", list_v(syms_to_list(&it->d, &it->sl)));
    list_push(a, bundle, rec_to_v(r));
  }
  List *drop = list_new(a);
  {
    Str all = buf_take(&dl);
    int o = 0;
    while (o < all.len) {
      int e = o;
      while (e < all.len && all.p[e] != '\n') e++;
      Str row = s_slice(all, o, e);
      Str tab = s_wrap("\t");
      int t = s_rfind(row, tab);
      if (t > 0) {
        Str pth = s_slice(row, 0, t);
        Str rea = s_slice(row, t + 1, row.len);
        if (pth.len > 0)
          list_push(a, drop, v_rec(a, 2,
            "path", v_str(s_from(a, pth.p, (size_t)pth.len)),
            "reason", v_str(s_from(a, rea.p, (size_t)rea.len))));
      }
      o = e + 1;
    }
  }
  return v_rec(a, 6,
    "bundle", list_v(bundle),
    "dropped", list_v(drop),
    "bytes", v_num((double)used),
    "cap", v_num((double)cap),
    "query", v_str(s_lit(a, query ? query : "-")),
    "tok", v_num((double)((used + 3) / 4)));
}


