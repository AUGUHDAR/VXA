/* util.c - arena, strings, buffers, sha256, glob, paths, error metadata. */
#include "vxa.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>

/* ---------------- error metadata ---------------- */
static const char *errnames[] = {
  "NONE","PARSE","SYNTAX","UNDEF","TYPE","ARITY","RANGE","GUARD","OOM",
  "NOENT","DENIED","EXISTS","IO","OUTSIDE_JAIL","NOTDIR","ISDIR",
  "NEED_CONFIRM","STALE_PLAN","BAD_TOKEN","POLICY",
  "ANCHOR_MISS","ANCHOR_DUP","BAD_INPUT","REGEX","TIMEOUT",
  "UNSUPPORTED","LIMIT","INTERNAL","SH"
};
static const char *errhints[] = {
  "-", "fix the expression at line reported",
  "tokens must match SPEC grammar; no fuzzy parsing by design",
  "declare it first, or check cfg.get for the binding you expected",
  "inspect .type() of the operand before the call",
  "wrong arity: see the builtin signature in SPEC section 4",
  "index/argument out of range",
  "raise --steps or lower recursion depth",
  "raise --max-mem or reduce input",
  "path missing: run fs.ls/fs.glob to confirm the real path",
  "permission denied",
  "target already exists; use fs.read first or overwrite explicitly",
  "io failure: verify disk and path length",
  "path escapes workspace root; pass --root or use a path under the jail",
  "not a directory",
  "is a directory; pass a file path",
  "rerun the same script with --confirm <token> from this plan",
  "file changed after plan was produced: rerun to get a fresh token",
  "token malformed or for a different plan/root",
  "blocked by policy in .vxa/manifest.json; change policies explicitly",
  "anchor not found: re-read with anchors:true and use a current hash",
  "anchor ambiguous: pass n or a longer anchor",
  "input value malformed",
  "invalid regex: POSIX ERE subset used by tx.find",
  "increase --timeout or split the work",
  "not supported by this build",
  "hard limit reached",
  "internal error: report with the emitted line",
  "command failed: inspect code/out_tail"
};
const char *err_name(ErrCode c) {
  if ((unsigned)c >= sizeof(errnames)/sizeof(*errnames)) return "INTERNAL";
  return errnames[c];
}
const char *err_hint(ErrCode c) {
  if ((unsigned)c >= sizeof(errhints)/sizeof(*errhints)) return errhints[E_INTERNAL];
  return errhints[c];
}
int err_exit(ErrCode c) {
  switch (c) {
    case E_PARSE: case E_SYNTAX: case E_UNDEF: case E_TYPE: case E_ARITY:
    case E_RANGE: case E_GUARD: case E_BAD_INPUT: case E_REGEX: case E_UNSUPPORTED:
    case E_LIMIT: case E_INTERNAL: return X_SCRIPT;
    case E_OOM: return X_INTERNAL;
    case E_NOENT: case E_EXISTS: case E_IO: case E_NOTDIR: case E_ISDIR:
    case E_ANCHOR_MISS: case E_ANCHOR_DUP: case E_STALE_PLAN: case E_BAD_TOKEN:
    case E_SH: return X_IO;
    case E_NEED_CONFIRM: return X_CONFIRM;
    case E_DENIED: case E_OUTSIDE_JAIL: case E_POLICY: return X_POLICY;
    case E_TIMEOUT: return X_TIMEOUT;
    default: return X_OK;
  }
}

/* ---------------- arena ---------------- */
#define BLK_MIN (64u * 1024u)
static Blk *blk_new(size_t cap) {
  Blk *b = (Blk*)malloc(cap + sizeof(Blk));
  if (!b) return NULL;
  b->next = NULL; b->off = 0; b->cap = cap;
  return b;
}
void arena_init(Arena *a, size_t limit) {
  a->cur = NULL; a->total = 0; a->nalloc = 0;
  a->limit = limit ? limit : (size_t)512 * 1024 * 1024;
}
void *arena_alloc(Arena *a, size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (n == 0) n = 8;
  if (a->total + n > a->limit) { fprintf(stderr, "vxa: arena limit exceeded\n"); abort(); }
  if (!a->cur || a->cur->cap - a->cur->off < n) {
    size_t cap = BLK_MIN;
    while (cap < n && cap < (16u*1024u*1024u)) cap *= 2;
    if (cap < n) cap = n;
    Blk *b = blk_new(cap);
    if (!b) { fprintf(stderr, "vxa: out of memory\n"); abort(); }
    b->next = a->cur; a->cur = b;
  }
  a->nalloc++;
  void *p = (char*)(a->cur + 1) + a->cur->off;
  a->cur->off += n; a->total += n;
  return p;
}
void *arena_zalloc(Arena *a, size_t n) {
  void *p = arena_alloc(a, n); memset(p, 0, n); return p;
}
char *arena_strndup(Arena *a, const char *s, size_t n) {
  char *p = (char*)arena_alloc(a, n + 1);
  if (n) memcpy(p, s, n);
  p[n] = 0; return p;
}
char *arena_strdup(Arena *a, const char *s) { return arena_strndup(a, s, strlen(s)); }
void arena_free(Arena *a) {
  Blk *b = a->cur;
  while (b) { Blk *nx = b->next; free(b); b = nx; }
  a->cur = NULL; a->total = 0; a->nalloc = 0;
}

/* ---------------- Str ---------------- */
static Str NS = {0, NULL};
Str s_null(void) { return NS; }
Str s_from(Arena *a, const char *p, size_t n) {
  Str s; s.p = arena_strndup(a, p, n); s.len = (int)n; return s;
}
Str s_lit(Arena *a, const char *c) {
  if (!c) c = "";
  return s_from(a, c, strlen(c));
}
Str s_wrap(const char *c) { Str s; s.p = (char*)c; s.len = c ? (int)strlen(c) : 0; return s; }
Str s_fmt(Arena *a, const char *fmt, ...) {
  va_list ap; char stack[256];
  va_start(ap, fmt);
  int n = vsnprintf(stack, sizeof stack, fmt, ap);
  va_end(ap);
  if (n < 0) return s_null();
  if ((size_t)n < sizeof stack) return s_from(a, stack, (size_t)n);
  char *big = (char*)arena_alloc(a, (size_t)n + 1);
  va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
  return s_from(a, big, (size_t)n);
}
bool s_eq(Str a, Str b) {
  if (a.len != b.len) return false;
  if (a.len == 0) return true;
  return memcmp(a.p, b.p, (size_t)a.len) == 0;
}
bool s_eqz(Str a, const char *b) { return s_eq(a, s_wrap(b)); }
bool s_ni(Str a, const char *p) {
  size_t n = strlen(p);
  if ((size_t)a.len < n) return false;
  for (size_t i = 0; i < n; i++) if (tolower((unsigned char)a.p[i]) != tolower((unsigned char)p[i])) return false;
  return true;
}
int s_cmp(Str a, Str b) {
  int n = a.len < b.len ? a.len : b.len;
  int c = n ? memcmp(a.p, b.p, (size_t)n) : 0;
  if (c) return c < 0 ? -1 : 1;
  return a.len == b.len ? 0 : (a.len < b.len ? -1 : 1);
}
Str s_concat(Arena *a, Str x, Str y) {
  char *p = (char*)arena_alloc(a, (size_t)x.len + y.len + 1);
  memcpy(p, x.p, (size_t)x.len); memcpy(p + x.len, y.p, (size_t)y.len);
  p[x.len + y.len] = 0;
  Str s; s.p = p; s.len = x.len + y.len; return s;
}
Str s_slice(Str s, int from, int to) {
  /* Loaned view: NOT NUL-terminated. Use .len, or copy with s_from/s_cut. */
  Str r = {0, NULL};
  if (!s.p) return r;
  if (from < 0) from = 0;
  if (to > s.len || to < 0) to = s.len;
  if (from > to) from = to;
  r.p = s.p + from; r.len = to - from;
  return r;
}
Str s_cut(Arena *a, Str s, int from, int to) {
  Str v = s_slice(s, from, to);
  return s_from(a, v.p, (size_t)v.len);
}
Str s_lower(Arena *a, Str s) {
  Str r = s_from(a, s.p ? s.p : "", (size_t)s.len);
  for (int i = 0; i < r.len; i++) r.p[i] = (char)tolower((unsigned char)r.p[i]);
  return r;
}
Str s_upper(Arena *a, Str s) {
  Str r = s_from(a, s.p ? s.p : "", (size_t)s.len);
  for (int i = 0; i < r.len; i++) r.p[i] = (char)toupper((unsigned char)r.p[i]);
  return r;
}
Str s_trim(Arena *a, Str s) {
  int i = 0, j = s.len;
  while (i < j && isspace((unsigned char)s.p[i])) i++;
  while (j > i && isspace((unsigned char)s.p[j-1])) j--;
  return s_from(a, s.p + i, (size_t)(j - i));
}
int s_find(Str hay, Str nd, int from) {
  if (from < 0) from = 0;
  if (nd.len == 0) return from <= hay.len ? from : -1;
  if (nd.len > hay.len - from) return -1;
  for (int i = from; i + nd.len <= hay.len; i++)
    if (!memcmp(hay.p + i, nd.p, (size_t)nd.len)) return i;
  return -1;
}
int s_findz(Str hay, const char *nd, int from) { return s_find(hay, s_wrap(nd), from); }
int s_rfind(Str hay, Str nd) {
  if (nd.len == 0 || nd.len > hay.len) return -1;
  for (int i = hay.len - nd.len; i >= 0; i--)
    if (!memcmp(hay.p + i, nd.p, (size_t)nd.len)) return i;
  return -1;
}
int s_count_char(Str s, char c) { int n = 0; for (int i = 0; i < s.len; i++) if (s.p[i] == c) n++; return n; }
bool s_is_int(Str s, long long *out) {
  if (s.len == 0 || !s.p) return false;
  int i = 0; bool neg = false;
  if (s.p[0] == '-') { i = 1; neg = true; }
  if (i >= s.len) return false;
  long long v = 0;
  for (; i < s.len; i++) {
    if (!isdigit((unsigned char)s.p[i])) return false;
    if (v > (9223372036854775807LL - 9) / 10) return false;
    v = v * 10 + (s.p[i] - '0');
  }
  *out = neg ? -v : v;
  return true;
}
int s_ucount(Str s) {
  int n = 0;
  for (int i = 0; i < s.len; i++) if ((s.p[i] & 0xC0) != 0x80) n++;
  return n;
}
Str s_replace(Arena *a, Str s, Str from, Str to) {
  if (from.len == 0 || !s.p) return s;
  int hits = 0;
  for (int i = 0; i + from.len <= s.len; ) {
    if (!memcmp(s.p + i, from.p, (size_t)from.len)) { hits++; i += from.len; }
    else i++;
  }
  if (!hits) return s;
  size_t n = (size_t)s.len + (size_t)hits * (size_t)to.len;
  char *p = (char*)arena_alloc(a, n + 1);
  size_t o = 0;
  for (int i = 0; i < s.len; ) {
    if (i + from.len <= s.len && !memcmp(s.p + i, from.p, (size_t)from.len)) {
      memcpy(p + o, to.p, (size_t)to.len); o += (size_t)to.len; i += from.len;
    } else p[o++] = s.p[i++];
  }
  p[o] = 0;
  Str r; r.p = p; r.len = (int)o; return r;
}
Str s_ellide(Arena *a, Str s, int max) {
  if (s.len <= max || max < 16) return s;
  int head = max / 2, tail = max - head - 4;
  Buf b; buf_init(&b, a);
  buf_put(&b, s.p, (size_t)head); buf_puts(&b, "...");
  buf_put(&b, s.p + s.len - tail, (size_t)tail);
  return buf_take(&b);
}
Str s_char_at(Arena *a, Str s, int i) {
  int cp = 0, o = 0;
  while (o < s.len) {
    int w = 1;
    unsigned char c = (unsigned char)s.p[o];
    if (c >= 0xF0) w = 4; else if (c >= 0xE0) w = 3; else if (c >= 0xC0) w = 2;
    if (o + w > s.len) w = 1;
    if (cp == i) return s_from(a, s.p + o, (size_t)w);
    cp++; o += w;
  }
  return s_null();
}

/* ---------------- Buf ---------------- */
void buf_init(Buf *b, Arena *a) { b->a = a; b->len = 0; b->cap = 0; b->p = NULL; }
static void buf_grow(Buf *b, size_t need) {
  if (b->cap >= need) return;
  size_t cap = b->cap ? b->cap : 128;
  while (cap < need) cap *= 2;
  char *p = (char*)arena_alloc(b->a, cap + 1);
  if (b->len) memcpy(p, b->p, b->len);
  b->p = p; b->cap = cap;
}
void buf_put(Buf *b, const void *p, size_t n) {
  if (!n) return;
  buf_grow(b, b->len + n + 1);
  memcpy(b->p + b->len, p, n);
  b->len += n; b->p[b->len] = 0;
}
void buf_putc(Buf *b, char c) { buf_put(b, &c, 1); }
void buf_puts(Buf *b, const char *s) { if (s) buf_put(b, s, strlen(s)); }
void buf_clear(Buf *b) { b->len = 0; }
Str buf_str(Buf *b) {
  if (!b->p) { buf_grow(b, 1); }
  b->p[b->len] = 0;
  Str s; s.p = b->p; s.len = (int)b->len; return s;
}
Str buf_take(Buf *b) { Str s = buf_str(b); return s_from(b->a, s.p, (size_t)s.len); }
void buf_fmt(Buf *b, const char *fmt, ...) {
  va_list ap; char stack[512];
  va_start(ap, fmt);
  int n = vsnprintf(stack, sizeof stack, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof stack) { buf_put(b, stack, (size_t)n); return; }
  char *big = (char*)arena_alloc(b->a, (size_t)n + 1);
  va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
  buf_put(b, big, (size_t)n);
  free(0);
}
void buf_c_escape(Buf *b, Str s) {
  buf_putc(b, '"');
  for (int i = 0; i < s.len; i++) {
    unsigned char c = (unsigned char)s.p[i];
    switch (c) {
      case '"': buf_puts(b, "\\\""); break;
      case '\\': buf_puts(b, "\\\\"); break;
      case '\n': buf_puts(b, "\\n"); break;
      case '\r': buf_puts(b, "\\r"); break;
      case '\t': buf_puts(b, "\\t"); break;
      default:
        if (c < 0x20 || c == 0x7f) buf_fmt(b, "\\x%02x", c);
        else buf_putc(b, (char)c);
    }
  }
  buf_putc(b, '"');
}
void buf_json_str(Buf *b, Str s) { buf_c_escape(b, s); }
/* compact form: quote only when needed */
void buf_add_escaped(Buf *b, Str s) {
  bool need = (s.len == 0);
  for (int i = 0; i < s.len && !need; i++) {
    unsigned char c = (unsigned char)s.p[i];
    if (c <= ' ' || c == '"' || c == '=' || c == ',' || c == '{' || c == '}' ||
        c == '[' || c == ']' || c == '#' || c == '\\' || c == ':' || c >= 0x7f) need = true;
  }
  if (!need) { buf_put(b, s.p, (size_t)s.len); return; }
  buf_c_escape(b, s);
}
void fmt_num(Buf *b, double d) {
  if (isnan(d)) { buf_puts(b, "NaN"); return; }
  if (isinf(d)) { buf_puts(b, d > 0 ? "Inf" : "-Inf"); return; }
  if (d == (double)(long long)d && fabs(d) < 1e15) buf_fmt(b, "%lld", (long long)d);
  else buf_fmt(b, "%.15g", d);
}
int tok_est(const char *p, size_t n) { (void)p; return (int)((n + 3) / 4); }

/* ---------------- sha256 (FIPS 180-4) ---------------- */
static const uint32_t K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define RORV(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const uint8_t *d) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)d[4*i] << 24) | ((uint32_t)d[4*i+1] << 16) | ((uint32_t)d[4*i+2] << 8) | (uint32_t)d[4*i+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = RORV(w[i-15],7) ^ RORV(w[i-15],18) ^ (w[i-15] >> 3);
    uint32_t s1 = RORV(w[i-2],17) ^ RORV(w[i-2],19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  uint32_t a=h[0],b=h[1],c=h[2],dd=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = RORV(e,6) ^ RORV(e,11) ^ RORV(e,25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + S1 + ch + K[i] + w[i];
    uint32_t S0 = RORV(a,2) ^ RORV(a,13) ^ RORV(a,22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    hh=g; g=f; f=e; e=dd+t1; dd=c; c=b; b=a; a=t1+t2;
  }
  h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=dd; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}
void sha256_init(Sha256 *s) {
  s->h[0]=0x6a09e667u; s->h[1]=0xbb67ae85u; s->h[2]=0x3c6ef372u; s->h[3]=0xa54ff53au;
  s->h[4]=0x510e527fu; s->h[5]=0x9b05688cu; s->h[6]=0x1f83d9abu; s->h[7]=0x5be0cd19u;
  s->ln = 0; s->n = 0;
}
void sha256_update(Sha256 *s, const void *p, size_t n) {
  const uint8_t *b = (const uint8_t*)p;
  s->ln += n;
  while (n) {
    size_t take = 64 - s->n; if (take > n) take = n;
    memcpy(s->buf + s->n, b, take); s->n += take; b += take; n -= take;
    if (s->n == 64) { sha_block(s->h, s->buf); s->n = 0; }
  }
}
void sha256_final(Sha256 *s, uint8_t out[32]) {
  uint64_t bits = s->ln * 8;
  uint8_t pad = 0x80, zero = 0;
  sha256_update(s, &pad, 1);
  while (s->n != 56) sha256_update(s, &zero, 1);
  uint8_t lenb[8];
  for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8*i));
  sha256_update(s, lenb, 8);
  for (int i = 0; i < 8; i++) {
    out[4*i]   = (uint8_t)(s->h[i] >> 24);
    out[4*i+1] = (uint8_t)(s->h[i] >> 16);
    out[4*i+2] = (uint8_t)(s->h[i] >> 8);
    out[4*i+3] = (uint8_t)(s->h[i]);
  }
}
void sha256_buf(const void *p, size_t n, uint8_t out[32]) {
  Sha256 s; sha256_init(&s); sha256_update(&s, p, n); sha256_final(&s, out);
}
static void hash_hex(const void *p, size_t n, char hx[65]) {
  uint8_t d[32]; sha256_buf(p, n, d); base16(d, 32, hx);
}
Str hash12(Arena *a, const void *p, size_t n) { char hx[65]; hash_hex(p, n, hx); return s_from(a, hx, 12); }
Str hash4(Arena *a, const void *p, size_t n)  { char hx[65]; hash_hex(p, n, hx); return s_from(a, hx, 4); }

/* ---------------- base16 / fnv ---------------- */
void base16(const uint8_t *d, size_t n, char *out) {
  static const char *hx = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2*i] = hx[d[i] >> 4]; out[2*i+1] = hx[d[i] & 15]; }
  out[2*n] = 0;
}
uint64_t fnv1a(const void *p, size_t n) {
  const uint8_t *b = (const uint8_t*)p; uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
  return h;
}

/* ---------------- glob ---------------- */
static const char *brk(const char *pat, const char *name) {
  /* handle [..] class; pat points at '[' */
  const char *q = pat + 1; bool neg = (*q == '!' || *q == '^'); size_t nlen = strlen(name);
  const char *close = strchr(q + (neg?1:0), ']');
  if (!close) return NULL;
  bool matched = false;
  for (const char *c = q + (neg?1:0); c < close; c++) {
    if (c + 2 < close && c[1] == '-') {
      for (char ch = c[0]; ch <= c[2]; ch++) if (nlen && name[0] == ch) matched = true;
      c += 2;
    } else if (*c == name[0] && nlen) matched = true;
  }
  if (matched == neg) return NULL;
  return close + 1;
}
bool glob_match(const char *pat, const char *name) {
  if (!pat || !name) return false;
  if (*pat == 0) return *name == 0;
  if (*pat == '*') {
    while (*pat == '*') pat++;
    if (!*pat) return true;
    for (; *name; name++) if (glob_match(pat, name)) return true;
    return glob_match(pat, name);
  }
  if (*pat == '?') { if (!*name) return false; return glob_match(pat + 1, name + 1); }
  if (*pat == '[') { const char *r = brk(pat, name); return r && *name && r && glob_match(r, name + 1); }
  if (*pat == *name) return glob_match(pat + 1, name + 1);
  return false;
}
bool glob_path(const char *pat, const char *path) {
  /* '*' never crosses '/'; only '**' does. */
  if (!pat || !path) return false;
  if (!strncmp(pat, "**", 2)) {
    pat += 2;
    if (*pat == '/') pat++;
    for (;;) {
      if (glob_path(pat, path)) return true;
      const char *s = strchr(path, '/');
      if (!s) s = strchr(path, '\\');
      if (!s) return false;
      path = s + 1;
    }
  }
  const char *ps = strchr(pat, '/'), *ss = strchr(path, '/');
  if (!ps) return !ss && glob_match(pat, path);
  if (!ss) return false;
  size_t n = (size_t)(ps - pat);
  char *comp = (char*)malloc(n + 1); memcpy(comp, pat, n); comp[n] = 0;
  size_t m = (size_t)(ss - path);
  char *nc = (char*)malloc(m + 1); memcpy(nc, path, m); nc[m] = 0;
  bool ok = glob_match(comp, nc) && glob_path(ps + 1, ss + 1);
  free(comp); free(nc);
  return ok;
}

/* ---------------- paths ---------------- */
Str path_join(Arena *a, const char *dir, const char *name) {
  if (!dir || !*dir) return s_lit(a, name ? name : "");
  size_t dn = strlen(dir);
  bool sep = dn && (dir[dn-1] == '/' || dir[dn-1] == '\\');
  if (name && (path_is_abs(name))) return s_lit(a, name);
  return s_fmt(a, "%s%s%s", dir, sep ? "" : "/", name ? name : "");
}
bool path_is_abs(const char *p) {
  if (!p || !*p) return false;
  if (p[0] == '/' || p[0] == '\\') return true;
  if (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) return true;
  return false;
}
Str path_norm(Arena *a, const char *p) {
  /* lexical normalize: '/' separators, collapse . .. //; keeps drive prefix */
  Buf b; buf_init(&b, a);
  if (!p || !*p) return s_lit(a, ".");
  const char *s = p;
  int root = 0;
  if (isalpha((unsigned char)s[0]) && s[1] == ':' && (s[2] == '/' || s[2] == '\\')) {
    buf_putc(&b, (char)toupper((unsigned char)s[0])); buf_puts(&b, ":/"); s += 3; root = 1;
  } else if (s[0] == '/' || s[0] == '\\') { buf_putc(&b, '/'); s++; root = 1; }
  const char *seg[160]; int segl[160]; int np = 0;
  while (*s) {
    while (*s == '/' || *s == '\\') s++;
    if (!*s) break;
    const char *st = s;
    while (*s && *s != '/' && *s != '\\') s++;
    int n = (int)(s - st);
    if (n == 1 && st[0] == '.') continue;
    if (n == 2 && st[0] == '.' && st[1] == '.') { if (np) np--; continue; }
    if (np < 160) { seg[np] = st; segl[np] = n; np++; }
  }
  for (int i = 0; i < np; i++) {
    if (i && b.p && b.len && b.p[b.len-1] != '/') buf_putc(&b, '/');
    buf_put(&b, seg[i], (size_t)segl[i]);
  }
  if (b.len == 0) return s_lit(a, root ? "/" : ".");
  return buf_take(&b);
}

const char *path_base(const char *p) {
  if (!p) return "";
  const char *s1 = strrchr(p, '/'), *s2 = strrchr(p, '\\');
  const char *s = s1 > s2 ? s1 : s2;
  return s ? s + 1 : p;
}
void path_ext(const char *p, char *out, size_t n) {
  out[0] = 0;
  const char *b = path_base(p);
  const char *d = strrchr(b, '.');
  if (!d || d == b) return;
  if (n && strlen(d) < n) { snprintf(out, n, "%s", d); }
}
bool path_within(const char *root, const char *p) {
  if (!root || !p) return false;
  size_t rn = strlen(root);
  if (rn == 0) return false;
  if (strlen(p) < rn) return false;
  int ci = 0;
#ifdef _WIN32
  ci = 1;
#endif
  for (size_t i = 0; i < rn; i++) {
    char x = p[i], y = root[i];
    if (ci) { x = (char)tolower((unsigned char)x); y = (char)tolower((unsigned char)y); }
    if (x != y) return false;
  }
  return p[rn] == 0 || p[rn] == '/' || p[rn] == '\\';
}
