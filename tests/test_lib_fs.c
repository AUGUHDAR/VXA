/* test_lib_fs.c - the fs namespace, asserted against the disk.
 * What is load-bearing here is not string matching but the protocol:
 *   - a read says how much it hid (truncated / shown / total)
 *   - a write never touches the disk until a token signs it
 *   - the token is bound to the old content hash, so a moved world STALEs
 *   - a consumed token replays instead of rewriting
 *   - writes are jailed, reads deliberately are not
 * Every file is created inside build/tmp_fs_test/ and removed again.
 */
#include "vxa.h"
#include "interp.h"
#include "lib.h"
#include "t.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ===================================================================== */
/* NAMESPACE STUBS - build.sh links this suite as core + src/lib_fs.c     */
/* only (see deps(lib_fs)): the tx and sh namespaces are tested by their  */
/* own suites, and pulling them in here would need their ctx, so lib.c is */
/* handed the two tables it cannot see and lib_method the plan dispatch   */
/* this suite exercises. Everything else runs against the real code:      */
/* util, val, fsx, plan, shell, outline, xdiff, sim, interp, lib.         */
/* ===================================================================== */
#ifndef VX_NO_FS_TEST_STUBS

const Builtin *t_tx(int *n) { if (n) *n = 0; return NULL; }
const Builtin *t_sh(int *n) { if (n) *n = 0; return NULL; }

/* interp.c routes x.apply(tok) / x.files here; src/lib_tx.c owns it and is not
 * in this link set, so the plan path is dispatched the way the evaluator would */
V lib_method(Ctx *c, V self, const char *name, V *args, int nargs) {
  if (self.t == V_PLAN && !strcmp(name, "apply"))          /* SPEC 3.5: p.apply([tok]) */
    return op_apply(c, self, nargs > 0 ? args[0] : VN);
  set_error(c, E_UNSUPPORTED, "lib_method stub: .%s is unavailable in this link set", name);
  return VN;
}

#endif /* VX_NO_FS_TEST_STUBS */

/* ===================================================================== */
/* fixture                                                               */
/* ===================================================================== */

static Arena A;
static Ctx *C = NULL;
static char ROOT[4096];

static void rmrf(const char *d) {
  DIR *dp = opendir(d);
  if (!dp) return;
  struct dirent *en;
  while ((en = readdir(dp))) {
    if (!strcmp(en->d_name, ".") || !strcmp(en->d_name, "..")) continue;
    char p[4096];
    snprintf(p, sizeof p, "%s/%s", d, en->d_name);
    if (fs_is_dir(p)) rmrf(p); else remove(p);
  }
  closedir(dp);
  rmdir(d);
}
/* the scratch dir holds nothing but what this suite made, so wiping it by name
 * is safe and it is the only cleanup that also drops .vxa/journal.ndjson       */
static void wipe_scratch(void) {
  DIR *dp = opendir(".");
  if (!dp) return;
  struct dirent *en;
  while ((en = readdir(dp))) {
    if (!strcmp(en->d_name, ".") || !strcmp(en->d_name, "..")) continue;
    if (fs_is_dir(en->d_name)) rmrf(en->d_name); else remove(en->d_name);
  }
  closedir(dp);
}

static void enter_scratch(void) {
  const char *cands[] = { "build/tmp_fs_test", "tmp_fs_test", "../build/tmp_fs_test" };
  for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
    ensure_dir_for(cands[i]);
    if (!chdir(cands[i])) {
      if (!getcwd(ROOT, sizeof ROOT)) { printf("FATAL: no cwd\n"); exit(2); }
      C = ctx_new(&A, ROOT, "test_lib_fs.vxa");
      return;
    }
  }
  printf("FATAL: cannot enter build/tmp_fs_test\n");
  exit(2);
}

/* fsx.c's ensure_dir_for() also creates the LEAF, so it is only correct for
 * directory paths; for a file path it would make a directory named after the
 * file and the write would then fail. This makes the parents only. */
static void mkdir_parents(const char *p) {
  char d[4096];
  snprintf(d, sizeof d, "%s", p);
  for (char *s = d + 1; *s; s++) {
    if (*s == '/' || *s == '\\') {
      *s = 0;
      if (*d && !fs_is_dir(d)) ensure_dir_for(d);
      *s = '/';
    }
  }
}

static void mkbytes(const char *p, const char *data, size_t n) {
  mkdir_parents(p);
  FILE *f = fopen(p, "wb");
  if (!f) { printf("FATAL: cannot write %s\n", p); exit(2); }
  if (n) fwrite(data, 1, n, f);
  fclose(f);
}
static void mkfile(const char *p, const char *s) { mkbytes(p, s, strlen(s)); }
static char *rdfile(const char *p) {                       /* static buffer, NULL if absent */
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  static char buf[400000];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = 0;
  fclose(f);
  return (char*)buf;
}
/* 500 lines, >4KB, 3 grep targets. The filler matters: a page of 11 bare
 * "rowN" lines is ~120 bytes, so no max_bytes small enough to be interesting
 * would ever clip it, and the paging assertions below would test nothing. */
static void mkpaged(const char *p) {
  char buf[32768];
  int o = 0;
  for (int i = 1; i <= 500; i++)
    o += snprintf(buf + o, sizeof buf - (size_t)o,
                  (i == 10 || i == 11 || i == 50) ? "HIT%d aaaaa bbbbb\n"
                                                  : "row%d aaaaa bbbbb\n", i);
  mkbytes(p, buf, (size_t)o);
}
static void mkblob(const char *p, int lines) {             /* big enough to hit 8000 */
  char buf[300000];
  int o = 0;
  for (int i = 1; i <= lines; i++)
    o += snprintf(buf + o, sizeof buf - (size_t)o, "line %06d aaaaa bbbbb ccccc\n", i);
  mkbytes(p, buf, (size_t)o);
}

/* ---------- calling builtins exactly the way the evaluator does ---------- */
static V F(const char *nm, V *args, int n) {
  ctx_clear_err(C);
  V r = run_builtin(C, "fs", nm, args, n);
  if (ctx_has_err(C) && !v_is_err(r)) r = ctx_err(C);
  return r;
}
static V F0(const char *nm) { return F(nm, NULL, 0); }
static V F1(const char *nm, const char *a) {
  V ar[1] = { v_strz(&A, a) };
  return F(nm, ar, 1);
}
static V F1n(const char *nm, double x) {
  V ar[1] = { v_num(x) };
  return F(nm, ar, 1);
}
static V F2(const char *nm, const char *a, const char *b) {
  V ar[2] = { v_strz(&A, a), v_strz(&A, b) };
  return F(nm, ar, 2);
}
static V F2o(const char *nm, const char *a, V o) {
  V ar[2] = { v_strz(&A, a), o };
  return F(nm, ar, 2);
}
static V F2v(const char *nm, V a, V o) {
  V ar[2] = { a, o };
  return F(nm, ar, 2);
}
static V F3o(const char *nm, const char *a, const char *b, V o) {
  V ar[3] = { v_strz(&A, a), v_strz(&A, b), o };
  return F(nm, ar, 3);
}

static ErrCode code_of(V v) { return v_is_err(v) ? v_errcode(v) : E_NONE; }
static const char *msg_of(V v) { return (v_is_err(v) && v.u.e) ? v.u.e->msg.p : ""; }
static const char *hint_of(V v) { return (v_is_err(v) && v.u.e) ? v.u.e->hint.p : ""; }
static const char *str_of(V v) { return v.t == V_STR && v.u.s.p ? v.u.s.p : ""; }
static double num_of(V v, double bad) { return v.t == V_NUM ? v.u.n : bad; }
static bool bool_of(V v, bool bad) { return v.t == V_BOOL ? v.u.b : bad; }
static int len_of(V v) { return v.t == V_LIST && v.u.l ? v.u.l->len : (v.t == V_REC && v.u.r ? v.u.r->len : -1); }

static V *kv(V v, const char *k) { return v.t == V_REC ? rec_getz(v.u.r, k) : NULL; }
static const char *ks(V v, const char *k) { V *p = kv(v, k); return p && p->t == V_STR ? str_of(*p) : ""; }
static double kn(V v, const char *k, double bad) { V *p = kv(v, k); return p && p->t == V_NUM ? p->u.n : bad; }
static bool kb(V v, const char *k, bool bad) { V *p = kv(v, k); return p && p->t == V_BOOL ? p->u.b : bad; }
static int kl(V v, const char *k) { V *p = kv(v, k); return p && p->t == V_LIST && p->u.l ? p->u.l->len : -1; }
static V ke(V v, const char *k, int i) {                       /* k[i] as a value */
  V *p = kv(v, k);
  if (!p || p->t != V_LIST || !p->u.l) return VN;
  V *e = list_get(p->u.l, i);
  return e ? *e : VN;
}
static V le(V v, int i) {                                      /* list element i */
  if (v.t != V_LIST || !v.u.l) return VN;
  V *e = list_get(v.u.l, i);
  return e ? *e : VN;
}
static bool has(V v, const char *k) { return kv(v, k) != NULL; }

/* the plan exactly as plan_value() prints it for a NEED_CONFIRM error */
/* the tokens are compared against each other, so each call needs its own copy */
static const char *tok_of(V plan) {
  if (plan.t != V_PLAN) return "";
  Str t = plan_token(C, plan.u.pl);
  char *copy = (char*)arena_alloc(&A, (size_t)t.len + 1);
  if (t.len && t.p) memcpy(copy, t.p, (size_t)t.len);
  copy[t.len] = 0;
  return copy;
}
static V plan_view(V plan) {
  Rec *r = rec_new(&A);
  if (plan.t == V_PLAN) plan_to_v(C, plan.u.pl, r);
  return rec_to_v(r);
}
static V plan_file(V plan, int i) { return ke(plan_view(plan), "files", i); }

static V apply(V plan, const char *tok) {
  ctx_clear_err(C);
  V r = op_apply(C, plan, tok && *tok ? v_strz(&A, tok) : VN);
  if (ctx_has_err(C) && !v_is_err(r)) r = ctx_err(C);
  return r;
}

static int is_hex(const char *p, int n) {
  if (!p || n < 4) return 0;
  for (int i = 0; i < n; i++) if (!isxdigit((unsigned char)p[i])) return 0;
  return 1;
}
static int nlines(const char *s) { int n = 0; for (const char *p = s; *p; p++) if (*p == '\n') n++; return n; }
/* the line numbers fs.read echoed ("L12| x" or "L12:abcd| x"), in order */
static int echoed_idx(const char *text, int *out, int cap) {
  int n = 0;
  for (const char *p = text; *p && n < cap; ) {
    if (*p == 'L' && p[1] >= '0' && p[1] <= '9') out[n++] = (int)strtol(p + 1, NULL, 10);
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
  }
  return n;
}
static int ascending(const int *v, int n) {
  for (int i = 1; i < n; i++) if (v[i] <= v[i - 1]) return 0;
  return 1;
}
static void set_policy(const char *lvl) { rec_setz(&A, ctx_cfg(C), "policies.fs", v_strz(&A, lvl)); }
static void clear_policy(void) { rec_del(&A, ctx_cfg(C), s_wrap("policies.fs")); }

int main(void) {
  arena_init(&A, 0);
  enter_scratch();
  wipe_scratch();

  T("fixture");
  CK(ROOT[0] != 0);
  CK(C != NULL);
  CK(strstr(ROOT, "tmp_fs_test") != NULL);

  /* ---------------- registry table integrity ---------------- */
  T("table_integrity");
  int tn = 0;
  const Builtin *tb = t_fs(&tn);
  CK(tb != NULL);
  CKI(tn, 16);
  for (int i = 0; i < tn; i++) {
    const Builtin *b = &tb[i];
    CK(b->ns && b->ns[0] && !strcmp(b->ns, "fs"));
    CK(b->name && b->name[0]);
    CK(b->fn != NULL);
    CK(b->min <= b->max);
    CK(b->min >= 0);
    CK(b->sig && b->sig[0] && !strchr(b->sig, '\n'));
    CK(b->doc && b->doc[0] && !strchr(b->doc, '\n'));
    CK(b->ex && b->ex[0] && strlen(b->ex) <= 80);
    int dup = 0;
    for (int j = 0; j < i; j++) if (!strcmp(tb[j].name, b->name)) dup = 1;
    CK(!dup);
    const Builtin *found = lib_find("fs", b->name);          /* reachable through the registry */
    CK(found && found->fn == b->fn);
  }

  /* ---------------- the cheap probes ---------------- */
  T("cwd_exists_size_hash");
  V cwdv = F0("cwd");
  CK(cwdv.t == V_STR);
  CKS(str_of(cwdv), ROOT);
  mkfile("probe.txt", "hello");
  CK(bool_of(F1("exists", "probe.txt"), false));
  CK(!bool_of(F1("exists", "gone.txt"), true));
  V hv = F1("hash", "probe.txt");
  CK(hv.t == V_STR);
  CK(is_hex(str_of(hv), 12));
  CKSTR(hash12(&A, "hello", 5), str_of(hv));
  CKI(num_of(F1("size", "probe.txt"), -1), 5);
  CKI(code_of(F1("size", "gone.txt")), E_NOENT);
  CKI(code_of(F1("size", ".")), E_ISDIR);
  CKI(code_of(F1("hash", "gone.txt")), E_NOENT);
  CKI(code_of(F1("size", "")), E_BAD_INPUT);
  remove("probe.txt");

  /* ---------------- jail: writes jailed, reads NOT ---------------- */
  T("jail_is_write_only");
  V esc = F2("write", "../escape.txt", "x");
  CKI(code_of(esc), E_OUTSIDE_JAIL);
  CK(err_exit(E_OUTSIDE_JAIL) == X_POLICY);                 /* exit-worthy code, not prose */
  CK(strstr(msg_of(esc), "escape.txt") != NULL);
  CK(strstr(msg_of(esc), "OUTSIDE_JAIL") != NULL || strstr(hint_of(esc), "workspace") != NULL);
  CK(!fs_exists("../escape.txt"));
  char outside[4096];
  if (ROOT[1] == ':') snprintf(outside, sizeof outside, "%c:/vxa_fs_outside.c", toupper(ROOT[0]));
  else snprintf(outside, sizeof outside, "/vxa_fs_outside.c");
  CKI(code_of(F2("write", outside, "x")), E_OUTSIDE_JAIL);
  CK(!fs_exists(outside));
  CKI(code_of(F1("delete", "../escape.txt")), E_OUTSIDE_JAIL);
  mkfile("in.txt", "x");
  CKI(code_of(F2("move", "in.txt", "../out.txt")), E_OUTSIDE_JAIL);   /* destination is jailed */
  CK(!fs_exists("../out.txt"));
  remove("in.txt");
  /* reads are NOT jailed on purpose (SPEC 5 jails write targets only), so an
   * agent can open the file it was pointed at from outside the workspace     */
  const char *cands[] = { "../../SPEC.md", "../SPEC.md", "../t_lib_fs.exe", "../vxa.exe" };
  const char *outside_file = NULL;
  for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
    if (fs_exists(cands[i])) { outside_file = cands[i]; break; }
  CK(outside_file != NULL);
  if (outside_file) {
    V r = F2o("read", outside_file, v_rec(&A, 2, "lines", v_strz(&A, "1-2"), "max_bytes", v_num(200)));
    CK(code_of(r) == E_NONE);                                /* allowed, not OUTSIDE_JAIL */
    CK(!strcmp(ks(r, "path"), outside_file));
    CK(kn(r, "total", 0) > 0);
    CK(is_hex(ks(r, "hash"), 12));
    CK(is_hex(str_of(F1("hash", outside_file)), 12));
    CK(num_of(F1("size", outside_file), -1) > 0);
    CK(bool_of(F1("exists", outside_file), false));
  }
  CKI(code_of(F1("read", "../nothing/here.txt")), E_NOENT);  /* NOENT, never OUTSIDE_JAIL */
  wipe_scratch();

  /* ---------------- fs.read paging ---------------- */
  T("read_paging");
  mkpaged("page.txt");
  long pbytes = fs_size("page.txt");
  CK(pbytes > 4000);
  V r = F2o("read", "page.txt", v_rec(&A, 1, "lines", v_strz(&A, "10-20")));
  CK(code_of(r) == E_NONE);
  CK(!strcmp(ks(r, "path"), "page.txt"));                    /* echoes the asked name */
  CK(!strcmp(ks(r, "lines"), "10-20"));
  CKI(kn(r, "total", -1), 500);
  CKI(kn(r, "shown", -1), 11);                               /* 10-20 is 11 lines */
  CKI(kn(r, "bytes", -1), pbytes);
  CK(!kb(r, "truncated", true));                             /* a whole page is not a clip */
  const char *txt = ks(r, "text");
  CKI(nlines(txt), 11);
  CK(!strncmp(txt, "L10| HIT10", 10));                       /* reads always say which line */
  CK(strstr(txt, "L20| row20") != NULL);
  CK(is_hex(ks(r, "hash"), 12));
  CK(kn(r, "est", 0) > 0 && kn(r, "est", 9999) < kn(r, "bytes", 0));
  CK(has(r, "shown") && has(r, "total") && has(r, "truncated"));

  T("read_byte_cap");
  V c = F2o("read", "page.txt", v_rec(&A, 2, "lines", v_strz(&A, "10-20"), "max_bytes", v_num(200)));
  CK(code_of(c) == E_NONE);
  CK(kb(c, "truncated", false));
  CK((long)strlen(ks(c, "text")) <= 200);
  CK(kn(c, "shown", 99) < 11 && kn(c, "shown", 0) > 0);
  CKI(kn(c, "total", -1), 500);                              /* still says the file is 500 long */
  mkblob("blob.txt", 3000);
  V big = F2o("read", "blob.txt", v_rec(&A, 1, "lines", v_strz(&A, "1-")));
  CK(kb(big, "truncated", false));                           /* default 8000 clamp is visible */
  CK((long)strlen(ks(big, "text")) <= 8000);
  CK(kn(big, "shown", 0) > 10 && kn(big, "shown", 9999) < 3000);
  CKI(kn(big, "total", -1), 3000);
  remove("blob.txt");

  T("read_grep_ctx");
  V g = F2o("read", "page.txt", v_rec(&A, 2, "grep", v_strz(&A, "HIT"), "ctx", v_num(1)));
  CK(code_of(g) == E_NONE);
  CKI(kn(g, "shown", -1), 7);                                /* {9..12} U {49..51}, 10/11 overlap */
  int idx[64];
  int gn = echoed_idx(ks(g, "text"), idx, 64);
  CKI(gn, 7);
  CK(ascending(idx, gn));                                    /* no duplicated window lines */
  CKI(idx[0], 9);
  CKI(idx[2], 11);
  CKI(idx[3], 12);
  CKI(idx[4], 49);
  CK(strstr(ks(g, "text"), "L11| HIT11") != NULL);
  CK(strstr(ks(g, "text"), "L12| row12") != NULL);           /* neighbour, not a match */
  V g0 = F2o("read", "page.txt", v_rec(&A, 1, "grep", v_strz(&A, "HIT")));
  CKI(kn(g0, "shown", -1), 3);                               /* ctx defaults to 0: matches only */
  V gcase = F2o("read", "page.txt", v_rec(&A, 1, "grep", v_strz(&A, "hit")));
  CKI(kn(gcase, "shown", -1), 3);                            /* grep ignores case */
  V gnone = F2o("read", "page.txt", v_rec(&A, 1, "grep", v_strz(&A, "zzzz")));
  CKI(kn(gnone, "shown", -1), 0);
  CK(!kb(gnone, "truncated", true));

  T("read_anchors");
  V an = F2o("read", "page.txt", v_rec(&A, 2, "lines", v_strz(&A, "12"), "anchors", VT));
  CK(code_of(an) == E_NONE);
  const char *al = ks(an, "text");
  CK(!strncmp(al, "L12:", 4));                               /* L<n>:<at>| content */
  const char *bar = strstr(al, "| ");
  CK(bar != NULL);
  if (bar) {
    CK(is_hex(al + 4, (int)(bar - (al + 4))));               /* anchor hash, 8 hex in the core */
    CK(!strcmp(bar + 2, "row12 aaaaa bbbbb\n"));             /* byte-exact line incl. terminator */
    CK(bar[1] == ' ');
  }
  V an2 = F2o("read", "page.txt", v_rec(&A, 2, "lines", v_strz(&A, "10-11"), "anchors", VT));
  CK(!strncmp(ks(an2, "text"), "L10:", 4));
  CKI(nlines(ks(an2, "text")), 2);
  CK(strstr(ks(an2, "text"), "| HIT10") != NULL);

  T("read_errors_and_ranges");
  CKI(code_of(F1("read", "gone.txt")), E_NOENT);
  CKI(code_of(F1("read", ".")), E_ISDIR);
  CKI(code_of(F2o("read", "page.txt", v_rec(&A, 1, "lines", v_strz(&A, "abc")))), E_BAD_INPUT);
  CKI(code_of(F2o("read", "page.txt", v_rec(&A, 1, "lines", v_strz(&A, "20-10")))), E_BAD_INPUT);
  CKI(code_of(F2o("read", "page.txt", v_rec(&A, 1, "lines", v_strz(&A, "0-3")))), E_BAD_INPUT);
  CKI(code_of(F2o("read", "page.txt", v_rec(&A, 1, "max_bytes", v_strz(&A, "200")))), E_TYPE);
  CKI(code_of(F2o("read", "page.txt", v_rec(&A, 1, "ctx", v_strz(&A, "2")))), E_TYPE);
  CKI(code_of(F2o("read", "page.txt", v_rec(&A, 1, "grep", v_num(3)))), E_TYPE);
  CKI(code_of(F2o("read", "page.txt", v_rec(&A, 1, "max_bytes", v_num(-5)))), E_RANGE);
  V one = F2o("read", "page.txt", v_rec(&A, 1, "lines", v_num(500)));
  CKI(kn(one, "shown", -1), 1);
  CK(!strcmp(ks(one, "lines"), "500-500"));
  V head = F2o("read", "page.txt", v_rec(&A, 2, "lines", v_strz(&A, "-3"), "max_bytes", v_num(4000)));
  CKI(kn(head, "shown", -1), 3);
  /* 9 bytes: the echoed number, the content, and the separator after it */
  CK(!strncmp(ks(head, "text"), "L1| row1 ", 9));
  V bt = F2o("read", "page.txt", v_rec(&A, 1, "max_tok", v_num(10)));
  CK((long)strlen(ks(bt, "text")) <= 40);                   /* max_tok prices bytes at 4/token */
  V both = F2o("read", "page.txt", v_rec(&A, 2, "max_tok", v_num(10), "max_bytes", v_num(200)));
  CK((long)strlen(ks(both, "text")) <= 200);                /* max_bytes wins over max_tok */
  remove("page.txt");

  /* ---------------- fs.check ---------------- */
  T("check_lf");
  /* 14 bytes of text: mkbytes takes a length, so a 13-char literal here would
   * append the string terminator and fs.check would report ends_newline=f */
  mkbytes("lf.txt", "x = 1\n\ty = 22\n", 14);
  V cl = F1("check", "lf.txt");
  CK(code_of(cl) == E_NONE);
  CK(!strcmp(ks(cl, "path"), "lf.txt"));
  CK(!strcmp(ks(cl, "eol"), "lf"));
  CK(!kb(cl, "bom", true));
  CKI(kn(cl, "lines", -1), 2);
  CKI(kn(cl, "tabs", -1), 1);
  CKI(kn(cl, "spaces", -1), 4);
  CK(kb(cl, "ends_newline", false));
  CKI(kn(cl, "bytes", -1), 14);
  CK(is_hex(ks(cl, "hash"), 12));
  CKSTR(hash12(&A, "x = 1\n\ty = 22\n", 14), ks(cl, "hash"));
  CKI(len_of(cl), 9);

  T("check_crlf");
  mkbytes("crlf.txt", "a\r\nb\r\n", 6);
  V cc = F1("check", "crlf.txt");
  CK(!strcmp(ks(cc, "eol"), "crlf"));
  CK(kb(cc, "ends_newline", false));
  CKI(kn(cc, "lines", -1), 2);
  CKI(kn(cc, "tabs", -1), 0);
  CK(kn(cl, "bytes", -1) != kn(cc, "bytes", -1));           /* the ending flip is measurable */

  T("check_mixed_bom_newline_loss");
  mkbytes("mix.txt", "a\r\nb\nc", 7);
  V cm = F1("check", "mix.txt");
  CK(!strcmp(ks(cm, "eol"), "mixed"));
  CK(!kb(cm, "ends_newline", true));                        /* trailing newline was lost */
  CKI(kn(cm, "lines", -1), 3);
  mkbytes("bom.txt", "\xEF\xBB\xBFhi\n", 6);
  V cb = F1("check", "bom.txt");
  CK(kb(cb, "bom", false));
  CK(!strcmp(ks(cb, "eol"), "lf"));
  CKI(kn(cb, "bytes", -1), 6);
  mkbytes("plain.txt", "hi\n", 3);
  CK(!kb(F1("check", "plain.txt"), "bom", true));
  mkbytes("one.txt", "no break", 8);
  V co = F1("check", "one.txt");
  CK(!strcmp(ks(co, "eol"), "none"));
  CKI(kn(co, "lines", -1), 1);
  CK(!kb(co, "ends_newline", true));
  mkbytes("tabs.txt", "\t\t  \n", 5);
  V ct = F1("check", "tabs.txt");
  CKI(kn(ct, "tabs", -1), 2);
  CKI(kn(ct, "spaces", -1), 2);
  CK(kb(ct, "ends_newline", false));
  mkbytes("empty.txt", "", 0);
  V ce = F1("check", "empty.txt");
  CK(code_of(ce) == E_NONE);
  CKI(kn(ce, "lines", -1), 0);
  CK(!kb(ce, "ends_newline", true));
  CKI(code_of(F1("check", "gone.txt")), E_NOENT);
  wipe_scratch();

  /* ---------------- fs.ls / fs.glob ---------------- */
  T("ls_defaults_and_order");
  mkfile("sub/b.txt", "11");
  mkfile("sub/a.txt", "222");
  mkfile("sub/.hidden.txt", "x");
  mkfile("sub/deep/c.log", "abc");
  V ls = F1("ls", "sub");
  CK(code_of(ls) == E_NONE);
  CK(ls.t == V_LIST);
  CKI(len_of(ls), 2);                                       /* dotfile skipped, dirs not listed */
  CK(!strcmp(ks(le(ls, 0), "path"), "sub/a.txt"));           /* sorted, never readdir order */
  CK(!strcmp(ks(le(ls, 1), "path"), "sub/b.txt"));
  CKI(kn(le(ls, 0), "bytes", -1), 3);
  CKI(kn(le(ls, 0), "lines", -1), 1);
  CK(!has(le(ls, 0), "hash"));                              /* hash absent unless requested */
  CK(has(le(ls, 0), "path") && has(le(ls, 0), "bytes") && has(le(ls, 0), "lines"));
  CKI(len_of(le(ls, 0)), 3);                                /* exactly the default fields */
  V lsg = F2o("ls", "sub", v_rec(&A, 1, "glob", v_strz(&A, "*.log")));
  CKI(len_of(lsg), 0);                                      /* not recursive by default */
  V lsgr = F2o("ls", "sub", v_rec(&A, 2, "glob", v_strz(&A, "*.log"), "recursive", VT));
  CKI(len_of(lsgr), 1);
  CK(!strcmp(ks(le(lsgr, 0), "path"), "sub/deep/c.log"));

  T("ls_fields_and_max");
  V lsh = F2o("ls", "sub", v_rec(&A, 1, "fields", v_strz(&A, "path,hash")));
  CK(code_of(lsh) == E_NONE);
  CKI(len_of(le(lsh, 0)), 2);
  CK(has(le(lsh, 0), "hash"));
  CK(is_hex(ks(le(lsh, 0), "hash"), 12));
  CK(!has(le(lsh, 0), "bytes") && !has(le(lsh, 0), "lines"));
  CKSTR(hash12(&A, "222", 3), ks(le(lsh, 0), "hash"));
  V lso = F2o("ls", "sub", v_rec(&A, 1, "fields", v_strz(&A, "hash,path")));
  CK(!strcmp(ks(le(lso, 0), "path"), "sub/a.txt"));          /* field order is fixed, not asked */
  CK(!has(le(lso, 0), "lines"));
  V lsl = F2o("ls", "sub", v_rec(&A, 1, "fields", v_list(&A, 2, v_strz(&A, "path"), v_strz(&A, "lines"))));
  CK(code_of(lsl) == E_NONE);
  CK(has(le(lsl, 0), "lines") && !has(le(lsl, 0), "hash"));
  CKI(code_of(F2o("ls", "sub", v_rec(&A, 1, "fields", v_strz(&A, "path,owner")))), E_BAD_INPUT);
  CKI(code_of(F2o("ls", "sub", v_rec(&A, 1, "fields", v_num(3)))), E_TYPE);
  V lsm1 = F2o("ls", "sub", v_rec(&A, 1, "max", v_num(1)));
  CKI(len_of(lsm1), 1);
  CK(!strcmp(ks(le(lsm1, 0), "path"), "sub/a.txt"));
  V lsm2 = F2o("ls", "sub", v_rec(&A, 1, "max", v_num(2)));
  CKI(len_of(lsm2), 2);
  CKI(code_of(F2o("ls", "sub", v_rec(&A, 1, "max", v_strz(&A, "2")))), E_TYPE);
  V lsr = F2o("ls", "sub", v_rec(&A, 2, "recursive", VT, "glob", v_strz(&A, "*.txt")));
  CKI(len_of(lsr), 2);                                      /* a.txt + b.txt, .hidden excluded */
  CK(!strcmp(ks(le(lsr, 0), "path"), "sub/a.txt"));
  CK(!strcmp(ks(le(lsr, 1), "path"), "sub/b.txt"));
  V lsdot = F2o("ls", "sub", v_rec(&A, 1, "glob", v_strz(&A, ".hidden*")));
  CKI(len_of(lsdot), 1);                                    /* a dot glob asks for dotfiles */
  CK(!strcmp(ks(le(lsdot, 0), "path"), "sub/.hidden.txt"));
  CKI(code_of(F1("ls", "nodir")), E_NOENT);

  T("glob");
  V gb = F2o("glob", "**/*.log", v_rec(&A, 1, "root", v_strz(&A, "sub")));
  CK(code_of(gb) == E_NONE);
  CKI(len_of(gb), 1);
  CK(!strcmp(str_of(le(gb, 0)), "deep/c.log"));              /* relative to root */
  V gt = F2o("glob", "**/*.txt", v_rec(&A, 1, "root", v_strz(&A, "sub")));
  CKI(len_of(gt), 2);
  CK(!strcmp(str_of(le(gt, 0)), "a.txt"));
  V gm = F2o("glob", "**/*.txt", v_rec(&A, 2, "root", v_strz(&A, "sub"), "max", v_num(1)));
  CKI(len_of(gm), 1);
  V gp = F2("glob", "**/*.log", "sub");                      /* SPEC 4.1 positional root */
  CKI(len_of(gp), 1);
  CKI(code_of(F2o("glob", "*", v_rec(&A, 1, "root", v_strz(&A, "nodir")))), E_NOENT);
  CKI(code_of(F2o("glob", "*", v_rec(&A, 1, "max", v_strz(&A, "3")))), E_TYPE);
  wipe_scratch();

  /* ---------------- plan + confirm for real ---------------- */
  T("plan_write_new_file_applies_in_jail");
  V p = F2("write", "new.txt", "a");
  CK(p.t == V_PLAN);
  CK(!strcmp(v_typename(p), "plan"));
  CK(strlen(tok_of(p)) == 12);
  V view = plan_view(p);
  CK(!strcmp(ks(view, "op"), "fs.write"));
  CK(!strcmp(ks(view, "confirm"), "jail"));
  CKI(kn(view, "count", -1), 1);
  CK(kl(view, "files") == 1);
  V f0 = ke(view, "files", 0);
  CK(!strcmp(ks(f0, "kind"), "create"));
  CK(!has(f0, "from"));                                     /* nothing to bind: it is new */
  CKSTR(hash12(&A, "a", 1), ks(f0, "to"));                  /* to = planned content hash */
  CKI(kn(f0, "bytes", -1), 1);
  CK(strstr(ks(f0, "path"), "tmp_fs_test") != NULL);         /* writes name the real file */
  Str compact = v_tostr(&A, p, true);
  CK(s_findz(compact, "PLAN(op=fs.write", 0) >= 0);
  CKI(s_count_char(compact, '\n'), 0);                       /* one line, always */
  V rec = apply(p, NULL);
  CK(code_of(rec) == E_NONE);
  CK(fs_exists("new.txt"));
  CK(!strcmp(rdfile("new.txt"), "a"));
  CK(rec.t == V_REC);
  CKI(kn(rec, "bytes", -1), 1);
  CK(!strcmp(ks(rec, "token"), tok_of(p)));
  CK(!fs_exists(".vxa/journal.ndjson") ? 0 : 1);             /* the journal recorded it */

  T("plan_replace_needs_confirm");
  V p2 = F2("write", "new.txt", "bb");
  CK(p2.t == V_PLAN);
  const char *t2 = tok_of(p2);
  CK(strlen(t2) == 12);
  V v2 = plan_view(p2);
  CK(!strcmp(ks(ke(v2, "files", 0), "kind"), "modify"));
  CKSTR(hash12(&A, "a", 1), ks(ke(v2, "files", 0), "from")); /* bound to the OLD content */
  CK(!strcmp(ks(v2, "confirm"), "jail"));
  V r2 = apply(p2, NULL);
  CKI(code_of(r2), E_NEED_CONFIRM);
  CK(err_exit(E_NEED_CONFIRM) == X_CONFIRM);
  CK(strstr(msg_of(r2), t2) != NULL);                        /* the token is in the message */
  CK(strstr(msg_of(r2), "--confirm") != NULL);
  CK(strstr(hint_of(r2), "--confirm") != NULL);
  CK(!strcmp(rdfile("new.txt"), "a"));                       /* NOT modified yet */
  V r3 = apply(p2, t2);
  CK(code_of(r3) == E_NONE);
  CK(!strcmp(rdfile("new.txt"), "bb"));
  mkfile("new.txt", "TAMPERED");                             /* consumed token must replay */
  V r4 = apply(p2, t2);
  CK(code_of(r4) == E_NONE);
  CK(kb(r4, "replayed", false));
  CK(!kb(r4, "applied", true));
  CK(!strcmp(rdfile("new.txt"), "TAMPERED"));                /* no rewrite on replay */
  CK(!strcmp(ks(r4, "token"), t2));
  CK(strstr(ks(r4, "note"), "journal") != NULL);

  T("plan_stale_world");
  mkfile("stale.txt", "one");
  V p5 = F2("write", "stale.txt", "two");
  const char *t5 = tok_of(p5);
  mkfile("stale.txt", "different");                          /* the world moved */
  V r5 = apply(p5, t5);
  CKI(code_of(r5), E_STALE_PLAN);
  CK(strstr(msg_of(r5), "STALE_PLAN") != NULL || strstr(hint_of(r5), "token") != NULL);
  CK(!strcmp(rdfile("stale.txt"), "different"));             /* nothing was overwritten */
  V p6 = F2("write", "stale.txt", "two");                    /* a fresh plan applies */
  CK(strcmp(tok_of(p6), t5) != 0);                           /* and its token differs */
  CK(code_of(apply(p6, tok_of(p6))) == E_NONE);
  CK(!strcmp(rdfile("stale.txt"), "two"));
  V p7 = F2("write", "stale.txt", "three");
  CKI(code_of(apply(p7, "deadbeefcafe")), E_BAD_TOKEN);
  CK(!strcmp(rdfile("stale.txt"), "two"));
  wipe_scratch();

  T("confirm_override_and_policy");
  mkfile("pol.txt", "old");
  V none = F3o("write", "pol.txt", "new", v_rec(&A, 1, "confirm", v_strz(&A, "none")));
  CK(none.t == V_PLAN);
  CK(!strcmp(ks(plan_view(none), "confirm"), "none"));
  CK(code_of(apply(none, NULL)) == E_NONE);
  CK(!strcmp(rdfile("pol.txt"), "new"));                     /* replaced with no token at all */
  V ball = F3o("write", "brand.txt", "x", v_rec(&A, 1, "confirm", v_strz(&A, "all")));
  CK(ball.t == V_PLAN);
  CK(!strcmp(ks(plan_view(ball), "confirm"), "all"));
  CKI(code_of(apply(ball, NULL)), E_NEED_CONFIRM);           /* 'all' is never free */
  CK(!fs_exists("brand.txt"));
  set_policy("ask");
  V pask = F2("write", "asked.txt", "x");
  CK(!strcmp(ks(plan_view(pask), "confirm"), "ask"));
  CKI(code_of(apply(pask, NULL)), E_NEED_CONFIRM);
  clear_policy();
  V pjail = F2("write", "asked.txt", "x");                   /* back to the default policy */
  CK(!strcmp(ks(plan_view(pjail), "confirm"), "jail"));
  CK(code_of(apply(pjail, NULL)) == E_NONE);
  CK(fs_exists("asked.txt"));
  CKI(code_of(F3o("write", "bad.txt", "x", v_rec(&A, 1, "confirm", v_strz(&A, "sometimes")))), E_BAD_INPUT);
  /* the confirm level is inside the token: two levels, two tokens */
  V pa = F3o("write", "lvl.txt", "z", v_rec(&A, 1, "confirm", v_strz(&A, "ask")));
  V pb = F3o("write", "lvl.txt", "z", v_rec(&A, 1, "confirm", v_strz(&A, "jail")));
  CK(strcmp(tok_of(pa), tok_of(pb)) != 0);
  wipe_scratch();

  T("fs_plan_is_a_dry_run");
  mkfile("dry.txt", "current");
  V dp = F2o("plan", "dry.txt", v_rec(&A, 1, "text", v_strz(&A, "planned")));
  CK(code_of(dp) == E_NONE);
  CK(dp.t == V_REC);
  CK(kb(dp, "planned", false));
  CK(!strcmp(ks(dp, "path"), "dry.txt"));
  CK(!strcmp(ks(dp, "mode"), "write"));
  CK(!strcmp(ks(dp, "op"), "fs.write"));
  CKI(kl(dp, "files"), 1);
  V df = ke(dp, "files", 0);
  CK(!strcmp(ks(df, "kind"), "modify"));
  CKSTR(hash12(&A, "current", 7), ks(df, "from"));           /* current target hash */
  CKSTR(hash12(&A, "planned", 7), ks(df, "to"));             /* planned payload hash */
  CKI(kn(df, "bytes", -1), 7);
  CK(strlen(ks(dp, "token")) == 12);
  CK(!strcmp(ks(dp, "token"), tok_of(F2("write", "dry.txt", "planned"))));  /* same plan */
  CK(!strcmp(ks(dp, "confirm"), "jail"));
  CK(!strcmp(rdfile("dry.txt"), "current"));                 /* nothing written */
  V dpn = F2o("plan", "fresh.txt", v_rec(&A, 1, "text", v_strz(&A, "q")));
  CK(!strcmp(ks(ke(dpn, "files", 0), "kind"), "create"));
  CK(!fs_exists("fresh.txt"));
  V ddel = F2o("plan", "dry.txt", v_rec(&A, 1, "mode", v_strz(&A, "delete")));
  CK(!strcmp(ks(ke(ddel, "files", 0), "kind"), "delete"));   /* a delete signs as a delete */
  CKSTR(hash12(&A, "current", 7), ks(ke(ddel, "files", 0), "from"));
  CK(!strcmp(rdfile("dry.txt"), "current"));
  V dmo = F2o("plan", "dry.txt", v_rec(&A, 2, "mode", v_strz(&A, "move"), "to", v_strz(&A, "moved.txt")));
  CKI(kl(dmo, "files"), 2);
  CK(!strcmp(ks(dmo, "op"), "fs.move"));
  CK(!fs_exists("moved.txt"));
  CKI(code_of(F2o("plan", "dry.txt", v_rec(&A, 1, "mode", v_strz(&A, "squash")))), E_BAD_INPUT);
  CKI(code_of(F2o("plan", "dry.txt", v_rec(&A, 1, "mode", v_strz(&A, "move")))), E_BAD_INPUT);
  CKI(code_of(F2o("plan", "../dry.txt", v_rec(&A, 1, "text", v_strz(&A, "x")))), E_OUTSIDE_JAIL);
  wipe_scratch();

  T("append");
  mkfile("app.txt", "ab");
  V ap = F2("append", "app.txt", "cd");
  CK(ap.t == V_PLAN);
  V av = plan_view(ap);
  CK(!strcmp(ks(av, "op"), "fs.append"));
  CKSTR(hash12(&A, "ab", 2), ks(ke(av, "files", 0), "from"));
  CKSTR(hash12(&A, "abcd", 4), ks(ke(av, "files", 0), "to"));   /* hash of the END state */
  CKI(code_of(apply(ap, NULL)), E_NEED_CONFIRM);               /* replacing content costs a token */
  CK(!strcmp(rdfile("app.txt"), "ab"));
  CK(code_of(apply(ap, tok_of(ap))) == E_NONE);
  CK(!strcmp(rdfile("app.txt"), "abcd"));                      /* appended exactly once */
  V ap2 = F2o("plan", "app.txt", v_rec(&A, 2, "mode", v_strz(&A, "append"), "text", v_strz(&A, "e")));
  CKSTR(hash12(&A, "abcd", 4), ks(ke(ap2, "files", 0), "from"));
  CKI(kn(ke(ap2, "files", 0), "bytes", -1), 5);
  V apn = F2("append", "newapp.txt", "first");
  CK(!strcmp(ks(plan_file(apn, 0), "kind"), "create"));
  CK(code_of(apply(apn, NULL)) == E_NONE);
  CK(!strcmp(rdfile("newapp.txt"), "first"));
  wipe_scratch();

  T("delete_to_trash_and_restore");
  mkfile("doomed.txt", "keepme");
  V dplan = F1("delete", "doomed.txt");
  CK(dplan.t == V_PLAN);
  V dv = plan_view(dplan);
  CK(!strcmp(ks(dv, "op"), "fs.delete"));
  CK(!strcmp(ks(dv, "confirm"), "ask"));                       /* never free */
  CK(strlen(ks(ke(dv, "files", 0), "from")) == 12);
  V rd = apply(dplan, NULL);
  CKI(code_of(rd), E_NEED_CONFIRM);
  CK(fs_exists("doomed.txt"));                                  /* still on disk */
  CKI(code_of(F1("delete", "doomed.txt")) , E_NONE);            /* still exists: re-plannable */
  V rd2 = apply(dplan, tok_of(dplan));
  CK(code_of(rd2) == E_NONE);
  CK(!fs_exists("doomed.txt"));                                 /* original gone */
  CK(kb(rd2, "restorable", false));
  long seq = (long)kn(rd2, "seq", -1);
  CK(seq > 0);
  char tname[4096];
  snprintf(tname, sizeof tname, ".vxa/trash/%ld__doomed.txt", seq);
  CK(fs_is_dir(".vxa/trash"));
  CK(fs_exists(tname));                                          /* the trash copy is there */
  CK(!strcmp(rdfile(tname), "keepme"));
  V rp = F1n("restore", (double)seq);
  CK(rp.t == V_PLAN);
  V rv = plan_view(rp);
  CK(!strcmp(ks(rv, "op"), "fs.restore"));
  CKI(kl(rv, "files"), 2);                                       /* leaves trash, lands original */
  CKI(code_of(apply(rp, NULL)), E_NEED_CONFIRM);
  V rr = apply(rp, tok_of(rp));
  CK(code_of(rr) == E_NONE);
  CK(fs_exists("doomed.txt"));
  CK(!strcmp(rdfile("doomed.txt"), "keepme"));                   /* identical bytes back */
  CK(!fs_exists(tname));
  CKI(code_of(F1n("restore", 999999999.0)), E_NOENT);
  CKI(code_of(F1n("restore", 0)), E_BAD_INPUT);
  CKI(code_of(F1("delete", "gone.txt")), E_NOENT);
  CKI(code_of(F1("delete", ".")), E_ISDIR);
  wipe_scratch();

  T("move");
  mkfile("src1.txt", "payload");
  V mp = F2("move", "src1.txt", "dst1.txt");
  CK(mp.t == V_PLAN);
  V mv = plan_view(mp);
  CK(!strcmp(ks(mv, "op"), "fs.move"));
  CKI(kl(mv, "files"), 2);
  CK(strstr(ks(ke(mv, "files", 1), "path"), "dst1.txt") != NULL);
  CK(!strcmp(ks(ke(mv, "files", 1), "kind"), "create"));         /* nothing overwritten yet */
  CKI(code_of(apply(mp, NULL)), E_NEED_CONFIRM);                 /* the source is being replaced */
  CK(fs_exists("src1.txt"));
  V mr = apply(mp, tok_of(mp));
  CK(code_of(mr) == E_NONE);
  CK(!fs_exists("src1.txt"));
  CK(!strcmp(rdfile("dst1.txt"), "payload"));
  mkfile("src2.txt", "x");
  mkfile("dst2.txt", "yyy");
  V mp2 = F2("move", "src2.txt", "dst2.txt");
  CK(!strcmp(ks(plan_file(mp2, 1), "kind"), "modify"));           /* the overwrite is listed */
  CK(!strcmp(ks(plan_file(mp2, 1), "from"), str_of(F1("hash", "dst2.txt"))));
  CKI(code_of(apply(mp2, NULL)), E_NEED_CONFIRM);
  CK(!strcmp(rdfile("dst2.txt"), "yyy"));
  CK(code_of(apply(mp2, tok_of(mp2))) == E_NONE);
  CK(!strcmp(rdfile("dst2.txt"), "x"));
  CK(!fs_exists("src2.txt"));
  CKI(code_of(F2("move", "gone.txt", "anywhere.txt")), E_NOENT);
  CKI(code_of(F2("move", "dst1.txt", "../outside.txt")), E_OUTSIDE_JAIL);
  CK(!fs_exists("../outside.txt"));
  wipe_scratch();

  T("arity_and_argument_types");
  CKI(code_of(F1("write", "one-arg.txt")), E_ARITY);
  CKI(code_of(F1("append", "one")), E_ARITY);
  CKI(code_of(F1("move", "one")), E_ARITY);
  CKI(code_of(F0("read")), E_ARITY);
  CKI(code_of(F0("exists")), E_ARITY);
  CKI(code_of(F("cwd", (V[]){ v_num(1) }, 1)), E_ARITY);
  CKI(code_of(F1("read", "")), E_BAD_INPUT);
  CKI(code_of(F("bundle", (V[]){ v_strz(&A, "not-a-list") }, 1)), E_TYPE);
  CKI(code_of(F("bundle", (V[]){ v_list(&A, 1, v_num(4)), v_rec(&A, 0) }, 2)), E_TYPE);
  CKI(code_of(F2o("read", "x", v_num(3))), E_TYPE);             /* opts must be a record */

  T("outline_and_bundle");
  mkfile("small.c", "int parse_thing(void) { return 0; }\nint other(void) { return 1; }\n");
  mkfile("other.c", "void nothing_here(void) {}\n");
  V ol = F1("outline", "small.c");
  CK(code_of(ol) == E_NONE);
  CK(ol.t == V_REC);
  CK(!strcmp(ks(ol, "path"), "small.c"));                        /* echoes the asked name */
  CK(kv(ol, "symbols") && kv(ol, "symbols")->t == V_LIST);
  CK(kv(ol, "lang") && kv(ol, "lang")->t == V_STR);
  CKI(kn(ol, "lines", -1), 2);
  CKI(kn(ol, "bytes", -1), fs_size("small.c"));
  CK(is_hex(ks(ol, "hash"), 12));
  CK(kl(ol, "symbols") >= 1);
  CKI(code_of(F1("outline", "gone.c")), E_NOENT);
  V bd = F2v("bundle", v_list(&A, 2, v_strz(&A, "small.c"), v_strz(&A, "other.c")),
             v_rec(&A, 2, "query", v_strz(&A, "parse"), "max_bytes", v_num(4000)));
  CK(code_of(bd) == E_NONE);
  CK(bd.t == V_REC);
  CK(kv(bd, "bundle") && kv(bd, "bundle")->t == V_LIST);
  CK(kv(bd, "dropped") && kv(bd, "dropped")->t == V_LIST);
  CKI(kn(bd, "cap", -1), 4000);
  CK(kn(bd, "bytes", 99999) <= 4000);                            /* the budget is hard */
  CKI(kl(bd, "bundle"), 2);
  CKI(kl(bd, "dropped"), 0);
  CK(strstr(ks(ke(bd, "bundle", 0), "path"), ".c") != NULL);
  V bmiss = F2v("bundle", v_list(&A, 2, v_strz(&A, "small.c"), v_strz(&A, "gone.c")), v_rec(&A, 0));
  CK(code_of(bmiss) == E_NONE);
  CKI(kl(bmiss, "dropped"), 1);                                  /* the missing one is named */
  V btok = F2v("bundle", v_list(&A, 1, v_strz(&A, "small.c")), v_rec(&A, 1, "max_tok", v_num(50)));
  CKI(kn(btok, "cap", -1), 200);                                 /* 4 bytes per token */
  V bsym = F2v("bundle", v_list(&A, 1, v_strz(&A, "small.c")), v_rec(&A, 1, "only", v_strz(&A, "symbols")));
  CK(code_of(bsym) == E_NONE);
  CKI(code_of(F2v("bundle", v_list(&A, 1, v_strz(&A, "small.c")), v_rec(&A, 1, "max_bytes", v_strz(&A, "9")))), E_TYPE);
  wipe_scratch();
  CK(!fs_exists("doomed.txt") && !fs_exists("new.txt"));

  T_REPORT("lib_fs");
}
