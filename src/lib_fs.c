/* lib_fs.c - the `fs` namespace: an agent's eyes and hands on the workspace.
 *
 * Two rules shape everything here.
 *
 * 1. READS are cheap and self-describing. They are NOT jailed (SPEC §5 jails
 *    writes only), they echo back the path the agent asked for, and they always
 *    say how much of the answer is missing (truncated/shown/total). A partial
 *    read that looks complete is the worst failure mode an agent can have: it
 *    keeps reasoning about bytes it never saw.
 *
 * 2. WRITES are never performed. They build a PLAN and hand it back;
 *    plan_execute() owns the policy test, the token, the journal replay and the
 *    TOCTOU re-hash (SPEC §3). This file's job is to make the plan say the
 *    truth: the right target kind, the current content hash in `from`, the
 *    planned content hash in `to`, and the effective confirm level folded into
 *    the token so a policy change cannot silently reuse an old signature.
 *
 * Where the plan/fsx API could not express something, it is stated in a comment
 * and reported - never papered over.
 */
#include "vxa.h"
#include "interp.h"
#include "lib.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
  #include <direct.h>
#endif
#include <unistd.h>

#define FS_DEF_LS_MAX       200
#define FS_DEF_GLOB_MAX     200
#define FS_DEF_READ_BYTES   8000
#define FS_DEF_BUNDLE_BYTES 4000
#define FS_FIELD_MAX        8

/* ---------- tiny predicates ---------- */
static bool is_hexstr(const char *p, int n) {
  if (!p || n <= 0) return false;
  for (int i = 0; i < n; i++) if (!isxdigit((unsigned char)p[i])) return false;
  return true;
}
static const char *s_z(Str s) { return s.p ? s.p : ""; }

static const char *cf_name(Confirm cf) {
  switch (cf) {
    case CF_NONE: return "none";
    case CF_JAIL: return "jail";
    case CF_ALL:  return "all";
    default:      return "ask";
  }
}

/* ---------- path resolution ----------
 * Reads: normalize lexically, no jail test. fs_slurp()/stat() are cwd-relative
 * and ws_resolve() resolves cwd-relative too, so fs.read("x") and fs.write("x")
 * name the same file - parity beats cleverness. fsx.c keeps its getcwd() helper
 * static, so the join is repeated here and only here. */
static Str norm_read(Arena *a, const char *p) {
  if (!p || !*p) return s_null();
  if (path_is_abs(p)) return path_norm(a, p);
  char cwd[4096];
  if (!getcwd(cwd, sizeof cwd)) return path_norm(a, p);
  return path_norm(a, path_join(a, cwd, p).p);
}

/* fail with the core's own hint: every error here is a stable code plus an
 * actionable line, which is what an agent branches on. */
static void fail_e(Ctx *c, ErrCode e, const char *op, const char *p) {
  set_error(c, e, "%s: '%s' - %s", op, p && *p ? p : "-", err_hint(e));
}

/* ---------- checked option readers ----------
 * arg_opt_* falls back to the default when a field holds the wrong type. For an
 * agent that is a silently lost option, and a lost option is a wrong assumption,
 * so these answer with E_TYPE / E_RANGE instead. */
/* the options slot itself must be a record: a mistyped options argument that is
 * quietly ignored leaves the agent with an assumption it never made */
static bool opts_rec(Args *x, int i) {
  if (!arg_present(x, i) || x->a[i].t == V_REC || x->a[i].t == V_NULL) return true;
  set_error(x->c, E_TYPE, "argument %d must be an options record {k:v}, got %s",
            i + 1, v_typename(x->a[i]));
  return false;
}
static V optv(Args *x, int i, const char *name, V dflt) {
  if (!opts_rec(x, i)) return dflt;
  return arg_opt(x, i, name, dflt);
}
static bool o_str(Args *x, int i, const char *name, const char *dflt, Str *out) {
  V v = optv(x, i, name, VN);
  if (v.t == V_NULL) { *out = s_wrap(dflt); return true; }
  if (v.t != V_STR) {
    set_error(x->c, E_TYPE, "option %s must be a string, got %s", name, v_typename(v));
    return false;
  }
  *out = v.u.s;
  return true;
}
static bool o_int(Args *x, int i, const char *name, int dflt, int *out) {
  V v = optv(x, i, name, VN);
  if (v.t == V_NULL) { *out = dflt; return true; }
  if (v.t != V_NUM) {
    set_error(x->c, E_TYPE, "option %s must be a number, got %s", name, v_typename(v));
    return false;
  }
  long long n = (long long)v.u.n;
  if (n < -2000000000LL || n > 2000000000LL) {
    set_error(x->c, E_RANGE, "option %s out of range: %lld", name, n);
    return false;
  }
  *out = (int)n;
  return true;
}
static bool o_nonneg(Args *x, int i, const char *name, int dflt, int *out) {
  if (!o_int(x, i, name, dflt, out)) return false;
  if (*out < 0) {
    set_error(x->c, E_RANGE, "option %s cannot be negative (got %d)", name, *out);
    return false;
  }
  return true;
}
static bool o_bool(Args *x, int i, const char *name, bool dflt) {
  V v = optv(x, i, name, VN);
  return v.t == V_NULL ? dflt : v_truthy(v);
}
/* SPEC §2 prices bytes at est = ceil(bytes/4), so an option given in tokens
 * converts to bytes at the language's own rate. max_bytes wins if both are set. */
static bool budget(Args *x, int i, int dflt, int *out) {
  int v = dflt;
  bool has = false;
  V b = optv(x, i, "max_bytes", VN);
  if (b.t != V_NULL) {
    if (b.t != V_NUM) {
      set_error(x->c, E_TYPE, "option max_bytes must be a number, got %s", v_typename(b));
      return false;
    }
    v = (int)b.u.n;
    has = true;
  }
  V t = optv(x, i, "max_tok", VN);
  if (t.t != V_NULL) {
    if (t.t != V_NUM) {
      set_error(x->c, E_TYPE, "option max_tok must be a number, got %s", v_typename(t));
      return false;
    }
    if (!has) v = (int)(t.u.n * 4);
  }
  if (v < 0) {
    set_error(x->c, E_RANGE, "byte budget cannot be negative (got %d)", v);
    return false;
  }
  *out = v;
  return true;
}

/* lines:"10-40" | "10" | "10-" | "-40"; both ends inclusive. */
static bool parse_lines(Ctx *c, Args *x, int i, int *from, int *to) {
  *from = 0; *to = 0;
  V v = optv(x, i, "lines", VN);
  if (v.t == V_NULL) return true;
  if (v.t == V_NUM) {
    if (v.u.n < 1) { set_error(c, E_BAD_INPUT, "lines must start at a line >= 1"); return false; }
    *from = (int)v.u.n;
    *to = *from;
    return true;
  }
  if (v.t != V_STR) {
    set_error(c, E_TYPE, "option lines must be a string like \"10-40\", got %s", v_typename(v));
    return false;
  }
  Str s = v.u.s;
  long long a = 0, b = 0;
  int dash = s_findz(s, "-", 0);
  if (dash < 0) {
    if (!s_is_int(s, &a) || a < 0) {
      set_error(c, E_BAD_INPUT, "lines=\"%s\" is not a range like \"10-40\"", s.p);
      return false;
    }
    *from = a ? (int)a : 1;
    *to = a ? (int)a : 1;
    if (!a) { set_error(c, E_BAD_INPUT, "lines=\"%s\": line 0 does not exist", s.p); return false; }
    return true;
  }
  if (dash == 0) {                                    /* "-40": the first 40 lines */
    Str tail = s_trim(ctx_arena(c), s_slice(s, 1, s.len));
    if (!s_is_int(tail, &b) || b < 0) {
      set_error(c, E_BAD_INPUT, "lines=\"%s\" is not a range like \"-40\"", s.p);
      return false;
    }
    *to = (int)b;
    return true;
  }
  Str x1 = s_trim(ctx_arena(c), s_slice(s, 0, dash)), x2 = s_trim(ctx_arena(c), s_slice(s, dash + 1, s.len));
  if (!s_is_int(x1, &a) || a < 1) {
    set_error(c, E_BAD_INPUT, "lines=\"%s\" must start at a line >= 1", s.p);
    return false;
  }
  *from = (int)a;
  if (x2.len == 0) { *to = 0; return true; }          /* "10-": to the end of file */
  if (!s_is_int(x2, &b) || b < a) {
    set_error(c, E_BAD_INPUT, "lines=\"%s\" must be \"from-to\" with to >= from >= 1", s.p);
    return false;
  }
  *to = (int)b;
  return true;
}

/* ===================================================================== */
/* reads                                                                 */
/* ===================================================================== */

static V b_cwd(Ctx *c, V *args, int nargs) {
  (void)args; (void)nargs;
  Arena *a = ctx_arena(c);
  char buf[4096];
  if (!getcwd(buf, sizeof buf)) {
    set_error(c, E_IO, "cwd: cannot read the current directory - %s", err_hint(E_IO));
    return VN;
  }
  return v_str(s_lit(a, buf));
}

static V b_exists(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  Str p = arg_str(&x, 0, "path");
  if (arg_has_err(&x)) return VN;
  Str rp = norm_read(ctx_arena(c), s_z(p));
  return v_bool(rp.len > 0 && fs_exists(rp.p));
}

static V b_size(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  Str p = arg_str(&x, 0, "path");
  if (arg_has_err(&x)) return VN;
  Arena *a = ctx_arena(c);
  Str rp = norm_read(a, s_z(p));
  if (!rp.len) { set_error(a ? c : c, E_BAD_INPUT, "size: path is empty - %s", err_hint(E_BAD_INPUT)); return VN; }
  if (!fs_exists(rp.p)) { fail_e(c, E_NOENT, "size", s_z(p)); return VN; }
  if (fs_is_dir(rp.p)) { fail_e(c, E_ISDIR, "size", s_z(p)); return VN; }
  long n = fs_size(rp.p);
  if (n < 0) { fail_e(c, E_IO, "size", s_z(p)); return VN; }
  return v_num((double)n);
}

static V b_hash(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  Str p = arg_str(&x, 0, "path");
  if (arg_has_err(&x)) return VN;
  Arena *a = ctx_arena(c);
  Str rp = norm_read(a, s_z(p));
  if (!rp.len) { set_error(c, E_BAD_INPUT, "hash: path is empty - %s", err_hint(E_BAD_INPUT)); return VN; }
  ErrCode e = E_NONE;
  Str h = fs_hash_file(a, rp.p, &e);
  if (e != E_NONE || !h.len || !is_hexstr(h.p, h.len)) {
    fail_e(c, e != E_NONE ? e : E_IO, "hash", s_z(p));
    return VN;
  }
  return v_str(h);
}

static V b_read(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  Str p = arg_str(&x, 0, "path");
  if (arg_has_err(&x)) return VN;
  Arena *a = ctx_arena(c);
  int from = 0, to = 0, cap = FS_DEF_READ_BYTES, ctxl = 0;
  if (!parse_lines(c, &x, 1, &from, &to)) return VN;
  if (!budget(&x, 1, FS_DEF_READ_BYTES, &cap)) return VN;
  Str grep;
  if (!o_str(&x, 1, "grep", "", &grep)) return VN;
  if (!o_nonneg(&x, 1, "ctx", 0, &ctxl)) return VN;

  /* cursor walks a big file in max_bytes-sized pages. It composes with `lines`
   * rather than duplicating it, and it must not be mixed with an explicit range
   * because then "what I have already seen" stops being well defined. */
  int cur = arg_opt_int(&x, 1, "cursor", 0);
  if (arg_has_err(&x)) return VN;
  if (cur < 0) { set_error(c, E_BAD_INPUT, "cursor must be >= 0, got %d", cur); return VN; }
  if (cur > 0) {
    if (from > 0 || to > 0) {
      set_error(c, E_BAD_INPUT, "cursor cannot be combined with lines (a cursor IS a line position): use one or the other");
      return VN;
    }
    from = cur; to = 0;
  }
  Str rp = norm_read(a, s_z(p));
  if (!rp.len) { set_error(c, E_BAD_INPUT, "read: path is empty - %s", err_hint(E_BAD_INPUT)); return VN; }
  ErrCode e = E_NONE;
  bool anchors = o_bool(&x, 1, "anchors", ctx_anchors(c));
  V r = fs_read_range(c, rp.p, from, to, grep.len ? grep.p : NULL, anchors, ctxl, cap, &e);
  if (e != E_NONE) { fail_e(c, e, "read", s_z(p)); return VN; }
  if (r.t != V_REC) { set_error(c, E_IO, "read: no result for '%s'", s_z(p)); return VN; }
  /* echo what the agent typed: absolute paths cost tokens and prove nothing */
  rec_setz(a, r.u.r, "path", v_str(s_lit(a, s_z(p))));
  V *sh = rec_getz(r.u.r, "shown");
  /* fs_read_range treats from=0 as "start at line 1", so the next unwritten
   * line is one past the last shown one in 1-based terms - a cursor that
   * repeated or skipped a line here would silently corrupt every paginated read */
  int lo0 = from > 0 ? from - 1 : 0;
  int next = lo0 + (sh && sh->t == V_NUM ? (int)sh->u.n : 0) + 1;
  /* next start line, valid whether or not the page was clipped: the agent can
   * keep reading until cursor >= total instead of having to infer it */
  rec_setz(a, r.u.r, "cursor", v_num((double)next));
  return r;
}

/* ---------- fs.check: cheap file sanity, run before and after an edit ---------- */
static V b_check(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  Str p = arg_str(&x, 0, "path");
  if (arg_has_err(&x)) return VN;
  Arena *a = ctx_arena(c);
  Str rp = norm_read(a, s_z(p));
  if (!rp.len) { set_error(c, E_BAD_INPUT, "check: path is empty - %s", err_hint(E_BAD_INPUT)); return VN; }
  size_t n = 0;
  ErrCode e = E_NONE;
  char *d = fs_slurp(a, rp.p, &n, &e);
  if (!d) { fail_e(c, e != E_NONE ? e : E_IO, "check", s_z(p)); return VN; }
  int lf = 0, crlf = 0, lone = 0, tabs = 0, spaces = 0, lines = 0;
  bool ends_nl = false;
  bool bom = n >= 3 && (unsigned char)d[0] == 0xEF && (unsigned char)d[1] == 0xBB
             && (unsigned char)d[2] == 0xBF;
  for (size_t i = 0; i < n; i++) {
    char ch = d[i];
    if (ch == '\t') tabs++;
    else if (ch == ' ') spaces++;
    else if (ch == '\n') { lf++; lines++; ends_nl = true; }
    else if (ch == '\r') {
      if (i + 1 < n && d[i + 1] == '\n') { crlf++; i++; lines++; ends_nl = true; }
      else { lone++; lines++; ends_nl = false; }              /* old-Mac break */
    } else ends_nl = false;
  }
  const char *eol = (lf && (crlf || lone)) ? "mixed" : crlf ? "crlf" : lone ? "cr"
                    : lf ? "lf" : "none";
  /* a file does not have to end in a line break; the bytes after the last one
   * are still a line, and reporting 0 for "no break" is how an agent loses it */
  if (n && !ends_nl) lines++;
  ErrCode he = E_NONE;
  Str h = fs_hash_file(a, rp.p, &he);
  return v_ok(a, 9,
    "path", v_str(s_lit(a, s_z(p))),
    "hash", v_str(h.len ? h : s_lit(a, "-")),
    "bytes", v_num((double)n),
    "lines", v_num((double)lines),
    "eol", v_str(s_lit(a, eol)),
    "bom", bom ? VT : VF,
    "tabs", v_num((double)tabs),
    "spaces", v_num((double)spaces),
    "ends_newline", ends_nl ? VT : VF);
}

/* ---------- fs.ls / fs.glob ---------- */
/* hash is deliberately NOT a default column: it reads every file it lists. */
static const char *const LS_ORDER[] = { "path", "bytes", "lines", "hash" };
static const int LS_NFIELDS = 4;

static bool field_wanted(Str *req, int nreq, const char *name) {
  if (nreq == 0) return strcmp(name, "hash") != 0;
  for (int i = 0; i < nreq; i++) if (s_eqz(req[i], name)) return true;
  return false;
}

/* fields:"path,hash" or ["path","hash"]; an unknown name is E_BAD_INPUT, because
 * a silently ignored field reads to an agent as "this tree has no hash column". */
static bool ls_fields(Args *x, Str *out, int *nout) {
  *nout = 0;
  V v = optv(x, 1, "fields", VN);
  if (v.t == V_NULL) return true;
  Arena *a = ctx_arena(x->c);
  Str tmp[FS_FIELD_MAX];
  int n = 0;
  if (v.t == V_LIST) {
    List *l = v.u.l;
    for (int i = 0; i < l->len; i++) {
      if (l->v[i].t != V_STR) {
        set_error(x->c, E_TYPE, "fields[%d] must be a string, got %s", i, v_typename(l->v[i]));
        return false;
      }
      if (n >= FS_FIELD_MAX) { set_error(x->c, E_RANGE, "fields: at most %d names", FS_FIELD_MAX); return false; }
      tmp[n++] = l->v[i].u.s;
    }
  } else if (v.t == V_STR) {
    Str s = v.u.s;
    int start = 0;
    for (int i = 0; i <= s.len; i++) {
      if (i == s.len || s.p[i] == ',') {
        Str tok = s_trim(a, s_slice(s, start, i));
        if (tok.len) {
          if (n >= FS_FIELD_MAX) { set_error(x->c, E_RANGE, "fields: at most %d names", FS_FIELD_MAX); return false; }
          tmp[n++] = tok;
        }
        start = i + 1;
      }
    }
  } else {
    set_error(x->c, E_TYPE, "fields must be \"path,bytes\" or [\"path\",\"bytes\"], got %s",
              v_typename(v));
    return false;
  }
  for (int i = 0; i < n; i++) {
    bool known = false;
    for (int k = 0; k < LS_NFIELDS; k++) if (s_eqz(tmp[i], LS_ORDER[k])) known = true;
    if (!known) {
      set_error(x->c, E_BAD_INPUT, "fields: unknown field \"%s\" - allowed: path,bytes,lines,hash",
                tmp[i].p);
      return false;
    }
  }
  for (int i = 0; i < n; i++) out[i] = tmp[i];
  *nout = n;
  return true;
}

static V b_ls(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  Str dir = arg_str(&x, 0, "dir");
  if (arg_has_err(&x)) return VN;
  Arena *a = ctx_arena(c);
  Str gpat;
  int max = FS_DEF_LS_MAX;
  if (!o_str(&x, 1, "glob", "", &gpat)) return VN;
  if (!o_nonneg(&x, 1, "max", FS_DEF_LS_MAX, &max)) return VN;
  bool rec = o_bool(&x, 1, "recursive", false);
  Str req[FS_FIELD_MAX];
  int nreq = 0;
  if (!ls_fields(&x, req, &nreq)) return VN;

  /* the listing is built from the directory string as asked, so rows read
   * "src/util.c" and not "/abs/.../src/util.c": an absolute listing costs the
   * agent tokens to say nothing new (fs.glob keeps its root-relative stripping,
   * which needs the normalized root to work). */
  Str dp = dir.len ? s_lit(a, s_z(dir)) : s_lit(a, ".");
  ErrCode e = E_NONE;
  /* ask for one row more than max: "hit the limit" and "exactly max files" are
   * different facts, and an agent that cannot tell them apart re-lists forever */
  V all = fs_ls(c, dp.p, gpat.len ? gpat.p : NULL, rec, max + 1, &e);
  if (e != E_NONE) { set_error(c, e, "ls: '%s' - %s", s_z(dir), err_hint(e)); return VN; }
  if (all.t != V_LIST || !all.u.l) { set_error(c, E_IO, "ls: no listing for '%s'", s_z(dir)); return VN; }
  int cnt = all.u.l->len;
  if (cnt > max) cnt = max;
  List *out = list_new(a);
  for (int i = 0; i < cnt; i++) {
    V *row = &all.u.l->v[i];
    if (row->t != V_REC) { list_push(a, out, *row); continue; }
    Rec *src = row->u.r;
    Rec *dst = rec_new(a);
    for (int k = 0; k < LS_NFIELDS; k++) {
      const char *f = LS_ORDER[k];
      if (!field_wanted(req, nreq, f)) continue;
      if (f[0] == 'h') {                                /* pay for hashing on purpose */
        ErrCode he = E_NONE;
        V *pv = rec_getz(src, "path");
        Str h = pv ? fs_hash_file(a, pv->u.s.p, &he) : s_null();
        rec_setz(a, dst, "hash", v_str(h.len && is_hexstr(h.p, h.len) ? h : s_lit(a, "-")));
      } else {
        V *val = rec_getz(src, f);
        if (val) rec_setz(a, dst, f, *val);
      }
    }
    list_push(a, out, rec_to_v(dst));
  }
  return list_of(out);
}

static V b_glob(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  Str pat = arg_str(&x, 0, "pattern");
  if (arg_has_err(&x)) return VN;
  Arena *a = ctx_arena(c);
  Str root = s_lit(a, ".");
  int max = FS_DEF_GLOB_MAX;
  if (arg_present(&x, 1)) {
    if (x.a[1].t == V_STR) {                            /* fs.glob(pat, "root") - SPEC 4.1 */
      if (x.a[1].u.s.len) {
        root = norm_read(a, s_z(x.a[1].u.s));
        if (!root.len) root = s_lit(a, ".");
      }
    } else if (x.a[1].t == V_REC) {
      Str r;
      if (!o_str(&x, 1, "root", ".", &r)) return VN;
      if (r.len) { root = norm_read(a, s_z(r)); if (!root.len) root = s_lit(a, "."); }
      if (!o_nonneg(&x, 1, "max", FS_DEF_GLOB_MAX, &max)) return VN;
    } else {
      set_error(c, E_TYPE, "glob: the second argument must be {root,max} or a root string, got %s",
                v_typename(x.a[1]));
      return VN;
    }
  }
  ErrCode e = E_NONE;
  V r = fs_glob(c, s_z(pat), root.p, max, &e);
  if (e != E_NONE) { set_error(c, e, "glob: '%s' in '%s' - %s", s_z(pat), root.p, err_hint(e)); return VN; }
  return r;
}

static V b_outline(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  Str p = arg_str(&x, 0, "path");
  if (arg_has_err(&x)) return VN;
  Arena *a = ctx_arena(c);
  Str lang;
  if (!o_str(&x, 1, "lang", "", &lang)) return VN;
  Str rp = norm_read(a, s_z(p));
  if (!rp.len) { set_error(c, E_BAD_INPUT, "outline: path is empty - %s", err_hint(E_BAD_INPUT)); return VN; }
  size_t n = 0;
  ErrCode e = E_NONE;
  char *d = fs_slurp(a, rp.p, &n, &e);
  if (!d) { fail_e(c, e != E_NONE ? e : E_IO, "outline", s_z(p)); return VN; }
  V r = outline_file(c, rp.p, s_wrap(d), lang.len ? lang.p : NULL);
  if (r.t == V_REC) rec_setz(a, r.u.r, "path", v_str(s_lit(a, s_z(p))));
  return r;
}

static V b_bundle(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  List *in = arg_list(&x, 0, "paths");
  if (arg_has_err(&x)) return VN;
  Str query;
  int cap = FS_DEF_BUNDLE_BYTES;
  if (!o_str(&x, 1, "query", "", &query)) return VN;
  if (!budget(&x, 1, FS_DEF_BUNDLE_BYTES, &cap)) return VN;
  bool only_sym = o_bool(&x, 1, "only_symbols", false);
  Str only;
  if (!o_str(&x, 1, "only", "", &only)) return VN;
  if (s_eqz(only, "symbols")) only_sym = true;          /* SPEC 4.1 spelling */

  /* validated here, passed through unchanged: the bundle echoes the paths the
   * agent asked for, not the absolute names we happened to resolve */
  for (int i = 0; i < in->len; i++) {
    if (in->v[i].t != V_STR) {
      set_error(c, E_TYPE, "bundle: paths[%d] must be a string, got %s", i, v_typename(in->v[i]));
      return VN;
    }
    if (!in->v[i].u.s.len) { set_error(c, E_BAD_INPUT, "bundle: paths[%d] is an empty path", i); return VN; }
  }
  return bundle_files(c, x.a[0], query.len ? query.p : NULL, cap, only_sym);
}

/* ===================================================================== */
/* plan builders (SPEC §3) - these never touch the disk                  */
/* ===================================================================== */

/* Effective confirm level: the manifest/config policy for fs, overridable per
 * call with {confirm:"none|jail|ask|all"}; "auto" (or absent) keeps the policy. */
static bool confirm_level(Ctx *c, Args *x, int oi, Confirm *io) {
  *io = policy_for(c, "policies.fs");
  Str s;
  if (!o_str(x, oi, "confirm", "auto", &s)) return false;
  if (s.len == 0 || s_eqz(s, "auto")) return true;
  bool ok = false;
  Confirm cf = confirm_from_str(s.p, &ok);
  if (!ok) {
    set_error(c, E_BAD_INPUT, "confirm=\"%s\" is not one of none,jail,ask,all", s.p);
    return false;
  }
  *io = cf;
  return true;
}

/* Absolute, jailed target of a write; every mutating builtin starts here so the
 * same code (OUTSIDE_JAIL, exit 4) answers for all of them. */
static Str jail_path(Ctx *c, Arena *a, const char *op, Str path, ErrCode *io) {
  ErrCode e = E_NONE;
  Str abs = ws_resolve(a, c, s_z(path), &e);
  if (!abs.len) { fail_e(c, e != E_NONE ? e : E_BAD_INPUT, op, s_z(path)); *io = e; return s_null(); }
  *io = E_NONE;
  return abs;
}

/* create/modify plus `from` = current content hash is plan_target()'s job;
 * `to` = planned content hash is plan_set_payload()'s job. The effective
 * confirm level goes into the plan's arg fingerprint, so a token signs exactly
 * one policy: changing policies.fs invalidates tokens handed out before it. */
static Plan *build_write_plan(Ctx *c, Args *x, int oi, const char *op, Str path,
                              Str text, bool append, Confirm *need_out) {
  Arena *a = ctx_arena(c);
  Confirm need = CF_JAIL;
  if (!confirm_level(c, x, oi, &need)) return NULL;
  ErrCode je = E_NONE;
  Str abs = jail_path(c, a, op, path, &je);
  if (!abs.len) return NULL;
  if (fs_is_dir(abs.p)) { fail_e(c, E_ISDIR, op, s_z(path)); return NULL; }

  const char *full = text.p;
  size_t fn = (size_t)text.len;
  if (append) {
    /* fs.append plans the WHOLE resulting file instead of the delta, because
     * SPEC §3.1 defines `to` as the hash of the file AFTER the operation and
     * plan_set_payload() can only hash what it is handed. Apply therefore
     * replaces the file with old+new - the same bytes, and a `to` an agent can
     * verify. Concurrency is still covered: `from` is the current hash, so a
     * write between plan and apply fails STALE_PLAN instead of losing data. */
    size_t on = 0;
    ErrCode se = E_NONE;
    char *old = fs_slurp(a, abs.p, &on, &se);
    if (!old && se != E_NOENT) { fail_e(c, se != E_NONE ? se : E_IO, op, s_z(path)); return NULL; }
    if (!old) on = 0;
    char *cat = (char*)arena_alloc(a, on + fn + 1);
    if (old && on) memcpy(cat, old, on);
    if (fn) memcpy(cat + (old ? on : 0), text.p, fn);
    cat[(old ? on : 0) + fn] = 0;
    full = cat;
    fn = (old ? on : 0) + fn;
  }
  Plan *p = plan_target(c, op, need, PK_WRITE, abs.p);
  if (!p) return NULL;
  plan_set_payload(c, p, full, fn);
  plan_add_arg(c, p, "confirm", cf_name(need));
  if (need_out) *need_out = need;
  return p;
}

/* fs.delete: kind delete, always confirmed.
 * plan_target() only ever labels a target create|modify, so the truth is
 * restored by plan_add_file() here; the operation kind (PK_DELETE) is set by
 * plan_target(), which is the only public way to set it (see the report about a
 * plan_set_kind()/target-kind setter). */
static Plan *build_delete_plan(Ctx *c, Args *x, int oi, Str path, Confirm *need_out) {
  Arena *a = ctx_arena(c);
  Confirm need = CF_JAIL;
  if (!confirm_level(c, x, oi, &need)) return NULL;
  /* SPEC §3.3: deleting replaces content that exists - it can never be free, so
   * even {confirm:"none"} cannot silence a delete. */
  if (need < CF_ASK) need = CF_ASK;
  ErrCode je = E_NONE;
  Str abs = jail_path(c, a, "fs.delete", path, &je);
  if (!abs.len) return NULL;
  if (!fs_exists(abs.p)) { fail_e(c, E_NOENT, "fs.delete", s_z(path)); return NULL; }
  if (fs_is_dir(abs.p)) { fail_e(c, E_ISDIR, "fs.delete", s_z(path)); return NULL; }
  Plan *p = plan_target(c, "fs.delete", need, PK_DELETE, abs.p);
  if (!p) return NULL;
  plan_add_arg(c, p, "confirm", cf_name(need));
  if (need_out) *need_out = need;
  return p;
}

static V b_write(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str path = arg_str(&x, 0, "path");
  Str text = arg_str(&x, 1, "text");
  if (arg_has_err(&x)) return VN;
  Plan *p = build_write_plan(c, &x, 2, "fs.write", path, text, false, NULL);
  return p ? v_plan(p) : VN;
}

static V b_append(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str path = arg_str(&x, 0, "path");
  Str text = arg_str(&x, 1, "text");
  if (arg_has_err(&x)) return VN;
  Plan *p = build_write_plan(c, &x, 2, "fs.append", path, text, true, NULL);
  return p ? v_plan(p) : VN;
}

static V b_delete(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  Str path = arg_str(&x, 0, "path");
  if (arg_has_err(&x)) return VN;
  Plan *p = build_delete_plan(c, &x, 1, path, NULL);
  return p ? v_plan(p) : VN;
}

static V b_move(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str from = arg_str(&x, 0, "from");
  Str to = arg_str(&x, 1, "to");
  if (arg_has_err(&x)) return VN;
  Arena *a = ctx_arena(c);
  Confirm need = CF_JAIL;
  if (!confirm_level(c, &x, 2, &need)) return VN;
  ErrCode je = E_NONE;
  Str abs = jail_path(c, a, "fs.move", from, &je);
  if (!abs.len) return VN;
  if (!fs_exists(abs.p)) { fail_e(c, E_NOENT, "fs.move", s_z(from)); return VN; }
  Plan *p = plan_target(c, "fs.move", need, PK_MOVE, abs.p);
  if (!p) return VN;
  plan_set_move(c, p, s_z(to));
  if (ctx_has_err(c)) return VN;                          /* destination outside the jail */
  plan_add_arg(c, p, "confirm", cf_name(need));
  return v_plan(p);
}

/* ---------- fs.restore ----------
 * Trash rows are written by fsx.c as
 *   {"seq":N,"path":"<original>","stored":"<trash file>","bytes":M}
 * Neither Plan.seq (no public setter in plan.c) nor a fsx lookup is available,
 * so the index is read here - the format belongs to the core, and a restore is
 * planned as the move it actually is: stored -> original. */
static bool trash_lookup(Ctx *c, long seq, char *orig, size_t on, char *stored, size_t sn,
                         ErrCode *err) {
  Arena *a = ctx_arena(c);
  *err = E_NONE;
  Str idxp = s_fmt(a, "%s/index.ndjson", ws_trash(c));
  FILE *f = fopen(idxp.p, "rb");
  if (!f) { *err = E_NOENT; return false; }
  char line[4096];
  bool hit = false;
  while (fgets(line, sizeof line, f)) {
    char *ps = strstr(line, "\"seq\":");
    if (!ps || atol(ps + 6) != seq) continue;
    char *po = strstr(line, "\"path\":\""), *pd = strstr(line, "\"stored\":\"");
    if (!po || !pd) continue;
    po += 8; pd += 10;
    char *e1 = strchr(po, '"'), *e2 = strchr(pd, '"');
    if (!e1 || !e2) continue;
    size_t n1 = (size_t)(e1 - po), n2 = (size_t)(e2 - pd);
    if (n1 >= on || n2 >= sn) continue;
    memcpy(orig, po, n1); orig[n1] = 0;
    memcpy(stored, pd, n2); stored[n2] = 0;
    hit = true;
  }
  fclose(f);
  if (!hit) *err = E_NOENT;
  return hit;
}

static V b_restore(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  double dseq = arg_num(&x, 0, "seq");
  if (arg_has_err(&x)) return VN;
  long seq = (long)dseq;
  if (seq <= 0) {
    set_error(c, E_BAD_INPUT, "restore: seq must be the positive number from an fs.delete receipt");
    return VN;
  }
  Confirm need = CF_JAIL;
  if (!confirm_level(c, &x, 1, &need)) return VN;
  if (need < CF_ASK) need = CF_ASK;                       /* writes back over a path */
  char orig[2048] = "", stored[2048] = "";
  ErrCode e = E_NONE;
  if (!trash_lookup(c, seq, orig, sizeof orig, stored, sizeof stored, &e)) {
    set_error(c, e, "restore: no trash entry with seq=%ld - %s", seq, err_hint(e));
    return VN;
  }
  if (!fs_exists(stored)) {
    set_error(c, E_NOENT, "restore: trash file is gone: %s - %s", stored, err_hint(E_NOENT));
    return VN;
  }
  Plan *p = plan_target(c, "fs.restore", need, PK_MOVE, stored);
  if (!p) return VN;
  plan_set_move(c, p, orig);
  if (ctx_has_err(c)) return VN;
  plan_add_arg(c, p, "confirm", cf_name(need));
  plan_add_arg(c, p, "seq", s_fmt(ctx_arena(c), "%ld", seq).p);
  return v_plan(p);
}

/* ---------- fs.plan: the dry run `vxa plan` prints ----------
 * Same builder, same token, no execution: an agent (or the CLI) reads the whole
 * impact - which files, which kind, old hash, planned hash - and can decide
 * before anything is signed. */
static V b_plan(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  (void)nargs;
  Str path = arg_str(&x, 0, "path");
  if (arg_has_err(&x)) return VN;
  Arena *a = ctx_arena(c);
  Str mode, text, dest;
  if (!o_str(&x, 1, "mode", "write", &mode)) return VN;
  if (!o_str(&x, 1, "text", "", &text)) return VN;
  if (!o_str(&x, 1, "to", "", &dest)) return VN;

  Plan *p = NULL;
  const char *op = s_z(mode);
  bool say_delete = false;
  if (s_eqz(mode, "write")) {
    p = build_write_plan(c, &x, 1, "fs.write", path, text, false, NULL);
  } else if (s_eqz(mode, "append")) {
    p = build_write_plan(c, &x, 1, "fs.append", path, text, true, NULL);
  } else if (s_eqz(mode, "delete")) {
    p = build_delete_plan(c, &x, 1, path, NULL);
    say_delete = true;
  } else if (s_eqz(mode, "move")) {
    if (!dest.len) {
      set_error(c, E_BAD_INPUT, "plan: mode=\"move\" needs {to:\"destination\"}");
      return VN;
    }
    Confirm need = CF_JAIL;
    if (!confirm_level(c, &x, 1, &need)) return VN;
    ErrCode je = E_NONE;
    Str abs = jail_path(c, a, "fs.plan", path, &je);
    if (!abs.len) return VN;
    if (!fs_exists(abs.p)) { fail_e(c, E_NOENT, "fs.plan", s_z(path)); return VN; }
    p = plan_target(c, "fs.move", need, PK_MOVE, abs.p);
    if (p) {
      plan_set_move(c, p, dest.p);
      if (ctx_has_err(c)) p = NULL;
      else plan_add_arg(c, p, "confirm", cf_name(need));
    }
  } else {
    set_error(c, E_BAD_INPUT, "plan: mode=\"%s\" is not write, append, delete or move", s_z(mode));
    return VN;
  }
  if (!p) return VN;

  Rec *view = rec_new(a);
  plan_to_v(c, p, view);                                  /* op,confirm,token,files,count */
  V *files = rec_getz(view, "files");
  /* plan_target labels an existing file "modify"; a delete row that says modify
   * would be signed as an edit, so the view states the effect instead. */
  if (say_delete && files && files->t == V_LIST) {
    for (int i = 0; i < files->u.l->len; i++) {
      V *row = &files->u.l->v[i];
      if (row->t == V_REC) rec_setz(a, row->u.r, "kind", v_str(s_lit(a, "delete")));
    }
  }
  Rec *out = rec_new(a);
  rec_setz(a, out, "planned", VT);
  rec_setz(a, out, "path", v_str(s_lit(a, s_z(path))));
  rec_setz(a, out, "mode", v_str(mode));
  rec_setz(a, out, "op", rec_getz(view, "op") ? *rec_getz(view, "op") : v_strz(a, op));
  rec_setz(a, out, "files", files ? *files : list_of(list_new(a)));
  rec_setz(a, out, "count", rec_getz(view, "count") ? *rec_getz(view, "count") : v_num(1));
  rec_setz(a, out, "confirm", rec_getz(view, "confirm") ? *rec_getz(view, "confirm")
                                                        : v_strz(a, cf_name(policy_for(c, "policies.fs"))));
  rec_setz(a, out, "token", rec_getz(view, "token") ? *rec_getz(view, "token") : v_strz(a, "-"));
  return rec_to_v(out);
}

/* ===================================================================== */
/* registry: code + signature + doc + example in one row (lib.h)         */
/* ===================================================================== */

static const Builtin TABLE[] = {
  { "fs", "cwd", b_cwd, 0, 0,
    "fs.cwd() -> str",
    "The directory the agent is working in; tells workspace paths from relative ones.",
    "fs.cwd()", BF_PURE },

  { "fs", "exists", b_exists, 1, 1,
    "fs.exists(path) -> t|f",
    "Cheapest answer to \"is this path there\"; reads are not jailed, so this works outside the root too.",
    "fs.exists(\"src/util.c\")", BF_PURE },

  { "fs", "hash", b_hash, 1, 1,
    "fs.hash(path) -> str:12hex | ERR NOENT|ISDIR",
    "Content fingerprint; compare it to a plan's from/to to prove a file is the one you think it is.",
    "fs.hash(\"src/util.c\")", BF_PURE },

  { "fs", "size", b_size, 1, 1,
    "fs.size(path) -> num bytes | ERR NOENT|ISDIR",
    "Byte length without reading the file; decide whether a read is worth the tokens before doing it.",
    "fs.size(\"SPEC.md\")", BF_PURE },

  { "fs", "read", b_read, 1, 2,
    "fs.read(path, {lines:\"10-40\", grep:\"foo\", ctx:0, anchors:false, max_bytes:8000, max_tok:2000}) -> {path,hash,lines,total,bytes,shown,truncated,est,text}",
    "Read a page, not a file: every answer says how many lines exist and how many you were shown.",
    "fs.read(\"src/util.c\", {lines:\"1-40\"})", BF_PURE },

  { "fs", "check", b_check, 1, 1,
    "fs.check(path) -> {path,hash,bytes,lines,eol:lf|crlf|cr|mixed|none,bom,tabs,spaces,ends_newline}",
    "Encoding and line-ending fingerprint; run before and after an edit to prove nothing was mangled.",
    "fs.check(\"src/util.c\")", BF_PURE },

  { "fs", "ls", b_ls, 1, 2,
    "fs.ls(dir, {glob:\"*.c\", recursive:false, max:200, fields:\"path,bytes,lines\"}) -> [{path,bytes,lines,hash}]",
    "Sorted listing with only the columns you asked for; hash is opt-in because it reads every file listed.",
    "fs.ls(\"src\", {glob:\"*.c\"})", BF_PURE },

  { "fs", "glob", b_glob, 1, 2,
    "fs.glob(pattern, {root:\".\", max:200}) -> [str] relative to root",
    "Name-only discovery: one ** pattern returns paths, no bytes and no stat columns.",
    "fs.glob(\"**/*.h\", {root:\"src\"})", BF_PURE },

  { "fs", "outline", b_outline, 1, 2,
    "fs.outline(path, {lang:\"c\"}) -> {path,lang,bytes,lines,hash,symbols}",
    "Skeleton instead of the whole file: symbols with line and hash, one call per file.",
    "fs.outline(\"src/util.c\")", BF_PURE },

  { "fs", "bundle", b_bundle, 1, 2,
    "fs.bundle([paths], {query:\"parse\", max_bytes:4000, max_tok:1000, only_symbols:false}) -> {bundle,dropped,bytes,cap,query,tok}",
    "Many files in one answer, ranked by the query and cut at the byte cap; dropped says what did not fit.",
    "fs.bundle([\"src/util.c\",\"src/val.c\"], {query:\"arena\"})", BF_PURE },

  { "fs", "write", b_write, 2, 3,
    "fs.write(path, text, {confirm:\"auto|none|jail|ask|all\"}) -> PLAN{op,confirm,token,files:[{path,kind,from,to,bytes}]}",
    "Returns the plan to sign, never the write. New file inside the root applies at once; replacing needs --confirm TOKEN.",
    "fs.write(\"a.txt\", \"hello\").auto.apply", 0 },

  { "fs", "append", b_append, 2, 3,
    "fs.append(path, text, {confirm:\"auto\"}) -> PLAN",
    "Add to the end; the plan's to hash is the whole resulting file, so you verify the end state before signing.",
    "fs.append(\"a.txt\", \"more\\n\")", 0 },

  { "fs", "delete", b_delete, 1, 2,
    "fs.delete(path, {confirm:\"auto\"}) -> PLAN{files:[{path,kind:delete,from,bytes}],token}",
    "Never unlinks: the file moves to .vxa/trash and stays restorable, so this always needs a token.",
    "fs.delete(\"a.txt\")", 0 },

  { "fs", "move", b_move, 2, 3,
    "fs.move(from, to, {confirm:\"auto\"}) -> PLAN",
    "Rename shown as impact: the plan lists the source it leaves and the destination it may overwrite.",
    "fs.move(\"a.txt\", \"b.txt\")", 0 },

  { "fs", "restore", b_restore, 1, 2,
    "fs.restore(seq, {confirm:\"auto\"}) -> PLAN",
    "Undo an fs.delete by the seq in its receipt; the plan moves the trash copy back to its original path.",
    "fs.restore(1719000000)", 0 },

  { "fs", "plan", b_plan, 1, 2,
    "fs.plan(path, {text:\"\", mode:\"write|append|delete|move\", to:\"dst\", confirm:\"auto\"}) -> {planned,op,confirm,token,files,count}",
    "Dry run: the exact files and token a mutating call would produce, without touching the disk.",
    "fs.plan(\"a.txt\", {text:\"hello\"})", BF_PURE },

};

const Builtin *t_fs(int *n) {
  if (n) *n = (int)(sizeof(TABLE) / sizeof(TABLE[0]));
  return TABLE;
}
