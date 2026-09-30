/* interp.c - tree-walking evaluator with lexical scoping.
 * Runtime failures are VALUES (V_ERR) carrying a stable code + hint; the process
 * never aborts on a script error, because an agent needs the code= to branch on. */
#include "vxa.h"
#include "interp.h"
#include "ast.h"
#include "lib.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>

static V ev(Ctx *c, Node *n);
V eval_node_impl(Ctx *c, Node *n, bool noscope);


struct Env { Env *parent; Rec *vars; };

struct Ctx {
  Arena *a;
  char *root, *script;
  Env *glob, *env;
  long long steps, maxsteps;
  int depth, maxdepth;
  int flow;                    /* F_NONE / F_RETURN / F_BREAK / F_CONT */
  V retval;
  V err;
  const Node *node;            /* node currently being evaluated, for hints */
  char *confirm, *argstr;
  Rec *cfg;
  Fmt fmt;
  bool strict, anchors;
  Str *applied; int napplied, capplied;  /* tokens already spent by an apply in this ctx */
};

enum { F_NONE = 0, F_RETURN = 1, F_BREAK = 2, F_CONT = 3 };

/* ---------- context ---------- */
Ctx *ctx_new(Arena *a, const char *root, const char *script_path) {
  Ctx *c = (Ctx*)arena_zalloc(a, sizeof(Ctx));
  c->a = a;
  c->root = arena_strdup(a, root && *root ? root : ".");
  c->script = arena_strdup(a, script_path ? script_path : "<eval>");
  c->glob = env_new(a, NULL);
  c->env = c->glob;
  c->maxsteps = 20000000;
  c->maxdepth = 512;
  c->confirm = (char*)"";
  c->argstr = (char*)"";
  c->cfg = rec_new(a);
  c->fmt = FMT_AUTO;
  return c;
}
void ctx_free(Ctx *c) { (void)c; }
const char *ctx_root(Ctx *c) { return c->root; }
const char *ctx_script(Ctx *c) { return c->script; }
Arena *ctx_arena(Ctx *a) { return a->a; }
Str ctx_argstr(Ctx *c) { return s_wrap(c->argstr); }
char *ctx_confirm(Ctx *c) { return c->confirm; }
Rec *ctx_cfg(Ctx *c) { return c->cfg; }
long long ctx_steps(Ctx *c) { return c->steps; }
void ctx_set_confirm(Ctx *c, const char *t) { c->confirm = t ? arena_strdup(c->a, t) : (char*)""; }
void ctx_set_argstr(Ctx *c, const char *s) { c->argstr = s ? arena_strdup(c->a, s) : (char*)""; }
void ctx_set_strict(Ctx *c, bool on) { c->strict = on; }
void ctx_set_steps(Ctx *c, long long m) { if (m > 0) c->maxsteps = m; }
void ctx_set_fmt(Ctx *c, Fmt f) { c->fmt = f; }
Fmt ctx_fmt(Ctx *c) { return c->fmt; }
void ctx_set_anchors(Ctx *c, bool on) { c->anchors = on; }
bool ctx_anchors(Ctx *c) { return c->anchors; }
Node *cur_node(Ctx *c) { return (Node*)c->node; }

/* ---------- which tokens this context has already applied (SPEC 3.4b) --------
 * plan.c notes every token it executes, and the evaluator reads that back to tell
 * "the receipt of a plan that already ran" from a plain record. That is what makes
 * `expr.apply()` and `p = expr; p.apply()` answer identically even for a builtin
 * that consumed --confirm itself (sh.run does), where the second spelling used to
 * die on a null function object with no name in the message. */
void ctx_note_applied(Ctx *c, const char *token) {
  if (!c || !token || !*token) return;
  Str t = s_lit(c->a, token);
  for (int i = 0; i < c->napplied; i++) if (s_eq(c->applied[i], t)) return;
  if (c->napplied == c->capplied) {
    int nc = c->capplied ? c->capplied * 2 : 8;
    Str *nv = (Str*)arena_zalloc(c->a, sizeof(Str) * (size_t)nc);
    for (int i = 0; i < c->napplied; i++) nv[i] = c->applied[i];
    c->applied = nv; c->capplied = nc;
  }
  c->applied[c->napplied++] = t;
}
bool ctx_applied(Ctx *c, Str token) {
  if (!c || !token.p || token.len <= 0) return false;
  for (int i = 0; i < c->napplied; i++) if (s_eq(c->applied[i], token)) return true;
  return false;
}

/* ---------- errors ---------- */
void set_error(Ctx *c, ErrCode code, const char *fmt, ...) {
  if (c->err.t != V_NULL) return;              /* first error wins */
  char msg[512];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  c->err = v_err_hint(code, s_lit(c->a, msg), s_lit(c->a, err_hint(code)));
  c->flow = F_NONE;
}
V at_err(Ctx *c, Node *n, ErrCode code, const char *fmt, ...) {
  if (c->err.t != V_NULL) return VN;
  char msg[512];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  char loc[160];
  snprintf(loc, sizeof loc, "%s:%d:%d: ", c->script, n ? n->line : 0, n ? n->col : 0);
  set_error(c, code, "%s%s", loc, msg);
  return VN;
}
bool ctx_has_err(Ctx *c) { return v_is_err(c->err); }
V  ctx_err(Ctx *c) { return c->err; }
void ctx_clear_err(Ctx *c) { c->err = VN; c->flow = F_NONE; }

/* ---------- env ---------- */
Env *env_new(Arena *a, Env *parent) {
  Env *e = (Env*)arena_zalloc(a, sizeof(Env));
  e->parent = parent;
  e->vars = rec_new(a);
  return e;
}
V *env_own(Env *e, const char *name) { return rec_getz(e->vars, name); }
V *env_get(Env *e, const char *name) {
  for (; e; e = e->parent) { V *v = rec_getz(e->vars, name); if (v) return v; }
  return NULL;
}
void env_set(Arena *a, Env *e, const char *name, V v) { rec_setz(a, e->vars, name, v); }
bool env_assign(Env *e, const char *name, V v) {
  for (; e; e = e->parent) { V *p = rec_getz(e->vars, name); if (p) { *p = v; return true; } }
  return false;
}
Str env_dump(Arena *a, Env *e) {
  Buf b; buf_init(&b, a);
  for (; e; e = e->parent)
    for (int i = 0; i < e->vars->len; i++) {
      if (b.len) buf_putc(&b, ' ');
      buf_add_escaped(&b, e->vars->kv[i].k);
    }
  return buf_take(&b);
}

/* ---------- small helpers ---------- */
static bool is_num(V v) { return v.t == V_NUM; }
static V num(double d) { return v_num(d); }
static V list_v(List *l) { V r; r.t = V_LIST; r.u.l = l; return r; }
static V rec_v(Rec *r) { V v; v.t = V_REC; v.u.r = r; return v; }

Str as_text(Ctx *c, V v) {
  if (v.t == V_STR) return v.u.s;
  if (v.t == V_NUM) { Buf b; buf_init(&b, c->a); fmt_num(&b, v.u.n); return buf_take(&b); }
  if (v.t == V_BOOL) return s_lit(c->a, v.u.b ? "true" : "false");
  if (v.t == V_NULL) return s_null();
  return v_tostr(c->a, v, true);
}

/* ---------- binops ---------- */
static V do_bin(Ctx *c, Node *n, V a, V b) {
  Arena *ar = c->a;
  const char *op = n->s.p;
  if (!strcmp(op, "~=")) {
    Str x = as_text(c, a), y = as_text(c, b);
    double r = sim_ratio(x.p, x.len, y.p, y.len);
    return num((double)((long long)(r * 10000 + 0.5)) / 10000);   /* 4 dp is enough and costs fewer bytes */
  }
  if (!strcmp(op, "==")) return v_bool(v_eq(a, b));
  if (!strcmp(op, "!=")) return v_bool(!v_eq(a, b));
  if (!strcmp(op, "+")) {
    if (is_num(a) && is_num(b)) return num(a.u.n + b.u.n);
    if (a.t == V_LIST && b.t == V_LIST) {
      List *l = list_new(ar);
      for (int i = 0; i < a.u.l->len; i++) list_push(ar, l, a.u.l->v[i]);
      for (int i = 0; i < b.u.l->len; i++) list_push(ar, l, b.u.l->v[i]);
      return list_v(l);
    }
    if (a.t == V_STR || b.t == V_STR) {
      /* str + (str|num|bool) is allowed because building output lines is the
       * common case; str + list/rec is refused rather than silently stringified */
      bool aok = a.t == V_STR || a.t == V_NUM || a.t == V_BOOL || a.t == V_NULL;
      bool bok = b.t == V_STR || b.t == V_NUM || b.t == V_BOOL || b.t == V_NULL;
      if (!aok || !bok) {
        at_err(c, n, E_TYPE, "'+' cannot join %s and %s (use tostr() to stringify on purpose)", v_typename(a), v_typename(b));
        return VN;
      }
      return v_str(s_concat(ar, as_text(c, a), as_text(c, b)));
    }
    if (a.t == V_REC && b.t == V_REC) {
      Rec *r = rec_new(ar);
      for (int i = 0; i < a.u.r->len; i++) rec_set(ar, r, a.u.r->kv[i].k, a.u.r->kv[i].v);
      for (int i = 0; i < b.u.r->len; i++) rec_set(ar, r, b.u.r->kv[i].k, b.u.r->kv[i].v);
      return rec_v(r);
    }
    at_err(c, n, E_TYPE, "'+' needs numbers, strings, lists or records, got %s and %s", v_typename(a), v_typename(b));
    return VN;
  }
  if (!strcmp(op, "*") && (a.t == V_STR || b.t == V_STR)) {
    V sv = a.t == V_STR ? a : b;
    V nv = a.t == V_STR ? b : a;
    if (!is_num(nv)) { at_err(c, n, E_TYPE, "'*' needs a number of repeats"); return VN; }
    double d = nv.u.n;
    if (d < 0 || d > 200000) { at_err(c, n, E_RANGE, "repeat count out of 0..200000"); return VN; }
    Buf bb; buf_init(&bb, ar);
    for (long long i = 0, k = (long long)d; i < k; i++) buf_put(&bb, sv.u.s.p, (size_t)sv.u.s.len);
    return v_str(buf_take(&bb));
  }
  if (!strcmp(op, "-") || !strcmp(op, "*") || !strcmp(op, "/") || !strcmp(op, "%")) {
    if (!is_num(a) || !is_num(b)) {
      at_err(c, n, E_TYPE, "'%s' needs numbers, got %s and %s", op, v_typename(a), v_typename(b));
      return VN;
    }
    double x = a.u.n, y = b.u.n;
    if (*op == '-') return num(x - y);
    if (*op == '*') return num(x * y);
    if (*op == '/') { if (y == 0) { at_err(c, n, E_RANGE, "division by zero"); return VN; } return num(x / y); }
    if (y == 0) { at_err(c, n, E_RANGE, "modulo by zero"); return VN; }
    return num(fmod(x, y));
  }
  if (is_num(a) && is_num(b)) {
    double x = a.u.n, y = b.u.n;
    if (!strcmp(op, "<")) return v_bool(x < y);
    if (!strcmp(op, "<=")) return v_bool(x <= y);
    if (!strcmp(op, ">")) return v_bool(x > y);
    if (!strcmp(op, ">=")) return v_bool(x >= y);
  }
  if (a.t == V_STR && b.t == V_STR) {
    int cm = s_cmp(a.u.s, b.u.s);
    if (!strcmp(op, "<")) return v_bool(cm < 0);
    if (!strcmp(op, "<=")) return v_bool(cm <= 0);
    if (!strcmp(op, ">")) return v_bool(cm > 0);
    if (!strcmp(op, ">=")) return v_bool(cm >= 0);
  }
  at_err(c, n, E_TYPE, "'%s' does not apply to %s and %s", op, v_typename(a), v_typename(b));
  return VN;
}

/* ---------- index / field ---------- */
static int norm_idx(int i, int len) { return i < 0 ? i + len : i; }
static int cp_off(Str s, int cp) {           /* byte offset of code point cp */
  int o = 0, k = 0;
  while (o < s.len && k < cp) {
    unsigned char ch = (unsigned char)s.p[o];
    int w = 1;
    if (ch >= 0xF0) w = 4; else if (ch >= 0xE0) w = 3; else if (ch >= 0xC0) w = 2;
    if (o + w > s.len) w = 1;
    o += w; k++;
  }
  return o > s.len ? s.len : o;
}

static V index_of(Ctx *c, Node *n, V self, V iv, V to, bool slice) {
  Arena *ar = c->a;
  if (self.t == V_LIST) {
    int len = self.u.l->len;
    if (slice) {
      int a = is_num(iv) ? norm_idx((int)iv.u.n, len) : 0;
      int b = (to.t == V_NULL) ? len : (is_num(to) ? norm_idx((int)to.u.n, len) : len);
            if (a < 0) a = 0;
      if (b > len) b = len;
      if (a > b) a = b;
      List *l = list_new(ar);
      for (int i = a; i < b; i++) list_push(ar, l, self.u.l->v[i]);
      return list_v(l);
    }
    if (!is_num(iv)) { at_err(c, n, E_TYPE, "list index must be a number, got %s", v_typename(iv)); return VN; }
    int i = norm_idx((int)iv.u.n, len);
    if (i < 0 || i >= len) { at_err(c, n, E_RANGE, "list index %d out of range (len %d)", (int)iv.u.n, len); return VN; }
    return self.u.l->v[i];
  }
  if (self.t == V_STR) {
    if (!is_num(iv)) { at_err(c, n, E_TYPE, "string index must be a number"); return VN; }
    int cl = s_ucount(self.u.s);
    if (slice) {
      int a = norm_idx((int)iv.u.n, cl), b = (to.t == V_NULL) ? cl : norm_idx((int)to.u.n, cl);
            if (a < 0) a = 0;
      if (b > cl) b = cl;
      if (a > b) a = b;
      return v_str(s_cut(ar, self.u.s, cp_off(self.u.s, a), cp_off(self.u.s, b)));
    }
    int i = norm_idx((int)iv.u.n, cl);
    if (i < 0 || i >= cl) { at_err(c, n, E_RANGE, "string index %d out of range (%d chars)", (int)iv.u.n, cl); return VN; }
    return v_str(s_char_at(ar, self.u.s, i));
  }
  if (self.t == V_REC) {
    if (slice) { at_err(c, n, E_TYPE, "records cannot be sliced"); return VN; }
    Str k = as_text(c, iv);
    V *p = rec_get(self.u.r, k);
    return p ? *p : VN;                    /* missing key reads as null by design */
  }
  if (self.t == V_NULL) { at_err(c, n, E_UNDEF, "indexing null: that value was missing or failed earlier"); return VN; }
  at_err(c, n, E_TYPE, "cannot index a %s", v_typename(self));
  return VN;
}

static V set_index(Ctx *c, Node *n, V self, V iv, V val) {
  if (self.t == V_LIST && is_num(iv)) {
    int i = norm_idx((int)iv.u.n, self.u.l->len);
    V *p = list_get(self.u.l, i);
    if (!p) {
      if (i != self.u.l->len) { at_err(c, n, E_RANGE, "cannot write list index %d (len %d)", i, self.u.l->len); return VN; }
      list_push(c->a, self.u.l, val);
    } else *p = val;
    return val;
  }
  if (self.t == V_REC) { rec_set(c->a, self.u.r, as_text(c, iv), val); return val; }
  at_err(c, n, E_TYPE, "cannot assign into a %s by index", v_typename(self));
  return VN;
}

typedef struct { V self; char name[48]; } Bound;

/* A name that needs arguments is handed back as a callable bound to this receiver;
 * call_fn refuses it on its own and the dot path in lib_call_value runs it. */
static V bound_method(Ctx *c, V self, Str name) {
  Bound *bd = (Bound*)arena_zalloc(c->a, sizeof(Bound));
  bd->self = self;
  snprintf(bd->name, sizeof bd->name, "%.*s", name.len, name.p);
  Fn *f = (Fn*)arena_zalloc(c->a, sizeof(Fn));
  f->kind = F_NATIVE; f->ud = bd; f->nparams = -1;
  f->name = arena_strdup(c->a, bd->name);
  f->native = NULL;
  V r; r.t = V_FN; r.u.f = f; return r;
}

/* Does this plan carry a data field called `name`? The apply method is only
 * reachable when it does not: data first, then method (SPEC 3.5). */
static bool plan_has_field(Ctx *c, V plan, const char *name) {
  Rec *r = rec_new(c->a);
  plan_to_v(c, plan.u.pl, r);
  return rec_getz(r, name) != NULL;
}

/* The token an applied plan puts on its receipt, or a null Str. */
static Str receipt_token(V v) {
  if (v.t != V_REC || !v.u.r) return s_null();
  V *t = rec_getz(v.u.r, "token");
  return (t && t->t == V_STR) ? t->u.s : s_null();
}

/* SPEC 1.5: an ERR is a VALUE carrying code/msg/hint, and the documented idiom is
 *   r = fs.read("nope.txt")        # -> !ERR code=NOENT
 *   if r.code { ... }
 * which needs two things: the name resolves against the error, and binding the
 * error to a name takes responsibility for it (SPEC 1.5: an unused ERR is silent,
 * only strict=1 stops at the first one). A builtin reports through the context, so
 * without this release the assignment never even happened: the pending error cut
 * the script short and `.code` was unreachable. SPEC 3.5 documents the same for
 * `r = p.apply()`, which reports through the context and hands back nothing.
 * A defect in the SCRIPT (SPEC 2.2 exit 2: parse, type, arity, undef, guard, die)
 * stays fatal - catching it would hide the line the agent has to go and fix. */
static bool err_catchable(Ctx *c, V v, bool *bind_pending) {
  *bind_pending = false;
  if (c->strict || !ctx_has_err(c)) return false;
  if (err_exit(v_errcode(c->err)) == X_SCRIPT) return false;
  if (v_is_err(v)) {
    if (c->err.u.e != v.u.e) return false;      /* a different error: not this value's */
    return true;
  }
  if (v.t != V_NULL) return false;              /* it produced a value; nothing to catch */
  *bind_pending = true;                         /* the builtin failed by context, not by value */
  return true;
}

/* The four names SPEC 1.5 gives an error value. `code` is the uppercase name
 * because that is what the CLI prints (code=NOENT) and a non-empty string is
 * truthy, which is what makes `if r.code` the test for "this is an error". */
static bool err_field_name(const char *n) {
  return n && (!strcmp(n, "code") || !strcmp(n, "msg") || !strcmp(n, "hint") || !strcmp(n, "data"));
}

static V err_field(Ctx *c, Node *n, V self, Str name) {
  PErr *e = self.u.e;
  if (s_eqz(name, "code")) return v_str(s_lit(c->a, err_name(e->code)));
  if (s_eqz(name, "msg"))  return v_str(e->msg.p ? e->msg : s_lit(c->a, ""));
  if (s_eqz(name, "hint")) return v_str(e->hint.p ? e->hint : s_lit(c->a, ""));
  if (s_eqz(name, "data")) return e->data;
  return VN;   /* not a field of an error: the caller falls to methods, then UNDEF */
}

static V field_of(Ctx *c, Node *n, V self, Str name) {
  if (self.t == V_ERR) {
    /* fields first, then the methods every value answers (.type(), .tostr()),
     * then E_UNDEF - never a silent null (SPEC 1.4/1.5). */
    if (err_field_name(name.p)) {
      if (!self.u.e) { at_err(c, n, E_UNDEF, "err value carries no error data"); return VN; }
      return err_field(c, n, self, name);
    }
    const Method *xm = meth_find("err", name.p);
    if (!xm) xm = meth_find("any", name.p);
    if (xm && xm->min == 0 && (xm->max == 0 || xm->max < 0))
      return lib_method(c, self, name.p, NULL, 0);
    if (xm) return bound_method(c, self, name);
    at_err(c, n, E_UNDEF,
      "err has no field '%s' - an error carries code,msg,hint,data (SPEC 1.5); "
      "code reads as the uppercase name, e.g. NOENT", name.p);
    return VN;
  }
  if (self.t == V_REC) {
    /* a record may legitimately own a key called "keys": the field wins.
     * Only when there is no such field does the name fall through to the
     * record's methods (SPEC 1.4), so r.keys works without hiding data. */
    V *p = rec_get(self.u.r, name);
    if (p) return *p;
    /* No such key - and a missing key reads as null by design, so this is where
     * the record's own methods and the ones every value answers get their turn:
     * `{a:1}.type()` is SPEC 1.4, not a typo. */
    const Method *rm = meth_find("rec", name.p);
    if (!rm) rm = meth_find("any", name.p);
    if (rm && rm->min == 0 && (rm->max == 0 || rm->max < 0))
      return lib_method(c, self, name.p, NULL, 0);
    if (rm) return bound_method(c, self, name);
    return VN;
  }
  if (self.t == V_PLAN) {
    /* a plan is addressable as data: p.token, p.files, p.op (SPEC 3.5 shows
     * `if p.files.len > 5 { die }`, which only works if fields are readable).
     * This data read runs BEFORE the apply method everywhere, so a plan field named
     * "apply" would be the field, never the method - the precedence is one rule. */
    Rec *r = rec_new(c->a);
    plan_to_v(c, self.u.pl, r);
    V *p = rec_get(r, name);
    if (p) return *p;
    at_err(c, n, E_UNDEF, "plan has no field '%s' - it has op,confirm,token,files,count", name.p);
    return VN;
  }
  /* Property vs method, one rule: a method that takes no arguments is read as a
   * property (`xs.len`, `r.keys`, `s.chars`) while one that needs arguments stays
   * callable (`xs.map(f)`). SPEC 1.4 lists .len as a field, so returning a
   * function object for it would make `p.files.len` silently be a function. */
  const Method *pm = meth_find(v_typename(self), name.p);
  if (!pm) pm = meth_find("any", name.p);
  if (pm && pm->min == 0 && (pm->max == 0 || pm->max < 0))
    return lib_method(c, self, name.p, NULL, 0);
  /* A method value is only real if that method exists for this type. Inventing one
   * for a typo would hand the agent a callable-looking object that silently does
   * nothing, which is worse than refusing at the point of the typo. */
  const char *t = v_typename(self);
  if (!meth_find(t, name.p) && !meth_find("any", name.p)) {
    at_err(c, n, E_UNDEF, "%s has no method .%s - list them with: vxa doc %s", t, name.p, t);
    return VN;
  }
  return bound_method(c, self, name);
}



static V set_field(Ctx *c, Node *n, V self, Str name, V val) {
  if (self.t == V_REC) { rec_set(c->a, self.u.r, name, val); return val; }
  at_err(c, n, E_TYPE, "cannot assign .%s on a %s (only records have writable fields)", name.p, v_typename(self));
  return VN;
}

/* ---------- calls ---------- */
V call_fn(Ctx *c, V fn, V *args, int nargs) {
  if (v_is_err(fn)) return fn;
  if (fn.t != V_FN) { set_error(c, E_TYPE, "cannot call a %s", v_typename(fn)); return VN; }
  Fn *f = fn.u.f;
  if (f->kind == F_NATIVE) {
    if (!f->native) { set_error(c, E_INTERNAL, "method value used without a receiver call"); return VN; }
    return f->native(args, f->ud);
  }
  if (nargs != f->nparams) {
    at_err(c, f->body, E_ARITY, "%s takes %d argument(s), got %d",
           f->name ? f->name : "function", f->nparams, nargs);
    return VN;
  }
  if (c->depth >= c->maxdepth) { at_err(c, f->body, E_GUARD, "call depth %d exceeded (raise --depth or fix recursion)", c->maxdepth); return VN; }
  Env *e = env_new(c->a, f->env);
  for (int i = 0; i < nargs; i++) env_set(c->a, e, f->names[i].p, args[i]);
  Env *save = c->env;
  c->env = e; c->depth++;
  V r = eval_node_impl(c, f->body, false);
  c->depth--; c->env = save;
  if (c->flow == F_RETURN) { r = c->retval; c->flow = F_NONE; }
  return r;
}

V run_builtin(Ctx *c, const char *ns, const char *name, V *args, int nargs) {
  const Builtin *b = lib_find(ns, name);
  if (!b) {
    const Builtin *sug = lib_suggest(ns, name);
    if (sug)
      set_error(c, E_UNDEF, "%s.%s does not exist - did you mean %s.%s? (sig: %s)",
                ns, name, sug->ns, sug->name, sug->sig);
    else
      set_error(c, E_UNDEF, "%s.%s does not exist - list the namespace with: vxa doc %s", ns, name, ns);
    return VN;
  }
  return lib_call(c, b, args, nargs);
}

/* is `node` a plain identifier that is not bound? (namespace probe) */
static bool unbound_id(Ctx *c, Node *node) {
  return node && node->k == NK_ID && !env_own(c->glob, node->s.p) && !env_get(c->env, node->s.p);
}

V lib_call_value(Ctx *c, Node *call, V *args_pre, int nargs_pre, bool piped) {
  Node *cn = call->kids[0];
  V *args = args_pre;
  int nargs = nargs_pre;
  /* global builtins in call position: len(x), keys(rec), sum(xs) - SPEC 4.4, ns "" */
  if (cn->k == NK_ID && !args && !env_get(c->env, cn->s.p) && lib_find("", cn->s.p)) {
    V *ga = call->nitems ? (V*)arena_zalloc(c->a, sizeof(V) * (size_t)call->nitems) : NULL;
    for (int i = 0; i < call->nitems; i++) {
      ga[i] = ev(c, call->items[i]);
      if (ctx_has_err(c)) return VN;
    }
    return run_builtin(c, "", cn->s.p, ga, call->nitems);
  }
  if (cn->k == NK_FIELD && unbound_id(c, cn->kids[0])) {
    const char *ns = cn->kids[0]->s.p;
    if (!args) {
      args = (V*)arena_zalloc(c->a, sizeof(V) * (size_t)(call->nitems ? call->nitems : 1));
      for (int i = 0; i < call->nitems; i++) {
        args[i] = eval_node_impl(c, call->items[i], true);
        if (ctx_has_err(c)) return VN;
      }
      nargs = call->nitems;
    }
    return run_builtin(c, ns, cn->s.p, args, nargs);
  }
  V self = VN; bool have_self = false;
  if (cn->k == NK_FIELD) {
    self = eval_node_impl(c, cn->kids[0], true);
    if (ctx_has_err(c)) return VN;
    have_self = true;
    /* PLAN.apply is one of the two ways a plan becomes a disk action (SPEC 3.4b),
     * so it is routed here rather than by name lookup - but AFTER the plan's data
     * fields (plan_has_field), so a field named "apply" still wins over the method.
     * A record holding a token this ctx already applied answers it too, unchanged:
     * that is what a builtin which spent --confirm itself (sh.run) hands back, and
     * it is why the chained and the two-step spelling now take the same route. */
    if (!strcmp(cn->s.p, "apply") &&
        ((self.t == V_PLAN && !plan_has_field(c, self, "apply")) ||
         (self.t == V_REC && !rec_getz(self.u.r, "apply") && ctx_applied(c, receipt_token(self))))) {
      if (!args) {
        args = (V*)arena_zalloc(c->a, sizeof(V) * (size_t)(call->nitems ? call->nitems : 1));
        for (int i = 0; i < call->nitems; i++) {
          args[i] = eval_node_impl(c, call->items[i], true);
          if (ctx_has_err(c)) return VN;
        }
        nargs = call->nitems;
      }
      if (self.t == V_REC) return self;                   /* already applied: no re-run */
      return op_apply(c, self, nargs ? args[0] : VN);
    }
    /* A record's own key beats a method of the same name (`r.keys` is data when r
     * owns "keys"); every other name is a method call, dispatched exactly the way
     * every other type dispatches one. That includes the no-argument methods:
     * asking field_of for `r.type()` would EVALUATE it and hand back "rec", which
     * the call below would then try to call. */
    if (self.t != V_REC || !rec_get(self.u.r, cn->s)) {
      if (!args) {
        args = (V*)arena_zalloc(c->a, sizeof(V) * (size_t)(call->nitems ? call->nitems : 1));
        for (int i = 0; i < call->nitems; i++) {
          args[i] = eval_node_impl(c, call->items[i], true);
          if (ctx_has_err(c)) return VN;
        }
        nargs = call->nitems;
      }
      return lib_method(c, self, cn->s.p, args, nargs);
    }
  }
  V fn = have_self ? field_of(c, cn, self, cn->s) : eval_node_impl(c, cn, true);
  if (ctx_has_err(c)) return VN;
  if (fn.t == V_FN && fn.u.f->kind == F_NATIVE && fn.u.f->ud && !fn.u.f->native) {
    Bound *bd = (Bound*)fn.u.f->ud;
    if (!args) {
      args = (V*)arena_zalloc(c->a, sizeof(V) * (size_t)(call->nitems ? call->nitems : 1));
      for (int i = 0; i < call->nitems; i++) {
        args[i] = eval_node_impl(c, call->items[i], true);
        if (ctx_has_err(c)) return VN;
      }
      nargs = call->nitems;
    }
    return lib_method(c, bd->self, bd->name, args, nargs);
  }
  if (!args) {
    args = (V*)arena_zalloc(c->a, sizeof(V) * (size_t)(call->nitems ? call->nitems : 1));
    for (int i = 0; i < call->nitems; i++) {
      args[i] = eval_node_impl(c, call->items[i], true);
      if (ctx_has_err(c)) return VN;
    }
    nargs = call->nitems;
  }
  (void)piped;
  return call_fn(c, fn, args, nargs);
}

/* ---------- evaluation ---------- */
static V ev(Ctx *c, Node *n);
V eval_node_impl(Ctx *c, Node *n, bool noscope);

static V ev_block_scoped(Ctx *c, Node *n) {
  Env *e = env_new(c->a, c->env);
  Env *save = c->env;
  c->env = e;
  V r = VN;
  for (int i = 0; i < n->nitems; i++) {
    r = ev(c, n->items[i]);
    if (ctx_has_err(c) || c->flow) break;
  }
  c->env = save;
  return r;
}

static V ev_interp(Ctx *c, Node *n) {
  Buf b; buf_init(&b, c->a);
  for (int i = 0; i < n->nitems; i++) {
    if (i % 2 == 0) { Str lit = n->items[i]->s; if (lit.len) buf_put(&b, lit.p, (size_t)lit.len); continue; }
    V v = ev(c, n->items[i]);
    if (ctx_has_err(c)) return VN;
    Str s = as_text(c, v);
    buf_put(&b, s.p, (size_t)s.len);
  }
  return v_str(buf_take(&b));
}

static V ev_for(Ctx *c, Node *n) {
  V it = ev(c, n->kids[0]);
  if (ctx_has_err(c)) return VN;
  int len = 0;
  if (it.t == V_LIST) len = it.u.l->len;
  else if (it.t == V_REC) len = it.u.r->len;
  else if (it.t == V_STR) len = s_ucount(it.u.s);
  else if (it.t == V_NUM) len = (int)it.u.n;
  else if (it.t == V_NULL) len = 0;
  else { at_err(c, n, E_TYPE, "cannot iterate over a %s", v_typename(it)); return VN; }
  if (n->nnames == 0 || n->nnames > 2) { at_err(c, n, E_SYNTAX, "for takes one or two names"); return VN; }
  Node *body = n->kids[1];
  V r = VN;
  for (int i = 0; i < len; i++) {
    Env *e = env_new(c->a, c->env);
    V key, val;
    if (it.t == V_LIST)      { key = num(i); val = it.u.l->v[i]; }
    else if (it.t == V_REC)  { key = v_str(it.u.r->kv[i].k); val = it.u.r->kv[i].v; }
    else if (it.t == V_STR)  { key = num(i); val = v_str(s_char_at(c->a, it.u.s, i)); }
    else                     { key = num(i); val = num(i); }
    /* for v in list/string/range  -> v = element
     * for k, v in rec             -> k = key, v = value  (SPEC 1.3) */
    if (n->nnames > 1 && it.t == V_REC) {
      env_set(c->a, e, n->names[0].p, key);
      env_set(c->a, e, n->names[1].p, val);
    } else {
      env_set(c->a, e, n->names[0].p, val);
      if (n->nnames > 1) env_set(c->a, e, n->names[1].p, key);
    }
    Env *save = c->env; c->env = e;
    for (int k = 0; k < body->nitems; k++) {
      r = ev(c, body->items[k]);
      if (ctx_has_err(c)) { c->env = save; return VN; }
      if (c->flow == F_BREAK) { c->flow = F_NONE; break; }
      if (c->flow == F_CONT)  { c->flow = F_NONE; continue; }
      if (c->flow == F_RETURN) { c->env = save; return r; }
    }
    c->env = save;
    if (c->steps > c->maxsteps) { at_err(c, n, E_GUARD, "step limit %lld exceeded", c->maxsteps); return VN; }
  }
  return r;
}

static V ev_while(Ctx *c, Node *n) {
  V r = VN;
  for (;;) {
    V cond = ev(c, n->kids[0]);
    if (ctx_has_err(c)) return VN;
    if (!v_truthy(cond)) break;
    r = ev(c, n->kids[1]);
    if (ctx_has_err(c)) return VN;
    if (c->flow == F_BREAK) { c->flow = F_NONE; break; }
    if (c->flow == F_CONT) c->flow = F_NONE;
    if (c->flow == F_RETURN) return r;
    if (c->steps > c->maxsteps) { at_err(c, n, E_GUARD, "step limit %lld exceeded (raise --steps)", c->maxsteps); return VN; }
  }
  return r;
}

static V ev_assign(Ctx *c, Node *n) {
  Node *t = n->kids[0];
  V v = ev(c, n->kids[1]);
  bool pending = false;
  if (t->k == NK_ID && err_catchable(c, v, &pending)) {
    /* SPEC 1.5: bind the error and keep going; the agent checks it as data. */
    V bind = pending ? c->err : v;
    if (!env_assign(c->env, t->s.p, bind)) env_set(c->a, c->env, t->s.p, bind);
    ctx_clear_err(c);
    return bind;
  }
  if (ctx_has_err(c)) return v;
  if (t->k == NK_ID) {
    const char *name = t->s.p;
    if (!env_assign(c->env, name, v)) env_set(c->a, c->env, name, v);
    return v;
  }
  if (t->k == NK_INDEX) {
    V self = ev(c, t->kids[0]);
    if (ctx_has_err(c)) return VN;
    V iv = ev(c, t->kids[1]);
    if (ctx_has_err(c)) return VN;
    return set_index(c, t, self, iv, v);
  }
  if (t->k == NK_FIELD) {
    V self = ev(c, t->kids[0]);
    if (ctx_has_err(c)) return VN;
    return set_field(c, t, self, t->s, v);
  }
  at_err(c, n, E_SYNTAX, "the left side of '=' must be a name, an index or a field");
  return VN;
}

static bool glob_fn(Ctx *c, Node *cn) {
  return cn && cn->k == NK_ID && !env_get(c->env, cn->s.p) && lib_find("", cn->s.p) != NULL;
}

static V ev_pipe(Ctx *c, Node *n) {
  V lhs = ev(c, n->kids[0]);
  if (ctx_has_err(c)) return VN;
  Node *call = n->kids[1];
  int nargs = call->nitems + 1;
  V *args = (V*)arena_zalloc(c->a, sizeof(V) * (size_t)nargs);
  args[0] = lhs;
  for (int i = 0; i < call->nitems; i++) {
    args[i + 1] = ev(c, call->items[i]);
    if (ctx_has_err(c)) return VN;
  }
  Node *cn = call->kids[0];
  if (cn->k == NK_FIELD && unbound_id(c, cn->kids[0]))
    return run_builtin(c, cn->kids[0]->s.p, cn->s.p, args, nargs);
  if (glob_fn(c, cn)) return run_builtin(c, "", cn->s.p, args, nargs);
  if (cn->k == NK_FIELD) {
    /* `xs | map(f)` is xs.map(f): the piped value becomes the receiver, not an
     * argument. This is the spelling an agent reaches for by reflex, and it keeps
     * the pipe rule single: the left side is always the thing on the left of the dot. */
    return lib_method(c, lhs, cn->s.p, args + 1, nargs - 1);
  }
  if (cn->k == NK_ID && !env_get(c->env, cn->s.p) && !lib_find("", cn->s.p)) {
    /* exact method name, receiver from the left side: `xs | map(f)` == `xs.map(f)`.
     * This is a name lookup, not a guess: the name must exist as a method. */
    return lib_method(c, lhs, cn->s.p, args + 1, nargs - 1);
  }
  /* plain function callee: SPEC 1.2 - the piped value is the FIRST argument */
  V fn = ev(c, cn);
  if (ctx_has_err(c)) return VN;
  return call_fn(c, fn, args, nargs);
}

static V ev(Ctx *c, Node *n) {
  if (!n) return VN;
  if (ctx_has_err(c)) return VN;
  c->node = n;
  if (n->k != NK_BLOCK && ++c->steps > c->maxsteps) {
    at_err(c, n, E_GUARD, "step limit %lld exceeded (raise --steps)", c->maxsteps);
    return VN;
  }
  switch (n->k) {
    case NK_INT: case NK_FLOAT: return num(n->num);
    case NK_STR: return v_str(n->s);
    case NK_INTERP: return ev_interp(c, n);
    case NK_ID: {
      const char *nm = n->s.p;
      if (!strcmp(nm, "null")) return VN;
      if (!strcmp(nm, "true")) return v_bool(true);
      if (!strcmp(nm, "false")) return v_bool(false);
      V *p = env_get(c->env, nm);
      if (!p) { at_err(c, n, E_UNDEF, "'%s' is not defined", nm); return VN; }
      return *p;
    }
    case NK_BLOCK: return ev_block_scoped(c, n);
    case NK_BIN: {
      const char *op = n->s.p;
      if (!strcmp(op, "and") || !strcmp(op, "or")) {
        V a = ev(c, n->kids[0]);
        if (ctx_has_err(c)) return VN;
        if (!strcmp(op, "or") && v_truthy(a)) return a;
        if (!strcmp(op, "and") && !v_truthy(a)) return a;
        return ev(c, n->kids[1]);
      }
      V a = ev(c, n->kids[0]);
      if (ctx_has_err(c)) return VN;
      V b = ev(c, n->kids[1]);
      if (ctx_has_err(c)) return VN;
      return do_bin(c, n, a, b);
    }
    case NK_NEG: {
      V a = ev(c, n->kids[0]);
      if (ctx_has_err(c)) return VN;
      if (!is_num(a)) { at_err(c, n, E_TYPE, "unary '-' needs a number, got %s", v_typename(a)); return VN; }
      return num(-a.u.n);
    }
    case NK_NOT: {
      V a = ev(c, n->kids[0]);
      if (ctx_has_err(c)) return VN;
      return v_bool(!v_truthy(a));
    }
    case NK_LIST: {
      List *l = list_new(c->a);
      for (int i = 0; i < n->nitems; i++) {
        V v = ev(c, n->items[i]);
        if (ctx_has_err(c)) return VN;
        list_push(c->a, l, v);
      }
      return list_v(l);
    }
    case NK_REC: {
      Rec *r = rec_new(c->a);
      for (int i = 0; i < n->nitems; i++) {
        V v = ev(c, n->items[i]->kids[0]);
        if (ctx_has_err(c)) return VN;
        rec_set(c->a, r, n->items[i]->s, v);
      }
      return rec_v(r);
    }
    case NK_PAIR: return ev(c, n->kids[0]);
    case NK_RANGE: {
      V a = ev(c, n->kids[0]), b = ev(c, n->kids[1]);
      if (ctx_has_err(c)) return VN;
      if (!is_num(a) || !is_num(b)) { at_err(c, n, E_TYPE, "'..' needs numbers"); return VN; }
      long long lo = (long long)a.u.n, hi = (long long)b.u.n;
      if (hi > lo + 1000000) { at_err(c, n, E_RANGE, "range too wide (limit 1000000)"); return VN; }
      List *l = list_new(c->a);
      for (long long i = lo; i <= hi; i++) list_push(c->a, l, num((double)i));
      return list_v(l);
    }
    case NK_LAMBDA: {
      Fn *f = (Fn*)arena_zalloc(c->a, sizeof(Fn));
      f->kind = F_SCRIPT;
      f->body = n->kids[0];
      f->names = n->names;
      f->nparams = n->nn;
      f->env = c->env;
      V r; r.t = V_FN; r.u.f = f;
      return r;
    }
    case NK_FUNC: {
      V f = ev(c, n->kids[0]);
      if (ctx_has_err(c)) return VN;
      if (f.t == V_FN) {
        f.u.f->name = arena_strdup(c->a, n->s.p);
        if (f.u.f->nparams != n->nnames) { /* def uses lambda names; keep in sync */ }
      }
      env_set(c->a, c->glob, n->s.p, f);      /* functions are global by declaration */
      return f;
    }
    case NK_ASSIGN: return ev_assign(c, n);
    case NK_IF: {
      V cond = ev(c, n->kids[0]);
      if (ctx_has_err(c)) return VN;
      if (v_truthy(cond)) return ev(c, n->kids[1]);
      if (n->kids[2]) return ev(c, n->kids[2]);
      return VN;
    }
    case NK_WHILE: return ev_while(c, n);
    case NK_FOR: return ev_for(c, n);
    case NK_CALL: return lib_call_value(c, n, NULL, 0, false);
    case NK_INDEX: {
      V self = ev(c, n->kids[0]);
      if (ctx_has_err(c)) return VN;
      V iv = ev(c, n->kids[1]);
      if (ctx_has_err(c)) return VN;
      V to = (n->nkids > 2 && n->kids[2]) ? ev(c, n->kids[2]) : VN;
      if (ctx_has_err(c)) return VN;
      return index_of(c, n, self, iv, to, n->nkids > 2);
    }
    case NK_FIELD: {
      Node *on = n->kids[0];
      if (on && on->k == NK_ID && unbound_id(c, on)) {
        at_err(c, n, E_UNDEF, "'%s' is a namespace, not a value - call %s.%s(...)", on->s.p, on->s.p, n->s.p);
        return VN;
      }
      V self = ev(c, on);
      if (ctx_has_err(c)) return VN;
      return field_of(c, n, self, n->s);
    }
    case NK_PIPE: return ev_pipe(c, n);
    case NK_RETURN: {
      V v = (n->nkids && n->kids[0]) ? ev(c, n->kids[0]) : VN;
      if (ctx_has_err(c)) return VN;
      c->flow = F_RETURN; c->retval = v;
      return v;
    }
    case NK_BREAK: c->flow = F_BREAK; return VN;
    case NK_CONTINUE: c->flow = F_CONT; return VN;
    case NK_GLOBAL: {
      for (int i = 0; i < n->nnames; i++)
        if (!env_own(c->glob, n->names[i].p)) env_set(c->a, c->glob, n->names[i].p, VN);
      return VN;
    }
    case NK_PAREN: return ev(c, n->kids[0]);
    case NK_LOCAL: return VN;
    default:
      at_err(c, n, E_INTERNAL, "unhandled node kind %d", n->k);
      return VN;
  }
}

V eval_node(Ctx *c, Node *n) { return ev(c, n); }
V eval_node_impl(Ctx *c, Node *n, bool noscope) {
  if (noscope && n && n->k == NK_BLOCK) return ev_block_scoped(c, n);
  return ev(c, n);
}
V eval_in_env(Ctx *c, Node *n, Env *e) {
  Env *save = c->env;
  c->env = e;
  V r = ev(c, n);
  c->env = save;
  return r;
}

/* ---------- parse glue ---------- */
Node *parse_script(Ctx *c, const char *src, size_t len, const char *name, V *err_out) {
  Arena *a = c->a;
  Lexer lx; memset(&lx, 0, sizeof lx);
  lx.a = a;
  lex(a, src, len, &lx);
  if (lx.err) {
    *err_out = v_err_hint(E_SYNTAX, s_lit(a, lx.err), q(a, err_hint(E_SYNTAX)));
    return NULL;
  }
  char *pe = NULL; int eline = 0;
  Node *prog = parse_program(a, lx.toks, lx.n, name, &pe, &eline);
  if (!prog) {
    *err_out = v_err_hint(E_SYNTAX, s_lit(a, pe ? pe : "syntax error"), q(a, err_hint(E_SYNTAX)));
    return NULL;
  }
  return prog;
}

/* ---------- Args: uniform argument checking for builtins ---------- */
static void a_err(Args *x, ErrCode code, const char *fmt, ...) {
  if (ctx_has_err(x->c)) return;
  char msg[400];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  set_error(x->c, code, "%s", msg);
}
bool arg_present(Args *x, int i) { return i >= 0 && i < x->n; }
V arg_at(Args *x, int i) {
  if (!arg_present(x, i)) { a_err(x, E_ARITY, "argument %d is missing", i + 1); return VN; }
  return x->a[i];
}
bool arg_has_err(Args *x) { return ctx_has_err(x->c); }
Str arg_str(Args *x, int i, const char *what) {
  V v = arg_at(x, i);
  if (ctx_has_err(x->c)) return s_null();
  if (v.t != V_STR) { a_err(x, E_TYPE, "%s must be a string, got %s", what, v_typename(v)); return s_null(); }
  return v.u.s;
}
double arg_num(Args *x, int i, const char *what) {
  V v = arg_at(x, i);
  if (ctx_has_err(x->c)) return 0;
  if (v.t != V_NUM) { a_err(x, E_TYPE, "%s must be a number, got %s", what, v_typename(v)); return 0; }
  return v.u.n;
}
bool arg_bool(Args *x, int i, bool dflt) {
  if (!arg_present(x, i)) return dflt;
  return v_truthy(x->a[i]);
}
Rec *arg_rec(Args *x, int i, const char *what) {
  V v = arg_at(x, i);
  if (ctx_has_err(x->c)) return NULL;
  if (v.t != V_REC) { a_err(x, E_TYPE, "%s must be a record {...}, got %s", what, v_typename(v)); return NULL; }
  return v.u.r;
}
List *arg_list(Args *x, int i, const char *what) {
  V v = arg_at(x, i);
  if (ctx_has_err(x->c)) return NULL;
  if (v.t != V_LIST) { a_err(x, E_TYPE, "%s must be a list [...], got %s", what, v_typename(v)); return NULL; }
  return v.u.l;
}
V arg_opt(Args *x, int i, const char *name, V dflt) {
  if (!arg_present(x, i) || x->a[i].t != V_REC) return dflt;
  V *p = rec_getz(x->a[i].u.r, name);
  return p ? *p : dflt;
}
int arg_opt_int(Args *x, int i, const char *name, int dflt) {
  V v = arg_opt(x, i, name, v_num(dflt));
  return v.t == V_NUM ? (int)v.u.n : dflt;
}
Str arg_opt_str(Args *x, int i, const char *name, const char *dflt) {
  V v = arg_opt(x, i, name, VN);
  return v.t == V_STR ? v.u.s : s_wrap(dflt);
}
bool arg_opt_bool(Args *x, int i, const char *name, bool dflt) {
  V v = arg_opt(x, i, name, VN);
  return v.t == V_NULL ? dflt : v_truthy(v);
}

/* ---------- constructors ---------- */
Str q(Arena *a, const char *s) { return s_lit(a, s ? s : ""); }
V v_ok(Arena *a, int n, ...) {
  Rec *r = rec_new(a);
  va_list ap; va_start(ap, n);
  for (int i = 0; i < n; i++) {
    const char *k = va_arg(ap, const char*);
    V v = va_arg(ap, V);
    rec_set(a, r, s_wrap(k), v);
  }
  va_end(ap);
  return rec_to_v(r);
}
V ok_rec(Arena *a) { return rec_to_v(rec_new(a)); }
