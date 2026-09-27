/* val.c - values: constructors, predicates, and the two serializers.
 * Compact form (SPEC §2.1) is what agents read and what plan tokens hash:
 * one line, structure in {}/[] delimiters, records keep insertion order
 * (no hash iteration), so the same value always yields the same bytes. */
#include "vxa.h"
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define V_MAX_DEPTH 32

const V VN = { V_NULL, { .b = false } };
const V VT = { V_BOOL, { .b = true } };
const V VF = { V_BOOL, { .b = false } };

/* v_err_hint() has no Arena parameter, so ERR cells live here. */
static Arena g_err_arena;
static Arena *err_arena(void) {
  if (!g_err_arena.cur) arena_init(&g_err_arena, 0);
  return &g_err_arena;
}

/* ---------- ctors / accessors ---------- */
V v_bool(bool b) { return b ? VT : VF; }
V v_num(double n) { V v; v.t = V_NUM; v.u.n = n; return v; }
V v_str(Str s) { V v; v.t = V_STR; v.u.s = s; return v; }
V v_nil(void) { return VN; }
V v_strz(Arena *a, const char *s) { return v_str(s_lit(a, s)); }
V rec_to_v(Rec *r) { V v; v.t = V_REC; v.u.r = r; return v; }

List *list_new(Arena *a) {
  List *l = (List*)arena_alloc(a, sizeof(List));
  l->len = 0; l->cap = 0; l->v = NULL;
  return l;
}
void list_push(Arena *a, List *l, V v) {
  if (!l) return;
  if (l->len == l->cap) {
    int cap = l->cap ? l->cap * 2 : 4;
    V *nv = (V*)arena_alloc(a, (size_t)cap * sizeof(V));
    if (l->len) memcpy(nv, l->v, (size_t)l->len * sizeof(V));
    l->v = nv; l->cap = cap;
  }
  l->v[l->len++] = v;
}
V *list_get(List *l, int i) {
  if (!l || i < 0 || i >= l->len) return NULL;
  return l->v + i;
}

Rec *rec_new(Arena *a) {
  Rec *r = (Rec*)arena_alloc(a, sizeof(Rec));
  r->len = 0; r->cap = 0; r->kv = NULL;
  return r;
}
V *rec_get(Rec *r, Str k) {
  if (!r) return NULL;
  for (int i = 0; i < r->len; i++) if (s_eq(r->kv[i].k, k)) return &r->kv[i].v;
  return NULL;
}
V *rec_getz(Rec *r, const char *k) { return rec_get(r, s_wrap(k)); }
void rec_set(Arena *a, Rec *r, Str k, V v) {
  if (!r) return;
  V *hit = rec_get(r, k);
  if (hit) { *hit = v; return; }             /* keep position */
  if (r->len == r->cap) {
    int cap = r->cap ? r->cap * 2 : 4;
    Pair *nk = (Pair*)arena_alloc(a, (size_t)cap * sizeof(Pair));
    if (r->len) memcpy(nk, r->kv, (size_t)r->len * sizeof(Pair));
    r->kv = nk; r->cap = cap;
  }
  r->kv[r->len].k = k; r->kv[r->len].v = v; r->len++;
}
void rec_setz(Arena *a, Rec *r, const char *k, V v) { rec_set(a, r, s_wrap(k), v); }
bool rec_del(Arena *a, Rec *r, Str k) {
  (void)a;
  if (!r) return false;
  for (int i = 0; i < r->len; i++) {
    if (!s_eq(r->kv[i].k, k)) continue;
    memmove(r->kv + i, r->kv + i + 1, (size_t)(r->len - i - 1) * sizeof(Pair));
    r->len--;
    return true;
  }
  return false;
}

V v_list(Arena *a, int n, ...) {                 /* n V args */
  List *l = list_new(a);
  va_list ap; va_start(ap, n);
  for (int i = 0; i < n; i++) list_push(a, l, va_arg(ap, V));
  va_end(ap);
  V v; v.t = V_LIST; v.u.l = l; return v;
}
V v_rec(Arena *a, int n, ...) {                  /* n pairs: (const char *key, V val) */
  Rec *r = rec_new(a);
  va_list ap; va_start(ap, n);
  for (int i = 0; i < n; i++) {
    const char *k = va_arg(ap, const char*);
    V val = va_arg(ap, V);
    rec_set(a, r, key(a, k), val);
  }
  va_end(ap);
  return rec_to_v(r);
}
Str key(Arena *a, const char *k) { (void)a; return s_wrap(k); }

/* ---------- predicates ---------- */
static const char *const tnames[] = {
  "null", "bool", "num", "str", "list", "rec", "fn", "plan", "err", "nat"
};
const char *v_typename(V v) {
  if ((unsigned)v.t >= sizeof(tnames) / sizeof(*tnames)) return "unknown";
  return tnames[v.t];
}
bool v_truthy(V v) {
  switch (v.t) {
    case V_NULL: return false;
    case V_BOOL: return v.u.b;
    case V_NUM:  return v.u.n != 0.0;           /* NaN != 0 -> truthy */
    case V_STR:  return v.u.s.len > 0;
    case V_LIST: return v.u.l && v.u.l->len > 0;
    case V_REC:  return v.u.r && v.u.r->len > 0;
    default: return true;
  }
}
bool v_eq(V a, V b) {
  if (a.t != b.t) return false;
  switch (a.t) {
    case V_NULL: return true;
    case V_BOOL: return a.u.b == b.u.b;
    case V_NUM:  return a.u.n == b.u.n;
    case V_STR:  return s_eq(a.u.s, b.u.s);
    case V_LIST: {
      List *x = a.u.l, *y = b.u.l;
      if (!x || !y) return x == y;
      if (x->len != y->len) return false;
      for (int i = 0; i < x->len; i++) if (!v_eq(x->v[i], y->v[i])) return false;
      return true;
    }
    case V_REC: {
      Rec *x = a.u.r, *y = b.u.r;
      if (!x || !y) return x == y;
      if (x->len != y->len) return false;
      for (int i = 0; i < x->len; i++) {        /* key set, order-insensitive */
        V *got = rec_get(y, x->kv[i].k);
        if (!got || !v_eq(x->kv[i].v, *got)) return false;
      }
      return true;
    }
    case V_FN:   return a.u.f == b.u.f;
    case V_PLAN: return a.u.pl == b.u.pl;
    case V_ERR:  return a.u.e == b.u.e;
    default:     return a.u.nat == b.u.nat;
  }
}
int v_cmp(V a, V b, bool *ok) {
  if (ok) *ok = false;
  if (a.t == V_NUM && b.t == V_NUM) {
    if (ok) *ok = true;
    return a.u.n < b.u.n ? -1 : (a.u.n > b.u.n ? 1 : 0);
  }
  if (a.t == V_STR && b.t == V_STR) {
    if (ok) *ok = true;
    return s_cmp(a.u.s, b.u.s);
  }
  return 0;
}

/* ---------- errors ---------- */
V v_err_hint(ErrCode c, Str msg, Str hint) {
  PErr *e = (PErr*)arena_zalloc(err_arena(), sizeof(PErr));
  e->code = c; e->msg = msg; e->hint = hint; e->data = VN;
  V v; v.t = V_ERR; v.u.e = e; return v;
}
V v_err(Arena *a, ErrCode c, const char *fmt, ...) {
  va_list ap; char stack[256];
  va_start(ap, fmt);
  int n = vsnprintf(stack, sizeof stack, fmt, ap);
  va_end(ap);
  Str msg;
  if (n < 0) msg = s_null();
  else if ((size_t)n < sizeof stack) msg = s_from(a, stack, (size_t)n);
  else {
    char *big = (char*)arena_alloc(a, (size_t)n + 1);
    va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
    msg = s_from(a, big, (size_t)n);
  }
  return v_err_hint(c, msg, s_lit(a, err_hint(c)));
}
bool v_is_err(V v) { return v.t == V_ERR; }
ErrCode v_errcode(V v) { return (v.t == V_ERR && v.u.e) ? v.u.e->code : E_NONE; }

/* ---------- compact form (SPEC §2.1) ----------
 * Structure is carried by DELIMITERS, never by line position:
 *   record  {k=v,k2=v2}   (the top level alone drops its outer pair)
 *   list    [1,2,3] / [{a=1,b=2},{a=3}]
 *   null -> '-'   bool -> 't'/'f'   ',' splits entries   '=' splits k/v
 *   strings bare when safe, else C-style escaped
 * Hard invariant: a compact value is EXACTLY ONE LINE. No raw '\n' / '\r' is
 * ever emitted (strings, keys and err texts escape them), which is what makes
 * the form safe to grep and to byte-compare across agent turns. */
static void c_leaf(Buf *b, V v, int depth);
static void c_rec_body(Buf *b, Rec *r, int depth);   /* entries only, no braces */

/* keys follow the same quoting rules as strings, so a weird key cannot smuggle
 * a newline or a structural character into the line */
static void c_key(Buf *b, Str k) { buf_add_escaped(b, k); }
static void c_fn(Buf *b, Fn *f, bool upper) {
  const char *tag = upper ? "FN" : "fn";
  if (f && f->name) { buf_puts(b, tag); buf_putc(b, '('); buf_puts(b, f->name); buf_putc(b, ')'); }
  else buf_fmt(b, "%s/%d", tag, f ? f->nparams : 0);
}
/* plan stays a delegation to plan.c, but its bytes must not break the
 * one-line invariant: a raw CR/LF coming back from plan_disp is escaped. */
static void c_plan(Buf *b, Plan *p) {
  if (!p) { buf_puts(b, "<plan>"); return; }
  if (!b->a) { plan_disp(p, b, true); return; }
  Buf t; buf_init(&t, b->a);
  plan_disp(p, &t, true);
  if (t.p && t.len) {
    bool dirty = false;
    for (size_t i = 0; i < t.len; i++) { char c = t.p[i]; if (c == '\n' || c == '\r') { dirty = true; break; } }
    if (!dirty) { buf_put(b, t.p, t.len); return; }
  }
  for (size_t i = 0; i < t.len; i++) {
    char c = t.p[i];
    if (c == '\n') buf_puts(b, "\\n");
    else if (c == '\r') buf_puts(b, "\\r");
    else buf_putc(b, c);
  }
}
static void c_err(Buf *b, PErr *e, int depth) {
  if (!e) { buf_puts(b, "!ERR code=INTERNAL msg=\"null err\" hint=\"-\""); return; }
  buf_puts(b, "!ERR code="); buf_puts(b, err_name(e->code));
  buf_puts(b, " msg="); buf_add_escaped(b, e->msg);
  buf_puts(b, " hint="); buf_add_escaped(b, e->hint);
  V d = e->data;
  if (d.t == V_REC && d.u.r && d.u.r->len) {   /* data rides flat: SPEC §2.1 example */
    for (int i = 0; i < d.u.r->len; i++) {
      buf_putc(b, ' '); c_key(b, d.u.r->kv[i].k); buf_putc(b, '=');
      c_leaf(b, d.u.r->kv[i].v, depth + 1);
    }
  } else if (d.t != V_NULL) { buf_puts(b, " data="); c_leaf(b, d, depth + 1); }
}
static void c_leaf(Buf *b, V v, int depth) {
  if (depth > V_MAX_DEPTH) { buf_puts(b, "..."); return; }
  switch (v.t) {
    case V_NULL: buf_putc(b, '-'); break;
    case V_BOOL: buf_puts(b, v.u.b ? "t" : "f"); break;
    case V_NUM:  fmt_num(b, v.u.n); break;
    case V_STR:  buf_add_escaped(b, v.u.s); break;
    case V_FN:   c_fn(b, v.u.f, false); break;
    case V_NAT:  buf_puts(b, "<nat>"); break;
    case V_ERR:  c_err(b, v.u.e, depth); break;
    case V_PLAN: c_plan(b, v.u.pl); break;
    case V_REC:
      if (!v.u.r || !v.u.r->len) buf_puts(b, "{}");
      else { buf_putc(b, '{'); c_rec_body(b, v.u.r, depth); buf_putc(b, '}'); }
      break;
    case V_LIST: {
      List *l = v.u.l;
      if (!l || !l->len) { buf_puts(b, "[]"); break; }
      buf_putc(b, '[');
      for (int i = 0; i < l->len; i++) { if (i) buf_putc(b, ','); c_leaf(b, l->v[i], depth + 1); }
      buf_putc(b, ']');
      break;
    }
    default: buf_puts(b, "?"); break;
  }
}
/* values of entries live one level deeper than the record itself, so a record
 * costs the same depth budget with or without its (top-level) outer braces */
static void c_rec_body(Buf *b, Rec *r, int depth) {
  if (!r) { buf_puts(b, "{}"); return; }
  for (int i = 0; i < r->len; i++) {
    if (i) buf_putc(b, ',');
    c_key(b, r->kv[i].k); buf_putc(b, '=');
    c_leaf(b, r->kv[i].v, depth + 1);
  }
}

/* ---------- JSON form ---------- */
static void j_val(Buf *b, V v, int depth);
static void j_err(Buf *b, PErr *e, int depth) {
  if (!e) { buf_puts(b, "null"); return; }
  buf_puts(b, "{\"__err\":\""); buf_puts(b, err_name(e->code));
  buf_puts(b, "\",\"msg\":"); buf_json_str(b, e->msg);
  buf_puts(b, ",\"hint\":"); buf_json_str(b, e->hint);
  V d = e->data;
  if (d.t == V_REC && d.u.r) {
    for (int i = 0; i < d.u.r->len; i++) {
      buf_putc(b, ','); buf_json_str(b, d.u.r->kv[i].k); buf_putc(b, ':');
      j_val(b, d.u.r->kv[i].v, depth + 1);
    }
  } else if (d.t != V_NULL) { buf_puts(b, ",\"data\":"); j_val(b, d, depth + 1); }
  buf_putc(b, '}');
}
static void j_val(Buf *b, V v, int depth) {
  if (depth > V_MAX_DEPTH) { buf_puts(b, "null"); return; }  /* stays valid JSON */
  switch (v.t) {
    case V_BOOL: buf_puts(b, v.u.b ? "true" : "false"); break;
    case V_NUM:  if (isfinite(v.u.n)) fmt_num(b, v.u.n); else buf_puts(b, "null"); break;
    case V_STR:  buf_json_str(b, v.u.s); break;
    case V_LIST: {
      List *l = v.u.l;
      buf_putc(b, '[');
      for (int i = 0; l && i < l->len; i++) { if (i) buf_putc(b, ','); j_val(b, l->v[i], depth + 1); }
      buf_putc(b, ']');
      break;
    }
    case V_REC: {
      Rec *r = v.u.r;
      buf_putc(b, '{');
      for (int i = 0; r && i < r->len; i++) {
        if (i) buf_putc(b, ',');
        buf_json_str(b, r->kv[i].k); buf_putc(b, ':');
        j_val(b, r->kv[i].v, depth + 1);
      }
      buf_putc(b, '}');
      break;
    }
    case V_ERR:  j_err(b, v.u.e, depth); break;
    case V_PLAN: if (v.u.pl) plan_disp(v.u.pl, b, false); else buf_puts(b, "null"); break;
    default:     buf_puts(b, "null"); break;   /* null, fn, nat */
  }
}

void v_disp(Buf *b, V v, int depth, bool compact) {
  if (!b) return;
  if (!compact) { j_val(b, v, depth); return; }
  if (depth > V_MAX_DEPTH) { buf_puts(b, "..."); return; }
  /* top level only: a non-empty record omits its outer braces, nothing else */
  if (v.t == V_REC && v.u.r && v.u.r->len) { c_rec_body(b, v.u.r, depth); return; }
  c_leaf(b, v, depth);
}

Str v_tostr(Arena *a, V v, bool compact) {
  Buf b; buf_init(&b, a);
  v_disp(&b, v, 0, compact);
  return buf_take(&b);
}
Str v_tojson(Arena *a, V v) { return v_tostr(a, v, false); }
Str v_repr(Arena *a, V v) {
  Buf b; buf_init(&b, a);
  switch (v.t) {
    case V_NULL: buf_puts(&b, "NIL"); break;
    case V_BOOL: buf_puts(&b, v.u.b ? "BOOL(t)" : "BOOL(f)"); break;
    case V_NUM:  buf_puts(&b, "NUM("); fmt_num(&b, v.u.n); buf_putc(&b, ')'); break;
    case V_STR:  buf_puts(&b, "STR("); buf_c_escape(&b, v.u.s); buf_putc(&b, ')'); break;
    case V_LIST: buf_fmt(&b, "LIST[%d]", v.u.l ? v.u.l->len : 0); break;
    case V_REC:  buf_fmt(&b, "REC[%d]", v.u.r ? v.u.r->len : 0); break;
    case V_FN:   c_fn(&b, v.u.f, true); break;
    case V_PLAN: buf_puts(&b, "PLAN"); break;
    case V_ERR:  buf_fmt(&b, "ERR(%s)", v.u.e ? err_name(v.u.e->code) : "INTERNAL"); break;
    case V_NAT:  buf_puts(&b, "NAT"); break;
    default:     buf_puts(&b, "?"); break;
  }
  return buf_take(&b);
}
int v_tok_est(V v) {
  static Arena a;
  if (!a.cur) arena_init(&a, 0);
  if (a.total > 1u << 20) { arena_free(&a); arena_init(&a, 0); }   /* bounded scratch */
  Buf b; buf_init(&b, &a);
  v_disp(&b, v, 0, true);
  return tok_est(b.p, b.len);
}
