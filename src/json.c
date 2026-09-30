/* json.c - SPEC 4.5: strict JSON parsing and rendering.
 *
 * The product's axioms, as they apply here:
 *  - Verifiable beats tolerant. Every malformed byte is an ERR with a position;
 *    nothing is repaired, skipped or inferred. Duplicate keys are legal JSON, so
 *    they are not an error - but they are reported (dupes=N), because a silently
 *    collapsed key is exactly the wrong premise for an agent's next step.
 *  - Output is one line. Error values therefore carry offset/line/col/near/path
 *    as flat ERR data (SPEC 2.1 flattens it), so one emitted line is enough to go
 *    and fix the input.
 *  - No crash, no exit, no abort: budgets are checked BEFORE allocating, so a
 *    pathological file yields E_LIMIT instead of reaching arena_alloc's abort().
 *  - All allocation comes from the caller's Arena; there is no file-scope state.
 */
#include "vxa.h"
#include "json.h"
#include <string.h>
#include <stdio.h>                  /* snprintf */
#include <stdarg.h>
#include <stdlib.h>                 /* strtod */
#include <math.h>

#define KEYLIST_MAX 8               /* how many keys a "no such key" line names */

/* ---------------- limits ---------------- */
JsonLimits json_limits_default(void) {
  JsonLimits L;
  L.max_depth = JSON_DEPTH_DEFAULT;
  L.max_bytes = JSON_BYTES_DEFAULT;
  L.max_nodes = JSON_NODES_DEFAULT;
  return L;
}
/* config.c seeds these, so `vxa --set json.max_depth=256` is the way an agent
 * raises a limit for a whole run instead of repeating an option per call.
 * A value that is not a number, or is smaller than 1, is ignored: a typo in a
 * config file must not turn a limit into "unlimited". */
V cfg_get_c(Ctx *c, const char *k);
JsonLimits json_limits(Ctx *c) {
  JsonLimits L = json_limits_default();
  if (!c) return L;
  V v;
  v = cfg_get_c(c, "json.max_depth");
  if (v.t == V_NUM && v.u.n >= 1 && isfinite(v.u.n)) {
    L.max_depth = (int)v.u.n;
    if (L.max_depth > JSON_DEPTH_HARD_MAX) L.max_depth = JSON_DEPTH_HARD_MAX;
  }
  v = cfg_get_c(c, "json.max_bytes");
  if (v.t == V_NUM && v.u.n >= 1 && isfinite(v.u.n)) L.max_bytes = (long long)v.u.n;
  v = cfg_get_c(c, "json.max_nodes");
  if (v.t == V_NUM && v.u.n >= 1 && isfinite(v.u.n)) L.max_nodes = (long long)v.u.n;
  return L;
}

/* One actionable hint per failure class: something to do, not something to read. */
#define H_SYN   "vxa does not repair JSON: fix the bytes at offset=; no trailing comma, single quote, comment or NaN"
#define H_DEPTH "nesting is deeper than the limit: pass {max_depth:N}, or read one subtree with json.query"
#define H_BYTES "input is bigger than the cap: pass {max_bytes:N}, or slice it first with fs.read"
#define H_NODES "more values than the cap: pass {max_nodes:N}, or select one subtree with json.query"
#define H_RANGE "number does not fit a double and infinity is not JSON: keep it as a string"
#define H_MISS  "no value at that path: use one of the keys or indexes named on this line"
#define H_PATH  "path forms are a.b, items[2].name and x[\"a.b\"]; nothing is matched fuzzily"
#define H_TYPE  "json.parse and json.query take the JSON text as a str - pass text, not a value"
#define H_OPT   "options are {meta,pretty,max_depth,max_bytes,max_nodes}: one of them is misspelled"
#define H_DEF   "-"                 /* use the standard hint for this code */

/* ================= dotted paths =================
 *   a  a.b         record key
 *   [2] a[2].b     list index (a bracketed integer is only ever an index)
 *   2  a.2         bare integer: an index on a list, the key "2" on a record,
 *                  because JSON records really do have keys like "2"
 *   ["a.b"]        a key that itself contains a dot, a bracket or a quote */
typedef struct { Str name; long long idx; bool isnum; bool brk; } JSeg;
typedef struct { int n; Str raw; JSeg s[JSON_MAX_SEGS]; } JPath;

static int hexv(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static bool ident_byte(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool jpath_add(JPath *p, const char *s, int n, bool brk, const char **bad) {
  if (p->n >= JSON_MAX_SEGS) { *bad = "more segments than JSON_MAX_SEGS"; return false; }
  if (n == 0 && !brk) { *bad = "empty path segment"; return false; }
  JSeg g;
  memset(&g, 0, sizeof g);
  g.name.p = (char*)s; g.name.len = n; g.brk = brk;
  bool numeric = n > 0;
  for (int i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') numeric = false;
  if (numeric && n <= 9) {                        /* longer than 9 digits: a key, never an index */
    long long v = 0;
    for (int i = 0; i < n; i++) v = v * 10 + (s[i] - '0');
    g.idx = v; g.isnum = true;
  }
  p->s[p->n++] = g;
  return true;
}

static bool jpath_parse(JPath *p, Str dotted, const char **bad) {
  memset(p, 0, sizeof *p);
  p->raw = dotted;
  *bad = NULL;
  if (!dotted.p || dotted.len <= 0) { *bad = "the path is empty"; return false; }
  const char *s = dotted.p;
  int n = dotted.len, i = 0;
  bool after_bracket = false;                     /* after ] a '.' is optional */
  while (i < n) {
    char c = s[i];
    if (c == '[') {
      int j = i + 1;
      if (j >= n) { *bad = "'[' is not closed"; return false; }
      if (s[j] == '"') {
        int k = j + 1;
        while (k < n && s[k] != '"') k++;
        if (k >= n) { *bad = "unterminated \" inside [ ]"; return false; }
        if (k + 1 >= n || s[k + 1] != ']') { *bad = "expected ] after the quoted key"; return false; }
        if (!jpath_add(p, s + j + 1, k - j - 1, true, bad)) return false;
        i = k + 2; after_bracket = true;
        continue;
      }
      int k = j;
      while (k < n && s[k] >= '0' && s[k] <= '9') k++;
      if (k == j) { *bad = "[ ] holds neither an index nor a quoted key"; return false; }
      if (k >= n || s[k] != ']') { *bad = "expected ] after the index"; return false; }
      if (!jpath_add(p, s + j, k - j, true, bad)) return false;
      i = k + 1; after_bracket = true;
      continue;
    }
    if (c == ']') { *bad = "']' without '['"; return false; }
    if (c == '.') {
      if (i == 0) { *bad = "a path does not start with '.'"; return false; }
      if (i + 1 >= n) { *bad = "the path ends with '.'"; return false; }
      if (s[i + 1] == '.') { *bad = "two '.' in a row: a segment cannot be empty"; return false; }
      if (i + 1 < n && s[i + 1] == ']') { *bad = "']' without '['"; return false; }
      i++; after_bracket = false;
      continue;
    }
    if (i > 0 && !after_bracket && s[i - 1] != '.') { *bad = "expected '.' or '[' between segments"; return false; }
    int j = i;
    while (j < n && s[j] != '.' && s[j] != '[' && s[j] != ']') j++;
    if (j == i) { *bad = "empty path segment"; return false; }
    if (!jpath_add(p, s + i, j - i, false, bad)) return false;
    i = j; after_bracket = false;
  }
  if (p->n == 0) { *bad = "the path has no segments"; return false; }
  return true;
}
static bool seg_is_index_only(const JSeg *g) { return g->brk && g->isnum; }

static Str seg_render(Arena *a, const JSeg *g) {
  Buf b; buf_init(&b, a);
  if (seg_is_index_only(g)) buf_fmt(&b, "[%lld]", g->idx);
  else if (g->brk) { buf_putc(&b, '['); buf_json_str(&b, g->name); buf_putc(&b, ']'); }
  else { buf_putc(&b, '.'); buf_put(&b, g->name.p, (size_t)g->name.len); }
  return buf_take(&b);
}

/* ================= error values =================
 * The msg has one fixed shape - "<problem> at offset=N line=L col=C" - and the
 * same numbers ride as flat ERR data, so the emitted line reads as machine fields. */
static V mk_err(Arena *a, ErrCode code, const char *msg, const char *hint, Rec *data) {
  const char *h = (hint && *hint && strcmp(hint, H_DEF)) ? hint : err_hint(code);
  if (!a) return v_err_hint(code, s_wrap(msg), s_wrap(h));   /* literals only */
  PErr *e = (PErr*)arena_zalloc(a, sizeof(PErr));
  e->code = code; e->msg = s_lit(a, msg); e->hint = s_lit(a, h);
  e->data = data ? rec_to_v(data) : VN;
  V v; v.t = V_ERR; v.u.e = e; return v;
}
static V mk_errf(Arena *a, ErrCode code, const char *hint, const char *fmt, ...) {
  char body[320];
  va_list ap; va_start(ap, fmt); vsnprintf(body, sizeof body, fmt, ap); va_end(ap);
  return mk_err(a, code, body, hint, NULL);
}

/* ================= parser ================= */
enum { J_FULL, J_SKIP, J_SEL };

typedef struct {
  Arena *a;
  const char *p; size_t n, i;
  JsonLimits L;
  JsonMeta *M;
  Str *snip;                                /* out-param: the escaped region */
  int depth;
  Str *ckey; long long *cidx; char *ctag;   /* current member of each open container */
} J;

static int j_at(J *j, size_t k) { return k < j->n ? (unsigned char)j->p[k] : -1; }
static void j_ws(J *j) {
  while (j->i < j->n) {
    char c = j->p[j->i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->i++;
    else break;
  }
}
/* RFC 8259 whitespace only. \f and \v are NOT JSON whitespace: a file leaning on
 * them is refused rather than accepted by half the tools and rejected by the rest. */

static void j_linecol(J *j, size_t off, int *line, int *col) {
  size_t start = 0;
  int ln = 1;
  if (off > j->n) off = j->n;
  for (size_t k = 0; k < off; k++) if (j->p[k] == '\n') { ln++; start = k + 1; }
  *line = ln; *col = (int)(off - start) + 1;
}

static void snip_put(Buf *b, unsigned char c) {
  switch (c) {
    case '"': buf_puts(b, "\\\""); break;
    case '\\': buf_puts(b, "\\\\"); break;
    case '\n': buf_puts(b, "\\n"); break;
    case '\r': buf_puts(b, "\\r"); break;
    case '\t': buf_puts(b, "\\t"); break;
    default:
      /* the emitter's own escape table, so a snippet byte can be pasted back */
      if (c < 0x20 || c == 0x7f) buf_fmt(b, "\\x%02x", c);
      else buf_putc(b, (char)c);
  }
}
/* The offending region, escaped, at most JSON_SNIPPET_MAX chars, never a raw
 * newline. Cuts on whole-escape boundaries: a truncated tail must not read as a
 * different byte than the one that broke the parse. */
static Str j_snip(J *j, size_t off) {
  if (!j->p || !j->n) return s_null();
  if (off > j->n) off = j->n;
  size_t f = off > 30 ? off - 30 : 0;
  size_t t = off + 29 < j->n ? off + 29 : j->n;
  bool front = f > 0, back = t < j->n;
  size_t room = (size_t)JSON_SNIPPET_MAX - (front ? 3u : 0u) - (back ? 3u : 0u);
  Buf b; buf_init(&b, j->a);
  for (size_t k = f; k < t; k++) {
    size_t before = b.len;
    snip_put(&b, (unsigned char)j->p[k]);
    if (b.len > room) { b.len = before; break; }
  }
  if (!b.len) {                                     /* a lone escape is the snippet */
    snip_put(&b, (unsigned char)j->p[off < j->n ? off : j->n - 1]);
    front = false; back = false;
  }
  Buf o; buf_init(&o, j->a);
  if (front) buf_puts(&o, "...");
  buf_put(&o, b.p, b.len);
  if (back) buf_puts(&o, "...");
  return buf_take(&o);
}

static Str j_path(J *j) {
  if (j->depth <= 0) return s_wrap("$");
  Buf b; buf_init(&b, j->a);
  buf_putc(&b, '$');
  for (int d = 1; d <= j->depth; d++) {
    if (j->ctag[d] == 'a') buf_fmt(&b, "[%lld]", j->cidx[d]);
    else if (j->ctag[d] == 'o') { buf_putc(&b, '.'); if (j->ckey[d].p) buf_put(&b, j->ckey[d].p, (size_t)j->ckey[d].len); }
    else break;
    if (b.len > JSON_PATH_MAX) { buf_puts(&b, "..."); break; }
  }
  return buf_take(&b);
}

/* what the parser choked on */
static Str j_here(J *j, char *tmp, size_t tsz) {
  if (j->i >= j->n) { snprintf(tmp, tsz, "end of input"); return s_wrap(tmp); }
  unsigned char c = (unsigned char)j->p[j->i];
  if (c >= 0x80) { snprintf(tmp, tsz, "byte 0x%02x", c); return s_wrap(tmp); }
  if (c < 0x20 || c == 0x7f) { snprintf(tmp, tsz, "control byte 0x%02x", c); return s_wrap(tmp); }
  if (ident_byte(c)) {
    size_t k = j->i;
    while (k < j->n && k < j->i + 16 && ident_byte(j->p[k])) k++;
    int w = (int)(k - j->i);
    if ((size_t)w + 3 >= tsz) w = (int)tsz - 3;
    tmp[0] = '\'';
    memcpy(tmp + 1, j->p + j->i, (size_t)w);
    tmp[1 + w] = '\''; tmp[2 + w] = 0;
    return s_wrap(tmp);
  }
  snprintf(tmp, tsz, "'%c'", (char)c);
  return s_wrap(tmp);
}

static V jf_at(J *j, ErrCode code, size_t off, const char *hint, Str limkey, long long limv, const char *body) {
  int line = 0, col = 0;
  j_linecol(j, off, &line, &col);
  Str near = j_snip(j, off);
  Buf m; buf_init(&m, j->a);
  buf_fmt(&m, "%s at offset=%lld line=%d col=%d", body, (long long)off, line, col);
  Str msg = buf_take(&m);
  Rec *d = rec_new(j->a);
  rec_setz(j->a, d, "offset", v_num((double)off));
  rec_setz(j->a, d, "line", v_num(line));
  rec_setz(j->a, d, "col", v_num(col));
  if (near.len) rec_setz(j->a, d, "near", v_str(near));
  if (j->depth > 0) rec_setz(j->a, d, "path", v_str(j_path(j)));
  if (limkey.len) rec_setz(j->a, d, limkey.p, v_num((double)limv));
  if (j->snip) *j->snip = near;
  return mk_err(j->a, code, msg.p ? msg.p : "", hint, d);
}
static V j_fail(J *j, ErrCode code, size_t off, const char *hint, const char *fmt, ...) {
  char body[256];
  va_list ap; va_start(ap, fmt); vsnprintf(body, sizeof body, fmt, ap); va_end(ap);
  return jf_at(j, code, off, hint, s_null(), 0, body);
}
static V j_fail_lim(J *j, ErrCode code, size_t off, const char *hint,
                    const char *limkey, long long limv, const char *fmt, ...) {
  char body[256];
  va_list ap; va_start(ap, fmt); vsnprintf(body, sizeof body, fmt, ap); va_end(ap);
  return jf_at(j, code, off, hint, s_wrap(limkey), limv, body);
}
/* a selection that found nothing: name the path, the segment and what was there */
static V j_miss(J *j, size_t off, const JPath *want, int wi, const char *why, Str keys, long long count) {
  char hintbuf[280];
  if (keys.len) snprintf(hintbuf, sizeof hintbuf, "keys here: %.*s", keys.len, keys.p ? keys.p : "");
  else if (count > 0) snprintf(hintbuf, sizeof hintbuf, "index it: [0]..[%lld]", count - 1);
  else if (count == 0) snprintf(hintbuf, sizeof hintbuf, "the list is empty");
  else snprintf(hintbuf, sizeof hintbuf, "%s", H_MISS);
  Str seg = wi < want->n ? want->s[wi].name : s_null();
  return j_fail(j, E_NOENT, off, hintbuf, "path %.*s stops: %s \"%.*s\"",
                want->raw.len, want->raw.p ? want->raw.p : "", why ? why : "nothing here matches",
                seg.len, seg.p ? seg.p : "");
}

/* refuse before arena_alloc can abort() */
static bool j_room(J *j, size_t need) {
  if (!j->a || j->a->limit == 0) return true;
  if (j->a->limit < j->a->total) return false;
  return j->a->limit - j->a->total > need + (256u * 1024u);
}
/* nodes counts what was BUILT: a skip-walk validates without materialising,
 * so json.query's count is the selected subtree, not the whole document */
static V j_node(J *j, size_t off) {
  if (!j->M) return VN;
  j->M->nodes++;
  if (j->M->nodes <= j->L.max_nodes) return VN;
  return j_fail_lim(j, E_LIMIT, off, H_NODES, "max_nodes", j->L.max_nodes,
                    "the document holds more values than the cap %lld", j->L.max_nodes);
}

/* ---------------- UTF-8 ---------------- */
/* Length of a well-formed sequence, or 0. Over-long encodings, encoded
 * surrogates, > U+10FFFF, bad continuations and truncation are all refused:
 * a replacement character would hide an encoding bug in an otherwise real file. */
static int utf8_seq(const char *p, size_t avail, unsigned *cp) {
  unsigned char c0 = (unsigned char)p[0];
  if (c0 < 0x80) { *cp = c0; return 1; }
  if (c0 < 0xC2 || c0 > 0xF4) return 0;
  int len = c0 < 0xE0 ? 2 : (c0 < 0xF0 ? 3 : 4);
  if ((size_t)len > avail) return 0;
  unsigned v = c0 & (len == 2 ? 0x1Fu : (len == 3 ? 0x0Fu : 0x07u));
  for (int k = 1; k < len; k++) {
    unsigned char ck = (unsigned char)p[k];
    unsigned lo = 0x80, hi = 0xBF;
    if (k == 1) {
      if (c0 == 0xE0) lo = 0xA0;                          /* not over-long */
      else if (c0 == 0xED) hi = 0x9F;                     /* no surrogates */
      else if (c0 == 0xF0) lo = 0x90;
      else if (c0 == 0xF4) hi = 0x8F;
    }
    if (ck < lo || ck > hi) return 0;
    v = (v << 6) | (ck & 0x3F);
  }
  if (v > 0x10FFFF) return 0;
  *cp = v;
  return len;
}
static void put_utf8(Buf *b, unsigned cp) {
  if (cp < 0x80) { buf_putc(b, (char)cp); return; }
  if (cp < 0x800) {
    buf_putc(b, (char)(0xC0 | (cp >> 6)));
    buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    return;
  }
  if (cp < 0x10000) {
    buf_putc(b, (char)(0xE0 | (cp >> 12)));
    buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    return;
  }
  buf_putc(b, (char)(0xF0 | (cp >> 18)));
  buf_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
  buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
  buf_putc(b, (char)(0x80 | (cp & 0x3F)));
}

/* ---------------- tokens ---------------- */
/* j->i is on the opening quote; returns past the closing one. out == NULL decodes
 * into nothing, which is how the skip-walk validates a string it does not want. */
static V j_string(J *j, Buf *out) {
  size_t open = j->i;
  j->i++;
  for (;;) {
    if (j->i >= j->n)
      return j_fail(j, E_PARSE, open + 1, H_SYN, "unterminated string: no closing quote before the input ended");
    unsigned char c = (unsigned char)j->p[j->i];
    if (c == '"') { j->i++; return VN; }
    if (c < 0x20) {
      size_t at = j->i; j->i++;
      return j_fail(j, E_PARSE, at, H_SYN, "raw control byte 0x%02x inside a string: escape it", c);
    }
    if (c != '\\') {
      if (c < 0x80) { if (out) buf_putc(out, (char)c); j->i++; continue; }
      unsigned cp = 0;
      int w = utf8_seq(j->p + j->i, j->n - j->i, &cp);
      if (w <= 0)
        return j_fail(j, E_PARSE, j->i, H_SYN,
                      "invalid UTF-8 byte 0x%02x in a string: text bytes pass through, they are never repaired", c);
      if (out) buf_put(out, j->p + j->i, (size_t)w);
      j->i += (size_t)w;
      continue;
    }
    size_t esc = j->i;
    if (j->i + 1 >= j->n) { j->i = j->n; return j_fail(j, E_PARSE, esc, H_SYN, "the input ends inside an escape"); }
    char e = j->p[j->i + 1];
    j->i += 2;
    if (e == 'u') {
      unsigned cp = 0;
      for (int k = 0; k < 4; k++) {
        int hx = j_at(j, j->i + (size_t)k);
        int v = hx < 0 ? -1 : hexv(hx);
        if (v < 0) {
          char got[8]; int w = 0;
          while (w < 4 && j_at(j, j->i + (size_t)w) >= 0) { got[w] = (char)j_at(j, j->i + (size_t)w); w++; }
          got[w] = 0;
          return j_fail(j, E_PARSE, j->i, H_SYN, "\\u needs 4 hex digits (found \"%s\")", got);
        }
        cp = (cp << 4) | (unsigned)v;
      }
      j->i += 4;
      if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (j_at(j, j->i) != '\\' || j_at(j, j->i + 1) != 'u')
          return j_fail(j, E_PARSE, esc, H_SYN,
                        "lone high surrogate \\u%04X: a code point above U+FFFF needs a \\uD800-\\uDBFF half "
                        "followed by a \\uDC00-\\uDFFF half", cp);
        size_t save = j->i;
        j->i += 2;
        unsigned lo = 0; bool ok = true;
        for (int k = 0; k < 4; k++) {
          int hx = j_at(j, j->i + (size_t)k);
          int v = hx < 0 ? -1 : hexv(hx);
          if (v < 0) { ok = false; break; }
          lo = (lo << 4) | (unsigned)v;
        }
        if (!ok) return j_fail(j, E_PARSE, save, H_SYN, "the low half of a surrogate pair needs 4 hex digits");
        j->i += 4;
        if (lo < 0xDC00 || lo > 0xDFFF)
          return j_fail(j, E_PARSE, save, H_SYN,
                        "a surrogate pair needs a low half \\uDC00-\\uDFFF (found \\u%04X)", lo);
        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
      } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        return j_fail(j, E_PARSE, esc, H_SYN, "lone low surrogate \\u%04X", cp);
      }
      if (out) put_utf8(out, cp);
      continue;
    }
    char raw = 0;
    switch (e) {
      case '"': raw = '"'; break;
      case '\\': raw = '\\'; break;
      case '/': raw = '/'; break;
      case 'b': raw = '\b'; break;
      case 'f': raw = '\f'; break;
      case 'n': raw = '\n'; break;
      case 'r': raw = '\r'; break;
      case 't': raw = '\t'; break;
      case 'x': {
        /* VXA's one documented extension: buf_json_str writes \xNN for control
         * bytes, so a parser that refused them could not read this project's own
         * output back. Capped at 7f so a decoded string stays valid UTF-8. */
        int h1 = hexv(j_at(j, j->i)), h2 = hexv(j_at(j, j->i + 1));
        if (h1 < 0 || h2 < 0) {
          size_t at = j->i;
          j->i += 2;
          return j_fail(j, E_PARSE, at, H_SYN, "\\x needs 2 hex digits");
        }
        int v = h1 * 16 + h2;
        j->i += 2;
        if (v > 0x7f)
          return j_fail(j, E_PARSE, j->i - 3, H_SYN,
                        "\\x%02x out of range: \\x covers control bytes only, write text as UTF-8", v);
        if (out) buf_putc(out, (char)v);
        continue;
      }
      default:
        return j_fail(j, E_PARSE, esc + 1, H_SYN, "unknown escape '\\%c' (the six JSON escapes, \\uXXXX and \\xNN are all there is)", e);
    }
    if (out) buf_putc(out, raw);
  }
}

/* RFC 8259 number grammar and nothing looser: no +5, no 5., no .5, no 01, no 0x1F */
static bool j_num_text(J *j, size_t *len_out) {
  size_t s = j->i;
  if (j_at(j, j->i) == '-') j->i++;
  int c = j_at(j, j->i);
  if (c == '0') {
    j->i++;
    int d = j_at(j, j->i);
    if (d >= '0' && d <= '9') { j->i = s; return false; }   /* 00 01 0123: leading zero */
  } else if (c >= '1' && c <= '9') {
    while (j_at(j, j->i) >= '0' && j_at(j, j->i) <= '9') j->i++;
  } else { j->i = s; return false; }
  if (j_at(j, j->i) == '.') {
    j->i++;
    if (!(j_at(j, j->i) >= '0' && j_at(j, j->i) <= '9')) { j->i = s; return false; }
    while (j_at(j, j->i) >= '0' && j_at(j, j->i) <= '9') j->i++;
  }
  if (j_at(j, j->i) == 'e' || j_at(j, j->i) == 'E') {
    j->i++;
    if (j_at(j, j->i) == '+' || j_at(j, j->i) == '-') j->i++;
    if (!(j_at(j, j->i) >= '0' && j_at(j, j->i) <= '9')) { j->i = s; return false; }
    while (j_at(j, j->i) >= '0' && j_at(j, j->i) <= '9') j->i++;
  }
  if (len_out) *len_out = j->i - s;
  return true;
}

/* the message for "this is not a value", spelled out for the forms that are
 * common in the wild precisely because other readers let them through */
static V j_notavalue(J *j, size_t s) {
  int a = j_at(j, s), b = j_at(j, s + 1);
  bool sign = (a == '-' || a == '+');
  int w = sign ? b : a;
  if (sign && (w == 'I'))
    return j_fail(j, E_PARSE, s, H_SYN, "-Infinity and Infinity are not JSON: no number here is representable (write null, or a string)");
  if (sign && (w == 'N'))
    return j_fail(j, E_PARSE, s, H_SYN, "NaN is not JSON: it has no JSON form (write null, or a string)");
  if (a == 'N' || (a == 'I' && (b == 'n' || b == 'N')))
    return j_fail(j, E_PARSE, s, H_SYN, "%c is not JSON: NaN and Infinity have no JSON form (write null, or a string)", a);
  if (a == '0' && (j_at(j, s + 1) == 'x' || j_at(j, s + 1) == 'X'))
    return j_fail(j, E_PARSE, s + 1, H_SYN, "hex is not JSON: 0x1F must be written 31 (VXA source allows hex, data does not)");
  if (a == '0' && j_at(j, s + 1) >= '0' && j_at(j, s + 1) <= '9')
    return j_fail(j, E_PARSE, s + 1, H_SYN, "leading zero: 01 is not a number, write 1");
  if (a == '\'')
    return j_fail(j, E_PARSE, s, H_SYN, "single quotes are not JSON: a string needs double quotes");
  if (a == '`')
    return j_fail(j, E_PARSE, s, H_SYN, "backticks are not JSON: a string needs double quotes");
  if (a == '/' && (j_at(j, s + 1) == '/' || j_at(j, s + 1) == '*'))
    return j_fail(j, E_PARSE, s, H_SYN, "comments are not JSON: nothing is stripped, because a stripped byte is a changed file");
  if (a == '+')
    return j_fail(j, E_PARSE, s, H_SYN, "a number never starts with '+': drop the sign");
  {
    char tmp[24]; Str here = j_here(j, tmp, sizeof tmp);
    return j_fail(j, E_PARSE, s, H_SYN, "expected a value (object, array, string, number, true, false, null), got %.*s",
                  here.len, here.p);
  }
}

static V j_number(J *j, bool mat, size_t s) {
  size_t len = 0;
  if (!j_num_text(j, &len)) return j_notavalue(j, s);
  if (mat) { V nc = j_node(j, s); if (v_is_err(nc)) return nc; }
  if (!mat) return VN;
  char small[64];
  char *txt = small;
  if (len + 1 > sizeof small) {
    if (!j_room(j, len + 64))
      return j_fail_lim(j, E_LIMIT, s, H_NODES, "max_nodes", j->L.max_nodes, "this number text is too large to keep");
    txt = (char*)arena_alloc(j->a, len + 1);
  }
  memcpy(txt, j->p + s, len); txt[len] = 0;
  char *endp = NULL;
  double d = strtod(txt, &endp);                /* C locale: nothing in vxa calls setlocale */
  if (endp != txt + len)
    return j_fail(j, E_PARSE, s, H_SYN, "number \"%s\" did not convert", txt);
  if (isinf(d)) {
    char shown[48];
    snprintf(shown, sizeof shown, "%.32s", txt);
    return j_fail_lim(j, E_RANGE, s, H_RANGE, "digits", (long long)len,
                      "number %s overflows double, and infinity is not JSON", shown);
  }
  /* Too small for a double lands on a denormal or 0. That is still exactly the
   * value a C double holds, and it round-trips, so it is not an error. */
  return v_num(d);
}

static V j_lit(J *j, bool mat, const char *w, V v, size_t s) {
  size_t wl = strlen(w);
  if (j->n - s < wl || memcmp(j->p + s, w, wl)) { j->i = s + 1; return j_notavalue(j, s); }
  size_t at = s + wl;
  if (ident_byte(j_at(j, at))) {
    j->i = at;
    return j_fail(j, E_PARSE, s, H_SYN, "\"%.*s\" is not a JSON value: a bare word is never one",
                  (int)(at - s), j->p + s);
  }
  j->i = at;
  if (!mat) return VN;
  V nc = j_node(j, s);
  if (v_is_err(nc)) return nc;
  return v;
}

static V j_value(J *j, int mode, const JPath *want, int wi);

static V j_enter(J *j) {
  j->depth++;
  if (j->depth > j->L.max_depth) {
    Str path = j_path(j);
    V e = j_fail_lim(j, E_LIMIT, j->i, H_DEPTH, "max_depth", j->L.max_depth,
                     "nesting depth %d exceeds the limit %d at %.*s",
                     j->depth, j->L.max_depth, path.len, path.p ? path.p : "$");
    j->depth--;
    return e;
  }
  if (!j_room(j, 1024)) {
    V e = j_fail_lim(j, E_LIMIT, j->i, H_NODES, "max_nodes", j->L.max_nodes,
                     "the arena has no budget left for another container");
    j->depth--;
    return e;
  }
  if (j->M && j->depth > j->M->depth) j->M->depth = j->depth;
  return VN;
}
static void j_leave(J *j) { j->depth--; }

/* ---- object ---- */
static V j_object(J *j, int mode, const JPath *want, int wi) {
  bool sel = mode == J_SEL;
  bool mat = !sel;
  V result = VN, pre = j_enter(j);
  Arena *a = j->a;
  Rec *r = NULL;
  Str keys[KEYLIST_MAX]; int nkeys = 0, morekeys = 0;
  V picked = VN; bool have_pick = false;
  const JSeg *seg = NULL;
  if (v_is_err(pre)) return pre;
  if (sel) {
    if (wi >= want->n) { j_leave(j); return j_value(j, J_FULL, want, wi); }
    seg = &want->s[wi];
    if (seg_is_index_only(seg)) {
      Str pth = j_path(j);
      result = j_fail(j, E_NOENT, j->i, H_MISS,
                      "[%lld] is an index but %.*s is a record: address it as .%.*s or [\"%.*s\"]",
                      seg->idx, pth.len, pth.p ? pth.p : "$", seg->name.len, seg->name.p, seg->name.len, seg->name.p);
      j_leave(j); return result;
    }
  }
  r = mat ? rec_new(a) : NULL;
  if (mat) { V nc = j_node(j, j->i); if (v_is_err(nc)) { result = nc; j_leave(j); return result; } }
  j->i++;                                                       /* the { */
  j_ws(j);
  if (j_at(j, j->i) == '}') {
    j->i++;
    if (sel) { result = j_miss(j, j->i - 1, want, wi, "the record is empty and has no", s_null(), -1); j_leave(j); return result; }
    j_leave(j);
    return mat ? rec_to_v(r) : VN;
  }
  for (;;) {
    j_ws(j);
    if (j_at(j, j->i) == '}') { result = j_fail(j, E_PARSE, j->i, H_SYN, "a trailing comma is not valid JSON: delete it"); goto done; }
    if (j_at(j, j->i) != '"') {
      char tmp[24]; Str here = j_here(j, tmp, sizeof tmp);
      if (j_at(j, j->i) == '\'') result = j_fail(j, E_PARSE, j->i, H_SYN, "a key needs double quotes, not single quotes");
      else result = j_fail(j, E_PARSE, j->i, H_SYN, "expected a quoted object key, got %.*s", here.len, here.p);
      goto done;
    }
    Buf kb; buf_init(&kb, a);
    V se = j_string(j, &kb);              /* keys are always decoded: they are compared */
    if (v_is_err(se)) { result = se; goto done; }
    Str k = buf_take(&kb);
    j_ws(j);
    if (j_at(j, j->i) != ':') {
      char tmp[24]; Str here = j_here(j, tmp, sizeof tmp);
      result = j_fail(j, E_PARSE, j->i, H_SYN, "expected ':' after object key \"%.*s\", got %.*s",
                      k.len, k.p ? k.p : "", here.len, here.p);
      goto done;
    }
    j->i++; j_ws(j);
    j->ckey[j->depth] = k; j->ctag[j->depth] = 'o';
    if (sel) { if (nkeys < KEYLIST_MAX) keys[nkeys++] = k; else morekeys++; }
    bool hit = sel && s_eq(k, seg->name);
    V v = j_value(j, mat ? J_FULL : (hit ? J_SEL : J_SKIP), want, hit ? wi + 1 : 0);
    if (v_is_err(v)) { result = v; goto done; }
    if (mat) {
      if (rec_get(r, k)) { if (j->M) j->M->dupes++; }            /* last one wins, and says so */
      else if (!j_room(j, 128)) {
        result = j_fail_lim(j, E_LIMIT, j->i, H_NODES, "max_nodes", j->L.max_nodes,
                            "the arena has no budget left for another member");
        goto done;
      }
      rec_set(a, r, k, v);
    } else if (hit) {
      if (have_pick && j->M) j->M->dupes++;
      picked = v; have_pick = true;
    }
    j_ws(j);
    if (j->i >= j->n) {
      Str pth = j_path(j);
      result = j_fail(j, E_PARSE, j->n, H_SYN, "the object at %.*s is never closed: the input ended", pth.len, pth.p ? pth.p : "$");
      goto done;
    }
    int c = j_at(j, j->i);
    if (c == ',') { j->i++; continue; }
    if (c == '}') { j->i++; break; }
    {
      char tmp[24]; Str here = j_here(j, tmp, sizeof tmp);
      result = j_fail(j, E_PARSE, j->i, H_SYN, "expected ',' or '}' after an object member, got %.*s",
                      here.len, here.p);
      goto done;
    }
  }
done:
  j_leave(j);
  if (v_is_err(result)) return result;
  if (!sel) return mat ? rec_to_v(r) : VN;
  if (have_pick) return picked;
  Buf b; buf_init(&b, a);
  for (int i = 0; i < nkeys; i++) { if (i) buf_putc(&b, ','); buf_put(&b, keys[i].p, (size_t)keys[i].len); }
  if (morekeys) buf_fmt(&b, ",+%d more", morekeys);
  Str kl = b.len ? buf_take(&b) : s_null();
  return j_miss(j, j->i, want, wi, "nothing in this record matches", kl, -1);
}

/* ---- array ---- */
static V j_array(J *j, int mode, const JPath *want, int wi) {
  bool sel = mode == J_SEL;
  bool mat = !sel;
  V result = VN, pre = j_enter(j);
  Arena *a = j->a;
  List *l = NULL;
  V picked = VN; bool have_pick = false;
  const JSeg *seg = NULL;
  long long idx = 0;
  if (v_is_err(pre)) return pre;
  if (sel) {
    if (wi >= want->n) { j_leave(j); return j_value(j, J_FULL, want, wi); }
    seg = &want->s[wi];
    if (!seg->isnum) {
      result = j_fail(j, E_NOENT, j->i, H_MISS,
                      "a list is selected by position, so \"%.*s\" cannot pick from it: [0] is the first item",
                      seg->name.len, seg->name.p ? seg->name.p : "");
      j_leave(j); return result;
    }
  }
  l = mat ? list_new(a) : NULL;
  if (mat) { V nc = j_node(j, j->i); if (v_is_err(nc)) { result = nc; j_leave(j); return result; } }
  j->i++;                                                       /* the [ */
  j_ws(j);
  if (j_at(j, j->i) == ']') {
    j->i++;
    if (sel) { result = j_miss(j, j->i - 1, want, wi, "the list is empty and has no", s_null(), 0); j_leave(j); return result; }
    j_leave(j);
    return mat ? list_of(l) : VN;
  }
  for (;;) {
    j_ws(j);
    if (j_at(j, j->i) == ']') { result = j_fail(j, E_PARSE, j->i, H_SYN, "a trailing comma is not valid JSON: delete it"); goto done; }
    j->cidx[j->depth] = idx; j->ctag[j->depth] = 'a';
    bool hit = sel && idx == seg->idx;
    V v = j_value(j, mat ? J_FULL : (hit ? J_SEL : J_SKIP), want, hit ? wi + 1 : 0);
    if (v_is_err(v)) { result = v; goto done; }
    if (mat) {
      if (!j_room(j, 64)) {
        result = j_fail_lim(j, E_LIMIT, j->i, H_NODES, "max_nodes", j->L.max_nodes,
                            "the arena has no budget left for another element");
        goto done;
      }
      list_push(a, l, v);
    } else if (hit) { picked = v; have_pick = true; }
    idx++;
    j_ws(j);
    if (j->i >= j->n) {
      Str pth = j_path(j);
      result = j_fail(j, E_PARSE, j->n, H_SYN, "the array at %.*s is never closed: the input ended", pth.len, pth.p ? pth.p : "$");
      goto done;
    }
    int c = j_at(j, j->i);
    if (c == ',') { j->i++; continue; }
    if (c == ']') { j->i++; break; }
    {
      char tmp[24]; Str here = j_here(j, tmp, sizeof tmp);
      result = j_fail(j, E_PARSE, j->i, H_SYN, "expected ',' or ']' after an array element, got %.*s",
                      here.len, here.p);
      goto done;
    }
  }
done:
  j_leave(j);
  if (v_is_err(result)) return result;
  if (!sel) return mat ? list_of(l) : VN;
  if (have_pick) return picked;
  return j_miss(j, j->i, want, wi, "this list has no element at position", s_null(), idx);
}

static V j_value(J *j, int mode, const JPath *want, int wi) {
  j_ws(j);
  if (mode == J_SEL && wi >= want->n) mode = J_FULL;
  bool sel = mode == J_SEL;
  bool mat = mode == J_FULL;
  if (j->i >= j->n) {
    if (j->depth > 0) {
      Str pth = j_path(j);
      const char *open = j->ctag[j->depth] == 'a' ? "array" : "object";
      return j_fail(j, E_PARSE, j->n, H_SYN, "the %s at %.*s is never closed: the input ended",
                    open, pth.len, pth.p ? pth.p : "$");
    }
    return j_fail(j, E_PARSE, j->n, H_SYN, "the input holds no JSON value");
  }
  size_t s = j->i;
  int c = j_at(j, j->i);
  if (c == '{') return j_object(j, mode, want, wi);
  if (c == '[') return j_array(j, mode, want, wi);
  if (sel && (c == '"' || c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n')) {
    char why[64];
    const char *kind = c == '"' ? "a string"
                     : (c == 't' || c == 'f') ? "a bool"
                     : (c == 'n' ? "null" : "a number");
    snprintf(why, sizeof why, "%s here is a leaf and has no child named", kind);
    return j_miss(j, s, want, wi, why, s_null(), -1);
  }
  if (c == '"') {
    Buf b; buf_init(&b, j->a);
    V se = j_string(j, mat ? &b : NULL);
    if (v_is_err(se)) return se;
    if (!mat) return VN;
    V nc = j_node(j, s);
    if (v_is_err(nc)) return nc;
    return v_str(buf_take(&b));
  }
  if (c == '-' || (c >= '0' && c <= '9')) return j_number(j, mat, s);
  if (c == 't') return j_lit(j, mat, "true", VT, s);
  if (c == 'f') return j_lit(j, mat, "false", VF, s);
  if (c == 'n') return j_lit(j, mat, "null", VN, s);
  return j_notavalue(j, s);
}

/* ---------------- entry ---------------- */
static V j_run(Arena *a, Str text, Str *err_snippet, const JsonLimits *lim, JsonMeta *meta, const JPath *want) {
  JsonLimits L = lim ? *lim : json_limits_default();
  /* *err_snippet is written on every failure (empty when there is no region to
   * show, e.g. an oversized input), so a caller can never read a stale value. */
  if (err_snippet) *err_snippet = s_null();
  if (L.max_depth < 1) L.max_depth = 1;
  if (L.max_depth > JSON_DEPTH_HARD_MAX) L.max_depth = JSON_DEPTH_HARD_MAX;
  if (L.max_nodes < 1) L.max_nodes = 1;
  if (L.max_bytes < 1) L.max_bytes = 1;
  if (!a) return mk_err(NULL, E_INTERNAL, "json: called without an arena", H_DEF, NULL);
  if (!text.p || text.len < 0) text = s_wrap("");
  if (meta) { memset(meta, 0, sizeof *meta); meta->bytes = (long long)text.len; }
  if ((long long)text.len > L.max_bytes)
    return mk_errf(a, E_LIMIT, H_BYTES, "input is %d bytes, over the cap %lld: pass {max_bytes:N} or slice it with fs.read",
                   text.len, L.max_bytes);
  int cap = L.max_depth + 2;
  size_t need = (size_t)cap * (sizeof(Str) + sizeof(long long) + 1) + 256;
  if (a->limit && a->total < a->limit && a->limit - a->total < need) {
    return mk_errf(a, E_LIMIT, H_DEPTH, "the arena has no room for a %d-deep path stack (limit=%zu used=%zu)",
                   cap, a->limit, a->total);
  }
  J j;
  memset(&j, 0, sizeof j);
  j.a = a; j.p = text.p; j.n = text.len > 0 ? (size_t)text.len : 0; j.L = L;
  j.M = meta; j.snip = err_snippet;
  j.ckey = (Str*)arena_alloc(a, (size_t)cap * sizeof(Str));
  j.cidx = (long long*)arena_alloc(a, (size_t)cap * sizeof(long long));
  j.ctag = (char*)arena_alloc(a, (size_t)cap);
  memset(j.ckey, 0, (size_t)cap * sizeof(Str));
  memset(j.cidx, 0, (size_t)cap * sizeof(long long));
  memset(j.ctag, 0, (size_t)cap);
  if (j.n == 0) {
    V e = j_fail(&j, E_PARSE, 0, H_SYN, "empty input: there is no JSON document here at all");
    if (err_snippet) *err_snippet = s_null();
    return e;
  }
  V v = j_value(&j, want ? J_SEL : J_FULL, want, 0);
  if (v_is_err(v)) return v;
  j_ws(&j);
  if (j.i != j.n) {
    char tmp[24]; Str here = j_here(&j, tmp, sizeof tmp);
    return j_fail(&j, E_PARSE, j.i, H_SYN, "extra data after the top-level value, got %.*s", here.len, here.p);
  }
  if (meta) meta->bytes = (long long)j.n;
  return v;
}

V json_parse_lim(Arena *a, Str text, Str *err_snippet, const JsonLimits *lim, JsonMeta *meta) {
  return j_run(a, text, err_snippet, lim, meta, NULL);
}
V json_parse(Arena *a, Str text, Str *err_snippet) {
  return j_run(a, text, err_snippet, NULL, NULL, NULL);
}
V json_pick_lim(Arena *a, Str text, Str dotted, Str *err_snippet, const JsonLimits *lim, JsonMeta *meta) {
  JPath p; const char *bad = NULL;
  if (!jpath_parse(&p, dotted, &bad))
    return mk_errf(a, E_SYNTAX, H_PATH, "json path %.*s is not addressable: %s",
                   dotted.len, dotted.p ? dotted.p : "", bad ? bad : "malformed");
  return j_run(a, text, err_snippet, lim, meta, &p);
}
V json_pick(Arena *a, Str text, Str dotted, Str *err_snippet) {
  return json_pick_lim(a, text, dotted, err_snippet, NULL, NULL);
}

/* walk a document that is already materialised; the errors name the path, the
 * segment and what was really there - there is no byte offset to give, because
 * the bytes are gone. Use json.query when you still have the text. */
V json_get_path(Ctx *c, V doc, Str dotted) {
  Arena *a = c ? ctx_arena(c) : NULL;
  if (!a) return mk_err(NULL, E_INTERNAL, "json_get_path needs a Ctx: an error value has to be allocated", H_DEF, NULL);
  JPath p; const char *bad = NULL;
  if (!jpath_parse(&p, dotted, &bad))
    return mk_errf(a, E_SYNTAX, H_PATH, "json path %.*s is not addressable: %s",
                   dotted.len, dotted.p ? dotted.p : "", bad ? bad : "malformed");
  V cur = doc;
  Buf at; buf_init(&at, a);
  buf_putc(&at, '$');
  for (int i = 0; i < p.n; i++) {
    Str here = buf_take(&at);
    const JSeg *g = &p.s[i];
    Buf nx; buf_init(&nx, a);
    buf_put(&nx, here.p, (size_t)here.len);
    if (cur.t == V_REC && cur.u.r) {
      if (seg_is_index_only(g))
        return mk_errf(a, E_NOENT, H_MISS, "json path %.*s: [%lld] is an index but %.*s is a record",
                       p.raw.len, p.raw.p, g->idx, here.len, here.p);
      V *hit = rec_get(cur.u.r, g->name);
      if (!hit) {
        Buf b; buf_init(&b, a);
        for (int k = 0; k < cur.u.r->len && k < KEYLIST_MAX; k++) {
          if (k) buf_putc(&b, ',');
          buf_put(&b, cur.u.r->kv[k].k.p, (size_t)cur.u.r->kv[k].k.len);
        }
        if (cur.u.r->len > KEYLIST_MAX) buf_fmt(&b, ",+%d more", cur.u.r->len - KEYLIST_MAX);
        Str kl = buf_take(&b);
        char hb[256];
        snprintf(hb, sizeof hb, "keys here: %.*s", kl.len, kl.p ? kl.p : "");
        return mk_errf(a, E_NOENT, hb,
                       "json path %.*s stops: no key \"%.*s\" in %.*s (%d keys)",
                       p.raw.len, p.raw.p, g->name.len, g->name.p,
                       here.len, here.p, cur.u.r->len);
      }
      cur = *hit;
    } else if (cur.t == V_LIST && cur.u.l) {
      if (!g->isnum)
        return mk_errf(a, E_NOENT, H_MISS, "json path %.*s: %.*s is a list of %d items, so it is indexed [0]..[%d]",
                       p.raw.len, p.raw.p, here.len, here.p, cur.u.l->len,
                       cur.u.l->len > 0 ? cur.u.l->len - 1 : 0);
      if (g->idx < 0 || g->idx >= cur.u.l->len)
        return mk_errf(a, E_NOENT, "index it: [0]..[%d]",
                       "json path %.*s stops: index %lld is past the end of the %d-item list at %.*s",
                       cur.u.l->len > 0 ? cur.u.l->len - 1 : 0,
                       p.raw.len, p.raw.p, g->idx, cur.u.l->len, here.len, here.p);
      cur = cur.u.l->v[g->idx];
    } else {
      return mk_errf(a, E_NOENT, H_MISS, "json path %.*s stops at %.*s, which is a %s and has no children",
                     p.raw.len, p.raw.p, here.len, here.p, v_typename(cur));
    }
    Str seg = seg_render(a, g);
    buf_put(&nx, seg.p, (size_t)seg.len);
    at = nx;
  }
  return cur;
}

/* ================= render ================= */
typedef struct { Buf *out; bool pretty; } R;

static void r_ind(R *r, int d) {
  if (!r->pretty) return;
  buf_putc(r->out, '\n');
  for (int i = 0; i < d; i++) buf_puts(r->out, "  ");
}
static void r_val(R *r, V v, int depth);

static void r_err(R *r, PErr *e, int depth) {
  if (!e) { buf_puts(r->out, "null"); return; }
  buf_puts(r->out, "{\"__err\":\""); buf_puts(r->out, err_name(e->code));
  buf_puts(r->out, "\",\"msg\":"); buf_json_str(r->out, e->msg);
  buf_puts(r->out, ",\"hint\":"); buf_json_str(r->out, e->hint);
  V d = e->data;
  if (d.t == V_REC && d.u.r) {
    for (int i = 0; i < d.u.r->len; i++) {
      buf_putc(r->out, ','); buf_json_str(r->out, d.u.r->kv[i].k); buf_putc(r->out, ':');
      r_val(r, d.u.r->kv[i].v, depth + 1);
    }
  } else if (d.t != V_NULL) { buf_puts(r->out, ",\"data\":"); r_val(r, d, depth + 1); }
  buf_putc(r->out, '}');
}

/* For depth <= 32 the bytes are identical to v_tojson (tested): same escape
 * table, same fmt_num, so an integer stays an integer and never prints 1.0.
 * The depth budget is JSON_RENDER_DEPTH rather than val.c's 32 because the
 * round-trip law has to hold for a document parsed 128 levels deep as well. */
static void r_val(R *r, V v, int depth) {
  if (depth > JSON_RENDER_DEPTH) { buf_puts(r->out, "null"); return; }
  switch (v.t) {
    case V_BOOL: buf_puts(r->out, v.u.b ? "true" : "false"); break;
    case V_NUM:
      if (isfinite(v.u.n)) fmt_num(r->out, v.u.n); else buf_puts(r->out, "null");
      break;
    case V_STR: buf_json_str(r->out, v.u.s); break;
    case V_LIST: {
      List *l = v.u.l;
      if (!l || !l->len) { buf_puts(r->out, "[]"); break; }
      buf_putc(r->out, '[');
      for (int i = 0; i < l->len; i++) { if (i) buf_putc(r->out, ','); r_ind(r, depth + 1); r_val(r, l->v[i], depth + 1); }
      r_ind(r, depth); buf_putc(r->out, ']');
      break;
    }
    case V_REC: {
      Rec *x = v.u.r;
      if (!x || !x->len) { buf_puts(r->out, "{}"); break; }
      buf_putc(r->out, '{');
      for (int i = 0; i < x->len; i++) {
        if (i) buf_putc(r->out, ',');
        r_ind(r, depth + 1);
        buf_json_str(r->out, x->kv[i].k); buf_putc(r->out, ':');
        if (r->pretty) buf_putc(r->out, ' ');
        r_val(r, x->kv[i].v, depth + 1);
      }
      r_ind(r, depth); buf_putc(r->out, '}');
      break;
    }
    case V_ERR: r_err(r, v.u.e, depth); break;
    case V_PLAN: if (v.u.pl) plan_disp(v.u.pl, r->out, false); else buf_puts(r->out, "null"); break;
    default: buf_puts(r->out, "null"); break;                  /* null, fn, nat */
  }
}

void json_render(Arena *a, V v, Buf *out, bool pretty) {
  if (!a || !out) return;
  R r; r.out = out; r.pretty = pretty;
  r_val(&r, v, 0);
  if (pretty) buf_putc(out, '\n');   /* a written file ends with a newline */
}
Str json_stringify(Arena *a, V v, bool pretty) {
  Buf b; buf_init(&b, a);
  json_render(a, v, &b, pretty);
  return buf_take(&b);
}

/* ================= builtins (SPEC 4.5) ================= */
static V berr(Arena *a, ErrCode c, const char *hint, const char *fmt, ...) {
  char body[256];
  va_list ap; va_start(ap, fmt); vsnprintf(body, sizeof body, fmt, ap); va_end(ap);
  return mk_err(a, c, body, hint, NULL);
}
static const char *const OPT_PARSE[] = { "meta", "max_depth", "max_bytes", "max_nodes", NULL };
static const char *const OPT_QUERY[] = { "meta", "max_depth", "max_bytes", "max_nodes", NULL };
static const char *const OPT_STR[]   = { "pretty", NULL };

static bool opt_known(const char *const *list, Str k) {
  for (int i = 0; list && list[i]; i++) if (s_eqz(k, list[i])) return true;
  return false;
}
/* An option nobody recognises is refused, not ignored: a silently dropped
 * {prety:true} is a formatting bug an agent cannot see. */
static V opt_at(Arena *a, V *args, int nargs, int idx, const char *const *known,
                const char *key, V dflt, V *bad) {
  if (v_is_err(*bad) || nargs <= idx) return dflt;
  V o = args[idx];
  if (o.t == V_NULL) return dflt;
  if (o.t != V_REC) {
    *bad = berr(a, E_TYPE, "the last argument is a record: {pretty:true}",
                "options must be a record {...}, got %s", v_typename(o));
    return dflt;
  }
  for (int i = 0; o.u.r && i < o.u.r->len; i++) {
    Str k = o.u.r->kv[i].k;
    if (!opt_known(known, k)) {
      *bad = berr(a, E_SYNTAX, H_OPT, "unknown option \"%.*s\": it would be ignored, so it is refused",
                  k.len, k.p ? k.p : "");
      return dflt;
    }
  }
  V *hit = o.u.r ? rec_getz(o.u.r, key) : NULL;
  return hit && hit->t != V_NULL ? *hit : dflt;
}
static bool opt_bool(Arena *a, V *args, int nargs, int idx, const char *const *known,
                     const char *key, bool dflt, V *bad) {
  V v = opt_at(a, args, nargs, idx, known, key, v_bool(dflt), bad);
  if (v_is_err(*bad) || v.t == V_NULL) return dflt;
  if (v.t != V_BOOL && v.t != V_NUM) {
    *bad = berr(a, E_TYPE, "pass true or false", "\"%s\" must be true or false, got %s", key, v_typename(v));
    return dflt;
  }
  return v_truthy(v);
}
static long long opt_int(Arena *a, V *args, int nargs, int idx, const char *const *known,
                         const char *key, long long dflt, V *bad) {
  V v = opt_at(a, args, nargs, idx, known, key, v_num((double)dflt), bad);
  if (v_is_err(*bad) || v.t == V_NULL) return dflt;
  if (v.t != V_NUM || !isfinite(v.u.n) || v.u.n != (double)(long long)v.u.n) {
    *bad = berr(a, E_TYPE, "pass a whole number, e.g. {max_depth:256}",
                "\"%s\" must be a whole number, got %s", key, v_typename(v));
    return dflt;
  }
  if (v.u.n < 1) {
    *bad = berr(a, E_RANGE, "it must be at least 1", "\"%s\" must be at least 1", key);
    return dflt;
  }
  return (long long)v.u.n;
}
static JsonLimits opts_limits(Ctx *c, Arena *a, V *args, int nargs, int idx, const char *const *known, V *bad) {
  JsonLimits L = json_limits(c);                     /* defaults, then config json.* */
  long long d = opt_int(a, args, nargs, idx, known, "max_depth", L.max_depth, bad);
  if (!v_is_err(*bad)) L.max_depth = d > JSON_DEPTH_HARD_MAX ? JSON_DEPTH_HARD_MAX : (int)d;
  if (!v_is_err(*bad)) L.max_bytes = opt_int(a, args, nargs, idx, known, "max_bytes", L.max_bytes, bad);
  if (!v_is_err(*bad)) L.max_nodes = opt_int(a, args, nargs, idx, known, "max_nodes", L.max_nodes, bad);
  return L;
}
/* The default return shape carries what the parse had to collapse, so an agent
 * never reads a document without knowing how many keys were dropped. */
static V meta_v(Arena *a, V doc, JsonMeta *m) {
  Rec *r = rec_new(a);
  rec_setz(a, r, "v", doc);
  rec_setz(a, r, "dupes", v_num((double)m->dupes));
  rec_setz(a, r, "nodes", v_num((double)m->nodes));
  rec_setz(a, r, "depth", v_num((double)m->depth));
  rec_setz(a, r, "bytes", v_num((double)m->bytes));
  return rec_to_v(r);
}

static V b_parse(Ctx *c, V *args, int nargs) {
  Arena *a = ctx_arena(c);
  if (nargs < 1) return berr(a, E_ARITY, "json.parse(text, {meta:false})", "json.parse needs the JSON text");
  if (args[0].t != V_STR) return berr(a, E_TYPE, H_TYPE, "json.parse needs a str of JSON, got %s", v_typename(args[0]));
  V bad = VN;
  bool meta = opt_bool(a, args, nargs, 1, OPT_PARSE, "meta", true, &bad);
  JsonLimits L = opts_limits(c, a, args, nargs, 1, OPT_PARSE, &bad);
  if (v_is_err(bad)) return bad;
  JsonMeta m;
  Str snip = s_null();
  V v = json_parse_lim(a, args[0].u.s, &snip, &L, &m);
  if (v_is_err(v)) return v;
  return meta ? meta_v(a, v, &m) : v;
}
static V b_stringify(Ctx *c, V *args, int nargs) {
  Arena *a = ctx_arena(c);
  if (nargs < 1) return berr(a, E_ARITY, "json.stringify(v, {pretty:false})", "json.stringify needs a value");
  V bad = VN;
  bool pretty = opt_bool(a, args, nargs, 1, OPT_STR, "pretty", false, &bad);
  if (v_is_err(bad)) return bad;
  return v_str(json_stringify(a, args[0], pretty));
}
static V b_query(Ctx *c, V *args, int nargs) {
  Arena *a = ctx_arena(c);
  if (nargs < 2) return berr(a, E_ARITY, H_TYPE, "json.query(text, \"a.b\") needs the text and a path");
  if (args[0].t != V_STR) return berr(a, E_TYPE, H_TYPE, "json.query needs a str of JSON, got %s", v_typename(args[0]));
  if (args[1].t != V_STR) return berr(a, E_TYPE, H_PATH, "json.query needs the path as a str, got %s", v_typename(args[1]));
  V bad = VN;
  bool meta = opt_bool(a, args, nargs, 2, OPT_QUERY, "meta", false, &bad);
  JsonLimits L = opts_limits(c, a, args, nargs, 2, OPT_QUERY, &bad);
  if (v_is_err(bad)) return bad;
  JsonMeta m;
  Str snip = s_null();
  V v = json_pick_lim(a, args[0].u.s, args[1].u.s, &snip, &L, &m);
  if (v_is_err(v)) return v;
  return meta ? meta_v(a, v, &m) : v;
}
static V b_get(Ctx *c, V *args, int nargs) {
  Arena *a = ctx_arena(c);
  if (nargs < 2) return berr(a, E_ARITY, "json.get(doc, \"a.b\") needs a document and a path", "json.get needs a document and a path");
  if (args[1].t != V_STR) return berr(a, E_TYPE, H_PATH, "json.get needs the path as a str, got %s", v_typename(args[1]));
  return json_get_path(c, args[0], args[1].u.s);
}

static const Builtin json_tab[] = {
  { "json", "parse", b_parse, 1, 2,
    "json.parse(text, {meta:true, max_depth, max_bytes, max_nodes}) -> {v,dupes,nodes,depth,bytes} | ERR",
    "read JSON strictly: nothing is repaired, and the meta fields say what the parse had to collapse",
    "d = json.parse(fs.read(\"package.json\").text)", BF_PURE },
  { "json", "stringify", b_stringify, 1, 2,
    "json.stringify(v, {pretty:false}) -> str",
    "write strict JSON; integers stay integers, so parsing it again gives back the same value",
    "fs.write(\"tsconfig.json\", json.stringify(cfg, {pretty:true}))", BF_PURE },
  { "json", "query", b_query, 2, 3,
    "json.query(text, \"a.b[0].c\", {meta:false}) -> value | ERR",
    "take one path out of a big document without building the rest of the tree",
    "json.query(fs.read(\"Cargo.lock\").text, \"package[0].name\")", BF_PURE },
  { "json", "get", b_get, 2, 2,
    "json.get(doc, \"a.b[0].c\") -> value | ERR",
    "the same path syntax over a document you already parsed; a miss names the keys it had",
    "json.get(json.parse(t, {meta:false}), \"dependencies.build\")", BF_PURE },
};
const Builtin *t_json(int *n) { *n = (int)(sizeof(json_tab) / sizeof(json_tab[0])); return json_tab; }
