/* tests/test_lib_sh.c - the sh.* namespace, exercised against real harmless commands.
 *
 * Sandbox: the test chdirs into build/tmp_sh_test/ and makes that directory the
 * ctx root, so commands' relative arguments, shell.c's capture files and the
 * journal receipt all stay inside build/tmp_sh_test/. Commands used are cat,
 * echo, cc --version and sleep: harmless, and anything that must NOT run is only
 * ever planned (the "did it run?" proof is a redirect target file or the journal).
 *
 * Two core facts this suite leans on:
 *   - `echo` is NOT in shell.c's read-only table (it is not a version probe, it is
 *     an unbounded row, and there is no echo.exe to prove read-only on Windows),
 *     so sh.run(["echo",..]) is gated and the fast path is asserted with `cat` and
 *     `cc --version`. `git --version` IS in the table, among the toolchain probes,
 *     so it is asserted as accepted; the refusals are asserted explicitly below.
 *   - sh_join_argv is defined by plan.c and quoted with the MSVC argv rules; the
 *     weak stub below only keeps this TU linkable if that definition ever moves.
 *     The real one MUST quote each argument; the "a space stays inside one
 *     argument" check below is what catches an unquoted join.
 */
#include "../src/vxa.h"
#include "../src/interp.h"
#include "../src/lib.h"
#include "../src/shell_int.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- link safety net: plan.c's strong sh_join_argv overrides this ---- */
__attribute__((weak)) char *sh_join_argv(char **argv, int n) {
  Arena t;
  arena_init(&t, 64u * 1024u);
  Str s = sh_command_line(&t, argv, n);
  size_t len = s.len > 0 ? (size_t)s.len : 0;
  char *out = (char*)malloc(len + 1);
  if (out) {
    if (len && s.p) memcpy(out, s.p, len);
    out[len] = 0;
  }
  arena_free(&t);
  return out;
}

/* ---------- harness ---------- */
static Arena ar;
static Ctx *cx = NULL;
static int n_run = 0;               /* commands this test actually let run */

static V S(const char *s) { return v_str(s_lit(&ar, s)); }

static V AV(int n, ...) {
  List *l = list_new(&ar);
  va_list ap;
  va_start(ap, n);
  for (int i = 0; i < n; i++) list_push(&ar, l, S(va_arg(ap, const char*)));
  va_end(ap);
  return list_of(l);
}

static V sh(const char *name, V *args, int n) {
  int k = 0;
  const Builtin *t = t_sh(&k), *row = NULL;
  for (int i = 0; i < k; i++) if (!strcmp(t[i].name, name)) { row = &t[i]; break; }
  if (!row) { CK(row != NULL); return VN; }
  V r = lib_call(cx, row, args, n);
  /* a set_error surfaces as the ERR value lib_call already returned; the ctx has
   * to be clean again or the next call's error would be swallowed */
  ctx_clear_err(cx);
  return r;
}
static V one(const char *name, V a0) { V x[1] = { a0 }; return sh(name, x, 1); }
static V two(const char *name, V a0, V a1) { V x[2] = { a0, a1 }; return sh(name, x, 2); }

static V *rk(V v, const char *k) { return v.t == V_REC ? rec_getz(v.u.r, k) : NULL; }
static double num(V v, const char *k) {
  V *p = rk(v, k);
  return p && p->t == V_NUM ? p->u.n : -999;
}
static const char *str(V v, const char *k) {
  V *p = rk(v, k);
  if (!p || p->t != V_STR) return NULL;
  return arena_strndup(&ar, p->u.s.p, (size_t)(p->u.s.len > 0 ? p->u.s.len : 0));
}
static const char *errmsg(V v) {
  if (!v_is_err(v) || !v.u.e) return NULL;
  return arena_strndup(&ar, v.u.e->msg.p, (size_t)(v.u.e->msg.len > 0 ? v.u.e->msg.len : 0));
}
/* sh_result_v returns raw bytes, and a Windows child writes CRLF: compare LF form */
static const char *nl(const char *raw) {
  if (!raw) return NULL;
  char *o = arena_strndup(&ar, raw, strlen(raw));
  char *w = o;
  for (char *r = o; *r; r++) if (*r != '\r') *w++ = *r;
  *w = 0;
  return o;
}
static const char *out_of(V v) { return nl(str(v, "out")); }
static bool is_err(V v, ErrCode e) { return v_is_err(v) && v_errcode(v) == e; }
static bool truth(V v, const char *k) {
  V *p = rk(v, k);
  return p && p->t == V_BOOL && p->u.b;
}

/* ---------- PLAN apply, exactly what PLAN.apply(token) does in a script ------ */
static V apply(V plan, const char *token) {
  V t = (token && *token) ? v_str(s_lit(&ar, token)) : VN;
  V r = op_apply(cx, plan, t);
  if (ctx_has_err(cx)) { V e = ctx_err(cx); ctx_clear_err(cx); return e; }
  return r;
}
static Str ptok(V plan) { return plan.t == V_PLAN ? plan_token(cx, plan.u.pl) : s_null(); }
static const char *tokz(V plan, char *buf, size_t n) {
  Str s = ptok(plan);
  snprintf(buf, n, "%.*s", s.len > 0 ? s.len : 0, s.p ? s.p : "");
  return buf;
}

/* ---------- sandbox ---------- */
static bool write_file(const char *p, const char *body) {
  FILE *f = fopen(p, "wb");
  if (!f) return false;
  fputs(body, f);
  fclose(f);
  return true;
}
static bool file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int count_lines(const char *s) {
  if (!s || !*s) return 0;
  int n = 0;
  for (const char *p = s; *p; p++) if (*p == '\n') n++;
  if (s[strlen(s) - 1] != '\n') n++;
  return n;
}
static bool journal_has(const char *token) {
  if (!token || !*token) return false;
  char path[512];
  snprintf(path, sizeof path, "%s/.vxa/journal.ndjson", ctx_root(cx));
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  char line[1024];
  bool hit = false;
  while (fgets(line, sizeof line, f)) if (strstr(line, token)) { hit = true; break; }
  fclose(f);
  return hit;
}

int main(void) {
  ensure_dir_for("build/tmp_sh_test");
  CHECK(chdir("build/tmp_sh_test") == 0, "cannot chdir into build/tmp_sh_test");
  char root[4096];
  CHECK(getcwd(root, sizeof root) != NULL, "getcwd failed");
  arena_init(&ar, 64u * 1024u * 1024u);
  cx = ctx_new(&ar, root, "test_lib_sh.vxa");
  CK(cx != NULL);
  CK(ctx_root(cx)[0] != 0);                          /* the jail needs an abs root */
  CK(write_file("one.txt", "one\n"));
  CK(write_file("two.txt", "two\n"));
  CK(write_file("marker", "do not remove me\n"));
  /* journal_note() is a no-op while .vxa/journal.ndjson is a DIRECTORY: ensure_dir_for
   * (fsx.c) mkdirs the final component of the path it was given, so the first receipt
   * attempt creates the file's own name as a folder and fopen("ab") then fails. Reported.
   * Pre-creating it as a regular file is what lets this test see the receipts at all. */
  ensure_dir_for(".vxa/keep");
  CK(write_file(".vxa/journal.ndjson", ""));
  rmdir(".vxa/keep");                                /* ensure_dir_for's side effect */
  {
    FILE *f = fopen("l300.txt", "wb");
    CK(f != NULL);
    if (f) {
      for (int i = 1; i <= 300; i++) fprintf(f, "line %d\n", i);
      fclose(f);
    }
  }

  /* ============ 1. table integrity ============ */
  T("table");
  {
    int k = 0;
    const Builtin *t = t_sh(&k);
    CKI(k, 5);
    for (int i = 0; i < k; i++) {
      CHECK(t[i].ns && *t[i].ns && !strcmp(t[i].ns, "sh"), "row %d ns", i);
      CHECK(t[i].name && *t[i].name, "row %d name", i);
      CHECK(t[i].fn != NULL, "row %d fn", i);
      CHECK(t[i].min >= 1 && t[i].min <= t[i].max, "row %d min<=max", i);
      CHECK(t[i].sig && *t[i].sig && strstr(t[i].sig, t[i].name), "row %d sig", i);
      CHECK(t[i].doc && *t[i].doc && !strchr(t[i].doc, '\n'), "row %d one-line doc", i);
      CHECK(t[i].ex && strlen(t[i].ex) <= 80, "row %d ex<=80 (%d)", i,
            t[i].ex ? (int)strlen(t[i].ex) : -1);
      CHECK(t[i].ex && strstr(t[i].ex, t[i].name) != NULL, "row %d ex uses its own builtin", i);
    }
    CKS(t[0].name, "run");
    CKS(t[1].name, "ro");
    CKS(t[2].name, "allowed");
    CKS(t[3].name, "out");
    CKS(t[4].name, "jobs");
  }

  /* ============ 2. a plain argv command: PLAN -> token -> ran ============ */
  T("run is gated by default");
  {
    V p = one("run", AV(2, "echo", "hi"));
    CK(p.t == V_PLAN);
    n_run += 0;
    char tok[24];
    tokz(p, tok, sizeof tok);
    CK(strlen(tok) == 12);
    CK(!journal_has(tok));                           /* nothing has run yet */
    Rec *pr = rec_new(&ar);
    if (p.t == V_PLAN) plan_to_v(cx, p.u.pl, pr);
    V pv = rec_to_v(pr);
    CKS(str(pv, "op"), "sh.run");
    CKS(str(pv, "confirm"), "ask");
    CKS(str(pv, "token"), tok);
    CK(num(pv, "count") == 0);                       /* an exec plan touches no files */
    V e0 = apply(p, "");
    CK(is_err(e0, E_NEED_CONFIRM));
    CK(!journal_has(tok));
    V e1 = apply(p, "0123456789ab");
    CK(is_err(e1, E_BAD_TOKEN));
    V r = apply(p, tok);
    n_run++;
    CK(!v_is_err(r));
    CKI(num(r, "code"), 0);
    CKS(out_of(r), "hi\n");
    CKS(str(r, "token"), tok);
    CK(journal_has(tok));                            /* the apply receipt */
  }

  /* ============ 3. the fast path: provably read-only argv runs at once ==== */
  T("read-only argv auto-runs");
  {
    V r = one("run", AV(2, "cat", "one.txt"));
    CK(r.t == V_REC);                                /* a result, not a PLAN */
    CK(!v_is_err(r));
    CKI(num(r, "code"), 0);
    CKS(out_of(r), "one\n");
    CKS(str(r, "confirm"), "none");
    CKS(str(r, "op"), "sh.run");
    CK(truth(r, "auto"));
    n_run++;
    const char *tok = str(r, "token");
    CK(tok && strlen(tok) == 12);
    CK(journal_has(tok));                            /* CF_NONE still leaves a receipt */
    /* the receipt must not turn the fast path into a one-shot: the same read-only
     * command has to keep working (its token is stable by design) */
    V r2 = one("run", AV(2, "cat", "one.txt"));
    CK(r2.t == V_REC);
    CKI(num(r2, "code"), 0);
    CKS(out_of(r2), "one\n");
    CKS(str(r2, "token"), tok);                      /* deterministic */
  }

  /* ============ 4. a string command is NEVER auto-approved ================= */
  T("string form is always gated");
  {
    V p = one("run", S("cat one.txt"));              /* allowlisted as argv... */
    CK(p.t == V_PLAN);                               /* ...gated as a string */
    V a = one("allowed", S("cat one.txt"));
    CK(a.t == V_BOOL && a.u.b);                      /* the table itself says yes */
    V q = one("run", S("echo hi > echo-ran"));
    CK(q.t == V_PLAN);
    CK(!file_exists("echo-ran"));                    /* planning ran nothing */
    char tk[24];
    tokz(q, tk, sizeof tk);
    V rr = apply(q, tk);
    n_run++;
    CK(!v_is_err(rr));
    CK(file_exists("echo-ran"));                     /* only the token let it run */
    remove("echo-ran");
  }

  T("ls; rm -rf cannot sneak through");
  {
    CK(file_exists("marker"));
    V p = one("run", S("ls; rm -rf marker"));
    CK(p.t == V_PLAN);                               /* one string, shell semantics */
    CK(file_exists("marker"));
    V al = one("allowed", S("ls; rm -rf marker"));
    CK(al.t == V_BOOL && !al.u.b);
    V e = one("ro", S("ls; rm -rf marker"));
    CK(is_err(e, E_POLICY));
    CK(file_exists("marker"));
    V ev = one("ro", AV(2, "cat", "a;b"));           /* metachar in an ARGUMENT too */
    CK(is_err(ev, E_POLICY));
    CK(file_exists("marker"));
  }

  /* ============ 5. argv fidelity: a space stays inside one argument ======== */
  T("argv element with a space");
  {
    V p = one("run", AV(2, "echo", "a b"));
    CK(p.t == V_PLAN);
    char tk[24];
    tokz(p, tk, sizeof tk);
    V r = apply(p, tk);
    n_run++;
    CK(!v_is_err(r));
    CKI(num(r, "code"), 0);
    CKS(out_of(r), "a b\n");                         /* a shell/naive join adds quotes */
    CKI(num(r, "total_lines"), 1);
  }

  /* ============ 6. sh.ro: allowlist or refuse, nothing in between ========== */
  T("sh.ro");
  {
    V r = one("ro", AV(2, "cat", "one.txt"));
    CK(!v_is_err(r));
    CKI(num(r, "code"), 0);
    CKS(out_of(r), "one\n");
    CK(truth(r, "ro"));
    CKS(str(r, "confirm"), "none");
    n_run++;
    V v = one("ro", AV(2, "cc", "--version"));       /* the compiler version probe */
    CK(!v_is_err(v));
    n_run++;
    CKI(num(v, "code"), 0);
    CK(str(v, "out") && strlen(out_of(v)) > 10);    /* the compiler banner is there */
    V e = one("ro", AV(2, "git", "push"));
    CK(is_err(e, E_POLICY));
    /* sh.ro runs at once, so it is one of the paths that can honour stdin */
    V si = two("ro", AV(2, "cat", "-"), v_rec(&ar, 1, "stdin", S("via ro\n")));
    CK(!v_is_err(si));
    CKS(out_of(si), "via ro\n");
    n_run++;
    CK(errmsg(e) && strstr(errmsg(e), "sh.run") != NULL);
    /* git --version IS in shell.c's table now (the toolchain-probe block), so ro
     * answers it instead of taxing an agent's "what is installed" step */
    V gv = one("ro", AV(2, "git", "--version"));
    CK(!v_is_err(gv));
    CKI(num(gv, "code"), 0);
    CK(truth(gv, "ro"));
    CKS(str(gv, "op"), "sh.ro");
    CK(out_of(gv) && strstr(out_of(gv), "git version") != NULL);
    n_run++;
    /* and the probe row stays exact: a bare git names no probe at all */
    CK(is_err(one("ro", AV(1, "git")), E_POLICY));
    CK(is_err(one("ro", AV(1, "./cat")), E_POLICY));         /* a path is never ro */
    CK(is_err(one("ro", AV(1, ".")), E_POLICY));
    CK(is_err(one("ro", S("cat one.txt; rm -rf marker")), E_POLICY));
    CK(is_err(one("ro", v_num(5)), E_TYPE));
    CK(file_exists("marker"));
    /* an auto-approved ro run leaves a receipt too */
    const char *tk = str(r, "token");
    CK(tk && journal_has(tk));
  }

  /* ============ 7. sh.allowed = the verdict sh.run/sh.ro will act on ====== */
  T("sh.allowed");
  {
    V a1 = one("allowed", AV(2, "cat", "one.txt"));
    CK(a1.t == V_BOOL && a1.u.b);
    V a2 = one("allowed", AV(2, "git", "status"));
    CK(a2.t == V_BOOL && a2.u.b);
    V a3 = one("allowed", AV(2, "git", "push"));
    CK(a3.t == V_BOOL && !a3.u.b);
    V a4 = one("allowed", AV(2, "echo", "hi"));
    CK(a4.t == V_BOOL && !a4.u.b);
    V a5 = one("allowed", S("echo hi"));
    CK(a5.t == V_BOOL && !a5.u.b);
    V a6 = one("allowed", AV(1, "ls"));
    CK(a6.t == V_BOOL && a6.u.b);
    V a7 = one("allowed", AV(2, "sed", "-i"));       /* in-place edit: guarded */
    CK(a7.t == V_BOOL && !a7.u.b);
    V a8 = one("allowed", AV(3, "git", "status", "--porcelain"));
    CK(a8.t == V_BOOL && a8.u.b);
    CK(is_err(one("allowed", v_num(1)), E_TYPE));
  }

  /* ============ 8. sh.out: a bare string; a bad exit code is data ========= */
  T("sh.out");
  {
    V s = one("out", AV(2, "cat", "one.txt"));
    CK(s.t == V_STR);
    CK(!v_is_err(s));
    CKS(nl(s.u.s.p), "one\n");
    V p = one("out", S("echo hi"));
    CK(p.t == V_PLAN);                               /* out cannot bypass confirmation */
    V f = one("run", AV(2, "cat", "nope-does-not-exist.txt"));
    CK(f.t == V_REC);
    CK(!v_is_err(f));                                /* non-zero exit is not an ERR */
    CK(num(f, "code") != 0);
    n_run++;
    V fs = one("out", AV(2, "cat", "nope-does-not-exist.txt"));
    CK(fs.t == V_STR);
    CK(!v_is_err(fs));
    CKI(fs.u.s.len, 0);
    n_run++;
    V big = one("out", AV(2, "cat", "l300.txt"));
    CK(big.t == V_STR);
    CKI(count_lines(nl(big.u.s.p)), 200);            /* sh.out's own default tail */
  }

  /* ============ 9. timeout ================================================ */
  T("timeout");
  {
    V r = two("run", AV(2, "sleep", "5"),
              v_rec(&ar, 2, "timeout", v_num(1), "confirm", S("none")));
    n_run++;
    CK(!v_is_err(r));
    CK(r.t == V_REC);
    CK(truth(r, "timed_out"));
    CK(num(r, "ms") >= 900);
    CK(num(r, "code") != 0);
    CK(is_err(two("run", AV(2, "echo", "x"), v_rec(&ar, 1, "timeout", v_num(0))), E_RANGE));
    CK(is_err(two("run", AV(2, "echo", "x"), v_rec(&ar, 1, "timeout", v_num(-3))), E_RANGE));
    CK(is_err(two("run", AV(2, "echo", "x"), v_rec(&ar, 1, "timeout", v_num(3601))), E_RANGE));
    V band = two("run", AV(2, "echo", "x"), v_rec(&ar, 1, "timeout", v_num(3600)));
    CK(band.t == V_PLAN);                            /* the top of the band is usable */
    CK(is_err(two("run", AV(2, "echo", "x"), v_rec(&ar, 1, "timeout", S("30"))), E_TYPE));
    V hint = two("run", AV(2, "echo", "x"), v_rec(&ar, 1, "timeout", v_num(0)));
    CK(errmsg(hint) && strstr(errmsg(hint), "3600") != NULL);   /* states the band */
  }

  /* ============ 10. tail shaping ========================================== */
  T("tail");
  {
    V d = one("run", AV(2, "cat", "l300.txt"));
    CK(!v_is_err(d));
    n_run++;
    CKI(num(d, "total_lines"), 300);
    CK(truth(d, "truncated"));
    CKI(count_lines(out_of(d)), 40);                 /* the documented default */
    V t10 = two("run", AV(2, "cat", "l300.txt"), v_rec(&ar, 1, "tail", v_num(10)));
    CKI(num(t10, "total_lines"), 300);
    CK(truth(t10, "truncated"));
    n_run++;
    CKI(count_lines(out_of(t10)), 10);
    CK(strstr(out_of(t10), "line 300") != NULL);
    CK(strstr(out_of(t10), "line 291") != NULL);
    CK(strstr(out_of(t10), "line 290") == NULL);
    V all = two("run", AV(2, "cat", "l300.txt"), v_rec(&ar, 1, "tail", v_num(0)));
    CK(!truth(all, "truncated"));                    /* tail:0 = unlimited lines */
    CKI(num(all, "total_lines"), 300);
    n_run++;
    CKI(count_lines(out_of(all)), 300);
    CK(is_err(two("run", AV(2, "cat", "l300.txt"), v_rec(&ar, 1, "tail", v_num(-1))), E_RANGE));
  }

  /* ============ 11. options that cannot be honoured are refused =========== */
  T("options");
  {
    V e = two("run", AV(2, "cat", "one.txt"), v_rec(&ar, 1, "env", v_rec(&ar, 0)));
    CK(is_err(e, E_UNSUPPORTED));
    CK(errmsg(e) && strstr(errmsg(e), "env") != NULL);
    V e2 = two("ro", AV(2, "cat", "one.txt"),
               v_rec(&ar, 1, "env", v_rec(&ar, 2, "PATH", S("/bin"))));
    CK(is_err(e2, E_UNSUPPORTED));
    CK(is_err(two("run", AV(2, "cat", "one.txt"), v_rec(&ar, 1, "tiemout", v_num(5))), E_BAD_INPUT));
    V ue = two("run", AV(2, "cat", "one.txt"), v_rec(&ar, 1, "tiemout", v_num(5)));
    CK(errmsg(ue) && strstr(errmsg(ue), "timeout") != NULL);    /* names the real keys */
    CK(is_err(two("run", AV(2, "cat", "one.txt"), v_rec(&ar, 1, "confirm", S("maybe"))), E_BAD_INPUT));
    CK(is_err(two("run", AV(2, "cat", "one.txt"), v_num(7)), E_TYPE));
    /* confirm=none is the caller's own opt-out, confirm=all is the strictest */
    V n = two("run", AV(2, "echo", "hi"), v_rec(&ar, 1, "confirm", S("none")));
    n_run++;
    CK(n.t == V_REC && !v_is_err(n));
    CKI(num(n, "code"), 0);
    V aa = two("run", AV(2, "cat", "one.txt"), v_rec(&ar, 1, "confirm", S("all")));
    CK(aa.t == V_PLAN);                              /* audit mode: even ro asks */
    /* stdin reaches the child on a run that executes at once, and is refused when
     * the command has to be confirmed (plan.c passes no stdin to its child) */
    V si = two("run", AV(2, "cat", "-"), v_rec(&ar, 1, "stdin", S("piped\n")));
    n_run++;
    CK(!v_is_err(si));
    CKS(out_of(si), "piped\n");
    V sg = two("run", S("cat -"), v_rec(&ar, 1, "stdin", S("piped\n")));
    CK(is_err(sg, E_UNSUPPORTED));
    CK(is_err(two("ro", AV(2, "cat", "one.txt"), v_rec(&ar, 1, "cwd", S("../.."))), E_OUTSIDE_JAIL));
    V cw = two("run", AV(2, "cat", "one.txt"), v_rec(&ar, 1, "cwd", S(".")));
    n_run++;
    CK(cw.t == V_REC && num(cw, "code") == 0);
  }

  /* ============ 12. sh.jobs =============================================== */
  T("sh.jobs runs in needs order");
  {
    V jobs = v_list(&ar, 3,
                    v_rec(&ar, 3, "name", S("c"), "cmd", AV(2, "cat", "two.txt"),
                          "needs", v_list(&ar, 1, S("b"))),
                    v_rec(&ar, 3, "name", S("b"), "cmd", AV(2, "cat", "one.txt"),
                          "needs", v_list(&ar, 1, S("a"))),
                    v_rec(&ar, 2, "name", S("a"), "cmd", AV(2, "cat", "one.txt")));
    V r = one("jobs", jobs);
    CK(!v_is_err(r));
    CK(r.t == V_REC);
    CKI(num(r, "ran"), 3);
    CKI(num(r, "planned"), 3);
    CK(truth(r, "auto"));
    CKS(str(r, "op"), "sh.jobs");
    CKS(str(r, "confirm"), "none");
    n_run += 3;
    V *dn = rk(r, "done");
    CK(dn && dn->t == V_LIST && dn->u.l->len == 3);
    if (dn && dn->t == V_LIST && dn->u.l->len == 3) {
      CKS(str(dn->u.l->v[0], "name"), "a");
      CKS(str(dn->u.l->v[1], "name"), "b");
      CKS(str(dn->u.l->v[2], "name"), "c");
      CKI(num(dn->u.l->v[0], "code"), 0);
      CK(rk(dn->u.l->v[0], "ms") != NULL);
      CK(rk(dn->u.l->v[0], "out") == NULL);          /* a done entry stays cheap */
    }
    V *fl = rk(r, "failed");
    CK(fl && fl->t == V_LIST && fl->u.l->len == 0);
    CK(rk(r, "first_fail") == NULL);
    CK(truth(r, "jobs_sequential"));                 /* jobs>1 is not parallel yet */
    V r4 = two("jobs", jobs, v_rec(&ar, 1, "jobs", v_num(4)));
    CK(!v_is_err(r4));
    CKI(num(r4, "jobs"), 4);
    CKI(num(r4, "ran"), 3);
    n_run += 3;
  }

  T("sh.jobs failure stops, all:true continues");
  {
    V jobs = v_list(&ar, 3,
                    v_rec(&ar, 2, "name", S("a"), "cmd", AV(2, "cat", "one.txt")),
                    v_rec(&ar, 2, "name", S("b"), "cmd", AV(2, "cat", "nope.txt")),
                    v_rec(&ar, 2, "name", S("c"), "cmd", AV(2, "cat", "two.txt")));
    V r = one("jobs", jobs);
    CK(!v_is_err(r));
    CKI(num(r, "ran"), 2);                           /* c never started */
    V *dn = rk(r, "done");
    CK(dn && dn->t == V_LIST && dn->u.l->len == 1);
    V *fl = rk(r, "failed");
    CK(fl && fl->t == V_LIST && fl->u.l->len == 1);
    if (fl && fl->t == V_LIST && fl->u.l->len) {
      CKS(str(fl->u.l->v[0], "name"), "b");
      CK(num(fl->u.l->v[0], "code") != 0);
      CK(str(fl->u.l->v[0], "err") != NULL);         /* the reason, not just the code */
    }
    CKS(str(r, "first_fail"), "b");
    n_run += 2;
    V ra = two("jobs", jobs, v_rec(&ar, 1, "all", v_bool(true)));
    CK(!v_is_err(ra));
    CKI(num(ra, "ran"), 3);
    CKI(num(ra, "planned"), 3);
    V *fl2 = rk(ra, "failed");
    CK(fl2 && fl2->t == V_LIST && fl2->u.l->len == 1);
    CKS(str(ra, "first_fail"), "b");
    n_run += 3;
  }

  T("sh.jobs bad graphs");
  {
    V cyc = v_list(&ar, 2,
                   v_rec(&ar, 3, "name", S("x"), "cmd", AV(2, "cat", "one.txt"),
                         "needs", v_list(&ar, 1, S("y"))),
                   v_rec(&ar, 3, "name", S("y"), "cmd", AV(2, "cat", "one.txt"),
                         "needs", v_list(&ar, 1, S("x"))));
    V e = one("jobs", cyc);
    CK(is_err(e, E_BAD_INPUT));
    const char *m = errmsg(e);
    CK(m && strstr(m, "cycle") != NULL);
    CK(m && strstr(m, "x") != NULL && strstr(m, "y") != NULL);
    V self = v_list(&ar, 1, v_rec(&ar, 3, "name", S("z"), "cmd", AV(2, "cat", "one.txt"),
                                  "needs", v_list(&ar, 1, S("z"))));
    CK(is_err(one("jobs", self), E_BAD_INPUT));
    V ghost = v_list(&ar, 1, v_rec(&ar, 3, "name", S("q"), "cmd", AV(2, "cat", "one.txt"),
                                   "needs", v_list(&ar, 1, S("nope"))));
    V eg = one("jobs", ghost);
    CK(is_err(eg, E_BAD_INPUT));
    CK(errmsg(eg) && strstr(errmsg(eg), "nope") != NULL);
    V dup = v_list(&ar, 2, v_rec(&ar, 2, "name", S("d"), "cmd", AV(2, "cat", "one.txt")),
                   v_rec(&ar, 2, "name", S("d"), "cmd", AV(2, "cat", "two.txt")));
    CK(is_err(one("jobs", dup), E_BAD_INPUT));
    CK(is_err(one("jobs", v_list(&ar, 1, v_rec(&ar, 1, "name", S("n")))), E_BAD_INPUT));
    CK(is_err(one("jobs", v_list(&ar, 1, S("just a string"))), E_TYPE));
    CK(is_err(one("jobs", v_list(&ar, 0)), E_BAD_INPUT));
    CK(is_err(one("jobs", v_num(3)), E_TYPE));
    CK(is_err(two("jobs", v_list(&ar, 1, v_rec(&ar, 2, "name", S("n"),
                                                 "cmd", AV(2, "cat", "one.txt"))),
                        v_rec(&ar, 1, "jobs", v_num(0))), E_RANGE));
    CK(is_err(one("jobs", v_list(&ar, 1, v_rec(&ar, 3, "name", S("n"),
                                               "cmd", AV(2, "cat", "one.txt"),
                                               "timeout", v_num(0)))), E_RANGE));
    V noexec = v_list(&ar, 1, v_rec(&ar, 3, "name", S("n"), "cmd", AV(2, "cat", "one.txt"),
                                    "needs", v_list(&ar, 0)));
    CK(!v_is_err(one("jobs", noexec)));
    n_run++;
  }

  T("sh.jobs second phase");
  {
    V jobs = v_list(&ar, 1, v_rec(&ar, 2, "name", S("w"), "cmd", S("echo hi > wrote.txt")));
    V p = one("jobs", jobs);
    CK(!v_is_err(p));
    CK(p.t == V_REC);                                /* plan fields, not a run */
    CKS(str(p, "op"), "sh.jobs");
    CKS(str(p, "confirm"), "ask");
    CK(rk(p, "ran") == NULL);
    CK(!file_exists("wrote.txt"));
    const char *tk = str(p, "token");
    CK(tk && strlen(tk) == 12);
    V *ord = rk(p, "order");
    CK(ord && ord->t == V_LIST && ord->u.l->len == 1);
    if (ord && ord->t == V_LIST && ord->u.l->len)
      CKS(str(ord->u.l->v[0], "name"), "w");
    CK(errmsg(p) == NULL);
    V *h = rk(p, "hint");
    CK(h && h->t == V_STR && h->u.s.p && strstr(h->u.s.p, "token") != NULL);
    V bad = two("jobs", jobs, v_rec(&ar, 1, "token", S("ffffffffffff")));
    CK(is_err(bad, E_BAD_TOKEN));
    CK(!file_exists("wrote.txt"));
    V ok = two("jobs", jobs, v_rec(&ar, 1, "token", S(tk)));
    n_run++;
    CK(!v_is_err(ok));
    CKI(num(ok, "ran"), 1);
    CK(file_exists("wrote.txt"));                    /* the token is what let it run */
    CKS(str(ok, "token"), tk);
    V changed = v_list(&ar, 2,
                       v_rec(&ar, 2, "name", S("w"), "cmd", S("echo hi > wrote.txt")),
                       v_rec(&ar, 2, "name", S("v"), "cmd", AV(2, "cat", "one.txt")));
    CK(is_err(two("jobs", changed, v_rec(&ar, 1, "token", S(tk))), E_BAD_TOKEN));
    remove("wrote.txt");
  }

  /* ============ 13. determinism =========================================== */
  T("determinism");
  {
    V a = one("run", S("echo hi"));
    V b = one("run", S("echo hi"));
    char t1[24], t2[24], t3[24], t4[24];
    tokz(a, t1, sizeof t1);
    tokz(b, t2, sizeof t2);
    CKS(t1, t2);
    CK(strlen(t1) == 12);
    tokz(two("run", S("echo hi"), v_rec(&ar, 1, "tail", v_num(7))), t3, sizeof t3);
    CK(strcmp(t1, t3) != 0);                         /* tail is inside the token */
    tokz(two("run", S("echo hi"), v_rec(&ar, 1, "timeout", v_num(11))), t4, sizeof t4);
    CK(strcmp(t1, t4) != 0);                         /* and so is the timeout */
    CK(strcmp(t3, t4) != 0);
    V r = one("run", AV(2, "cat", "one.txt"));
    n_run++;
    CK(r.t == V_REC);
    if (r.t == V_REC) {
      for (int i = 0; i < r.u.r->len; i++) {
        const char *k = arena_strndup(&ar, r.u.r->kv[i].k.p,
                                      (size_t)(r.u.r->kv[i].k.len > 0 ? r.u.r->kv[i].k.len : 0));
        CHECK(strcmp(k, "t") && strcmp(k, "time") && strcmp(k, "ts") && strcmp(k, "date")
                && strcmp(k, "at") && strcmp(k, "when"),
              "no timestamp key allowed, found %s", k);
      }
      V *pm = rk(r, "ms");
      CK(pm && pm->t == V_NUM);                      /* ms durations are the exception */
    }
    /* a plan is inert until confirmed: two inspections of the same plan agree */
    V p = one("run", S("echo stable"));
    char s1[24], s2[24];
    tokz(p, s1, sizeof s1);
    tokz(p, s2, sizeof s2);
    CKS(s1, s2);
    CK(p.t == V_PLAN);
  }

  printf("commands this test let run: %d\n", n_run);
  T_REPORT("lib_sh");
}
