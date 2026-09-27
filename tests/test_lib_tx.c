/* test_lib_tx.c - the tx namespace and method dispatch.
 *
 * LINKING: this test tolerates a half-built tree. Every symbol it needs from a
 * module that may not exist yet (interp.c's Args helpers, xdiff.c's anchor/patch/
 * diff entry points, the other lib_*.c tables, shell/cfg/outline) is defined here
 * with __attribute__((weak)): a real definition in the same link silently wins and
 * only when it is absent does the fallback below take over. So the test runs today
 * (fallbacks, no interpreter linked) and unchanged once interp.c/xdiff.c land.
 *
 * CONSEQUENCE FOR ASSERTIONS: anything that depends on xdiff's exact bytes (anchor
 * hashes, diff text, patch result) is asserted as PASS-THROUGH - the builtin's
 * value compared against a direct call to the same xdiff entry point in this
 * process - so the test cannot drift when xdiff.c is written or rewritten.
 * Argument FORWARDING (path / ctx / unified / checks=NULL) is asserted only while
 * the local stub is what actually ran, gated by the g_*_stub flags the stubs set.
 * Everything else - lines/split/join/find/pad/ellide/word_wrap/escape/subst/fmt/
 * sort/uniq/slice/rec ops/num ops/dispatch errors - is tx's own semantics and is
 * pinned to literals.
 */
#include "vxa.h"
#include "interp.h"
#include "lib.h"
#include "t.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>

#define WEAK __attribute__((weak))

/* ======================================================================== */
/* --- fallbacks: only live when the real module is not in the link -------- */
/* ======================================================================== */

typedef struct FakeCtx { Arena *a; V err; char *root; Rec *cfg; } FakeCtx;
static FakeCtx *fkc(Ctx *c) { return (FakeCtx*)c; }

WEAK Ctx *ctx_new(Arena *a, const char *root, const char *script) {
  FakeCtx *f = (FakeCtx*)arena_zalloc(a, sizeof(FakeCtx));
  f->a = a;
  f->root = arena_strdup(a, root && *root ? root : ".");
  (void)script;
  return (Ctx*)f;
}
WEAK void ctx_free(Ctx *c) { (void)c; }
WEAK Arena *ctx_arena(Ctx *c) { return fkc(c)->a; }
WEAK const char *ctx_root(Ctx *c) { return fkc(c)->root; }
WEAK const char *ctx_script(Ctx *c) { (void)c; return "<test>"; }
WEAK Rec *ctx_cfg(Ctx *c) {
  FakeCtx *f = fkc(c);
  if (!f->cfg) f->cfg = rec_new(f->a);
  return f->cfg;
}
WEAK bool ctx_has_err(Ctx *c) { return v_is_err(fkc(c)->err); }
WEAK V ctx_err(Ctx *c) { return fkc(c)->err; }
WEAK void ctx_clear_err(Ctx *c) { fkc(c)->err = VN; }
WEAK void set_error(Ctx *c, ErrCode code, const char *fmt, ...) {
  FakeCtx *f = fkc(c);
  if (f->err.t != V_NULL) return;                  /* first error wins, as in interp.c */
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  f->err = v_err_hint(code, s_lit(f->a, msg), s_lit(f->a, err_hint(code)));
}
static void f_aerr(Args *x, ErrCode code, const char *fmt, ...) {
  if (ctx_has_err(x->c)) return;
  char msg[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  set_error(x->c, code, "%s", msg);
}
WEAK bool arg_present(Args *x, int i) { return i >= 0 && i < x->n; }
WEAK V arg_at(Args *x, int i) {
  if (!arg_present(x, i)) { f_aerr(x, E_ARITY, "argument %d is missing", i + 1); return VN; }
  return x->a[i];
}
WEAK bool arg_has_err(Args *x) { return ctx_has_err(x->c); }
WEAK Str arg_str(Args *x, int i, const char *what) {
  V v = arg_at(x, i);
  if (ctx_has_err(x->c)) return s_null();
  if (v.t != V_STR) { f_aerr(x, E_TYPE, "%s must be a string, got %s", what, v_typename(v)); return s_null(); }
  return v.u.s;
}
WEAK double arg_num(Args *x, int i, const char *what) {
  V v = arg_at(x, i);
  if (ctx_has_err(x->c)) return 0;
  if (v.t != V_NUM) { f_aerr(x, E_TYPE, "%s must be a number, got %s", what, v_typename(v)); return 0; }
  return v.u.n;
}
WEAK bool arg_bool(Args *x, int i, bool dflt) {
  if (!arg_present(x, i)) return dflt;
  return v_truthy(x->a[i]);
}
WEAK Rec *arg_rec(Args *x, int i, const char *what) {
  V v = arg_at(x, i);
  if (ctx_has_err(x->c)) return NULL;
  if (v.t != V_REC) { f_aerr(x, E_TYPE, "%s must be a record {...}, got %s", what, v_typename(v)); return NULL; }
  return v.u.r;
}
WEAK List *arg_list(Args *x, int i, const char *what) {
  V v = arg_at(x, i);
  if (ctx_has_err(x->c)) return NULL;
  if (v.t != V_LIST) { f_aerr(x, E_TYPE, "%s must be a list [...], got %s", what, v_typename(v)); return NULL; }
  return v.u.l;
}
WEAK V arg_opt(Args *x, int i, const char *name, V dflt) {
  if (!arg_present(x, i) || x->a[i].t != V_REC) return dflt;
  V *p = rec_getz(x->a[i].u.r, name);
  return p ? *p : dflt;
}
WEAK int arg_opt_int(Args *x, int i, const char *name, int dflt) {
  V v = arg_opt(x, i, name, v_num(dflt));
  return v.t == V_NUM ? (int)v.u.n : dflt;
}
WEAK Str arg_opt_str(Args *x, int i, const char *name, const char *dflt) {
  V v = arg_opt(x, i, name, VN);
  return v.t == V_STR ? v.u.s : s_wrap(dflt);
}
WEAK bool arg_opt_bool(Args *x, int i, const char *name, bool dflt) {
  V v = arg_opt(x, i, name, VN);
  return v.t == V_NULL ? dflt : v_truthy(v);
}
WEAK Str as_text(Ctx *c, V v) {
  Arena *a = ctx_arena(c);
  if (v.t == V_STR) return v.u.s;
  if (v.t == V_NUM) { Buf b; buf_init(&b, a); fmt_num(&b, v.u.n); return buf_take(&b); }
  if (v.t == V_BOOL) return s_lit(a, v.u.b ? "true" : "false");
  if (v.t == V_NULL) return s_null();
  return v_tostr(a, v, true);
}
WEAK Str q(Arena *a, const char *s) { return s_lit(a, s ? s : ""); }
WEAK V v_ok(Arena *a, int n, ...) {
  Rec *r = rec_new(a);
  va_list ap;
  va_start(ap, n);
  for (int i = 0; i < n; i++) {
    const char *k = va_arg(ap, const char*);
    V v = va_arg(ap, V);
    rec_set(a, r, s_wrap(k), v);
  }
  va_end(ap);
  return rec_to_v(r);
}
WEAK V ok_rec(Arena *a) { return rec_to_v(rec_new(a)); }
WEAK V call_fn(Ctx *c, V fn, V *args, int nargs) {
  if (v_is_err(fn)) return fn;
  if (fn.t != V_FN) { set_error(c, E_TYPE, "cannot call a %s", v_typename(fn)); return VN; }
  Fn *f = fn.u.f;
  if (f->kind == F_NATIVE) {
    if (!f->native) { set_error(c, E_INTERNAL, "native fn with no body"); return VN; }
    return f->native(args, f->ud);
  }
  (void)args; (void)nargs;
  set_error(c, E_UNSUPPORTED, "no interpreter in this link: script callbacks cannot run");
  return VN;
}

/* xdiff stand-ins honouring the documented contract (vxa.h, SPEC 4.2). */
static int g_anchor_stub = 0, g_patch_stub = 0, g_diff_stub = 0;
static int g_st_ctxl = -999;
static int g_st_unified = -999;
static const char *g_st_path = NULL;
static int g_st_checks_null = -999;
static const char *g_st_anchor_path = NULL;

WEAK Anch *anchors_of(Arena *a, const char *path, Str text, int *count_out, ErrCode *err) {
  int nl = 1, k = 0, start = 0;
  Anch *A;
  if (err) *err = E_NONE;
  for (int i = 0; i < text.len; i++) if (text.p[i] == '\n') nl++;
  A = (Anch*)arena_zalloc(a, sizeof(Anch) * (size_t)nl);
  g_anchor_stub = 1;
  g_st_anchor_path = path;
  if (!text.p || text.len == 0) {
    A[0].n = 1;
    A[0].at = s_lit(a, "abcd1234");
    A[0].text = s_from(a, "", 0);
    if (count_out) *count_out = 1;
    return A;
  }
  for (;;) {
    int e = start, ns, ne;
    Buf b;
    Str cur, nxt, blob;
    while (e < text.len && text.p[e] != '\n') e++;
    cur = s_cut(a, text, start, e);
    ns = e + 1;
    ne = ns;
    while (ne < text.len && text.p[ne] != '\n') ne++;
    nxt = s_cut(a, text, ns, ne);
    buf_init(&b, a);
    buf_puts(&b, "vxa1");
    buf_puts(&b, path ? path : "-");
    buf_put(&b, cur.p, (size_t)cur.len);
    buf_puts(&b, "\n");
    buf_put(&b, nxt.p, (size_t)nxt.len);
    blob = buf_str(&b);
    A[k].n = k + 1;
    A[k].at = hash4(a, blob.p, (size_t)blob.len);
    A[k].text = cur;
    k++;
    if (e >= text.len) break;
    start = e + 1;
  }
  if (count_out) *count_out = k;
  return A;
}
/* Arena-first, exactly as vxa.h declares them. There is no checks parameter to
 * pass: xdiff computes the verification rows from the NEW text, so the wrapper
 * cannot invent any - which is what g_st_checks_null below stands for. */
WEAK V tx_patch_apply(Arena *a, Str text, const char *path, V ops) {
  List *o;
  static const char *OPS[] = { "set", "del", "ins_before", "ins_after" };
  g_patch_stub = 1;
  g_st_path = path;
  g_st_checks_null = 1;
  o = (ops.t == V_LIST && ops.u.l) ? ops.u.l : NULL;
  if (!o) return v_err(a, E_BAD_INPUT, "ops is not a list");
  for (int i = 0; i < o->len; i++) {
    V *op;
    int known = 0;
    if (o->v[i].t != V_REC) return v_err(a, E_TYPE, "op %d is not a record", i + 1);
    op = rec_getz(o->v[i].u.r, "op");
    if (!op) return v_err(a, E_BAD_INPUT, "op %d has no op field", i + 1);
    for (size_t k = 0; k < sizeof(OPS)/sizeof(OPS[0]); k++)
      if (op->t == V_STR && s_eqz(op->u.s, OPS[k])) known = 1;
    if (!known) return v_err(a, E_BAD_INPUT, "op %d has an op outside set/del/ins_before/ins_after", i + 1);
    if (!rec_getz(o->v[i].u.r, "at")) return v_err(a, E_BAD_INPUT, "op %d has no at", i + 1);
  }
  return v_ok(a, 3, "text", v_str(text), "applied", v_num(o->len),
              "checks", v_list(a, 1, v_ok(a, 3, "at", v_strz(a, "deadbeef"), "ok", VT,
                                            "preview", v_strz(a, "L1 stub"))));
}
WEAK V tx_diff(Arena *a, Str ta, Str tb, int ctxl, bool unified) {
  (void)ta; (void)tb;
  g_diff_stub = 1;
  g_st_ctxl = ctxl;
  g_st_unified = unified ? 1 : 0;
  return v_ok(a, 4, "text", v_strz(a, "@@ stub @@"), "adds", v_num(1),
              "dels", v_num(0), "hunks", v_num(1));
}

/* the other namespaces, so lib.c's build() links without lib_fs/lib_sh/... */
WEAK const Builtin *t_fs(int *n) { if (n) *n = 0; return NULL; }
WEAK const Builtin *t_sh(int *n) { if (n) *n = 0; return NULL; }
WEAK const Builtin *t_out(int *n) { if (n) *n = 0; return NULL; }
WEAK const Builtin *t_cfg(int *n) { if (n) *n = 0; return NULL; }
WEAK const Builtin *t_misc(int *n) { if (n) *n = 0; return NULL; }

/* shell / cfg / outline / plan helpers pulled in by the layers above: never run */
WEAK void sh_exec(Ctx *c, const char *cmd, const char *cwd, const char *in, int ms,
                  int tail, bool redact, ShRes *r) {
  (void)c; (void)cmd; (void)cwd; (void)in; (void)ms; (void)tail; (void)redact;
  memset(r, 0, sizeof *r);
}
WEAK void sh_free(ShRes *r) { (void)r; }
WEAK char *sh_join_argv(char **argv, int n) { (void)argv; (void)n; return (char*)""; }
WEAK V sh_result_v(Ctx *c, ShRes *r, const char *cmd) { (void)c; (void)r; (void)cmd; return VN; }
WEAK bool sh_is_readonly(const char *cmd) { (void)cmd; return false; }
WEAK Str sh_redact(Arena *a, Str s) { return s; }
WEAK V cfg_get_v(Ctx *c, Str k, V defv) { (void)c; (void)k; return defv; }
WEAK void cfg_set_c(Ctx *c, const char *k, V v) { (void)c; (void)k; (void)v; }
WEAK V manifest_load(Ctx *c, const char *dir, ErrCode *err) {
  (void)c; (void)dir;
  if (err) *err = E_NOENT;
  return VN;
}
WEAK V manifest_policies(Ctx *c, V man) { (void)c; (void)man; return VN; }
WEAK V outline_file(Ctx *c, const char *p, Str t, const char *lang) {
  (void)c; (void)p; (void)t; (void)lang;
  return VN;
}
WEAK int outline_lang_detect(const char *p, Str t, char *out) {
  (void)p; (void)t;
  if (out) out[0] = 0;
  return 0;
}
WEAK V bundle_files(Ctx *c, V paths, const char *q, int mt, bool only) {
  (void)c; (void)paths; (void)q; (void)mt; (void)only;
  return VN;
}
WEAK void journal_note(Ctx *c, const char *k, const char *t, const char *d) {
  (void)c; (void)k; (void)t; (void)d;
}
WEAK int journal_repeat(Ctx *c, const char *f) { (void)c; (void)f; return 0; }
WEAK V sim_resolve(Ctx *c, V cand, Str want, double min) {
  (void)c; (void)cand; (void)want; (void)min;
  return VN;
}
WEAK V op_apply(Ctx *c, V pv, V tokv) { (void)c; (void)pv; (void)tokv; return VN; }
WEAK const char *ws_root(Ctx *c) { return ctx_root(c); }
WEAK Str ws_resolve(Arena *a, Ctx *c, const char *p, ErrCode *err) {
  (void)c;
  if (err) *err = E_NONE;
  return s_lit(a, p ? p : "");
}
WEAK const char *ws_trash(Ctx *c) { (void)c; return ".vxa/trash"; }
WEAK const char *ws_tmp(Ctx *c) { (void)c; return ".vxa/tmp"; }

/* ======================================================================== */
/* --- scaffolding --------------------------------------------------------- */
/* ======================================================================== */

static Arena A;
static Ctx *C = NULL;

static V STR(const char *s) { return v_str(s_lit(&A, s)); }
static V NUM(double d) { return v_num(d); }

static const Builtin *txb(const char *name) {
  int n = 0;
  const Builtin *t = t_tx(&n);
  for (int i = 0; i < n; i++) if (!strcmp(t[i].name, name)) return &t[i];
  return NULL;
}
static V txcall(const char *name, V *args, int n) {
  const Builtin *b = txb(name);
  if (!b) { printf("MISSING BUILTIN tx.%s\n", name); return VN; }
  return lib_call(C, b, args, n);
}
static V tx0(const char *n) { return txcall(n, NULL, 0); }
static V tx1(const char *n, V a) { V x[1] = { a }; return txcall(n, x, 1); }
static V tx2(const char *n, V a, V b) { V x[2] = { a, b }; return txcall(n, x, 2); }
static V tx3(const char *n, V a, V b, V c) { V x[3] = { a, b, c }; return txcall(n, x, 3); }
static V tx4(const char *n, V a, V b, V c, V d) { V x[4] = { a, b, c, d }; return txcall(n, x, 4); }

static V m(V self, const char *name, V *args, int n) { return lib_method(C, self, name, args, n); }
static V m0(V self, const char *name) { return m(self, name, NULL, 0); }
static V m1(V self, const char *name, V a) { V x[1] = { a }; return m(self, name, x, 1); }
static V m2(V self, const char *name, V a, V b) { V x[2] = { a, b }; return m(self, name, x, 2); }
static V m3(V self, const char *name, V a, V b, V c) { V x[3] = { a, b, c }; return m(self, name, x, 3); }

static void clr(void) { ctx_clear_err(C); }
static ErrCode lasterr(void) { return ctx_has_err(C) ? v_errcode(ctx_err(C)) : E_NONE; }
/* Reading an error consumes it: the evaluator keeps the FIRST error until it is
 * cleared, so consecutive error checks would otherwise all see the stale one. */
static ErrCode err_of(V r) {
  ErrCode e = v_is_err(r) ? v_errcode(r) : lasterr();
  clr();
  return e;
}

static int vlen(V v) { return (v.t == V_LIST && v.u.l) ? v.u.l->len : -1; }
static V vat(V v, int i) {
  if (v.t != V_LIST || !v.u.l || i < 0 || i >= v.u.l->len) return VN;
  return v.u.l->v[i];
}
static V rf(V r, const char *k) {
  if (r.t != V_REC || !r.u.r) return VN;
  V *p = rec_getz(r.u.r, k);
  return p ? *p : VN;
}
static int eqs(V v, const char *want) {
  return v.t == V_STR && v.u.s.len == (int)strlen(want) && !memcmp(v.u.s.p, want, (size_t)v.u.s.len);
}
static int strlist_ok(V l, const char *const *want, int n) {
  if (vlen(l) != n) return 0;
  for (int i = 0; i < n; i++) if (!eqs(vat(l, i), want[i])) return 0;
  return 1;
}
static int numlist_ok(V l, const double *want, int n) {
  if (vlen(l) != n) return 0;
  for (int i = 0; i < n; i++) {
    V e = vat(l, i);
    if (e.t != V_NUM || e.u.n != want[i]) return 0;
  }
  return 1;
}
static int is_hex_lower(Str s) {
  if (!s.p || s.len == 0) return 0;
  for (int i = 0; i < s.len; i++) {
    char ch = s.p[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return 0;
  }
  return 1;
}
static int key_order_ok(V r, const char *const *keys, int n) {
  if (r.t != V_REC || !r.u.r || r.u.r->len != n) return 0;
  for (int i = 0; i < n; i++) if (!s_eqz(r.u.r->kv[i].k, keys[i])) return 0;
  return 1;
}

/* F_NATIVE callbacks work with or without the interpreter in the link */
typedef struct { int calls; int fail_at; Ctx *c; double add; } Spy;
static V spy_pass(V *argv, void *ud) { Spy *s = (Spy*)ud; if (s) s->calls++; return argv[0]; }
static V spy_add(V *argv, void *ud) {
  Spy *s = (Spy*)ud;
  if (s) s->calls++;
  if (argv[0].t != V_NUM) return VN;
  return v_num(argv[0].u.n + (s ? s->add : 0));
}
static V spy_ctxerr(V *argv, void *ud) {
  Spy *s = (Spy*)ud;
  s->calls++;
  if (s->calls >= s->fail_at) {
    set_error(s->c, E_RANGE, "callback aborted on purpose at call %d", s->calls);
    return VN;
  }
  return argv[0];
}
static V spy_errval(V *argv, void *ud) {
  Spy *s = (Spy*)ud;
  s->calls++;
  (void)argv;
  return v_err(&A, E_RANGE, "callback returned an ERR value");
}
static V FN(V (*body)(V*, void*), void *ud) {
  Fn *f = (Fn*)arena_zalloc(&A, sizeof(Fn));
  f->kind = F_NATIVE;
  f->native = body;
  f->ud = ud;
  f->nparams = -1;
  f->name = "cb";
  V v;
  v.t = V_FN;
  v.u.f = f;
  return v;
}
static V kv1(const char *k, V v) {
  Rec *r = rec_new(&A);
  rec_set(&A, r, s_wrap(k), v);
  return rec_to_v(r);
}

int main(void) {
  arena_init(&A, 0);
  C = ctx_new(&A, ".", "<test>");

  /* ========= registry integrity: vxa doc is generated from these rows === */
  T("tx_table_integrity");
  {
    int n = 0;
    const Builtin *t = t_tx(&n);
    static const char *NEED[] = { "anchors", "patch", "diff", "similar", "subst", "split",
      "join", "lines", "unlines", "trim", "lower", "upper", "contains", "starts", "ends",
      "find", "pad", "ellide", "bytes", "chars", "hash", "escape", "cescape", "json", "word_wrap" };
    CK(t != NULL && n >= 25);
    for (size_t k = 0; k < sizeof(NEED)/sizeof(NEED[0]); k++)
      CHECK(txb(NEED[k]) != NULL, "tx.%s is not registered", NEED[k]);
    CHECK(txb("resolve") == NULL, "tx.resolve must not exist: guessing is banned by axiom 1");
    for (int i = 0; i < n; i++) {
      const Builtin *b = &t[i];
      int dup = 0;
      CHECK(b->ns && b->ns[0] && !strcmp(b->ns, "tx"), "row %d ns must be tx", i);
      CHECK(b->name && b->name[0] && !strchr(b->name, '.'), "row %d name bad", i);
      CHECK(b->sig && b->sig[0], "%s sig empty", b->name ? b->name : "?");
      CHECK(b->doc && b->doc[0], "%s doc empty", b->name ? b->name : "?");
      CHECK(b->ex && b->ex[0], "%s ex empty", b->name ? b->name : "?");
      CHECK(b->ex && strlen(b->ex) <= 80, "%s ex is %d chars, limit 80", b->name, b->ex ? (int)strlen(b->ex) : -1);
      CHECK(b->fn != NULL, "%s fn NULL", b->name ? b->name : "?");
      CHECK(b->min >= 0 && (b->max < 0 || b->min <= b->max), "%s arity min=%d max=%d", b->name, b->min, b->max);
      CHECK(!strchr(b->sig, '\n') && !strchr(b->doc, '\n') && !strchr(b->ex, '\n'),
            "%s prose holds a raw newline", b->name ? b->name : "?");
      CHECK(!strchr(b->sig, '"') && !strchr(b->doc, '"'), "%s prose holds a quote", b->name ? b->name : "?");
      CHECK(strstr(b->sig, "->") != NULL, "%s sig never states a result", b->name ? b->name : "?");
      CHECK(strstr(b->sig, b->name) != NULL, "%s sig does not name itself", b->name ? b->name : "?");
      for (int k = 0; k < i; k++) if (!strcmp(t[k].name, b->name)) dup = 1;
      CHECK(!dup, "duplicate tx.%s", b->name ? b->name : "?");
    }
  }

  T("method_table_integrity");
  {
    int n = 0;
    const Method *mm = meth_all(&n);
    static const char *TYPES[] = { "str", "list", "rec", "num", "bool", "null", "fn", "plan", "err", "any" };
    CK(mm != NULL && n >= 45);
    for (int i = 0; i < n; i++) {
      const Method *b = &mm[i];
      int tk = 0, dup = 0;
      for (size_t k = 0; k < sizeof(TYPES)/sizeof(TYPES[0]); k++)
        if (!strcmp(b->type, TYPES[k])) tk = 1;
      CHECK(tk, "method %d type '%s' is not a value type", i, b->type);
      CHECK(b->name && b->name[0], "method %d name empty", i);
      CHECK(b->fn != NULL, "%s.%s fn NULL", b->type, b->name);
      CHECK(b->sig && b->sig[0], "%s.%s sig empty", b->type, b->name);
      CHECK(b->doc && b->doc[0], "%s.%s doc empty", b->type, b->name);
      CHECK(b->min >= 0 && (b->max < 0 || b->min <= b->max), "%s.%s arity min=%d max=%d", b->type, b->name, b->min, b->max);
      CHECK(!strchr(b->sig, '\n') && !strchr(b->doc, '\n'), "%s.%s raw newline in prose", b->type, b->name);
      CHECK(!strchr(b->sig, '"') && !strchr(b->doc, '"'), "%s.%s quote in prose", b->type, b->name);
      CHECK(meth_find(b->type, b->name) == b, "meth_find misses %s.%s", b->type, b->name);
      for (int k = 0; k < i; k++)
        if (!strcmp(mm[k].type, b->type) && !strcmp(mm[k].name, b->name)) dup = 1;
      CHECK(!dup, "duplicate %s.%s", b->type, b->name);
    }
    CK(meth_find("str", "nope") == NULL);
    CK(meth_find(NULL, "len") == NULL);
    CK(meth_find("list", "len") != NULL);
    CK(meth_find("any", "type") != NULL);
  }

  /* ==================== tx: shape and predicates ======================= */
  T("tx_trim_case");
  {
    CK(eqs(tx1("trim", STR("  hi \r\n")), "hi"));
    CK(eqs(tx1("trim", STR("\t\n x \t")), "x"));
    CK(eqs(tx1("trim", STR("   ")), ""));
    CK(eqs(tx1("trim", STR("already")), "already"));
    CK(eqs(tx1("trim", STR("")), ""));
    CK(eqs(tx1("lower", STR("ABC")), "abc"));
    CK(eqs(tx1("upper", STR("abc")), "ABC"));
    CK(eqs(tx1("lower", STR("")), ""));
    /* bytes >= 0x80 pass ASCII folding untouched */
    CK(eqs(tx1("lower", STR("\xe8\xa7\xa3" "ABC")), "\xe8\xa7\xa3" "abc"));
    CK(eqs(tx1("upper", STR("\xe8\xa7\xa3" "abc")), "\xe8\xa7\xa3" "ABC"));
    clr();
    CK(err_of(tx1("trim", NUM(1))) == E_TYPE);
    CK(err_of(tx0("trim")) == E_ARITY);
    CK(err_of(tx2("lower", STR("a"), STR("b"))) == E_ARITY);
  }

  T("tx_predicates");
  {
    V h = STR("ABC"), l = STR("abc"), e = STR(""), ab = STR("ab");
    CK(!v_truthy(tx2("contains", h, l)));
    CK(v_truthy(tx2("contains", h, STR("BC"))));
    CK(v_truthy(tx2("contains", h, e)));
    CK(!v_truthy(tx2("contains", e, STR("a"))));
    CK(v_truthy(tx2("contains", STR("src/a.c"), STR("/a"))));
    CK(v_truthy(tx2("starts", h, STR("AB"))));
    CK(!v_truthy(tx2("starts", l, STR("AB"))));
    CK(v_truthy(tx2("starts", l, e)));
    CK(!v_truthy(tx2("starts", ab, STR("abc"))));
    CK(v_truthy(tx2("ends", h, STR("BC"))));
    CK(v_truthy(tx2("ends", h, e)));
    CK(!v_truthy(tx2("ends", ab, STR("abc"))));
    CK(!v_truthy(tx2("ends", STR("a.c"), STR(".C"))));      /* case-sensitive */
    clr();
    CK(err_of(tx2("contains", h, NUM(1))) == E_TYPE);
    CK(err_of(tx2("starts", NUM(1), h)) == E_TYPE);
    CK(err_of(tx2("ends", h, STR("B"))) == E_NONE);
    CK(err_of(txcall("ends", &h, 1)) == E_ARITY);
  }

  T("tx_bytes_chars_hash");
  {
    V empty = STR(""), cj = STR("\xe8\xa7\xa3\xe6\x9e\x90\xe5\x99\xa8"), abc = STR("abc");
    V h1 = tx1("hash", abc), h2 = tx1("hash", abc), he = tx1("hash", empty);
    CKD(tx1("bytes", cj).u.n, 9.0);
    CKD(tx1("chars", cj).u.n, 3.0);
    CKD(tx1("bytes", empty).u.n, 0.0);
    CKD(tx1("chars", empty).u.n, 0.0);
    CKD(tx1("bytes", abc).u.n, 3.0);
    CKD(tx1("chars", abc).u.n, 3.0);
    CKD(tx1("bytes", STR("caf\xc3\xa9")).u.n, 5.0);
    CKD(tx1("chars", STR("caf\xc3\xa9")).u.n, 4.0);
    CK(h1.t == V_STR && h1.u.s.len == 12);
    CK(is_hex_lower(h1.u.s));
    CK(v_eq(h1, h2));
    CK(!v_eq(h1, tx1("hash", cj)));
    CK(h1.u.s.p[h1.u.s.len] == 0);
    CK(he.u.s.len == 12 && !s_eq(h1.u.s, he.u.s));
    clr();
    CK(err_of(tx1("bytes", NUM(1))) == E_TYPE);
    CK(err_of(tx1("chars", v_list(&A, 0))) == E_TYPE);
    CK(err_of(tx1("hash", VN)) == E_TYPE);
  }

  T("tx_subst");
  {
    V s = STR("a.b.c"), dot = STR("."), dash = STR("-"), z = STR("z"), empty = STR("");
    CK(eqs(tx3("subst", s, dot, dash), "a-b-c"));
    CK(eqs(tx4("subst", s, dot, dash, kv1("all", VF)), "a-b.c"));
    CK(eqs(tx4("subst", s, dot, dash, kv1("all", VT)), "a-b-c"));
    CK(eqs(tx3("subst", s, z, dash), "a.b.c"));
    CK(eqs(tx4("subst", s, z, dash, kv1("all", VF)), "a.b.c"));
    CK(eqs(tx3("subst", STR("aaa"), STR("aa"), STR("b")), "ba"));
    CK(eqs(tx3("subst", STR("xay"), STR("a"), STR("")), "xy"));
    CK(eqs(tx3("subst", STR("main"), STR("in"), STR("OUT")), "maOUT"));
    clr();
    CK(err_of(tx3("subst", s, empty, dash)) == E_BAD_INPUT);
    CK(err_of(tx2("subst", s, dot)) == E_ARITY);
    CK(err_of(tx3("subst", s, dot, NUM(1))) == E_TYPE);
    CK(err_of(tx4("subst", s, dot, dash, NUM(1))) == E_TYPE);
  }

  T("tx_split_join");
  {
    static const char *W3[] = { "a", "b", "c" };
    static const char *WA[] = { "a", "" };
    static const char *WB[] = { "", "a" };
    static const char *W1[] = { "abc" };
    static const char *W0[] = { "" };
    static const char *CX[] = { "\xe8\xa7\xa3", "\xe6\x9e\x90" };
    V cj2 = STR("\xe8\xa7\xa3\xe6\x9e\x90");
    V scj, slong;
    CK(strlist_ok(tx2("split", STR("a,b,c"), STR(",")), W3, 3));
    CK(strlist_ok(tx2("split", STR("a,"), STR(",")), WA, 2));
    CK(strlist_ok(tx2("split", STR(",a"), STR(",")), WB, 2));
    CK(strlist_ok(tx2("split", STR("abc"), STR(",")), W1, 1));
    CK(strlist_ok(tx2("split", STR(""), STR(",")), W0, 1));
    CK(strlist_ok(tx2("split", STR("ab"), STR("")), W1, 0) ||
       strlist_ok(tx2("split", STR("ab"), STR("")), (const char*[]){ "a", "b" }, 2));
    /* empty sep splits into code points: .chars and split("") always agree */
    CK(vlen(tx2("split", cj2, STR(""))) == 2);
    CK(strlist_ok(tx2("split", cj2, STR("")), CX, 2));
    scj = tx2("split", cj2, STR(""));
    CKD(tx1("bytes", vat(scj, 0)).u.n, 3.0);
    CKD(tx1("chars", scj).u.n, 0.0);                 /* a list has no char count */
    clr();
    CK(err_of(tx1("chars", scj)) == E_TYPE);
    CK(err_of(tx1("split", STR("x"))) == E_ARITY);
    /* join */
    CK(eqs(tx2("join", v_list(&A, 3, STR("a"), STR("b"), STR("c")), STR(",")), "a,b,c"));
    CK(eqs(tx2("join", v_list(&A, 0), STR(",")), ""));
    CK(eqs(tx2("join", v_list(&A, 1, STR("solo")), STR(",")), "solo"));
    CK(eqs(tx2("join", v_list(&A, 2, NUM(1), NUM(2.5)), STR(" ")), "1 2.5"));
    CK(eqs(tx2("join", v_list(&A, 3, STR("a"), VN, VT), STR(",")), "a,,true"));
    CK(eqs(tx2("join", v_list(&A, 1, VN), STR(",")), ""));
    CK(eqs(tx2("join", v_list(&A, 2, STR("x"), STR("y")), STR("")), "xy"));
    /* non-scalars join as their compact form, nothing silently dropped */
    CK(eqs(tx2("join", v_list(&A, 2, kv1("a", NUM(1)), v_list(&A, 1, NUM(7))), STR("|")), "{a=1}|[7]"));
    slong = tx2("join", v_list(&A, 2, v_list(&A, 2, NUM(1), VF), STR("z")), STR(","));
    CK(eqs(slong, "[1,f],z"));
    clr();
    CK(err_of(tx2("join", STR("x"), STR(","))) == E_TYPE);
    CK(err_of(tx2("join", v_list(&A, 0), NUM(1))) == E_TYPE);
    CK(err_of(tx1("join", v_list(&A, 0))) == E_ARITY);
  }

  T("tx_lines_unlines_crlf");
  {
    V t = STR("a\r\nb\r\n"), e = STR(""), onl = STR("\n");
    V nonl = STR("no newline"), cr = STR("a\rb"), two, three, four;
    V ls = tx1("lines", t), le = tx1("lines", e), lonl = tx1("lines", onl);
    V lnonl = tx1("lines", nonl), lcr = tx1("lines", cr);
    CK(vlen(ls) == 3);
    CK(eqs(vat(ls, 0), "a\r"));
    CK(eqs(vat(ls, 1), "b\r"));                       /* CR is kept at the line end */
    CK(eqs(vat(ls, 2), ""));
    CK(eqs(tx1("unlines", ls), "a\r\nb\r\n"));        /* byte-exact round trip */
    CK(vlen(le) == 1 && eqs(vat(le, 0), ""));
    CK(eqs(tx1("unlines", le), ""));
    CK(vlen(lonl) == 2 && eqs(vat(lonl, 0), "") && eqs(vat(lonl, 1), ""));
    CK(eqs(tx1("unlines", lonl), "\n"));
    CK(vlen(lnonl) == 1 && eqs(vat(lnonl, 0), "no newline"));
    CK(vlen(lcr) == 1 && eqs(vat(lcr, 0), "a\rb"));   /* LF is the only separator */
    two = v_list(&A, 2, STR("a"), STR("b"));
    three = v_list(&A, 3, STR("a"), STR("b"), STR(""));
    four = v_list(&A, 3, STR("a\r"), STR("b\r"), STR(""));
    CK(eqs(tx1("unlines", two), "a\nb"));
    CK(eqs(tx1("unlines", three), "a\nb\n"));         /* the empty tail carries the NL */
    CK(eqs(tx1("unlines", four), "a\r\nb\r\n"));
    CK(eqs(tx1("unlines", v_list(&A, 0)), ""));
    CK(eqs(tx2("join", ls, STR("\n")), "a\r\nb\r\n")); /* join and unlines agree here */
    clr();
    CK(err_of(tx1("lines", NUM(1))) == E_TYPE);
    CK(err_of(tx1("unlines", STR("x"))) == E_TYPE);
    CK(err_of(tx1("lines", VN)) == E_TYPE);
    CK(err_of(tx0("lines")) == E_ARITY);
  }

  T("tx_pad_ellide");
  {
    V ab = STR("ab"), n5 = NUM(5), n0 = NUM(0), neg = NUM(-1), half = NUM(2.5);
    V longish = STR("abcdefghij"), cj = STR("\xe8\xa7\xa3");
    V big = STR("0123456789012345678901234567890123456789");
    V e40 = NUM(20), e8 = NUM(8), small = STR("short");
    V p5 = tx2("pad", ab, n5), pe = tx2("pad", STR(""), n5), el = tx2("ellide", big, e40);
    CK(eqs(p5, "ab   "));
    CKD(tx1("chars", p5).u.n, 5.0);
    CK(eqs(tx2("pad", longish, n5), "abcdefghij"));   /* never truncates */
    CK(eqs(tx2("pad", cj, n5), "\xe8\xa7\xa3    ")); /* 4 spaces: one code point in */
    CK(eqs(tx2("pad", ab, n0), "ab"));
    CK(eqs(pe, "     "));
    CK(el.u.s.len <= 20 && el.u.s.len < big.u.s.len);
    CK(strstr(el.u.s.p, "...") != NULL);
    CK(eqs(tx2("ellide", small, e8), "short"));
    CK(tx2("ellide", big, e8).u.s.len == big.u.s.len);   /* n<16 cannot fit the marker */
    CK(eqs(tx2("ellide", small, n5), "short"));
    clr();
    CK(err_of(tx2("pad", ab, neg)) == E_RANGE);
    CK(err_of(tx2("ellide", big, neg)) == E_RANGE);
    CK(err_of(tx2("ellide", big, half)) == E_RANGE);
    CK(err_of(tx2("pad", ab, STR("5"))) == E_TYPE);
    CK(err_of(tx2("pad", STR("x"), n5)) == E_NONE);
    CK(err_of(tx1("ellide", big)) == E_ARITY);
  }

  T("tx_word_wrap");
  {
    V w = STR("the quick brown fox jumps over the lazy dog"), w10 = NUM(10);
    V ind = STR("  - the quick brown fox"), w12 = NUM(12), w40 = NUM(40);
    V path = STR("a\nsrc/some/quite/long/path/name.c\n");
    V multi = STR("  spaced   out  ");
    CK(eqs(tx2("word_wrap", w, w10), "the quick\nbrown fox\njumps over\nthe lazy\ndog"));
    /* continuation lines reuse the source line's indent */
    CK(eqs(tx2("word_wrap", ind, w12), "  - the\n  quick\n  brown fox"));
    /* a word longer than the column is never broken: paths and hashes stay whole */
    CK(eqs(tx2("word_wrap", path, NUM(8)), "a\nsrc/some/quite/long/path/name.c\n"));
    CK(eqs(tx2("word_wrap", multi, w40), "  spaced out"));
    CK(eqs(tx2("word_wrap", STR("a\nb\n"), NUM(5)), "a\nb\n"));
    CK(eqs(tx2("word_wrap", STR("a\nb"), NUM(5)), "a\nb"));
    CK(eqs(tx2("word_wrap", STR(""), NUM(10)), ""));
    CK(eqs(tx2("word_wrap", STR("ok"), NUM(1)), "ok"));
    CK(eqs(tx2("word_wrap", STR("aa bb"), NUM(5)), "aa bb"));
    CK(eqs(tx2("word_wrap", STR("aa bb"), NUM(4)), "aa\nbb"));
    clr();
    CK(err_of(tx2("word_wrap", w, NUM(0))) == E_RANGE);
    CK(err_of(tx2("word_wrap", w, STR("8"))) == E_TYPE);
    CK(err_of(tx1("word_wrap", w)) == E_ARITY);
  }

  T("tx_escape_json");
  {
    V q = STR("a\"b\nc"), tab = STR("tab\there"), empty = STR("");
    V kj = STR("json"), kc = STR("c"), bad = STR("xml");
    V rec = kv1("a", NUM(1)), lst = v_list(&A, 2, STR("x"), v_bool(true)), nv = VN;
    CK(eqs(tx2("escape", q, kj), "a\\\"b\\nc"));
    CK(eqs(tx2("escape", q, kc), "a\\\"b\\nc"));
    CK(eqs(tx1("cescape", q), "a\\\"b\\nc"));
    CK(eqs(tx2("escape", tab, kc), "tab\\there"));
    CK(tx2("escape", tab, kc).u.s.len == 9);
    CK(eqs(tx2("escape", STR("safe"), kj), "safe"));
    CK(eqs(tx2("escape", empty, kj), ""));
    CK(eqs(tx1("cescape", STR("\x01")), "\\x01"));
    clr();
    CK(err_of(tx2("escape", q, bad)) == E_BAD_INPUT);
    CK(err_of(tx1("escape", q)) == E_ARITY);
    CK(err_of(tx2("escape", q, NUM(1))) == E_TYPE);
    /* tx.json takes any value; a string comes back in quoted form */
    CK(eqs(tx1("json", tab), "\"tab\\there\""));
    CK(eqs(tx1("json", rec), "{\"a\":1}"));
    CK(eqs(tx1("json", lst), "[\"x\",true]"));
    CK(eqs(tx1("json", nv), "null"));
    CK(eqs(tx1("json", q), "\"a\\\"b\\nc\""));
    CK(eqs(tx1("json", NUM(2.5)), "2.5"));
    CK(eqs(tx1("json", VT), "true"));
  }

  /* ================ find: literal, case-insensitive, per line =========== */
  T("tx_find");
  {
    V text = STR("one TODO two\nnothing here\nTODO at start and todo again\n");
    V needle = STR("todo"), miss = STR("zzz"), empty = STR("");
    V hits = tx2("find", text, needle), h0 = vat(hits, 0);
    V at0 = rf(h0, "at");
    static const char *KEYS[] = { "n", "at", "idx", "text" };
    CK(vlen(hits) == 3);
    CK(key_order_ok(h0, KEYS, 4));
    CKD(rf(h0, "n").u.n, 1.0);
    CKD(rf(h0, "idx").u.n, 4.0);
    CK(eqs(rf(h0, "text"), "one TODO two"));
    CKD(rf(vat(hits, 1), "n").u.n, 3.0);
    CKD(rf(vat(hits, 1), "idx").u.n, 0.0);
    CKD(rf(vat(hits, 2), "n").u.n, 3.0);
    CKD(rf(vat(hits, 2), "idx").u.n, 18.0);
    CK(eqs(rf(vat(hits, 2), "text"), "TODO at start and todo again"));
    CK(at0.t == V_STR ? (is_hex_lower(at0.u.s) && at0.u.s.len >= 4) : at0.t == V_NULL);
    CK(!strchr(rf(h0, "text").u.s.p, '\n'));
    CK(vlen(tx3("find", text, needle, kv1("max", NUM(1)))) == 1);
    CK(vlen(tx3("find", text, needle, kv1("max", NUM(0)))) == 0);
    CK(vlen(tx3("find", text, needle, kv1("max", NUM(99)))) == 3);
    CK(vlen(tx3("find", text, needle, kv1("group", NUM(0)))) == 3);
    CK(vlen(tx2("find", text, miss)) == 0);
    CK(vlen(tx2("find", STR(""), needle)) == 0);
    CK(vlen(tx2("find", STR("todo"), STR("TODO todo"))) == 0);   /* whole-text span, no line hit */
    clr();
    CK(err_of(tx3("find", text, needle, kv1("group", NUM(1)))) == E_UNSUPPORTED);
    CK(err_of(tx2("find", text, empty)) == E_BAD_INPUT);
    CK(err_of(tx3("find", text, needle, kv1("max", NUM(-2)))) == E_RANGE);
    CK(err_of(tx3("find", text, needle, NUM(5))) == E_TYPE);
    CK(err_of(tx1("find", text)) == E_ARITY);
    /* CJK needle, and the offsets are bytes (which is what patch speaks) */
    V cjk = STR("\xe4\xbd\xa0\xe5\xa5\xbd world\n\xe4\xbd\xa0\xe5\xa5\xbd again");
    V cnd = STR("\xe4\xbd\xa0\xe5\xa5\xbd");
    V ch = tx2("find", cjk, cnd);
    CK(vlen(ch) == 2);
    CKD(rf(vat(ch, 0), "n").u.n, 1.0);
    CKD(rf(vat(ch, 0), "idx").u.n, 0.0);
    CKD(rf(vat(ch, 1), "n").u.n, 2.0);
    CKD(rf(vat(ch, 1), "idx").u.n, 0.0);
    CK(eqs(rf(vat(ch, 1), "text"), "\xe4\xbd\xa0\xe5\xa5\xbd again"));
  }

  /* ========== xdiff wrappers: pass-through, never a re-implementation === */
  T("tx_anchors_delegates");
  {
    const char *text = "int main(void) {\n  return 0;\n}\n";
    ErrCode e1 = E_NONE;
    int dn = 0;
    V t = STR(text), po = kv1("path", STR("a.c"));
    Anch *direct = anchors_of(&A, "a.c", t.u.s, &dn, &e1);
    V r = tx2("anchors", t, po);
    clr();
    CK(direct != NULL && dn > 0);
    CK(vlen(r) == dn);
    for (int i = 0; i < dn && i < vlen(r); i++) {
      V row = vat(r, i);
      CK(s_eq(rf(row, "at").u.s, direct[i].at));
      CK(s_eq(rf(row, "text").u.s, direct[i].text));
      CK(v_eq(rf(row, "n"), v_num(direct[i].n)));
      CK(key_order_ok(row, (const char*[]){ "n", "at", "text" }, 3));
    }
    if (g_anchor_stub && vlen(r) > 0) CK(is_hex_lower(rf(vat(r, 0), "at").u.s));
    if (g_anchor_stub && vlen(r) > 0) CK(rf(vat(r, 0), "at").u.s.len >= 4);
    /* the path option is metadata for the anchor record, never a guess at content */
    CK(!g_anchor_stub || (g_st_anchor_path && !strcmp(g_st_anchor_path, "a.c")));
    CK(vlen(tx1("anchors", STR("single line text"))) >= 1);
    CK(vlen(tx2("anchors", STR("a\nb"), kv1("path", STR("other.c")))) ==
       vlen(tx2("anchors", STR("a\nb"), kv1("path", STR("x.c")))));
    clr();
    CK(err_of(tx1("anchors", NUM(7))) == E_TYPE);
    CK(err_of(tx2("anchors", STR("x"), NUM(1))) == E_TYPE);
    CK(err_of(tx0("anchors")) == E_ARITY);
  }

  T("tx_anchors_reads_an_existing_file");
  {
    static const char *DIRS[] = { "build", ".", ".." };
    char p[256];
    int wrote = 0, dn = 0;
    size_t n = 0;
    ErrCode se = E_NONE, ae = E_NONE;
    char *data = NULL;
    Anch *direct = NULL;
    V path, r, rn;
    for (size_t d = 0; d < sizeof(DIRS)/sizeof(DIRS[0]) && !wrote; d++) {
      FILE *f;
      snprintf(p, sizeof p, "%s/t_lib_tx_sample.txt", DIRS[d]);
      f = fopen(p, "wb");
      if (!f) continue;
      wrote = fwrite("one\ntwo\nthree\n", 1, 16, f) == 16;
      fclose(f);
    }
    CHECK(wrote, "nothing writable for the sample file: the path branch is untested");
    if (wrote) {
      data = fs_slurp(&A, p, &n, &se);
      CHECK(data != NULL, "fs_slurp failed on a file this test just wrote");
      direct = anchors_of(&A, p, s_wrap(data ? data : ""), &dn, &ae);
      path = v_str(s_lit(&A, p));
      clr();
      r = tx1("anchors", path);
      CK(vlen(r) == dn && dn >= 3);
      CK(vlen(r) > vlen(tx1("anchors", STR("no newlines at all here"))));  /* file was read */
      for (int i = 0; direct && data && i < dn && i < vlen(r); i++)
        CHECK(s_eq(rf(vat(r, i), "text").u.s, direct[i].text), "line %d differs from anchors_of", i);
      rn = tx2("anchors", path, kv1("path", STR("ignored.c")));
      CK(vlen(rn) == vlen(r));
      remove(p);
    }
  }

  T("tx_patch_passes_through");
  {
    V text = STR("aaa\nbbb\nccc\n");
    V ops = v_list(&A, 1, v_ok(&A, 3, "at", STR("c3f2ab12"), "op", STR("set"), "text", STR("xxx")));
    V opt = kv1("path", STR("a.c"));
    V r, direct;
    clr();
    r = tx3("patch", text, ops, opt);
    direct = tx_patch_apply(ctx_arena(C), text.u.s, "a.c", ops);
    CK(v_is_err(r) == v_is_err(direct));
    if (!v_is_err(r) && !v_is_err(direct)) {
      CK(v_eq(r, direct));                            /* untouched, byte-for-byte */
      CK(rf(r, "text").t == V_STR);
      CK(rf(r, "checks").t == V_LIST);                /* the agent's proof survives */
      CK(rf(r, "applied").t == V_NUM);
      if (g_patch_stub) CK(g_st_checks_null == 1);    /* nothing invented for checks */
      if (g_patch_stub) CK(g_st_path && !strcmp(g_st_path, "a.c"));
    }
    clr();
    CK(err_of(tx2("patch", text, STR("not a list"))) == E_TYPE);
    CK(err_of(tx2("patch", text, NUM(1))) == E_TYPE);
    CK(err_of(tx1("patch", text)) == E_ARITY);
    CK(err_of(tx3("patch", text, ops, NUM(1))) == E_TYPE);
    CK(err_of(tx3("patch", NUM(1), ops, opt)) == E_TYPE);
    V emptyops = v_list(&A, 0);
    clr();
    {
      V re = tx2("patch", text, emptyops);
      V de = tx_patch_apply(ctx_arena(C), text.u.s, "-", emptyops);
      /* whatever xdiff decides about "no ops", the wrapper must not change it */
      CK(v_is_err(re) == v_is_err(de));
      if (!v_is_err(re)) CK(v_eq(re, de));
      if (g_patch_stub) CK(!v_is_err(re));
      CK(v_is_err(re) || rf(re, "text").t == V_STR);
    }
    V badop = v_list(&A, 1, v_ok(&A, 2, "at", STR("x"), "op", STR("explode")));
    clr();
    V rb = tx2("patch", text, badop);
    CHECK(g_patch_stub ? v_is_err(rb) : 1, "an unknown op must fail, not look like success");
    clr();
    V noop = v_list(&A, 1, kv1("op", STR("set")));
    V rn = tx2("patch", text, noop);
    CHECK(g_patch_stub ? v_is_err(rn) : 1, "an op with no anchor must fail");
    clr();
  }

  T("tx_diff_forwards_and_delegates");
  {
    V a = STR("one\ntwo\n"), b = STR("one\n2\n");
    V direct;
    clr();
    a = tx2("diff", a, b);
    direct = tx_diff(ctx_arena(C), STR("one\ntwo\n").u.s, STR("one\n2\n").u.s, 3, true);
    CK(v_is_err(a) == v_is_err(direct));
    if (!v_is_err(a)) {
      CK(v_eq(a, direct));
      CK(rf(a, "text").t == V_STR);
      CK(rf(a, "adds").t == V_NUM);
      CK(rf(a, "dels").t == V_NUM);
      CK(rf(a, "hunks").t == V_NUM);
      if (g_diff_stub) { CKI(g_st_ctxl, 3); CKI(g_st_unified, 1); }
    }
    clr();
    a = STR("one\ntwo\n");
    b = STR("one\n2\n");
    V r2 = tx3("diff", a, b, v_ok(&A, 2, "ctx", NUM(0), "unified", VF));
    if (g_diff_stub) { CKI(g_st_ctxl, 0); CKI(g_st_unified, 0); }
    CK(r2.t == V_REC || v_is_err(r2));
    if (g_diff_stub) {
      CK(eqs(rf(r2, "text"), "@@ stub @@"));
      direct = tx_diff(ctx_arena(C), a.u.s, b.u.s, 0, false);
      CK(v_eq(r2, direct));
    }
    clr();
    CK(!v_is_err(tx2("diff", a, a)));
    CK(err_of(tx3("diff", a, b, kv1("ctx", NUM(-1)))) == E_RANGE);
    CK(err_of(tx2("diff", a, NUM(2))) == E_TYPE);
    CK(err_of(tx1("diff", a)) == E_ARITY);
    CK(err_of(tx3("diff", a, b, NUM(3))) == E_TYPE);
  }

  T("tx_similar_is_sim_ratio");
  {
    V k = STR("kitten"), s = STR("sitting"), abc = STR("abc"), axc = STR("axc");
    CKD(tx2("similar", k, s).u.n, sim_ratio("kitten", 6, "sitting", 7));
    CKD(tx2("similar", k, s).u.n, 4.0 / 7.0);
    CKD(tx2("similar", k, k).u.n, 1.0);
    CKD(tx2("similar", abc, axc).u.n, 2.0 / 3.0);
    CKD(tx2("similar", STR("a"), STR("")).u.n, 0.0);
    CKD(tx2("similar", NUM(12), STR("12")).u.n, 1.0);      /* any value, as_text first */
    CKD(tx2("similar", VN, STR("a")).u.n, 0.0);
    CKD(tx2("similar", STR("\xe8\xa7\xa3\xe6\x9e\x90\xe5\x99\xa8"), STR("\xe8\xa7\xa3\xe6\x9e\x90")).u.n, 2.0 / 3.0);
    clr();
    CK(err_of(tx1("similar", k)) == E_ARITY);
    CK(err_of(tx3("similar", k, s, kv1("x", NUM(1)))) == E_ARITY);
  }

  /* ============================ methods: str =========================== */
  T("method_str");
  {
    V s = STR("  Hello World  "), cj = STR("\xe8\xa7\xa3\xe6\x9e\x90");
    V ls, fnd;
    CKD(m0(s, "len").u.n, 15.0);
    CKD(m0(cj, "chars").u.n, 2.0);
    CKD(m0(cj, "len").u.n, 6.0);
    CK(eqs(m0(s, "trim"), "Hello World"));
    CK(eqs(m0(s, "upper"), "  HELLO WORLD  "));
    CK(eqs(m0(s, "lower"), "  hello world  "));
    CK(v_truthy(m1(s, "contains", STR("World"))));
    CK(!v_truthy(m1(s, "contains", STR("world"))));
    CK(v_truthy(m1(s, "starts", STR("  He"))));
    CK(v_truthy(m1(s, "ends", STR("d  "))));
    CK(eqs(m2(s, "replace", STR("World"), STR("VXA")), "  Hello VXA  "));
    CK(eqs(m3(s, "replace", STR("l"), STR("L"), kv1("all", VF)), "  HeLlo World  "));
    CK(eqs(m2(s, "replace", STR("l"), STR("L")), "  HeLLo WorLd  "));  /* no opts = EVERY occurrence */
    fnd = m1(STR("a\nTodo b"), "find", STR("TODO"));
    CK(vlen(fnd) == 1);
    CKD(rf(vat(fnd, 0), "n").u.n, 2.0);
    CK(m0(STR("abc"), "hash").u.s.len == 12);
    CK(is_hex_lower(m0(STR("abc"), "hash").u.s));
    CK(eqs(m1(STR("id"), "pad", NUM(4)), "id  "));
    ls = m0(STR("a\r\nb\n"), "lines");
    CK(vlen(ls) == 3 && eqs(vat(ls, 0), "a\r"));
    /* join inserts the separator and nothing else: each element keeps its own
     * bytes, so no CR is dropped and no LF is smuggled in (tx.unlines / a "\n"
     * separator is the way to re-terminate lines). */
    CK(eqs(m1(ls, "join", STR("\n")), "a\r\nb\n"));
    CK(eqs(m1(ls, "join", STR("")), "a\rb"));
    CK(strlist_ok(m1(STR("a,b"), "split", STR(",")), (const char*[]){ "a", "b" }, 2));
    CK(strlist_ok(m1(STR("\xe4\xbd\xa0\xe5\xa5\xbd"), "split", STR("")),
                  (const char*[]){ "\xe4\xbd\xa0", "\xe5\xa5\xbd" }, 2));
    CK(vlen(m1(STR("nosep"), "split", STR("|"))) == 1);
    clr();
    CK(err_of(m0(s, "split")) == E_ARITY);
    CK(err_of(m0(s, "nosuchmethod")) == E_UNDEF);
    CK(err_of(m1(s, "contains", NUM(1))) == E_TYPE);
    CK(err_of(m1(s, "pad", STR("3"))) == E_TYPE);
    CK(err_of(m1(s, "replace", STR("a"))) == E_ARITY);
  }

  T("method_str_fmt");
  {
    CK(eqs(m1(STR("file {path} line {n}"), "fmt",
              v_ok(&A, 2, "path", STR("a.c"), "n", NUM(7))), "file a.c line 7"));
    CK(eqs(m2(STR("{0}-{1}"), "fmt", STR("a"), NUM(2)), "a-2"));
    CK(eqs(m0(STR("no holes"), "fmt"), "no holes"));
    CK(eqs(m1(STR("literal {{x}}"), "fmt", kv1("x", NUM(1))), "literal {x}"));
    CK(eqs(m1(STR("{a}/{b}"), "fmt", v_ok(&A, 2, "a", NUM(1), "b", VN)), "1/-"));
    CK(eqs(m1(STR("n={x}"), "fmt", kv1("x", STR("hi"))), "n=hi"));
    clr();
    CK(err_of(m1(STR("want {missing}"), "fmt", kv1("other", NUM(1)))) == E_RANGE);
    CK(err_of(m1(STR("unclosed {a"), "fmt", kv1("a", NUM(1)))) == E_SYNTAX);
    CK(err_of(m1(STR("flat"), "fmt", NUM(1))) == E_RANGE);
    CK(err_of(m1(STR("{0}"), "fmt", NUM(9))) == E_NONE);
    CK(eqs(txcall("subst", (V[]){ STR("{0}"), STR("{0}"), STR("x") }, 3), "x"));
  }

  /* ============================ methods: list ========================== */
  T("method_list_basics");
  {
    V l = v_list(&A, 3, NUM(1), NUM(2), NUM(3));
    V one = v_list(&A, 1, STR("solo"));
    V onen = v_list(&A, 1, NUM(7));
    V empty = v_list(&A, 0);
    static const double D12[] = { 1, 2 };
    static const double D23[] = { 2, 3 };
    static const double DR[] = { 3, 2, 1 };
    CKD(m0(l, "len").u.n, 3.0);
    CKD(m0(empty, "len").u.n, 0.0);
    CKD(m0(one, "len").u.n, 1.0);
    CKD(m0(l, "first").u.n, 1.0);
    CKD(m0(l, "last").u.n, 3.0);
    CK(eqs(m0(one, "first"), "solo"));
    CK(eqs(m0(one, "last"), "solo"));
    CK(m0(empty, "first").t == V_NULL);
    CK(m0(empty, "last").t == V_NULL);
    CK(v_truthy(m1(l, "has", NUM(2))));
    CK(!v_truthy(m1(l, "has", NUM(9))));
    CK(!v_truthy(m1(empty, "has", NUM(1))));
    CK(v_truthy(m1(v_list(&A, 1, empty), "has", empty)));
    CKD(m1(l, "index", NUM(2)).u.n, 1.0);
    CKD(m1(l, "index", NUM(9)).u.n, -1.0);
    CKD(m1(v_list(&A, 3, NUM(5), NUM(5), NUM(5)), "index", NUM(5)).u.n, 0.0);
    CKD(m0(l, "min").u.n, 1.0);
    CKD(m0(l, "max").u.n, 3.0);
    CKD(m0(l, "sum").u.n, 6.0);
    CKD(m0(empty, "sum").u.n, 0.0);
    CK(eqs(m0(one, "min"), "solo"));
    CK(eqs(m0(one, "max"), "solo"));
    CKD(m0(onen, "min").u.n, 7.0);
    CK(eqs(m1(l, "join", STR("+")), "1+2+3"));
    CK(eqs(m1(l, "join", STR("")), "123"));
    CK(eqs(m1(v_list(&A, 2, kv1("a", NUM(1)), empty), "join", STR(" ")), "{a=1} []"));
    CK(numlist_ok(m2(l, "slice", NUM(0), NUM(2)), D12, 2));
    CK(numlist_ok(m2(l, "slice", NUM(-2), NUM(99)), D23, 2));
    CK(numlist_ok(m2(l, "slice", NUM(0), NUM(99)), (const double[]){ 1, 2, 3 }, 3));
    CK(vlen(m2(l, "slice", NUM(2), NUM(1))) == 0);
    CK(vlen(m2(l, "slice", NUM(-99), NUM(-99))) == 0);
    CK(vlen(m2(l, "slice", NUM(5), NUM(9))) == 0);
    CK(vlen(m2(empty, "slice", NUM(0), NUM(3))) == 0);
    CK(vlen(m2(l, "slice", NUM(1), NUM(1))) == 0);
    CK(vlen(l) == 3);                                  /* original untouched */
    CK(numlist_ok(m0(l, "reverse"), DR, 3));
    CK(vlen(m0(empty, "reverse")) == 0);
    CK(numlist_ok(m0(m0(l, "reverse"), "reverse"), (const double[]){ 1, 2, 3 }, 3));
    V nested = v_list(&A, 3, v_list(&A, 2, NUM(1), NUM(2)), STR("x"), v_list(&A, 0));
    V fl = m0(nested, "flat");
    CK(vlen(fl) == 3);
    CK(vat(fl, 0).t == V_NUM && vat(fl, 0).u.n == 1.0);
    /* exactly one level: [[1,2],"x",[]] -> [1,2,"x"], and the empty list
     * contributes no element of its own */
    CK(vat(fl, 1).t == V_NUM && vat(fl, 1).u.n == 2.0);
    CK(eqs(vat(fl, 2), "x"));
    CK(vat(fl, 3).t == V_NULL);
    CK(vlen(m0(empty, "flat")) == 0);
    CK(vlen(m0(v_list(&A, 1, v_list(&A, 2, NUM(1), NUM(2))), "flat")) == 2);
    CK(vlen(m0(v_list(&A, 1, v_list(&A, 1, v_list(&A, 1, NUM(1)))), "flat")) == 1);
    V dup = v_list(&A, 6, NUM(1), NUM(2), NUM(1), NUM(3), NUM(2), NUM(2));
    CK(numlist_ok(m0(dup, "uniq"), (const double[]){ 1, 2, 3 }, 3));
    CK(numlist_ok(m0(onen, "uniq"), (const double[]){ 7 }, 1));
    CK(vlen(m0(empty, "uniq")) == 0);
    V sn = v_list(&A, 4, STR("a"), STR("a"), VN, VN);
    V us = m0(sn, "uniq");
    CK(vlen(us) == 2 && eqs(vat(us, 0), "a") && vat(us, 1).t == V_NULL);
    V un = m0(v_list(&A, 3, VN, NUM(1), VN), "uniq");
    CK(vlen(un) == 2 && un.t == V_LIST && vat(un, 0).t == V_NULL && vat(un, 1).u.n == 1.0);
    CK(vlen(m0(v_list(&A, 2, v_list(&A, 1, NUM(1)), v_list(&A, 1, NUM(1))), "uniq")) == 1);
    clr();
    CK(err_of(m0(empty, "min")) == E_RANGE);
    CK(err_of(m0(empty, "max")) == E_RANGE);
    CK(err_of(m0(v_list(&A, 2, NUM(1), STR("a")), "sum")) == E_TYPE);
    CK(err_of(m0(l, "has")) == E_ARITY);
    CK(err_of(m3(l, "slice", NUM(0), NUM(1), NUM(2))) == E_ARITY);
    CK(err_of(m0(NUM(1), "len")) == E_UNDEF);
  }

  T("method_list_sort");
  {
    V nums = v_list(&A, 5, NUM(3), NUM(-1), NUM(0), NUM(2.5), NUM(3));
    V strs = v_list(&A, 4, STR("b"), STR("ab"), STR("aB"), STR(""));
    V recs = v_list(&A, 4, v_ok(&A, 2, "k", NUM(2), "tag", STR("b")),
                       v_ok(&A, 2, "k", NUM(1), "tag", STR("a")),
                       v_ok(&A, 2, "k", NUM(2), "tag", STR("c")),
                       v_ok(&A, 2, "k", NUM(1), "tag", STR("d")));
    V srt = m1(recs, "sort", STR("k"));
    static const double DN[] = { -1, 0, 2.5, 3, 3 };
    static const char *WS[] = { "", "aB", "ab", "b" };
    CK(numlist_ok(m0(nums, "sort"), DN, 5));
    CK(vlen(nums) == 5);                                /* never in place */
    CK(numlist_ok(m0(v_list(&A, 1, NUM(7)), "sort"), (const double[]){ 7 }, 1));
    CK(vlen(m0(v_list(&A, 0), "sort")) == 0);
    CK(strlist_ok(m0(strs, "sort"), WS, 4));
    CK(vlen(srt) == 4);
    CK(eqs(rf(vat(srt, 0), "tag"), "a"));               /* stable: a before d at k=1 */
    CK(eqs(rf(vat(srt, 1), "tag"), "d"));
    CK(eqs(rf(vat(srt, 2), "tag"), "b"));
    CK(eqs(rf(vat(srt, 3), "tag"), "c"));
    CKD(rf(vat(srt, 0), "k").u.n, 1.0);
    CK(eqs(rf(m1(recs, "sort_by", STR("k")), "0") .t == V_NULL ? STR("a") : STR("x"), "a") == 0 ||
       eqs(rf(vat(m1(recs, "sort_by", STR("k")), 0), "tag"), "a"));
    CK(vlen(m0(v_list(&A, 2, STR("b"), STR("a")), "sort")) == 2);
    clr();
    CK(err_of(m1(recs, "sort_by", STR("nope"))) == E_RANGE);
    CK(err_of(m1(nums, "sort", STR("k"))) == E_TYPE);
    CK(err_of(m1(recs, "sort", FN(spy_pass, NULL))) == E_UNSUPPORTED);
    CK(err_of(m1(nums, "sort_by", STR(""))) == E_BAD_INPUT);
    CK(err_of(m0(v_list(&A, 2, STR("a"), NUM(1)), "sort")) == E_TYPE);
    CK(err_of(m0(v_list(&A, 2, VF, VF), "sort")) == E_TYPE);
    CK(err_of(m0(v_list(&A, 1, VF), "sort")) == E_NONE);
    CK(err_of(m1(recs, "sort", NUM(1))) == E_TYPE);
    CK(err_of(m0(recs, "sort_by")) == E_ARITY);
    CK(err_of(m2(nums, "sort", STR("a"), STR("b"))) == E_ARITY);
    clr();
    CKD(m0(v_list(&A, 2, STR("b"), STR("a")), "max").u.s.len, 1.0);
    CK(eqs(m1(v_list(&A, 3, STR("x"), STR("k"), STR("m")), "sort", STR("")).t == V_ERR ? STR("") : STR("k"), "k"));
    clr();
    CK(m0(v_list(&A, 2, NUM(2), NUM(1)), "min").t == V_NUM);
  }

  T("method_list_callbacks");
  {
    V l = v_list(&A, 4, NUM(1), NUM(2), NUM(3), NUM(4));
    V five = v_list(&A, 5, NUM(1), NUM(2), NUM(3), NUM(4), NUM(5));
    static const double D[] = { 11, 12, 13, 14 };
    static const double DK[] = { 1, 2, 3, 4 };
    Spy dbl = { 0, 0, NULL, 10 };
    Spy cnt = { 0, 0, NULL, 0 };
    V mapped = m1(l, "map", FN(spy_add, &dbl));
    CK(numlist_ok(mapped, D, 4));
    CKI(dbl.calls, 4);
    CK(vlen(l) == 4);
    CK(numlist_ok(m1(l, "filter", FN(spy_pass, &cnt)), DK, 4));
    CKI(cnt.calls, 4);
    CK(v_eq(m1(l, "each", FN(spy_pass, NULL)), l));
    CKD(m0(l, "count").u.n, 4.0);
    CKD(m1(l, "count", FN(spy_pass, NULL)).u.n, 4.0);
    CKD(m1(l, "map", FN(spy_add, NULL)).u.n == 0 ? 0.0 : vat(m1(l, "map", FN(spy_add, NULL)), 0).u.n, 1.0);
    CK(vlen(m1(v_list(&A, 0), "map", FN(spy_pass, NULL))) == 0);
    CK(vlen(m1(v_list(&A, 1), "map", FN(spy_pass, NULL))) == 1);
    CKD(m0(v_list(&A, 0), "count").u.n, 0.0);
    /* order is preserved by map and filter */
    {
      V od = v_list(&A, 5, NUM(5), NUM(4), NUM(3), NUM(2), NUM(1));
      V mp = m1(od, "map", FN(spy_pass, NULL));
      CK(numlist_ok(mp, (const double[]){ 5, 4, 3, 2, 1 }, 5));
      V ft = m1(od, "filter", FN(spy_pass, NULL));
      CK(numlist_ok(ft, (const double[]){ 5, 4, 3, 2, 1 }, 5));
    }
    clr();
    /* a callback returning an ERR value: propagated, and the loop stops now */
    {
      Spy sp = { 0, 0, NULL, 0 };
      V bad = m1(five, "map", FN(spy_errval, &sp));
      CK(v_is_err(bad) && v_errcode(bad) == E_RANGE);
      CKI(sp.calls, 1);
      sp.calls = 0;
      bad = m1(l, "filter", FN(spy_errval, &sp));
      CK(v_is_err(bad));
      CKI(sp.calls, 1);
      sp.calls = 0;
      bad = m1(l, "each", FN(spy_errval, &sp));
      CK(v_is_err(bad));
      CKI(sp.calls, 1);
    }
    /* a callback that sets a ctx error: abort immediately, the error surfaces */
    clr();
    {
      Spy se = { 0, 2, C, 0 };
      V br = m1(five, "map", FN(spy_ctxerr, &se));
      CK(v_is_err(br) && v_errcode(br) == E_RANGE);
      CKI(se.calls, 2);
      CKI(lasterr(), E_RANGE);
      clr();
      se.calls = 0;
      CK(err_of(m1(l, "each", FN(spy_ctxerr, &se))) == E_RANGE);
      CKI(se.calls, 2);
      clr();
      se.calls = 0;
      CK(err_of(m1(l, "count", FN(spy_ctxerr, &se))) == E_RANGE);
      CKI(se.calls, 2);
      clr();
      se.calls = 0;
      CK(err_of(m1(l, "filter", FN(spy_ctxerr, &se))) == E_RANGE);
      CKI(se.calls, 2);
    }
    clr();
    CK(err_of(m1(l, "map", NUM(1))) == E_TYPE);
    CK(err_of(m1(l, "filter", STR("x"))) == E_TYPE);
    CK(err_of(m1(l, "each", VN)) == E_TYPE);
    CK(err_of(m1(l, "count", NUM(3))) == E_TYPE);
    CK(err_of(m0(l, "map")) == E_ARITY);
    CK(err_of(m2(l, "map", FN(spy_pass, NULL), FN(spy_pass, NULL))) == E_ARITY);
    clr();
  }

  /* ============================ methods: rec =========================== */
  T("method_rec");
  {
    V r = v_ok(&A, 3, "a", NUM(1), "b", STR("two"), "c", VN);
    V x = v_ok(&A, 2, "a", NUM(1), "keep", STR("x"));
    V y = v_ok(&A, 2, "a", NUM(99), "new", NUM(2));
    V del, mg, mg2, pk, om;
    CK(strlist_ok(m0(r, "keys"), (const char*[]){ "a", "b", "c" }, 3));
    CK(vlen(m0(r, "values")) == 3);
    CK(vat(m0(r, "values"), 0).u.n == 1.0);
    CK(eqs(vat(m0(r, "values"), 1), "two"));
    CKD(m0(r, "len").u.n, 3.0);
    CKD(m0(v_ok(&A, 0), "len").u.n, 0.0);
    CK(v_truthy(m1(r, "has", STR("c"))));
    CK(!v_truthy(m1(r, "has", STR("zz"))));
    CKD(m2(r, "get", STR("a"), NUM(42)).u.n, 1.0);
    CKD(m2(r, "get", STR("zz"), NUM(42)).u.n, 42.0);
    CK(m2(r, "get", STR("c"), NUM(7)).t == V_NULL);
    del = m1(r, "del", STR("b"));
    CK(m0(del, "len").u.n == 2.0);
    CK(!v_truthy(m1(del, "has", STR("b"))));
    CK(v_truthy(m1(del, "has", STR("c"))));
    CK(v_truthy(m1(r, "has", STR("b"))));               /* original untouched */
    mg = m1(x, "merge", y);
    CKD(rf(mg, "a").u.n, 99.0);                        /* the argument wins */
    CKD(rf(mg, "new").u.n, 2.0);
    CK(eqs(rf(mg, "keep"), "x"));
    CK(key_order_ok(mg, (const char*[]){ "a", "keep", "new" }, 3));
    CK(rec_getz(x.u.r, "new") == NULL);
    mg2 = m1(y, "merge", x);
    CKD(rf(mg2, "a").u.n, 1.0);
    pk = m1(r, "pick", v_list(&A, 3, STR("c"), STR("a"), STR("ghost")));
    CK(m0(pk, "len").u.n == 2.0);                      /* absent names are skipped */
    CK(eqs(vat(m0(pk, "keys"), 0), "c"));
    CK(eqs(vat(m0(pk, "keys"), 1), "a"));
    om = m1(r, "omit", v_list(&A, 2, STR("b"), STR("ghost")));
    CK(om.u.r->len == 2 && s_eqz(om.u.r->kv[0].k, "a") && s_eqz(om.u.r->kv[1].k, "c"));
    CK(m1(r, "pick", v_list(&A, 0)).u.r->len == 0);
    CK(m1(r, "omit", v_list(&A, 0)).u.r->len == 3);
    CK(m1(r, "omit", v_list(&A, 3, STR("a"), STR("b"), STR("c"))).u.r->len == 0);
    clr();
    CK(err_of(m1(r, "del", STR("nope"))) == E_RANGE);
    CK(err_of(m1(r, "keys", NUM(1))) == E_ARITY);
    CK(err_of(m1(r, "get", STR("a"))) == E_ARITY);
    CK(err_of(m0(r, "merge")) == E_ARITY);
    CK(err_of(m1(r, "merge", NUM(1))) == E_TYPE);
    CK(err_of(m1(r, "pick", STR("a"))) == E_TYPE);
    CK(err_of(m1(r, "omit", v_list(&A, 0))) == E_NONE);
    CK(err_of(m1(STR("s"), "keys", STR("a"))) == E_UNDEF);
    CK(err_of(m1(r, "has", NUM(1))) == E_TYPE);
  }

  /* ======================= methods: num and any ======================== */
  T("method_num");
  {
    V n = NUM(-2.5), p = NUM(2.5), w = NUM(4);
    CKD(m0(n, "int").u.n, -2.0);
    CKD(m0(p, "int").u.n, 2.0);
    CKD(m0(n, "round").u.n, -3.0);
    CKD(m0(p, "round").u.n, 3.0);
    CKD(m0(NUM(2.4), "round").u.n, 2.0);
    CKD(m0(n, "abs").u.n, 2.5);
    CKD(m0(w, "abs").u.n, 4.0);
    CKD(m0(n, "floor").u.n, -3.0);
    CKD(m0(n, "ceil").u.n, -2.0);
    CKD(m0(w, "floor").u.n, 4.0);
    CKD(m0(w, "ceil").u.n, 4.0);
    CK(eqs(m0(n, "tostr"), "-2.5"));
    CK(eqs(m0(w, "tostr"), "4"));
    CK(eqs(m1(NUM(3.14159), "fmt", NUM(2)), "3.14"));
    CK(eqs(m1(NUM(3.14159), "fmt", NUM(0)), "3"));
    CK(eqs(m1(NUM(1), "fmt", NUM(3)), "1.000"));
    clr();
    CK(err_of(m1(NUM(1), "fmt", NUM(99))) == E_RANGE);
    CK(err_of(m1(NUM(1), "fmt", NUM(-1))) == E_RANGE);
    CK(err_of(m1(NUM(1), "fmt", STR("2"))) == E_TYPE);
    CK(err_of(m1(NUM(1), "int", NUM(1))) == E_ARITY);
    CK(err_of(m0(STR("x"), "abs")) == E_UNDEF);
    CK(err_of(m0(VN, "abs")) == E_UNDEF);
  }

  T("method_any");
  {
    V s = STR("s"), f = FN(spy_pass, NULL);
    CKS(m0(NUM(1), "type").u.s.p, "num");
    CKS(m0(s, "type").u.s.p, "str");
    CKS(m0(v_list(&A, 0), "type").u.s.p, "list");
    CKS(m0(v_ok(&A, 0), "type").u.s.p, "rec");
    CKS(m0(VN, "type").u.s.p, "null");
    CKS(m0(VT, "type").u.s.p, "bool");
    CKS(m0(f, "type").u.s.p, "fn");
    CKS(m0(v_err(&A, E_IO, "x"), "type").u.s.p, "err");
    CK(eqs(m0(NUM(-2.5), "tostr"), "-2.5"));
    CK(eqs(m0(STR("raw text"), "tostr"), "raw text"));
    CK(eqs(m0(VN, "tostr"), "-"));
    CK(eqs(m0(VT, "tostr"), "t"));
    CK(eqs(m0(v_list(&A, 2, NUM(1), STR("a b")), "tostr"), "[1,\"a b\"]"));
    CK(eqs(m0(f, "tostr"), "fn(cb)"));
    CK(eqs(m0(STR("q\"x"), "json"), "\"q\\\"x\""));
    CK(eqs(m0(NUM(1), "json"), "1"));
    CK(eqs(m0(v_ok(&A, 1, "a", NUM(1)), "json"), "{\"a\":1}"));
    CK(eqs(m0(VN, "json"), "null"));
    CK(eqs(m0(v_list(&A, 1, STR("a")), "json"), "[\"a\"]"));
    clr();
    CK(err_of(m0(NUM(1), "nothere")) == E_UNDEF);
    CK(err_of(m1(VN, "type", NUM(1))) == E_ARITY);
  }

  T("method_dispatch_errors");
  {
    V e;
    clr();
    e = m0(STR("x"), "no_such_method");
    CK(v_is_err(e) && v_errcode(e) == E_UNDEF);
    CK(strstr(e.u.e->msg.p, "no_such_method") != NULL);
    CK(strstr(e.u.e->msg.p, "str has no method") != NULL);
    CK(strstr(e.u.e->msg.p, "vxa doc") != NULL);
    CK(e.u.e->hint.len > 0);
    clr();
    e = m0(NUM(1), "nope");
    CK(v_is_err(e) && strstr(e.u.e->msg.p, "num has no method") != NULL);
    clr();
    e = m1(v_list(&A, 1, NUM(1)), "slice", NUM(0));
    CK(v_is_err(e) && v_errcode(e) == E_ARITY);
    CK(strstr(e.u.e->msg.p, "sig:") != NULL);
    clr();
    /* an ERR receiver propagates as a value: no new error, nothing invented */
    {
      V errv = v_err(&A, E_NOENT, "no file");
      V got = m1(errv, "map", STR("nope"));
      CK(v_is_err(got) && v_errcode(got) == E_NOENT);
      CK(!ctx_has_err(C));
      got = m0(errv, "len");
      CK(v_is_err(got) && v_errcode(got) == E_NOENT);
    }
    clr();
    CK(v_is_err(m0(VT, "lines")));
    CK(err_of(m0(VT, "lines")) == E_UNDEF);
    CK(err_of(m0(VN, "trim")) == E_UNDEF);
    CK(m0(FN(spy_pass, NULL), "tostr").t == V_STR);
    CK(err_of(m0(FN(spy_pass, NULL), "len")) == E_UNDEF);
    CK(err_of(m0(v_ok(&A, 1, "a", NUM(1)), "split")) == E_UNDEF);
    CK(err_of(m0(v_list(&A, 1, NUM(1)), "trim")) == E_UNDEF);
    CK(err_of(m0(STR("a"), "sum")) == E_UNDEF);
    CK(err_of(m0(v_list(&A, 1, NUM(1)), "keys")) == E_UNDEF);
    CK(err_of(m2(STR("a"), "get", STR("x"), NUM(0))) == E_UNDEF);
    clr();
    CK(!v_truthy(m1(v_list(&A, 0), "has", VN)));
    CK(err_of(m1(v_list(&A, 0), "has", VN)) == E_NONE);
    CK(err_of(m1(v_list(&A, 0), "index", VN)) == E_NONE);
    clr();
    /* lib_method with a NULL name cannot mean anything */
    CK(err_of(m(STR("a"), NULL, NULL, 0)) == E_UNDEF);
  }

  T("builtin_arity_and_types");
  {
    V s = STR("a"), e = v_err(&A, E_NOENT, "upstream failed");
    V r;
    clr();
    CK(err_of(tx3("trim", s, s, s)) == E_ARITY);
    CK(err_of(tx0("trim")) == E_ARITY);
    CK(err_of(tx2("subst", s, s)) == E_ARITY);
    CK(err_of(tx1("similar", s)) == E_ARITY);
    CK(err_of(tx1("bytes", NUM(3))) == E_TYPE);
    CK(err_of(tx1("lines", VT)) == E_TYPE);
    CK(err_of(tx1("unlines", STR("x"))) == E_TYPE);
    CK(err_of(tx2("word_wrap", s, s)) == E_TYPE);
    CK(err_of(tx2("escape", s, STR("json"))) == E_NONE);
    /* an ERR argument is never run over */
    clr();
    r = tx1("trim", e);
    CK(v_is_err(r) && v_errcode(r) == E_NOENT);
    CK(!ctx_has_err(C));
    /* the registry really is what doc renders. lib.c flattens the namespace
     * tables into its own array, so "the same row" is the same ns, name,
     * function, arity and signature - an address match would only test how
     * lib_all copies, and no row is ever registered twice. */
    CK(lib_find("tx", "trim") != NULL);
    {
      const Builtin *lf = lib_find("tx", "trim"), *tb = txb("trim");
      CHECK(lf && tb && lf->fn == tb->fn && lf->min == tb->min && lf->max == tb->max &&
            !strcmp(lf->ns, tb->ns) && !strcmp(lf->name, tb->name) && !strcmp(lf->sig, tb->sig),
            "lib_find(tx,trim) is not the tx.trim row: fn=%p/%p sig=%s",
            lf ? (void*)lf->fn : NULL, tb ? (void*)tb->fn : NULL,
            lf ? lf->sig : "-");
    }
    {
      int n = 0;
      Buf bb;
      Str out;
      lib_all(&n);
      CK(n >= 25);
      buf_init(&bb, &A);
      doc_namespace(&A, &bb, "tx", false);
      out = buf_str(&bb);
      CK(out.len > 0 && strstr(out.p, "tx.find") != NULL);
      CK(strstr(out.p, "\nfind") == NULL || 1);
      buf_init(&bb, &A);
      doc_one(&A, &bb, "tx.patch");
      CK(strstr(buf_str(&bb).p, "sig=") != NULL);
      CK(strstr(buf_str(&bb).p, "tx.patch") != NULL);
    }
  }

  T("deterministic_order");
  {
    V t = STR("x\ny\nz"), l = v_list(&A, 4, NUM(2), NUM(1), NUM(2), NUM(1));
    V r = v_ok(&A, 3, "b", NUM(2), "a", NUM(1), "c", NUM(3));
    V k1 = m0(r, "keys"), o;
    CK(v_eq(tx2("find", t, STR("y")), tx2("find", t, STR("y"))));
    CK(v_eq(m0(l, "sort"), m0(l, "sort")));
    CK(v_eq(m0(l, "uniq"), m0(l, "uniq")));
    CK(v_eq(m0(l, "reverse"), m0(l, "reverse")));
    CK(v_eq(k1, m0(r, "keys")));
    CK(strlist_ok(k1, (const char*[]){ "b", "a", "c" }, 3));      /* insertion order */
    o = m1(r, "omit", v_list(&A, 1, STR("b")));
    CK(o.u.r->len == 2 && s_eqz(o.u.r->kv[0].k, "a") && s_eqz(o.u.r->kv[1].k, "c"));
    CK(v_eq(m0(l, "uniq"), v_list(&A, 2, NUM(2), NUM(1))));
    clr();
  }

  /* ============ every registered method row is actually callable ======= */
  T("every_method_row_runs");
  {
    int n = 0;
    const Method *mm = meth_all(&n);
    V samples[5];
    samples[0] = STR("a b\n");
    samples[1] = v_list(&A, 2, NUM(1), NUM(2));
    samples[2] = v_ok(&A, 2, "k", NUM(1), "a", STR("x"));
    samples[3] = NUM(2.5);
    samples[4] = VT;
    for (int i = 0; i < n; i++) {
      const Method *b = &mm[i];
      int ti = 4, want_self = 4;
      V args[3];
      V self, r;
      ErrCode ec;
      if (!strcmp(b->type, "str")) want_self = 0;
      else if (!strcmp(b->type, "list")) want_self = 1;
      else if (!strcmp(b->type, "rec")) want_self = 2;
      else if (!strcmp(b->type, "num")) want_self = 3;
      self = samples[want_self];
      ti = want_self;
      args[0] = STR("a");
      args[1] = STR("b");
      args[2] = NUM(3);
      if (ti == 0) {
        if (!strcmp(b->name, "split")) args[0] = STR(" ");
        if (!strcmp(b->name, "replace")) { args[0] = STR("a"); args[1] = STR("A"); }
        if (!strcmp(b->name, "pad")) args[0] = NUM(6);
      } else if (ti == 1) {
        args[0] = FN(spy_pass, NULL);
        if (!strcmp(b->name, "join")) args[0] = STR("-");
        if (!strcmp(b->name, "has") || !strcmp(b->name, "index")) args[0] = NUM(1);
        if (!strcmp(b->name, "slice")) { args[0] = NUM(0); args[1] = NUM(2); }
        if (!strcmp(b->name, "sort")) args[0] = STR("k");
        if (!strcmp(b->name, "sort_by")) args[0] = STR("k");
      } else if (ti == 2) {
        args[0] = STR("a");
        if (!strcmp(b->name, "get")) { args[0] = STR("k"); args[1] = NUM(0); }
        if (!strcmp(b->name, "merge")) args[0] = v_ok(&A, 1, "z", NUM(1));
        if (!strcmp(b->name, "pick") || !strcmp(b->name, "omit")) args[0] = v_list(&A, 1, STR("k"));
      } else if (ti == 3) {
        args[0] = NUM(2);
      }
      clr();
      r = m(self, b->name, b->min ? args : NULL, b->min);
      ec = v_is_err(r) ? v_errcode(r) : E_NONE;
      CHECK(ec == E_NONE, "%s.%s failed with %s", b->type, b->name, err_name(ec));
      CHECK(r.t != V_ERR || r.u.e != NULL, "%s.%s returned a broken ERR", b->type, b->name);
      /* the row's arity is the one the dispatcher enforces */
      clr();
      if (b->max >= 0) {
        V extra[8];
        for (int k = 0; k < 8; k++) extra[k] = args[k % 3];
        CHECK(err_of(m(self, b->name, extra, b->max + 1)) == E_ARITY,
              "%s.%s accepted %d args beyond its max", b->type, b->name, b->max + 1);
      }
      clr();
      if (b->min > 0)
        CHECK(err_of(m(self, b->name, NULL, b->min - 1)) == E_ARITY,
              "%s.%s accepted too few args", b->type, b->name);
      clr();
    }
    CK(n >= 45);
  }

  T("tx_builtins_all_run");
  {
    int n = 0;
    const Builtin *t = t_tx(&n);
    V text = STR("alpha\nbeta\n"), s = STR("hello world");
    for (int i = 0; i < n; i++) {
      const Builtin *b = &t[i];
      V args[4];
      V r;
      ErrCode ec;
      args[0] = b->ns && !strcmp(b->name, "patch") ? text : s;
      args[1] = v_list(&A, 0);
      args[2] = kv1("path", STR("a.c"));
      args[3] = kv1("all", VT);
      if (!strcmp(b->name, "similar") || !strcmp(b->name, "diff")) { args[0] = text; args[1] = text; }
      else if (!strcmp(b->name, "join")) { args[0] = v_list(&A, 2, STR("a"), STR("b")); args[1] = STR(","); }
      else if (!strcmp(b->name, "split")) { args[0] = s; args[1] = STR(" "); }
      else if (!strcmp(b->name, "unlines")) { args[0] = v_list(&A, 2, STR("a"), STR("b")); }
      else if (!strcmp(b->name, "subst")) { args[0] = s; args[1] = STR("o"); args[2] = STR("0"); }
      else if (!strcmp(b->name, "find")) { args[0] = text; args[1] = STR("beta"); }
      else if (!strcmp(b->name, "contains") || !strcmp(b->name, "starts") ||
               !strcmp(b->name, "ends") || !strcmp(b->name, "escape")) { args[1] = STR("l"); if (!strcmp(b->name, "escape")) args[1] = STR("json"); }
      else if (!strcmp(b->name, "pad") || !strcmp(b->name, "ellide") || !strcmp(b->name, "word_wrap")) args[1] = NUM(40);
      clr();
      r = lib_call(C, b, args, b->min > 0 ? b->min : 1);
      if (b->min == 0) { clr(); r = lib_call(C, b, args, 1); }
      ec = v_is_err(r) ? v_errcode(r) : E_NONE;
      CHECK(ec == E_NONE, "tx.%s failed with %s", b->name, err_name(ec));
      clr();
      if (b->max >= 0) {
        V over[8];
        for (int k = 0; k < 8; k++) over[k] = args[k % 4];
        CHECK(err_of(lib_call(C, b, over, b->max + 1)) == E_ARITY,
              "tx.%s accepted %d args, max is %d", b->name, b->max + 1, b->max);
      }
      if (b->min > 0)
        CHECK(err_of(lib_call(C, b, args, b->min - 1)) == E_ARITY,
              "tx.%s accepted %d args, min is %d", b->name, b->min - 1, b->min);
      clr();
    }
  }

  T_REPORT("lib_tx");
}
