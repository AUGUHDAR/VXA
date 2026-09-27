/* lib_core.c - out / cfg / misc namespaces.
 * out.* is how a script talks back to the agent; cfg.* is the three-layer config
 * (CLI --set > .vxa/config.json > defaults); misc.* is the small global toolbox. */
#include "vxa.h"
#include "interp.h"
#include "lib.h"
V cfg_get_c(Ctx *c, const char *k);
void cfg_set_c(Ctx *c, const char *k, V v);
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* ================= out ================= */
static V f_emit(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  V v = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  Str s = v_tostr(ctx_arena(c), v, ctx_fmt(c) != FMT_JSON);
  if (s.p) { fwrite(s.p, 1, (size_t)s.len, stdout); fputc('\n', stdout); }
  fflush(stdout);
  return v_ok(ctx_arena(c), 1, "emitted", VT);
}
static V f_raw(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  Str s = arg_str(&x, 0, "raw value");
  if (arg_has_err(&x)) return VN;
  fwrite(s.p, 1, (size_t)s.len, stdout);
  fflush(stdout);
  return v_ok(ctx_arena(c), 1, "emitted", VT);
}
static V f_json(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  V v = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  Str s = v_tojson(ctx_arena(c), v);
  if (s.p) { fwrite(s.p, 1, (size_t)s.len, stdout); fputc('\n', stdout); }
  fflush(stdout);
  return v_ok(ctx_arena(c), 1, "emitted", VT);
}
static V f_log(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  V v = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  Str s = v_tostr(ctx_arena(c), v, true);
  if (s.p) { fprintf(stderr, "%.*s\n", s.len, s.p); fflush(stderr); }
  return v_ok(ctx_arena(c), 1, "logged", VT);
}
static V f_die(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  V v = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  if (v_is_err(v)) { set_error(c, v_errcode(v), "%.*s", v.u.e->msg.len, v.u.e->msg.p); return VN; }
  Str s = v_tostr(ctx_arena(c), v, true);
  set_error(c, E_LIMIT, "%.*s", s.len, s.p ? s.p : "");
  return VN;
}
static V f_kv(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  Str k = arg_str(&x, 0, "key");
  V v = arg_at(&x, 1);
  if (arg_has_err(&x)) return VN;
  Buf b; buf_init(&b, ctx_arena(c));
  buf_add_escaped(&b, k); buf_putc(&b, '='); buf_add_escaped(&b, v_tostr(ctx_arena(c), v, true));
  fprintf(stdout, "%s\n", b.p ? b.p : "");
  fflush(stdout);
  return v_ok(ctx_arena(c), 1, "emitted", VT);
}

static const Builtin out_tab[] = {
  { "out", "emit", f_emit, 1, 1, "out.emit(v)", "print v in the compact machine format (stdout is machine output only)", "out.emit({ok:true,n:3})", BF_PURE },
  { "out", "kv",   f_kv,   2, 2, "out.kv(key, v)", "print one key=value line", "out.kv(\"bytes\", 12)", BF_PURE },
  { "out", "json", f_json, 1, 1, "out.json(v)", "print strict JSON when you need to parse it mechanically", "out.json([1,2])", BF_PURE },
  { "out", "raw",  f_raw,  1, 1, "out.raw(s)", "print a string with no formatting at all", "out.raw(text)", BF_PURE },
  { "out", "log",  f_log,  1, 1, "out.log(msg)", "progress to stderr; never pollutes stdout", "out.log(\"2/5 files\")", 0 },
  { "out", "die",  f_die,  1, 1, "out.die(err|msg)", "stop the script with this error instead of returning it", "if n>100 { out.die(\"too many\") }", 0 },
};
const Builtin *t_out(int *n) { *n = (int)(sizeof(out_tab)/sizeof(out_tab[0])); return out_tab; }

/* ================= cfg ================= */
V cfg_get_c(Ctx *c, const char *k) { V *p = rec_getz(ctx_cfg(c), k); return p ? *p : VN; }
void cfg_set_c(Ctx *c, const char *k, V v) { rec_setz(ctx_arena(c), ctx_cfg(c), k, v); }

static V f_cfg_get(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  Str k = arg_str(&x, 0, "cfg.get key");
  V def = arg_present(&x, 1) ? a[1] : VN;
  if (arg_has_err(&x)) return VN;
  V *p = rec_getz(ctx_cfg(c), k.p);
  return p ? *p : def;
}
static V f_cfg_set(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  Str k = arg_str(&x, 0, "cfg.set key");
  V v = arg_at(&x, 1);
  if (arg_has_err(&x)) return VN;
  rec_set(ctx_arena(c), ctx_cfg(c), k, v);
  return v;
}
static V f_cfg_merge(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  Rec *r = arg_rec(&x, 0, "cfg.merge value");
  if (arg_has_err(&x)) return VN;
  for (int i = 0; i < r->len; i++) rec_set(ctx_arena(c), ctx_cfg(c), r->kv[i].k, r->kv[i].v);
  return rec_to_v(ctx_cfg(c));
}
static V f_cfg_keys(Ctx *c, V *a, int n) {
  (void)a; (void)n;
  List *l = list_new(ctx_arena(c));
  for (int i = 0; i < ctx_cfg(c)->len; i++) list_push(ctx_arena(c), l, v_str(ctx_cfg(c)->kv[i].k));
  return list_of(l);
}
static V f_cfg_save(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  Str path = arg_opt_str(&x, 0, "path", ".vxa/config.json");
  Arena *ar = ctx_arena(c);
  Str body = v_tojson(ar, rec_to_v(ctx_cfg(c)));
  Confirm need = policy_for(c, "policies.fs");
  Plan *p = plan_target(c, "cfg.save", need, PK_CFG, path.p);
  if (!p) return VN;
  plan_set_payload(c, p, body.p, (size_t)body.len);
  return v_plan(p);
}

static const Builtin cfg_tab[] = {
  { "cfg", "get",   f_cfg_get,   1, 2, "cfg.get(key, default?)", "read config (CLI --set > .vxa/config.json > defaults)", "fmt = cfg.get(\"fmt\", \"auto\")", BF_PURE },
  { "cfg", "set",   f_cfg_set,   2, 2, "cfg.set(key, value)", "set in-memory config; use cfg.save to persist", "cfg.set(\"max_bytes\", 4000)", 0 },
  { "cfg", "merge", f_cfg_merge, 1, 1, "cfg.merge(rec)", "merge a record into config at once", "cfg.merge({a:1,b:2})", 0 },
  { "cfg", "keys",  f_cfg_keys,  0, 0, "cfg.keys()", "list every config key", "for k in cfg.keys() { out.kv(k, cfg.get(k)) }", BF_PURE },
  { "cfg", "save",  f_cfg_save,  0, 1, "cfg.save({path:\".vxa/config.json\"}) -> PLAN", "persist config through the confirm protocol", "cfg.save().apply()", 0 },
};
const Builtin *t_cfg(int *n) { *n = (int)(sizeof(cfg_tab)/sizeof(cfg_tab[0])); return cfg_tab; }

/* ================= misc ================= */
static V f_len(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  V v = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  if (v.t == V_STR) return v_num(v.u.s.len);
  if (v.t == V_LIST) return v_num(v.u.l->len);
  if (v.t == V_REC) return v_num(v.u.r->len);
  if (v.t == V_NULL) return v_num(0);
  set_error(c, E_TYPE, "len does not apply to %s", v_typename(v));
  return VN;
}
static V f_type(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  V v = arg_at(&x, 0);
  return v_str(s_lit(ctx_arena(c), v_typename(v)));
}
static V f_tostr(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  V v = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  return v_str(v_tostr(ctx_arena(c), v, !arg_opt_bool(&x, 1, "json", false)));
}
static V f_hash(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  V v = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  Str s = v_tostr(ctx_arena(c), v, true);
  return v_str(hash12(ctx_arena(c), s.p, (size_t)s.len));
}
static V f_num(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  V v = arg_at(&x, 0);
  Arena *ar = ctx_arena(c);
  if (arg_has_err(&x)) return VN;
  if (v.t == V_NUM) return v;
  if (v.t == V_BOOL) return v_num(v.u.b ? 1 : 0);
  if (v.t == V_STR) {
    long long iv;
    if (s_is_int(v.u.s, &iv)) return v_num((double)iv);
    char *e; double d = strtod(v.u.s.p, &e);
    if (e && e != v.u.s.p) return v_num(d);
  }
  set_error(c, E_TYPE, "cannot turn a %s into a number", v_typename(v));
  (void)ar;
  return VN;
}
static V pick2(Ctx *c, V *a, int n, int mode) {
  Args x = { c, a, n, NULL };
  List *l = arg_list(&x, 0, "argument");
  if (arg_has_err(&x)) return VN;
  if (!l->len) return VN;
  V best = l->v[0];
  for (int i = 1; i < l->len; i++) {
    V v = l->v[i];
    if (v.t == V_NUM && best.t == V_NUM) {
      if (mode == 0 ? v.u.n < best.u.n : v.u.n > best.u.n) best = v;
    } else if (v.t == V_STR && best.t == V_STR) {
      if (mode == 0 ? s_cmp(v.u.s, best.u.s) < 0 : s_cmp(v.u.s, best.u.s) > 0) best = v;
    }
  }
  return best;
}
static V f_min(Ctx *c, V *a, int n) { return pick2(c, a, n, 0); }
static V f_max(Ctx *c, V *a, int n) { return pick2(c, a, n, 1); }
static V f_sum(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  List *l = arg_list(&x, 0, "sum");
  if (arg_has_err(&x)) return VN;
  double s = 0;
  for (int i = 0; i < l->len; i++) {
    if (l->v[i].t != V_NUM) { set_error(c, E_TYPE, "sum needs numbers, item %d is %s", i + 1, v_typename(l->v[i])); return VN; }
    s += l->v[i].u.n;
  }
  return v_num(s);
}
static V f_range(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  double lo = arg_num(&x, 0, "range start");
  double hi = arg_present(&x, 1) ? arg_num(&x, 1, "range end") : 0;
  if (arg_has_err(&x)) return VN;
  if (!arg_present(&x, 1)) { hi = lo - 1; lo = 0; }
  Arena *ar = ctx_arena(c);
  List *l = list_new(ar);
  long long L = (long long)lo, H = (long long)hi;
  if (H > L + 1000000) { set_error(c, E_RANGE, "range too wide (limit 1000000)"); return VN; }
  for (long long i = L; i <= H; i++) list_push(ar, l, v_num((double)i));
  return list_of(l);
}
static V f_abs(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  double d = arg_num(&x, 0, "abs");
  return arg_has_err(&x) ? VN : v_num(fabs(d));
}
static V f_now(Ctx *c, V *a, int n) {
  (void)a; (void)n;
  Arena *ar = ctx_arena(c);
  time_t t = time(NULL);
  struct tm tmv;
  char buf[40];
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tmv);
  return v_ok(ar, 2, "epoch", v_num((double)t), "iso", v_str(s_lit(ar, buf)));
}
static V f_sleep(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  double ms = arg_num(&x, 0, "sleep ms");
  if (arg_has_err(&x)) return VN;
  if (ms < 0 || ms > 60000) { set_error(c, E_RANGE, "sleep must be 0..60000 ms"); return VN; }
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  struct timespec ts = { (time_t)(ms / 1000), (long)(fmod(ms, 1000) * 1e6) };
  nanosleep(&ts, NULL);
#endif
  return v_num(ms);
}
static V f_env(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  Str k = arg_str(&x, 0, "env name");
  if (arg_has_err(&x)) return VN;
  char *v = getenv(k.p);
  return v ? v_str(s_lit(ctx_arena(c), v)) : VN;
}
static V f_keys(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  V v = arg_at(&x, 0);
  if (arg_has_err(&x) || v.t != V_REC) { if (!arg_has_err(&x)) set_error(c, E_TYPE, "keys needs a record, got %s", v_typename(v)); return VN; }
  Arena *ar = ctx_arena(c);
  List *l = list_new(ar);
  for (int i = 0; i < v.u.r->len; i++) list_push(ar, l, v_str(v.u.r->kv[i].k));
  return list_of(l);
}
static V f_vals(Ctx *c, V *a, int n) {
  Args x = { c, a, n, NULL };
  V v = arg_at(&x, 0);
  if (arg_has_err(&x) || v.t != V_REC) { if (!arg_has_err(&x)) set_error(c, E_TYPE, "vals needs a record, got %s", v_typename(v)); return VN; }
  Arena *ar = ctx_arena(c);
  List *l = list_new(ar);
  for (int i = 0; i < v.u.r->len; i++) list_push(ar, l, v.u.r->kv[i].v);
  return list_of(l);
}

static const Builtin misc_tab[] = {
  { "", "len",   f_len,   1, 1, "len(x)", "size of a string (bytes), list, or record", "len(lines)", BF_PURE },
  { "", "type",  f_type,  1, 1, "type(v)", "the value's type name - check it before you branch", "type(r) == \"list\"", BF_PURE },
  { "", "tostr", f_tostr, 1, 2, "tostr(v, {json:false})", "render any value to text", "tostr(plan)", BF_PURE },
  { "", "hash",  f_hash,  1, 1, "hash(v)", "12-char content fingerprint: compare two states cheaply", "hash(text) == fs.hash(p)", BF_PURE },
  { "", "num",   f_num,   1, 1, "num(v)", "parse a number out of a string; errors instead of returning 0", "num(\"42\")", BF_PURE },
  { "", "abs",   f_abs,   1, 1, "abs(x)", "absolute value", "abs(a - b)", BF_PURE },
  { "", "min",   f_min,   1, 1, "min([..])", "smallest number or string in a list", "min(sizes)", BF_PURE },
  { "", "max",   f_max,   1, 1, "max([..])", "largest number or string in a list", "max(sizes)", BF_PURE },
  { "", "sum",   f_sum,   1, 1, "sum([..])", "sum of numbers; errors on a non-number", "sum(bytes)", BF_PURE },
  { "", "range", f_range, 1, 2, "range(n) | range(a,b)", "list of integers; b inclusive", "for i in range(3) { out.kv(\"i\", i) }", BF_PURE },
  { "", "keys",  f_keys,  1, 1, "keys(rec)", "record keys in insertion order", "keys(rec)", BF_PURE },
  { "", "vals",  f_vals,  1, 1, "vals(rec)", "record values in insertion order", "vals(rec)", BF_PURE },
  { "", "now",   f_now,   0, 0, "now()", "epoch + iso timestamp", "t0 = now().epoch", BF_PURE },
  { "", "sleep", f_sleep, 1, 1, "sleep(ms)", "wait, capped at 60s", "sleep(200)", 0 },
  { "", "env",   f_env,   1, 1, "env(name)", "read an environment variable (null when absent)", "root = env(\"VXA_ROOT\")", BF_PURE },
};
const Builtin *t_misc(int *n) { *n = (int)(sizeof(misc_tab)/sizeof(misc_tab[0])); return misc_tab; }
