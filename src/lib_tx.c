/* lib_tx.c - the tx namespace (text, anchors, patch, diff) and method dispatch.
 *
 * Two jobs in one file because they are the same code seen from two ends:
 *   - tx.* builtins are what an agent CALLS by name (and what `vxa doc tx` shows)
 *   - methods (s.trim, l.map(f), r.keys) are those SAME operations reached with
 *     dot syntax: lib_method() prepends `self` to the argument vector, so most
 *     method rows point straight at the builtin below them. No body twice.
 *
 * Anchors, patch, diff and similarity are DELEGATION ONLY: xdiff.c owns the
 * anchor/patch algorithm and sim.c owns ~=. Nothing here recomputes either.
 *
 * Rules obeyed (SPEC 1, 2, 4.2):
 *   - never abort, never write to stdout: failure is set_error(...) + VN.
 *   - never guess (axiom 1): a wrong type, an unknown sort field, a comparator
 *     function or an empty needle is an ERROR, not a silent default.
 *   - deterministic (axiom 4): records keep insertion order, lists keep order,
 *     and sort is STABLE, so equal keys never reorder between runs.
 *   - VXA has no regex engine. Every search here is a literal byte scan; the
 *     docs say so rather than implying a pattern language.
 */
#include "vxa.h"
#include "interp.h"
#include "lib.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* =============================================================== helpers == */

/* NUL-terminated copy for the fs_* / xdiff entry points that take char*. */
static char *cz(Arena *a, Str s) {
  if (!s.p || s.len <= 0) return (char*)arena_zalloc(a, 1);
  return arena_strndup(a, s.p, (size_t)s.len);
}

static Str dup_or_empty(Arena *a, Str s) {
  return s_from(a, s.p ? s.p : "", s.len > 0 ? (size_t)s.len : 0);
}

/* ASCII-only case fold: locale-free, so no UTF-8 lead byte is ever rewritten. */
static char fold(char ch) { return (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; }

/* case-insensitive LITERAL search (no regex in VXA); -1 when absent. */
static int ni_at(Str hay, Str nd, int from) {
  if (!hay.p || from < 0) from = 0;
  if (!nd.p || nd.len == 0 || nd.len > hay.len) return -1;
  for (int i = from; i + nd.len <= hay.len; i++) {
    int ok = 1;
    for (int j = 0; j < nd.len; j++)
      if (fold(hay.p[i + j]) != fold(nd.p[j])) { ok = 0; break; }
    if (ok) return i;
  }
  return -1;
}

static int cp_w(const char *p, int avail) {
  unsigned char ch = (unsigned char)p[0];
  int w = 1;
  if (ch >= 0xF0) w = 4; else if (ch >= 0xE0) w = 3; else if (ch >= 0xC0) w = 2;
  return (w > avail) ? 1 : w;
}

static V v_listv(List *l) { return list_of(l); }
static int norm_idx(int i, int len) { return i < 0 ? i + len : i; }

/* An options argument, when supplied, must be a record: a bare number where
 * {...} was expected is a mistake to report, not a default to shrug at. */
static bool opts_bad(Args *x, int i) {
  if (ctx_has_err(x->c)) return true;
  if (!arg_present(x, i)) return false;
  V v = x->a[i];
  if (v.t == V_REC) return false;
  set_error(x->c, E_TYPE, "argument %d must be an options record {...}, got %s",
            i + 1, v_typename(v));
  return true;
}

/* ================================================= tx: anchors / patch ==== */

/* tx.anchors(path_or_text, {path:"a.c"}) -> [{n,at,text}]
 * The same table fs.read({anchors:true}) prints as an `L12:c3f2|` prefix. When
 * arg 0 names an existing file it is read first, so anchoring a file costs one
 * call instead of a read plus an anchor pass. */
static V b_anchors(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Str in = arg_str(&x, 0, "path_or_text");
  if (arg_has_err(&x)) return VN;
  if (opts_bad(&x, 1)) return VN;
  Str popt = arg_opt_str(&x, 1, "path", "-");
  if (arg_has_err(&x)) return VN;

  Str text = in;
  const char *label = cz(a, popt);
  /* A short argument that names a real file is read, so anchoring a file costs
   * one call; anything else is treated as the text itself, never as a typo. */
  if (in.len > 0 && in.len <= 1024) {
    char *p = cz(a, in);
    if (fs_exists(p) && !fs_is_dir(p)) {
      label = p;
      size_t n = 0;
      ErrCode fe = E_NONE;
      char *data = fs_slurp(a, p, &n, &fe);
      if (!data) {
        set_error(c, fe == E_NONE ? E_IO : fe, "tx.anchors: cannot read %s", p);
        return VN;
      }
      text = s_wrap(data);
    }
  }

  ErrCode e = E_NONE;
  int cnt = 0;
  Anch *A = anchors_of(a, label, text, &cnt, &e);
  if (e != E_NONE) {
    set_error(c, e, "tx.anchors: %s", err_hint(e));
    return VN;
  }
  List *out = list_new(a);
  if (A && cnt > 0) {
    for (int i = 0; i < cnt; i++)
      list_push(a, out, v_ok(a, 3, "n", v_num(A[i].n),
                                        "at", v_str(A[i].at),
                                        "text", v_str(A[i].text)));
  }
  return v_listv(out);
}

/* tx.patch(text, ops, {path:"a.c"}) -> {text,applied,checks}
 * xdiff's record passes through UNTOUCHED: `checks` is the agent's own
 * verification of the edit, so rebuilding it here would void its meaning.
 * Anchor failures come back as ANCHOR_MISS / ANCHOR_DUP from xdiff. */
static V b_patch(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str text = arg_str(&x, 0, "text");
  List *ops = arg_list(&x, 1, "ops");
  if (arg_has_err(&x) || !ops) return VN;
  if (opts_bad(&x, 2)) return VN;
  Str popt = arg_opt_str(&x, 2, "path", "-");
  if (arg_has_err(&x)) return VN;
  return tx_patch_apply(ctx_arena(c), text, cz(ctx_arena(c), popt), x.a[1]);
}

/* tx.diff(a, b, {ctx:3, unified:true}) -> {text,adds,dels,hunks} */
static V b_diff(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str sa = arg_str(&x, 0, "a");
  Str sb = arg_str(&x, 1, "b");
  if (arg_has_err(&x)) return VN;
  if (opts_bad(&x, 2)) return VN;
  int ctxl = arg_opt_int(&x, 2, "ctx", 3);
  bool uni = arg_opt_bool(&x, 2, "unified", true);
  if (arg_has_err(&x)) return VN;
  if (ctxl < 0) {
    set_error(c, E_RANGE, "tx.diff: ctx must be 0 or more, got %d", ctxl);
    return VN;
  }
  return tx_diff(ctx_arena(c), sa, sb, ctxl, uni);
}

/* tx.similar(a, b) -> num 0..1: the function form of ~= (sim.c does the work). */
static V b_similar(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  V va = arg_at(&x, 0);
  V vb = arg_at(&x, 1);
  if (arg_has_err(&x)) return VN;
  Str sa = as_text(c, va), sb = as_text(c, vb);
  return v_num(sim_ratio(sa.p, sa.len, sb.p, sb.len));
}

/* ========================================================== edit / shape == */

/* tx.subst(text, needle, repl, {all:false}) - literal text, never a pattern. */
static V b_subst(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Str s = arg_str(&x, 0, "text");
  Str nd = arg_str(&x, 1, "needle");
  Str rp = arg_str(&x, 2, "repl");
  if (arg_has_err(&x)) return VN;
  if (opts_bad(&x, 3)) return VN;
  bool all = arg_opt_bool(&x, 3, "all", true);
  if (arg_has_err(&x)) return VN;
  if (nd.len == 0) {
    set_error(c, E_BAD_INPUT, "tx.subst: the needle is empty, so there is nothing to replace");
    return VN;
  }
  if (!all) {
    int at = s_find(s, nd, 0);
    if (at < 0) return v_str(dup_or_empty(a, s));
    Str head = s_cut(a, s, 0, at);
    Str tail = s_cut(a, s, at + nd.len, s.len);
    return v_str(s_concat(a, s_concat(a, head, rp), tail));
  }
  return v_str(s_replace(a, s, nd, rp));
}

/* tx.split(s, sep) - an empty sep splits into CODE POINTS, matching .chars. */
static V b_split(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Str s = arg_str(&x, 0, "s");
  Str sep = arg_str(&x, 1, "sep");
  if (arg_has_err(&x)) return VN;
  List *out = list_new(a);
  if (!s.p || s.len == 0) { list_push(a, out, v_str(s_from(a, "", 0))); return v_listv(out); }
  if (sep.len == 0) {
    int i = 0;
    while (i < s.len) {
      int w = cp_w(s.p + i, s.len - i);
      list_push(a, out, v_str(s_from(a, s.p + i, (size_t)w)));
      i += w;
    }
    return v_listv(out);
  }
  int start = 0;
  for (;;) {
    int at = s_find(s, sep, start);
    if (at < 0) break;
    list_push(a, out, v_str(s_cut(a, s, start, at)));
    start = at + sep.len;
  }
  list_push(a, out, v_str(s_cut(a, s, start, s.len)));
  return v_listv(out);
}

/* A joined value must stay readable inside the string it lands in. v_tostr
 * drops a record's outer braces at the TOP level (SPEC 2.1), which is right for
 * output but wrong inside a join: "a=1,b=2" would look like two items. Lists keep
 * their brackets already, so only records need the braces put back. */
static Str joinable(Ctx *c, V v) {
  Arena *a = ctx_arena(c);
  if (v.t == V_REC && v.u.r && v.u.r->len) {
    Str body = v_tostr(a, v, true);
    Buf b;
    buf_init(&b, a);
    buf_putc(&b, '{');
    buf_put(&b, body.p, (size_t)body.len);
    buf_putc(&b, '}');
    return buf_take(&b);
  }
  return as_text(c, v);
}

/* tx.join(list, sep) - non-scalars join as their compact form, so a nested
 * value still yields one predictable single-line string. */
static V b_join(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  List *l = arg_list(&x, 0, "list");
  Str sep = arg_str(&x, 1, "sep");
  if (arg_has_err(&x) || !l) return VN;
  Buf b; buf_init(&b, a);
  for (int i = 0; i < l->len; i++) {
    if (i && sep.len) buf_put(&b, sep.p, (size_t)sep.len);
    Str t = joinable(c, l->v[i]);
    buf_put(&b, t.p, (size_t)t.len);
  }
  return v_str(buf_take(&b));
}

/* tx.lines(s) - splits on LF ONLY. A CR stays glued to the end of its line,
 * which is exactly what makes unlines(lines(s)) byte-identical for CRLF text. */
static V b_lines(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Str s = arg_str(&x, 0, "s");
  if (arg_has_err(&x)) return VN;
  List *out = list_new(a);
  if (!s.p) { list_push(a, out, v_str(s_from(a, "", 0))); return v_listv(out); }
  int start = 0;
  for (int i = 0; i < s.len; i++) {
    if (s.p[i] != '\n') continue;
    list_push(a, out, v_str(s_cut(a, s, start, i)));
    start = i + 1;
  }
  list_push(a, out, v_str(s_cut(a, s, start, s.len)));
  return v_listv(out);
}

/* tx.unlines(list) - the exact inverse: the empty tail piece a terminated
 * string produced re-creates the final newline, and only that piece does. */
static V b_unlines(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  Buf b; buf_init(&b, a);
  for (int i = 0; i < l->len; i++) {
    if (i) buf_putc(&b, '\n');
    Str t = joinable(c, l->v[i]);
    buf_put(&b, t.p, (size_t)t.len);
  }
  return v_str(buf_take(&b));
}

static V b_trim(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str s = arg_str(&x, 0, "s");
  if (arg_has_err(&x)) return VN;
  return v_str(s_trim(ctx_arena(c), s));
}
static V b_lower(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str s = arg_str(&x, 0, "s");
  if (arg_has_err(&x)) return VN;
  return v_str(s_lower(ctx_arena(c), s));
}
static V b_upper(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str s = arg_str(&x, 0, "s");
  if (arg_has_err(&x)) return VN;
  return v_str(s_upper(ctx_arena(c), s));
}
static V b_contains(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str s = arg_str(&x, 0, "s");
  Str nd = arg_str(&x, 1, "needle");
  if (arg_has_err(&x)) return VN;
  return v_bool(s_find(s, nd, 0) >= 0);
}
static V b_starts(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str s = arg_str(&x, 0, "s");
  Str p = arg_str(&x, 1, "prefix");
  if (arg_has_err(&x)) return VN;
  if (p.len > s.len) return VF;
  if (p.len == 0 || !s.p) return VT;
  return v_bool(!memcmp(s.p, p.p, (size_t)p.len));
}
static V b_ends(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str s = arg_str(&x, 0, "s");
  Str p = arg_str(&x, 1, "suffix");
  if (arg_has_err(&x)) return VN;
  if (p.len > s.len) return VF;
  if (p.len == 0 || !s.p) return VT;
  return v_bool(!memcmp(s.p + s.len - p.len, p.p, (size_t)p.len));
}

/* tx.find(s, needle, {max:20}) -> [{n,at,idx,text}]
 * Line-oriented on purpose: n is the line number and at is that line's anchor,
 * so a hit list feeds tx.patch with no extra read. Literal, case-insensitive;
 * VXA has no regex engine (group > 0 is refused, not ignored). */
static V b_find(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Str s = arg_str(&x, 0, "s");
  Str nd = arg_str(&x, 1, "needle");
  if (arg_has_err(&x)) return VN;
  if (opts_bad(&x, 2)) return VN;
  V gv = arg_opt(&x, 2, "group", v_num(0));
  int max = arg_opt_int(&x, 2, "max", 20);
  if (arg_has_err(&x)) return VN;
  if (gv.t == V_NUM && gv.u.n != 0) {
    set_error(c, E_UNSUPPORTED, "tx.find: VXA has no regex engine, so there are no capture groups - group must be 0");
    return VN;
  }
  if (nd.len == 0) {
    set_error(c, E_BAD_INPUT, "tx.find: the needle is empty - every position would match");
    return VN;
  }
  if (max < 0) {
    set_error(c, E_RANGE, "tx.find: max must be 0 or more, got %d", max);
    return VN;
  }
  List *out = list_new(a);
  if (!s.p || s.len == 0) return v_listv(out);

  int nl = 1;
  for (int k = 0; k < s.len; k++) if (s.p[k] == '\n') nl++;
  ErrCode ae = E_NONE;
  int acnt = 0;
  Anch *A = anchors_of(a, "-", s, &acnt, &ae);
  bool have_at = (A != NULL) && ae == E_NONE && acnt == nl;
  Str lnd = s_lower(a, nd);

  int ln = 0, start = 0;
  for (;;) {
    int e = start;
    while (e < s.len && s.p[e] != '\n') e++;
    int from = start;
    while (out->len < max) {
      int hit = ni_at(s, lnd, from);
      if (hit < 0 || hit >= e) break;
      list_push(a, out, v_ok(a, 4,
        "n", v_num(ln + 1),
        "at", have_at ? v_str(A[ln].at) : VN,
        "idx", v_num(hit - start),
        "text", v_str(s_cut(a, s, start, e))));
      from = hit + lnd.len;
    }
    if (e >= s.len || out->len >= max) break;
    start = e + 1;
    ln++;
  }
  return v_listv(out);
}

/* tx.pad(s, n) - left-align to n CODE POINTS (matching .chars). A value wider
 * than the column comes back whole: display must never lose data silently. */
static V b_pad(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Str s = arg_str(&x, 0, "s");
  double dn = arg_num(&x, 1, "n");
  if (arg_has_err(&x)) return VN;
  int n = (int)dn;
  if (n < 0 || dn != floor(dn)) {
    set_error(c, E_RANGE, "tx.pad: n must be a whole code point count 0 or more, got %g", dn);
    return VN;
  }
  int cl = s_ucount(s);
  if (cl >= n) return v_str(dup_or_empty(a, s));
  Buf b; buf_init(&b, a);
  if (s.len > 0) buf_put(&b, s.p, (size_t)s.len);
  for (int k = cl; k < n; k++) buf_putc(&b, ' ');
  return v_str(buf_take(&b));
}

/* tx.ellide(s, n) - head + ... + tail cut to n BYTES (util.c's rule: below 16
 * it cannot fit the marker and returns the string untouched). */
static V b_ellide(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str s = arg_str(&x, 0, "s");
  double dn = arg_num(&x, 1, "n");
  if (arg_has_err(&x)) return VN;
  if (dn < 0 || dn != floor(dn)) {
    set_error(c, E_RANGE, "tx.ellide: n must be a whole byte count 0 or more, got %g", dn);
    return VN;
  }
  return v_str(s_ellide(ctx_arena(c), s, (int)dn));
}

static V b_bytes(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str s = arg_str(&x, 0, "s");
  if (arg_has_err(&x)) return VN;
  return v_num(s.len > 0 ? (double)s.len : 0.0);
}
static V b_chars(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str s = arg_str(&x, 0, "s");
  if (arg_has_err(&x)) return VN;
  return v_num((double)s_ucount(s));
}
static V b_hash(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Str s = arg_str(&x, 0, "s");
  if (arg_has_err(&x)) return VN;
  return v_str(hash12(ctx_arena(c), s.p ? s.p : "", s.len > 0 ? (size_t)s.len : 0));
}

/* tx.escape(s, kind) -> the escaped BODY, no surrounding quotes, so the result
 * drops straight into a template. kind is json or c (identical today). */
static V b_escape(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Str s = arg_str(&x, 0, "s");
  Str kind = arg_str(&x, 1, "kind");
  if (arg_has_err(&x)) return VN;
  Buf b; buf_init(&b, a);
  if (s_eqz(kind, "json")) buf_json_str(&b, s);
  else if (s_eqz(kind, "c")) buf_c_escape(&b, s);
  else {
    set_error(c, E_BAD_INPUT, "tx.escape: kind must be json or c, got %.*s",
              kind.len > 0 ? kind.len : 0, kind.p ? kind.p : "");
    return VN;
  }
  Str q = buf_take(&b);
  if (q.len < 2) return v_str(s_from(a, "", 0));
  return v_str(s_cut(a, q, 1, q.len - 1));
}
static V b_cescape(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Str s = arg_str(&x, 0, "s");
  if (arg_has_err(&x)) return VN;
  Buf b; buf_init(&b, a);
  buf_c_escape(&b, s);
  Str q = buf_take(&b);
  if (q.len < 2) return v_str(s_from(a, "", 0));
  return v_str(s_cut(a, q, 1, q.len - 1));
}

/* tx.json(v) - any value as JSON; a string comes back in quoted form. */
static V b_json(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  V v = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  return v_str(v_tojson(ctx_arena(c), v));
}

/* tx.word_wrap(s, width) - greedy wrap for readable plan output: no dynamic
 * programming, no hyphenation. Keeps line breaks, reuses each line's indent on
 * its continuation lines, and never breaks a word (paths and hashes stay whole). */
static void wrap_line(Buf *b, Str ln, int width) {
  if (!ln.p || ln.len == 0) return;
  int ind = 0;
  while (ind < ln.len && (ln.p[ind] == ' ' || ln.p[ind] == '\t')) ind++;
  Str pre = s_slice(ln, 0, ind);
  if (pre.len) buf_put(b, pre.p, (size_t)pre.len);
  int col = pre.len, i = ind;
  while (i < ln.len) {
    while (i < ln.len && (ln.p[i] == ' ' || ln.p[i] == '\t')) i++;
    if (i >= ln.len) break;
    int ws = i;
    while (i < ln.len && ln.p[i] != ' ' && ln.p[i] != '\t') i++;
    int wl = i - ws;
    if (col > pre.len && col + 1 + wl > width) {
      buf_putc(b, '\n');
      if (pre.len) buf_put(b, pre.p, (size_t)pre.len);
      col = pre.len;
    } else if (col > pre.len) {
      buf_putc(b, ' ');
      col++;
    }
    buf_put(b, ln.p + ws, (size_t)wl);
    col += wl;
  }
}

static V b_word_wrap(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Str s = arg_str(&x, 0, "s");
  double dw = arg_num(&x, 1, "width");
  if (arg_has_err(&x)) return VN;
  int width = (int)dw;
  if (width < 1) {
    set_error(c, E_RANGE, "tx.word_wrap: width must be 1 or more, got %d", width);
    return VN;
  }
  Buf b; buf_init(&b, a);
  if (!s.p || s.len == 0) return v_str(s_from(a, "", 0));
  /* walk LF-separated pieces, re-joining with LF: an empty trailing piece stays
   * empty, so a terminated text keeps exactly one newline and no more */
  int start = 0;
  for (;;) {
    int e = start;
    while (e < s.len && s.p[e] != '\n') e++;
    if (start) buf_putc(&b, '\n');
    wrap_line(&b, s_slice(s, start, e), width);
    if (e >= s.len) break;
    start = e + 1;
  }
  return v_str(buf_take(&b));
}

/* ======================================================== list: callbacks = */

/* Callbacks go through call_fn with EXACTLY one argument, and every single call
 * is checked: a failing callback stops the loop on the spot, because a map that
 * swallows an error is how an agent ends up trusting corrupted data. */
static V cb1(Ctx *c, V f, V arg, V *out) {
  V one = arg;
  V r = call_fn(c, f, &one, 1);
  if (ctx_has_err(c)) return VN;
  if (v_is_err(r)) return r;
  *out = r;
  return VT;
}

static bool bad_fn(Args *x, V f, const char *what) {
  if (f.t == V_FN) return false;
  set_error(x->c, E_TYPE, "%s takes a function, got %s", what, v_typename(f));
  return true;
}

static V m_map(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  List *l = arg_list(&x, 0, "list");
  V f = arg_at(&x, 1);
  if (arg_has_err(&x) || !l) return VN;
  if (bad_fn(&x, f, "list.map")) return VN;
  List *out = list_new(a);
  for (int i = 0; i < l->len; i++) {
    V got = VN;
    V st = cb1(c, f, l->v[i], &got);
    if (ctx_has_err(c)) return VN;
    if (v_is_err(st)) return st;
    list_push(a, out, got);
  }
  return v_listv(out);
}

static V m_filter(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  List *l = arg_list(&x, 0, "list");
  V f = arg_at(&x, 1);
  if (arg_has_err(&x) || !l) return VN;
  if (bad_fn(&x, f, "list.filter")) return VN;
  List *out = list_new(a);
  for (int i = 0; i < l->len; i++) {
    V got = VN;
    V st = cb1(c, f, l->v[i], &got);
    if (ctx_has_err(c)) return VN;
    if (v_is_err(st)) return st;
    if (v_truthy(got)) list_push(a, out, l->v[i]);
  }
  return v_listv(out);
}

static V m_each(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  List *l = arg_list(&x, 0, "list");
  V f = arg_at(&x, 1);
  if (arg_has_err(&x) || !l) return VN;
  if (bad_fn(&x, f, "list.each")) return VN;
  for (int i = 0; i < l->len; i++) {
    V got = VN;
    V st = cb1(c, f, l->v[i], &got);
    if (ctx_has_err(c)) return VN;
    if (v_is_err(st)) return st;
  }
  return v_listv(l);                                   /* unchanged: chains */
}

static V m_count(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  if (!arg_present(&x, 1)) return v_num(l->len);
  V f = args[1];
  if (bad_fn(&x, f, "list.count")) return VN;
  int n = 0;
  for (int i = 0; i < l->len; i++) {
    V got = VN;
    V st = cb1(c, f, l->v[i], &got);
    if (ctx_has_err(c)) return VN;
    if (v_is_err(st)) return st;
    if (v_truthy(got)) n++;
  }
  return v_num(n);
}

/* ====================================================== list: order / cut = */

typedef struct { const V *kv; int bad; int badix; } Sort;

static int key_cmp(Sort *s, int ia, int ib) {
  bool ok = false;
  int r = v_cmp(s->kv[ia], s->kv[ib], &ok);
  if (!ok) {
    if (!s->bad) { s->bad = 1; s->badix = ia; }        /* 0-based, reported +1 */
    return 0;
  }
  if (r) return r;
  return ia < ib ? -1 : (ia > ib ? 1 : 0);             /* stable on ties */
}

/* bottom-up stable merge sort over an index vector; returns the vector holding
 * the result (it ping-pongs), and stops early when key_cmp flags a bad value. */
static int *sort_idx(Sort *s, int *ix, int *tmp, int n, const V *kv) {
  s->kv = kv; s->bad = 0; s->badix = 0;
  for (int i = 0; i < n; i++) ix[i] = i;
  int *src = ix, *dst = tmp;
  for (int width = 1; width < n && !s->bad; width *= 2) {
    for (int lo = 0; lo < n; lo += 2 * width) {
      int mid = lo + width, hi = lo + 2 * width;
      if (mid > n) mid = n;
      if (hi > n) hi = n;
      int i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        if (key_cmp(s, src[i], src[j]) <= 0) dst[k++] = src[i++];
        else dst[k++] = src[j++];
        if (s->bad) return src;
      }
      while (i < mid) dst[k++] = src[i++];
      while (j < hi) dst[k++] = src[j++];
    }
    int *sw = src; src = dst; dst = sw;
  }
  return src;
}

/* keys[i] for every element: identity, or one record field. A missing field is
 * an error naming the element - sorting nulls to one end would hide a typo.
 * as_key=true is list.sort_by's looser contract: the key names a field of a
 * RECORD, and anything that is not a record (a plain list of numbers or
 * strings) is its own key, so sorting by a key needs no type pre-check. */
static bool sort_keys(Ctx *c, List *l, Str key, bool as_key, const V **kv_out) {
  Arena *a = ctx_arena(c);
  V *kv = (V*)arena_alloc(a, sizeof(V) * (size_t)(l->len ? l->len : 1));
  for (int i = 0; i < l->len; i++) {
    V e = l->v[i];
    if (key.len == 0) { kv[i] = e; continue; }
    if (e.t != V_REC) {
      if (as_key) { kv[i] = e; continue; }            /* sorts as itself */
      set_error(c, E_TYPE, "list.sort: key '%.*s' needs records, element %d is a %s",
                key.len, key.p, i + 1, v_typename(e));
      return false;
    }
    V *p = rec_get(e.u.r, key);
    if (!p) {
      set_error(c, E_RANGE, "list.sort: element %d has no field '%.*s' - r.keys shows what it has",
                i + 1, key.len, key.p);
      return false;
    }
    kv[i] = *p;
  }
  *kv_out = kv;
  return true;
}

static V sorted(Ctx *c, List *l, const V *kv) {
  Arena *a = ctx_arena(c);
  int n = l->len;
  Sort s;
  int *ix = (int*)arena_alloc(a, sizeof(int) * (size_t)(n ? n : 1));
  int *tmp = (int*)arena_alloc(a, sizeof(int) * (size_t)(n ? n : 1));
  int *ord = sort_idx(&s, ix, tmp, n, kv);
  if (s.bad) {
    set_error(c, E_TYPE, "list.sort: element %d is a %s, which has no order here - VXA sorts numbers, strings and record keys, and there is no 3-way comparator to hand it",
              s.badix + 1, v_typename(kv[s.badix]));
    return VN;
  }
  List *out = list_new(a);
  for (int i = 0; i < n; i++) list_push(a, out, l->v[ord[i]]);
  return v_listv(out);
}

/* list.sort(k?) / list.sort_by(k)
 * k is a KEY NAME, not a function: VXA has no 3-way comparison, so a
 * comparator cannot be expressed. sort() orders numbers or strings; sort(k)
 * orders records by field k; the sort is stable either way. */
static V m_sort(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  Str key = s_null();
  if (arg_present(&x, 1)) {
    V k = args[1];
    if (k.t == V_FN) {
      set_error(c, E_UNSUPPORTED, "list.sort takes a key name, not a function: VXA has no 3-way comparison - use sort(field) or sort_by(field)");
      return VN;
    }
    if (k.t != V_STR) {
      set_error(c, E_TYPE, "list.sort: the argument must be a key name (string), got %s - sig: l.sort(k?)", v_typename(k));
      return VN;
    }
    key = k.u.s;
  }
  const V *kv = NULL;
  if (!sort_keys(c, l, key, false, &kv)) return VN;
  return sorted(c, l, kv);
}

static V m_sort_by(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  List *l = arg_list(&x, 0, "list");
  Str key = arg_str(&x, 1, "key");
  if (arg_has_err(&x) || !l) return VN;
  if (key.len == 0) {
    set_error(c, E_BAD_INPUT, "list.sort_by: the key name is empty - sig: l.sort_by(k)");
    return VN;
  }
  const V *kv = NULL;
  if (!sort_keys(c, l, key, true, &kv)) return VN;
  return sorted(c, l, kv);
}

static V m_uniq(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  List *out = list_new(a);
  for (int i = 0; i < l->len; i++) {
    bool seen = false;
    for (int k = 0; k < out->len; k++) if (v_eq(out->v[k], l->v[i])) { seen = true; break; }
    if (!seen) list_push(a, out, l->v[i]);            /* first occurrence wins */
  }
  return v_listv(out);
}

static V m_flat(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  List *out = list_new(a);
  for (int i = 0; i < l->len; i++) {
    V e = l->v[i];
    if (e.t == V_LIST && e.u.l) {
      for (int k = 0; k < e.u.l->len; k++) list_push(a, out, e.u.l->v[k]);
    } else list_push(a, out, e);
  }
  return v_listv(out);
}

static V m_reverse(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  List *out = list_new(a);
  for (int i = l->len - 1; i >= 0; i--) list_push(a, out, l->v[i]);
  return v_listv(out);
}

/* list.slice(a, b) - same clamping and negative indices as s[a..b]: out of
 * range is truncated, never an error, and a > b yields the empty list. */
static V m_slice(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  List *l = arg_list(&x, 0, "list");
  double da = arg_num(&x, 1, "a");
  double db = arg_num(&x, 2, "b");
  if (arg_has_err(&x) || !l) return VN;
  int n = l->len;
  int lo = norm_idx((int)da, n), hi = norm_idx((int)db, n);
  if (lo < 0) lo = 0;
  if (hi > n) hi = n;
  if (lo > hi) lo = hi;
  List *out = list_new(a);
  for (int i = lo; i < hi; i++) list_push(a, out, l->v[i]);
  return v_listv(out);
}

static V m_first(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  return l->len ? l->v[0] : VN;                       /* empty -> null, per SPEC */
}
static V m_last(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  return l->len ? l->v[l->len - 1] : VN;
}
static V m_has(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  V want = arg_at(&x, 1);
  if (arg_has_err(&x)) return VN;
  for (int i = 0; i < l->len; i++) if (v_eq(l->v[i], want)) return VT;
  return VF;
}
static V m_index(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  V want = arg_at(&x, 1);
  if (arg_has_err(&x)) return VN;
  for (int i = 0; i < l->len; i++) if (v_eq(l->v[i], want)) return v_num(i);
  return v_num(-1);
}
static V m_minmax(Ctx *c, V *args, int nargs, bool want_max) {
  Args x = ARGS();
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  if (l->len == 0) {
    set_error(c, E_RANGE, "list.%s of an empty list has no answer - guard with l.len first",
              want_max ? "max" : "min");
    return VN;
  }
  V best = l->v[0];
  for (int i = 1; i < l->len; i++) {
    bool ok = false;
    int r = v_cmp(l->v[i], best, &ok);
    if (!ok) {
      set_error(c, E_TYPE, "list.%s: element %d is a %s, which cannot be compared with a %s",
                want_max ? "max" : "min", i + 1, v_typename(l->v[i]), v_typename(best));
      return VN;
    }
    if ((want_max && r > 0) || (!want_max && r < 0)) best = l->v[i];
  }
  return best;
}
static V m_min(Ctx *c, V *args, int nargs) { return m_minmax(c, args, nargs, false); }
static V m_max(Ctx *c, V *args, int nargs) { return m_minmax(c, args, nargs, true); }

static V m_sum(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  List *l = arg_list(&x, 0, "list");
  if (arg_has_err(&x) || !l) return VN;
  double acc = 0;
  for (int i = 0; i < l->len; i++) {
    if (l->v[i].t != V_NUM) {
      set_error(c, E_TYPE, "list.sum: element %d is a %s, not a number", i + 1, v_typename(l->v[i]));
      return VN;
    }
    acc += l->v[i].u.n;
  }
  return v_num(acc);
}

static V m_len(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  V self = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  if (self.t == V_LIST) return v_num(self.u.l ? self.u.l->len : 0);
  if (self.t == V_REC) return v_num(self.u.r ? self.u.r->len : 0);
  if (self.t == V_STR) return v_num(self.u.s.len > 0 ? self.u.s.len : 0);
  set_error(c, E_TYPE, ".len counts list items, record fields or string bytes, not a %s", v_typename(self));
  return VN;
}

/* ============================================================ rec methods = */

static Rec *rec_copy(Arena *a, Rec *r) {
  Rec *n = rec_new(a);
  if (!r) return n;
  for (int i = 0; i < r->len; i++) rec_set(a, n, r->kv[i].k, r->kv[i].v);
  return n;
}

static V m_keys(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Rec *r = arg_rec(&x, 0, "rec");
  if (arg_has_err(&x) || !r) return VN;
  List *out = list_new(a);
  for (int i = 0; i < r->len; i++) list_push(a, out, v_str(r->kv[i].k));
  return v_listv(out);
}
static V m_values(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Rec *r = arg_rec(&x, 0, "rec");
  if (arg_has_err(&x) || !r) return VN;
  List *out = list_new(a);
  for (int i = 0; i < r->len; i++) list_push(a, out, r->kv[i].v);
  return v_listv(out);
}
static V m_rhas(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Rec *r = arg_rec(&x, 0, "rec");
  Str k = arg_str(&x, 1, "key");
  if (arg_has_err(&x) || !r) return VN;
  return v_bool(rec_get(r, k) != NULL);
}
static V m_get(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Rec *r = arg_rec(&x, 0, "rec");
  Str k = arg_str(&x, 1, "key");
  if (arg_has_err(&x) || !r) return VN;
  V dflt = arg_at(&x, 2);
  if (arg_has_err(&x)) return VN;
  V *p = rec_get(r, k);
  return p ? *p : dflt;
}
static V m_len_rec(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Rec *r = arg_rec(&x, 0, "rec");
  if (arg_has_err(&x) || !r) return VN;
  return v_num(r->len);
}
/* del returns a COPY without the field; the original stays intact, so a plan
 * pass cannot mutate a value an earlier line already printed. */
static V m_del(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Rec *r = arg_rec(&x, 0, "rec");
  Str k = arg_str(&x, 1, "key");
  if (arg_has_err(&x) || !r) return VN;
  if (!rec_get(r, k)) {
    set_error(c, E_RANGE, "rec.del: no field '%.*s' to delete - r.keys shows what is there",
              k.len > 0 ? k.len : 0, k.p ? k.p : "");
    return VN;
  }
  Rec *n = rec_new(a);
  for (int i = 0; i < r->len; i++) if (!s_eq(r->kv[i].k, k)) rec_set(a, n, r->kv[i].k, r->kv[i].v);
  return rec_to_v(n);
}
/* merge: self first, then other - so the argument wins on a shared key. */
static V m_merge(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Rec *r = arg_rec(&x, 0, "rec");
  Rec *o = arg_rec(&x, 1, "rec");
  if (arg_has_err(&x) || !r || !o) return VN;
  Rec *n = rec_copy(a, r);
  for (int i = 0; i < o->len; i++) rec_set(a, n, o->kv[i].k, o->kv[i].v);
  return rec_to_v(n);
}
static V m_pick(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Rec *r = arg_rec(&x, 0, "rec");
  List *want = arg_list(&x, 1, "keys");
  if (arg_has_err(&x) || !r || !want) return VN;
  Rec *n = rec_new(a);
  for (int i = 0; i < want->len; i++) {
    Str k = as_text(c, want->v[i]);
    for (int j = 0; j < r->len; j++) {              /* absent names are skipped */
      if (!s_eq(r->kv[j].k, k)) continue;
      rec_set(a, n, r->kv[j].k, r->kv[j].v);
      break;
    }
  }
  return rec_to_v(n);
}
static V m_omit(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Rec *r = arg_rec(&x, 0, "rec");
  List *drop = arg_list(&x, 1, "keys");
  if (arg_has_err(&x) || !r || !drop) return VN;
  Rec *n = rec_new(a);
  for (int i = 0; i < r->len; i++) {
    bool hit = false;
    for (int k = 0; k < drop->len; k++) {
      Str dk = as_text(c, drop->v[k]);
      if (s_eq(r->kv[i].k, dk)) { hit = true; break; }
    }
    if (!hit) rec_set(a, n, r->kv[i].k, r->kv[i].v);
  }
  return rec_to_v(n);
}

/* ============================================================ num methods = */

static V num1(Ctx *c, V *args, double (*f)(double), const char *what) {
  Args x = { c, args, 1, NULL };                    /* receiver only, no nargs */
  double v = arg_num(&x, 0, what);
  if (arg_has_err(&x)) return VN;
  return v_num(f(v));
}
static double d_trunc(double v) { return trunc(v); }
static double d_round(double v) { return round(v); }
static double d_abs(double v) { return fabs(v); }
static double d_floor(double v) { return floor(v); }
static double d_ceil(double v) { return ceil(v); }

static V m_num_int(Ctx *c, V *args, int nargs) { return num1(c, args, d_trunc, "number"); }
static V m_num_round(Ctx *c, V *args, int nargs) { return num1(c, args, d_round, "number"); }
static V m_num_abs(Ctx *c, V *args, int nargs) { return num1(c, args, d_abs, "number"); }
static V m_num_floor(Ctx *c, V *args, int nargs) { return num1(c, args, d_floor, "number"); }
static V m_num_ceil(Ctx *c, V *args, int nargs) { return num1(c, args, d_ceil, "number"); }
static V m_num_tostr(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  double v = arg_num(&x, 0, "number");
  if (arg_has_err(&x)) return VN;
  Buf b; buf_init(&b, ctx_arena(c));
  fmt_num(&b, v);
  return v_str(buf_take(&b));
}
/* num.fmt(d) - fixed point, d decimals, no grouping: a cheap way to shorten a
 * metric before it costs tokens. */
static V m_num_fmt(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  double v = arg_num(&x, 0, "number");
  double dd = arg_num(&x, 1, "d");
  if (arg_has_err(&x)) return VN;
  int d = (int)dd;
  if (d < 0 || d > 15 || dd != floor(dd)) {
    set_error(c, E_RANGE, "num.fmt: d must be a whole number of decimals 0..15, got %g", dd);
    return VN;
  }
  return v_str(s_fmt(ctx_arena(c), "%.*f", d, v));
}

/* ============================================================ any methods = */

static V m_type(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  V self = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  return v_str(s_lit(ctx_arena(c), v_typename(self)));
}
static V m_tostr(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  V self = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  if (self.t == V_STR) return v_str(dup_or_empty(a, self.u.s));
  return v_str(v_tostr(a, self, true));
}
static V m_json(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  V self = arg_at(&x, 0);
  if (arg_has_err(&x)) return VN;
  return v_str(v_tojson(ctx_arena(c), self));
}

/* str.fmt(v...) - fill {name} / {0} holes from a record and/or positional
 * values. An unknown hole is an error: a template that silently prints "{}"
 * is how a wrong field name survives to the final report. A hole holding null
 * fills with '-' (the compact form, SPEC 2.1) and a string fills raw, because
 * this is text assembly and not printing. A doubled brace is literal: '{{x}}'
 * renders '{x}', and such a pair counts as template that was used, so it is
 * never mistaken for an unfilled hole. */
static V m_fmt(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  Str tpl = arg_str(&x, 0, "template");
  if (arg_has_err(&x)) return VN;
  Rec *nm = rec_new(a);
  List *pos = list_new(a);
  for (int i = 1; i < nargs; i++) {
    V v = args[i];
    if (v.t == V_REC && v.u.r) {
      for (int k = 0; k < v.u.r->len; k++) rec_set(a, nm, v.u.r->kv[k].k, v.u.r->kv[k].v);
    } else list_push(a, pos, v);
  }
  Buf b; buf_init(&b, a);
  int holes = 0, esc = 0;
  for (int i = 0; i < tpl.len; ) {
    char ch = tpl.p[i];
    if (ch == '{' && i + 1 < tpl.len && tpl.p[i + 1] == '{') { buf_putc(&b, '{'); i += 2; esc++; continue; }
    if (ch == '}' && i + 1 < tpl.len && tpl.p[i + 1] == '}') { buf_putc(&b, '}'); i += 2; esc++; continue; }
    if (ch == '{') {
      int j = i + 1;
      while (j < tpl.len && tpl.p[j] != '}') j++;
      if (j >= tpl.len) {
        set_error(c, E_SYNTAX, "str.fmt: unclosed '{' at byte %d of the template", i);
        return VN;
      }
      Str k = s_slice(tpl, i + 1, j);
      V got;
      V *p = rec_get(nm, k);
      long long idx = -1;
      if (p) got = *p;
      else if (s_is_int(k, &idx) && idx >= 0 && idx < pos->len) got = pos->v[idx];
      else {
        set_error(c, E_RANGE, "str.fmt: no value for hole '{%.*s}' - pass a record with that field, or a positional argument (holes are numbered from 0)",
                  k.len > 0 ? k.len : 0, k.p ? k.p : "");
        return VN;
      }
      Str t = got.t == V_NULL ? q(a, "-") : as_text(c, got);
      buf_put(&b, t.p, (size_t)t.len);
      holes++;
      i = j + 1;
      continue;
    }
    buf_putc(&b, ch);
    i++;
  }
  if (!holes && !esc && nargs > 1) {
    set_error(c, E_RANGE, "str.fmt: the template has no {hole} to fill, but %d value(s) were passed", nargs - 1);
    return VN;
  }
  return v_str(buf_take(&b));
}

/* ============================================================= tx table === */

static const Builtin TX[] = {
{ "tx", "anchors", b_anchors, 1, 2,
  "tx.anchors(path_or_text, {path:'a.c'}) -> [{n,at,text}]",
  "one call to turn text (or an existing file, if arg0 is a path) into the anchor table tx.patch needs; cost is O(lines) hashing, no read when you already hold the text",
  "tx.anchors('a\\nb\\n', {path:'a.c'})", 0 },
{ "tx", "patch", b_patch, 2, 3,
  "tx.patch(text, [{at,op,text?,n?}], {path:'a.c'}) -> {text,applied,checks}",
  "apply content-addressed edits by anchor instead of line number; op is set/del/ins_before/ins_after, n disambiguates a repeated anchor; the checks array is your own proof the edit landed",
  "tx.patch(t, [{at:'c3f2ab12',op:'set',text:'x=1'}])", 0 },
{ "tx", "diff", b_diff, 2, 3,
  "tx.diff(a, b, {ctx:3, unified:true}) -> {text,adds,dels,hunks}",
  "unified diff of two texts; read adds/dels first and only pull text when you must, it is the expensive field",
  "tx.diff(before, after, {ctx:2})", 0 },
{ "tx", "similar", b_similar, 2, 2,
  "tx.similar(a, b) -> num 0..1",
  "the ~= operator as a function: code point Levenshtein, so CJK is scored per glyph; use it to match data, never to forgive a typo in your own script",
  "tx.similar('kitten', 'sitting')", 0 },
{ "tx", "subst", b_subst, 3, 4,
  "tx.subst(text, needle, repl, {all:false}) -> str",
  "literal replace, not a pattern: needle is matched byte for byte, case sensibly; every occurrence by default, {all:false} for the first only",
  "tx.subst('a.b.c', '.', '-')", 0 },
{ "tx", "split", b_split, 2, 2,
  "tx.split(s, sep) -> [str]",
  "cut on a literal separator; sep '' splits into code points, and empty input yields one empty piece so join() reverses it",
  "tx.split('a,b,c', ',')", 0 },
{ "tx", "join", b_join, 2, 2,
  "tx.join(list, sep) -> str",
  "glue a list into one string: each item takes the text a string hole would use, and nested records and lists keep their delimiters so the result stays parseable",
  "tx.join(['a','b'], '-')", 0 },
{ "tx", "lines", b_lines, 1, 1,
  "tx.lines(s) -> [str]",
  "split on LF only, so a CRLF file keeps its CR at each line end and tx.unlines puts the bytes back exactly; a terminated string yields a trailing empty piece",
  "tx.lines('a\\nb\\n')", 0 },
{ "tx", "unlines", b_unlines, 1, 1,
  "tx.unlines(list) -> str",
  "join with LF; exact inverse of tx.lines, trailing newline included",
  "tx.unlines(['a','b',''])", 0 },
{ "tx", "trim", b_trim, 1, 1,
  "tx.trim(s) -> str",
  "strip leading and trailing whitespace, including CR and tab",
  "tx.trim('  hi \\n')", 0 },
{ "tx", "lower", b_lower, 1, 1,
  "tx.lower(s) -> str",
  "ASCII lowercase only: bytes 0x80 and up (CJK, accents) pass through untouched",
  "tx.lower('MAIN.C')", 0 },
{ "tx", "upper", b_upper, 1, 1,
  "tx.upper(s) -> str",
  "ASCII uppercase only: bytes 0x80 and up pass through untouched",
  "tx.upper('abc')", 0 },
{ "tx", "contains", b_contains, 2, 2,
  "tx.contains(s, needle) -> bool",
  "case-SENSITIVE substring test; lower both sides first, or use tx.find, if case must not matter",
  "tx.contains('src/a.c', '/a')", 0 },
{ "tx", "starts", b_starts, 2, 2,
  "tx.starts(s, prefix) -> bool",
  "case-sensitive prefix test; an empty prefix is true for every string",
  "tx.starts('src/a.c', 'src/')", 0 },
{ "tx", "ends", b_ends, 2, 2,
  "tx.ends(s, suffix) -> bool",
  "case-sensitive suffix test, e.g. extension checks; an empty suffix is always true",
  "tx.ends('a.c', '.c')", 0 },
{ "tx", "find", b_find, 2, 3,
  "tx.find(s, needle, {group:0, max:20}) -> [{n,at,idx,text}]",
  "literal, case-insensitive hits reported per line: n is the line number and at is that line's anchor, so the result feeds tx.patch with no extra read - VXA has NO regex engine, so group>0 is refused",
  "tx.find(src, 'TODO', {max:5})", 0 },
{ "tx", "pad", b_pad, 2, 2,
  "tx.pad(s, n) -> str",
  "left-align to n code points with trailing spaces; longer values come back whole instead of being cut",
  "tx.pad('id', 8)", 0 },
{ "tx", "ellide", b_ellide, 2, 2,
  "tx.ellide(s, n) -> str",
  "cut to about n bytes keeping head and tail with ... between them; n below 16 cannot fit the marker and returns s unchanged",
  "tx.ellide(line, 40)", 0 },
{ "tx", "bytes", b_bytes, 1, 1,
  "tx.bytes(s) -> num",
  "byte length - what offsets, slices and patch sizes are counted in",
  "tx.bytes('abc')", 0 },
{ "tx", "chars", b_chars, 1, 1,
  "tx.chars(s) -> num",
  "code point length: 'abc' is 3 and 3, CJK is 3 bytes per glyph but 1 char",
  "tx.chars('abc')", 0 },
{ "tx", "hash", b_hash, 1, 1,
  "tx.hash(s) -> str",
  "12 hex chars of sha256: cheap content fingerprint to compare two texts or to spot that a file moved under you",
  "tx.hash('abc')", 0 },
{ "tx", "escape", b_escape, 2, 2,
  "tx.escape(s, kind) -> str",
  "escaped BODY without the surrounding quotes, kind json or c (they agree today); use it when you assemble a template yourself",
  "tx.escape('a\\nb', 'json')", 0 },
{ "tx", "cescape", b_cescape, 1, 1,
  "tx.cescape(s) -> str",
  "C-style escaped body, no quotes: control bytes become \\n \\t \\xNN and everything printable stays as it is",
  "tx.cescape('tab\\there')", 0 },
{ "tx", "json", b_json, 1, 1,
  "tx.json(v) -> str",
  "any value as JSON text; a string comes back in quoted form, which is what a JSON writer needs",
  "tx.json({a:1})", 0 },
{ "tx", "word_wrap", b_word_wrap, 2, 2,
  "tx.word_wrap(s, width) -> str",
  "greedy wrap for readable plan output: keeps line breaks and each line's indent, never breaks a long word such as a path or a hash",
  "tx.word_wrap(plan_text, 72)", 0 },
};

const Builtin *t_tx(int *n) {
  if (n) *n = (int)(sizeof(TX) / sizeof(TX[0]));
  return TX;
}

/* ======================================================== method dispatch = */

static const Method METHODS[] = {
  /* ---- str: the tx text tools, same functions, self is argument 0 ---- */
  { "str", "len",      b_bytes,   0, 0, "s.len -> num",            "byte length (SPEC: a string's length is its bytes); .chars counts code points" },
  { "str", "chars",    b_chars,   0, 0, "s.chars -> num",          "code point count: one CJK glyph is 3 bytes but 1 char" },
  { "str", "lines",    b_lines,   0, 0, "s.lines -> [str]",        "split on LF only, CR kept at the line end, so .lines.join(LF) is byte-exact" },
  { "str", "split",    b_split,   1, 1, "s.split(sep) -> [str]",   "cut on a literal separator; sep '' splits into code points" },
  { "str", "trim",     b_trim,    0, 0, "s.trim -> str",           "strip leading and trailing whitespace including CR and tab" },
  { "str", "upper",    b_upper,   0, 0, "s.upper -> str",          "ASCII uppercase; bytes 0x80 and up untouched" },
  { "str", "lower",    b_lower,   0, 0, "s.lower -> str",          "ASCII lowercase; bytes 0x80 and up untouched" },
  { "str", "contains", b_contains,1, 1, "s.contains(x) -> bool",   "case-sensitive substring test; use .lower first when case must not matter" },
  { "str", "starts",   b_starts,  1, 1, "s.starts(p) -> bool",     "case-sensitive prefix test; empty prefix is true" },
  { "str", "ends",     b_ends,    1, 1, "s.ends(p) -> bool",       "case-sensitive suffix test; empty suffix is true" },
  { "str", "replace",  b_subst,   2, 3, "s.replace(a, b, {all:false}) -> str", "literal replace of every occurrence (all:false for the first only); never a pattern" },
  { "str", "find",     b_find,    1, 2, "s.find(needle, {max:20}) -> [{n,at,idx,text}]", "literal case-insensitive hits with line number and anchor; VXA has no regex engine" },
  { "str", "hash",     b_hash,    0, 0, "s.hash -> str",           "12 hex chars of sha256 over the bytes of this string" },
  { "str", "pad",      b_pad,     1, 1, "s.pad(n) -> str",         "left-align to n code points with trailing spaces" },
  { "str", "fmt",      m_fmt,     0, -1, "s.fmt(rec | v...) -> str", "fill {name} and {0} holes from a record or positional values; an unknown hole is an error, never an empty string; doubled braces render literally and null fills as the compact dash" },

  /* ---- list ---- */
  { "list", "len",     m_len,     0, 0, "l.len -> num",            "item count" },
  { "list", "first",   m_first,   0, 0, "l.first -> v",            "first item, or null on an empty list" },
  { "list", "last",    m_last,    0, 0, "l.last -> v",             "last item, or null on an empty list" },
  { "list", "has",     m_has,     1, 1, "l.has(x) -> bool",        "deep equality scan; order does not matter, type does" },
  { "list", "join",    b_join,    1, 1, "l.join(sep) -> str",      "glue items into one string; null joins as empty and nested records keep their braces so the text stays parseable" },
  { "list", "map",     m_map,     1, 1, "l.map(f) -> [v]",         "f(item) per element, order kept; the first failing f stops the loop and its error is the result" },
  { "list", "filter",  m_filter,  1, 1, "l.filter(f) -> [v]",      "keep items where f(item) is truthy, order kept; f errors abort the loop" },
  { "list", "each",    m_each,    1, 1, "l.each(f) -> l",          "run f for effect and return the list unchanged so it chains in a pipe; f errors abort" },
  { "list", "sort",    m_sort,    0, 1, "l.sort(k?) -> [v]",       "stable sort: numbers or strings ascending, or records by key name k; a function is refused because VXA has no 3-way comparison" },
  { "list", "sort_by", m_sort_by, 1, 1, "l.sort_by(k) -> [v]",     "stable sort records by field name k; an element that is not a record sorts as itself, so a plain list works too; a record missing k names the offending element instead of guessing" },
  { "list", "uniq",    m_uniq,    0, 0, "l.uniq -> [v]",           "drop later duplicates, keeping first occurrences in order; O(n^2), fine for hundreds" },
  { "list", "flat",    m_flat,    0, 0, "l.flat -> [v]",           "flatten exactly one level of nested lists" },
  { "list", "reverse", m_reverse, 0, 0, "l.reverse -> [v]",        "new list in reverse order; the original is untouched" },
  { "list", "slice",   m_slice,   2, 2, "l.slice(a, b) -> [v]",    "items a..b-1, negative indices count from the end, out of range is clamped" },
  { "list", "count",   m_count,   0, 1, "l.count(f?) -> num",      "length, or how many items f accepts; f errors abort" },
  { "list", "index",   m_index,   1, 1, "l.index(x) -> num",       "first position equal to x, or -1" },
  { "list", "min",     m_min,     0, 0, "l.min -> v",              "smallest number or string; empty or mixed types are errors" },
  { "list", "max",     m_max,     0, 0, "l.max -> v",              "largest number or string; empty or mixed types are errors" },
  { "list", "sum",     m_sum,     0, 0, "l.sum -> num",            "add up numbers; an empty list is 0, a non-number names itself" },

  /* ---- rec ---- */
  { "rec", "keys",     m_keys,    0, 0, "r.keys -> [str]",         "field names in insertion order, which is deterministic unlike a hash walk" },
  { "rec", "values",   m_values,  0, 0, "r.values -> [v]",         "field values in insertion order, paired with .keys" },
  { "rec", "has",      m_rhas,    1, 1, "r.has(k) -> bool",        "is this field present, whatever its value (null included)" },
  { "rec", "get",      m_get,     2, 2, "r.get(k, d) -> v",        "field value or d when absent; the default is explicit on purpose" },
  { "rec", "del",      m_del,     1, 1, "r.del(k) -> rec",         "a copy without field k; the original is untouched and a wrong k is an error" },
  { "rec", "merge",    m_merge,   1, 1, "r.merge(o) -> rec",       "new record, r's fields first then o's, so o wins on shared keys" },
  { "rec", "pick",     m_pick,    1, 1, "r.pick([k]) -> rec",      "keep only the listed fields, in the order listed; absent names are skipped" },
  { "rec", "omit",     m_omit,    1, 1, "r.omit([k]) -> rec",      "drop the listed fields and keep insertion order of the rest" },
  { "rec", "len",      m_len_rec, 0, 0, "r.len -> num",            "field count" },

  /* ---- num ---- */
  { "num", "int",      m_num_int,   0, 0, "n.int -> num",          "truncate toward zero" },
  { "num", "round",    m_num_round, 0, 0, "n.round -> num",        "round half away from zero" },
  { "num", "abs",      m_num_abs,   0, 0, "n.abs -> num",          "magnitude" },
  { "num", "floor",    m_num_floor, 0, 0, "n.floor -> num",        "down to the next integer, so -2.5 gives -3" },
  { "num", "ceil",     m_num_ceil,  0, 0, "n.ceil -> num",         "up to the next integer, so 2.1 gives 3" },
  { "num", "tostr",    m_num_tostr, 0, 0, "n.tostr -> str",        "number as text with no trailing .0, the same bytes the printer emits" },
  { "num", "fmt",      m_num_fmt,   1, 1, "n.fmt(d) -> str",       "fixed point with d decimals (0..15), no thousands separators" },

  /* ---- any: reached only when the value's own type has no such method ---- */
  { "any", "type",     m_type,    0, 0, "v.type -> str",           "the type name: null bool num str list rec fn plan err nat" },
  { "any", "tostr",    m_tostr,   0, 0, "v.tostr -> str",          "compact single-line form of any value; a string is itself, unquoted" },
  { "any", "json",     m_json,    0, 0, "v.json -> str",           "JSON text for any value, what you hand to a parser or write to a file" },
};

const Method *meth_all(int *n) {
  if (n) *n = (int)(sizeof(METHODS) / sizeof(METHODS[0]));
  return METHODS;
}

const Method *meth_find(const char *type, const char *name) {
  if (!type || !name) return NULL;
  int n = 0;
  const Method *m = METHODS;
  meth_all(&n);
  for (int i = 0; i < n; i++)
    if (!strcmp(m[i].type, type) && !strcmp(m[i].name, name)) return &m[i];
  return NULL;
}

/* Method call from the evaluator: `self` is NOT part of the agent's argument
 * list, so arity is checked without it and the callee sees self at index 0 -
 * the same layout as the tx.* builtin, which is why the tables can share code.
 * An ERR receiver carries no data to operate on, so the error propagates as a
 * value unchanged and no new error is set (SPEC 1.5) - with one exception: the
 * universal any methods, because SPEC 1.4 says EVERY value answers .type(), so
 * errv.type() reports "err" instead of being swallowed by the propagation. */
V lib_method(Ctx *c, V self, const char *name, V *args, int nargs) {
  if (!name) { set_error(c, E_UNDEF, "method name is missing"); return ctx_err(c); }
  const char *t = v_typename(self);
  const Method *m = meth_find(t, name);
  const Method *am = meth_find("any", name);
  if (!m && v_is_err(self) && !am) return self;   /* errors propagate as values */
  if (!m) m = am;
  if (!m) {
    set_error(c, E_UNDEF, "%s has no method .%s - try: vxa doc %s", t, name, t);
    return ctx_err(c);
  }
  if (nargs < m->min || (m->max >= 0 && nargs > m->max)) {
    set_error(c, E_ARITY, ".%s takes %d..%s%d argument(s), got %d - sig: %s",
              name, m->min, m->max < 0 ? "or more, " : "", m->max < 0 ? m->min : m->max,
              nargs, m->sig);
    return ctx_err(c);
  }
  Arena *a = ctx_arena(c);
  V *all = (V*)arena_alloc(a, sizeof(V) * (size_t)(nargs + 1));
  all[0] = self;
  for (int i = 0; i < nargs; i++) all[i + 1] = args[i];
  V r = m->fn(c, all, nargs + 1);
  if (ctx_has_err(c)) return ctx_err(c);
  return r;
}
