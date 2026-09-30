/* test_spec.c - conformance suite: SPEC.md's OWN examples, executed.
 *
 * This is not a unit test of the implementation. Every assertion quotes a form, a
 * value or a byte string that SPEC.md (the contract) or docs/LEARN.md (its literal
 * rendering, "these are real emitted bytes") promises, and then checks what the
 * language actually produces. Where the two documents disagree SPEC.md wins and the
 * assertion is left failing: a suite that cannot fail proves nothing, and a suite
 * that only re-asserts whatever the code happens to do is documentation, not a
 * test. Every deliberate, reported failure is marked "LEFT FAILING" and is listed
 * in the commit report - do not "fix" one by editing the expectation.
 *
 * Sections: 1.1 lex | 1.2 operators | 1.3 statements | 1.4 property vs method |
 * 1.5 the ERR idiom (the bug this suite was written for) | 1.6 truthiness |
 * 2.1 compact bytes, compared exactly | 2.2 every documented exit code |
 * 3.x PLAN shape, the token law, jail, receipt, replay, TOCTOU | 4.x round-trip laws.
 *
 * Everything runs in build/scratch/spec/, which is the workspace root for the
 * suite, so the jail, the journal and the audit trail live in a directory that is
 * removed again. Nothing here touches the repository tree.
 */
#include "vxa.h"
#include "interp.h"
#include "t.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

static Arena A;
static Ctx *C = NULL;
static char ROOT[4096];

/* ===================================================================== */
/* fixture: a scratch workspace, the same shape `vxa run` gets            */
/* ===================================================================== */
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
static void wipe_here(void) {                       /* only ever called inside the scratch dir */
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
  const char *cands[] = { "build/scratch/spec", "scratch/spec", "../build/scratch/spec" };
  for (size_t i = 0; i < sizeof(cands)/sizeof(cands[0]); i++) {
    ensure_dir_for(cands[i]);
    if (!chdir(cands[i])) {
      if (!getcwd(ROOT, sizeof ROOT)) { printf("FATAL: no cwd\n"); exit(2); }
      C = ctx_new(&A, ROOT, "test_spec.vxa");
      return;
    }
  }
  printf("FATAL: cannot enter build/scratch/spec\n");
  exit(2);
}
static void mkfile(const char *p, const char *s) {
  FILE *f = fopen(p, "wb");
  if (!f) { printf("FATAL: cannot create %s\n", p); exit(2); }
  fwrite(s, 1, strlen(s), f);
  fclose(f);
}
static char *rdfile(const char *p) {                      /* NULL when absent */
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  static char buf[262144];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = 0;
  fclose(f);
  return buf;
}

/* ===================================================================== */
/* running a program the way the CLI does, and the shapes of claim        */
/* ===================================================================== */
static V ev_in(Ctx *cx, const char *src) {
  ctx_clear_err(cx);
  V perr = VN;
  Node *p = parse_script(cx, src, strlen(src), "<spec>", &perr);
  if (!p) return perr;                              /* E_SYNTAX arrives as a value, per SPEC 1.5 */
  V r = eval_node(cx, p);
  if (ctx_has_err(cx)) { V e = ctx_err(cx); ctx_clear_err(cx); return e; }
  return r;
}
#define EV(src) ev_in(C, (src))

static Str bytes_of(V v) { return v_tostr(&A, v, true); }     /* the bytes an agent reads (§2.1) */
static void describe(V v, Buf *b) { buf_init(b, &A); v_disp(b, v, 0, true); }
static int is_err(V v) { return v_is_err(v); }

/* CLAIM - a form evaluates to an exactly known number */
static void ck_num(const char *src, double want) {
  V v = EV(src);
  Buf b; describe(v, &b);
  CHECK(v.t == V_NUM && v.u.n == want,
        "SPEC value  {%s}\n      want num %g\n      got  %s", src, want, b.p ? b.p : "(null)");
}
/* CLAIM - the EMITTED value is these exact bytes (the §2.1 compact form) */
static void ck_str(const char *src, const char *want) {
  V v = EV(src);
  Str g = bytes_of(v);
  size_t wl = strlen(want);
  CHECK(g.p && (size_t)g.len == wl && !memcmp(g.p, want, wl),
        "SPEC bytes  {%s}\n      want \"%s\"\n      got  \"%.*s\"", src, want, g.len, g.p ? g.p : "");
}
/* CLAIM - the value is a string holding exactly these bytes (no quoting rules involved) */
static void ck_txt(const char *src, const char *want) {
  V v = EV(src);
  size_t wl = strlen(want);
  CHECK(v.t == V_STR && v.u.s.p && (size_t)v.u.s.len == wl && !memcmp(v.u.s.p, want, wl),
        "SPEC text   {%s}\n      want \"%s\"\n      got  \"%.*s\"", src, want,
      v.t == V_STR ? v.u.s.len : 0, v.t == V_STR ? (v.u.s.p ? v.u.s.p : "") : "");
}
/* CLAIM - the form is accepted and the answer is true / false */
static void ck_true(const char *src) {
  V v = EV(src);
  Buf b; describe(v, &b);
  CHECK(!is_err(v) && v_truthy(v), "SPEC expects truthy {%s} got %s", src, b.p ? b.p : "(err)");
}
static void ck_false(const char *src) {
  V v = EV(src);
  Buf b; describe(v, &b);
  CHECK(!is_err(v) && !v_truthy(v), "SPEC expects falsy {%s} got %s", src, b.p ? b.p : "(err)");
}
/* a form the spec documents as NOT a form must be refused, never repaired (§0 axiom 1) */
static void ck_refused(const char *src) {
  V v = EV(src);
  Buf b; describe(v, &b);
  CHECK(is_err(v), "SPEC refuses this form, but it evaluated to: {%s} -> %s", src, b.p ? b.p : "(null)");
}
/* the ERR idiom: the run must produce an ERR carrying exactly this stable code */
static void ck_code(const char *src, const char *want) {
  V v = EV(src);
  Str g = bytes_of(v);
  CHECK(is_err(v) && !strcmp(err_name(v_errcode(v)), want),
        "SPEC error code  {%s}\n      want code=%s\n      got  \"%.*s\"", src, want, g.len, g.p ? g.p : "");
}
/* SPEC 1.5: `src` binds an error to the name `e`; now read one documented field of it */
static void ck_field(const char *src, const char *field, const char *want) {
  char buf[1600];
  snprintf(buf, sizeof buf, "%s\ne.%s", src, field);
  ck_txt(buf, want);
}
static void ck_field_prefix(const char *src, const char *field, const char *want) {
  char buf[1600];
  snprintf(buf, sizeof buf, "%s\ne.%s", src, field);
  V v = EV(buf);
  Str g = v.t == V_STR ? v.u.s : s_null();
  size_t wl = strlen(want);
  CHECK(v.t == V_STR && g.p && (size_t)g.len >= wl && !memcmp(g.p, want, wl),
        "SPEC {%s}\n      want e.%s to start with \"%s\"\n      got  \"%.*s\"", buf, field, want,
        g.len, g.p ? g.p : "");
}
/* the length of an expression, as §1.4's `.len` property */
static void ck_len(const char *expr, int want) {
  char buf[1600];
  snprintf(buf, sizeof buf, "(%s).len", expr);
  V v = EV(buf);
  Buf b; describe(v, &b);
  CHECK(v.t == V_NUM && (int)v.u.n == want,
        "SPEC length {%s}\n      want %d\n      got  %s", buf, want, b.p ? b.p : "(?)");
}
static void ck_named(const char *src, const char *want, const char *what) {
  V v = EV(src);
  Buf b; describe(v, &b);
  CHECK(v.t == V_STR && v.u.s.p && !strcmp(v.u.s.p, want),
        "SPEC %s {%s}\n      want \"%s\"\n      got  %s", what, src, want, b.p ? b.p : "(?)");
}
static int hex_only(const char *p, int n) {
  if (n <= 0) return 0;
  for (int i = 0; i < n; i++) if (!isxdigit((unsigned char)p[i])) return 0;
  return 1;
}
static V fld(V v, const char *k) {
  if (v.t != V_REC || !v.u.r) return VN;
  V *p = rec_getz(v.u.r, k);
  return p ? *p : VN;
}
static V item(V v, int i) {
  if (v.t != V_LIST || !v.u.l) return VN;
  V *p = list_get(v.u.l, i);
  return p ? *p : VN;
}

/* ===================================================================== */
/* SPEC 1.5 - the error idiom: r = fs.read("nope.txt") ; if r.code { }    */
/* ===================================================================== */
#define NOENT "e = fs.read(\"nope-not-here.txt\")"
#define AMISS "e = tx.patch(\"alpha\\nbeta\\ngamma\", [{at:\"deadbeef\", op:\"del\"}], {path:\"m.c\"})"
#define DUP   "t = \"x\\nSAME\\ny\\nx\\nSAME\\ny\"\na = tx.anchors(t, {path:\"d\"})\n" \
              "e = tx.patch(t, [{at:a[1].at, op:\"del\"}], {path:\"d\"})"
#define JAIL  "p = fs.write(\"spec15-overwrite.txt\", \"new-content\")\ne = p.apply()"

static void spec15_err_idiom(void) {
  T("SPEC 1.5 the documented error idiom");
  mkfile("spec15.txt", "there\n");
  mkfile("spec15-overwrite.txt", "old-content");

  /* the doc's example, field by field */
  ck_field(NOENT, "code", "NOENT");        /* code is the uppercase name: the CLI prints code=NOENT */
  ck_field(NOENT, "hint", err_hint(E_NOENT));             /* §0/1.5: hint is one actionable line */
  ck_field_prefix(NOENT, "msg", "read: 'nope-not-here.txt'");
  ck_named(NOENT "\ntype(e.data)", "null", "e.data on a plain error is a null value");
  ck_str(NOENT "\ne.type()", "err");                       /* §1.4: every value answers .type() */
  /* a name that is neither field nor method is E_UNDEF, never a silent null */
  ck_code(NOENT "\ne.bogus", "UNDEF");

  /* the point of the section: r.code is the truthiness test for "is this an error" */
  ck_str(NOENT "\nif e.code { \"error\" } else { \"ok\" }", "error");
  ck_str("e = fs.read(\"spec15.txt\")\nif e.code { \"error\" } else { \"ok\" }", "ok");
  ck_str("e = fs.read(\"spec15.txt\")\ne.code", "-");      /* a good read has no code: reads null */

  /* SPEC 1.5: an unused ERR is silent - once the script has taken it as a value. */
  ck_str(NOENT "\n42", "42");
  ck_code("fs.read(\"nope-not-here.txt\")\n42", "NOENT");
  CKI(err_exit(v_errcode(EV("fs.read(\"nope-not-here.txt\")"))), 5);  /* the agent still branches */

  T("SPEC 1.5/4.2 ANCHOR_MISS");
  ck_field(AMISS, "code", "ANCHOR_MISS");
  ck_field(AMISS, "hint", err_hint(E_ANCHOR_MISS));
  ck_field_prefix(AMISS, "msg", "at=deadbeef");
  {                                                        /* §4.2: "附最近 3 个候选" */
    V v = EV(AMISS);
    Str g = bytes_of(v);
    CK(is_err(v) && g.p && strstr(g.p, "nearest:") != NULL);
  }

  T("SPEC 4.2 ANCHOR_DUP, and the documented n disambiguation");
  ck_field(DUP, "code", "ANCHOR_DUP");
  ck_field_prefix(DUP, "msg", "at=");
  {                                                        /* §4.2: it "回列每处的行号" */
    V v = EV(DUP);
    Str g = bytes_of(v);
    CK(is_err(v) && g.p && strstr(g.p, "L2") && strstr(g.p, "L5"));
  }
  ck_true("t = \"x\\nSAME\\ny\\nx\\nSAME\\ny\"\na = tx.anchors(t, {path:\"d\"})\n"
          "p = tx.patch(t, [{at:a[1].at, op:\"set\", text:\"SECOND\", n:2}], {path:\"d\"})\n"
          "p.text == \"x\\nSAME\\ny\\nx\\nSECOND\\ny\"");

  T("SPEC 3.5 NEED_CONFIRM, caught as a value");
  ck_field(JAIL, "code", "NEED_CONFIRM");
  ck_field(JAIL, "hint", err_hint(E_NEED_CONFIRM));
  ck_str(JAIL "\ne.type()", "err");
  {                                                        /* the msg names the very token */
    V t = EV("p = fs.write(\"spec15-overwrite.txt\", \"new-content\")\np.token");
    CHECK(t.t == V_STR && t.u.s.len == 12 && hex_only(t.u.s.p, t.u.s.len),
          "SPEC 3.2 the plan token is 12 hex chars");
    char want[64];
    snprintf(want, sizeof want, "rerun with --confirm %.*s", t.u.s.len, t.u.s.p);
    ck_field_prefix(JAIL, "msg", want);
  }
  /* LEFT FAILING: SPEC 2.1 renders a NEED_CONFIRM line with `op=` and `token=` as
   * flat fields, and SPEC 3.5 says the value 含 token. Flat fields come out of the
   * error's `data`, which the plan path never fills - so the token is only readable
   * by parsing msg, and an agent cannot bind it to --confirm without string work. */
  ck_true(JAIL "\ne.data.token == p.token");
  {
    const char *on = rdfile("spec15-overwrite.txt");
    CHECK(on && !strcmp(on, "old-content"), "SPEC 3.4 NEED_CONFIRM left the file as \"%s\"",
          on ? on : "(absent)");
  }

  T("SPEC 5 OUTSIDE_JAIL");
  ck_field("e = fs.write(\"../../vxa-outside-jail-probe.txt\", \"x\")", "code", "OUTSIDE_JAIL");
  ck_field("e = fs.write(\"../../vxa-outside-jail-probe.txt\", \"x\")", "hint", err_hint(E_OUTSIDE_JAIL));
  ck_str("w = fs.write(\"../../vxa-outside-jail-probe.txt\", \"x\")\n"
         "if w.code == \"OUTSIDE_JAIL\" { \"refused\" } else { \"written\" }", "refused");
  CHECK(rdfile("../../vxa-outside-jail-probe.txt") == NULL, "SPEC 5 OUTSIDE_JAIL touches nothing");

  T("SPEC 1.5 out.die(r) ends the script with that error");
  ck_code(NOENT "\nout.die(e)", "NOENT");
  ck_code("out.die(\"too wide\")", "LIMIT");

  T("SPEC 1.5 the literal example: `if r.code? { ... }`");
  /* LEFT FAILING: SPEC.md prints the idiom with a postfix ?. docs/LEARN.md line 38
   * says the opposite - "there is no postfix ? form" - and the parser agrees with
   * LEARN. The contract quoted here is SPEC.md, so this stays red until one of the
   * two documents is corrected. */
  ck_true(NOENT "\nif e.code? { 1 } else { 0 }");
}

/* ===================================================================== */
/* SPEC 1.4 - value types, and the property/method rule                   */
/* ===================================================================== */
static void spec14_values(void) {
  T("SPEC 1.4 property vs method");
  ck_num("[1,2,3].len", 3);                        /* a zero-arg method reads as a property */
  ck_str("[1,2,3].len", "3");
  ck_true("type([1,2,3].len) == \"num\"");
  ck_str("{a:1}.keys", "[a]");
  ck_true("type({a:1}.keys) == \"list\"");
  ck_num("{keys:9}.keys", 9);                     /* in a record the FIELD wins over the method */
  ck_str("{keys:9}.keys", "9");
  ck_num("{keys:9}.len", 1);
  ck_str("[1,2,3].map(x => x * 2)", "[2,4,6]");   /* anything taking arguments needs parens */
  ck_str("[1,2,3].filter(x => x > 1)", "[2,3]");

  T("SPEC 1.4 x.type() on every value kind");
  ck_str("null.type()", "null");
  ck_str("true.type()", "bool");
  ck_str("false.type()", "bool");
  ck_str("42.type()", "num");
  ck_str("\"s\".type()", "str");
  ck_str("[1].type()", "list");
  ck_str("{a:1}.type()", "rec");
  ck_str("def f(a) { a }\nf.type()", "fn");
  ck_str("fs.write(\"spec14-plan.txt\", \"x\").type()", "plan");
  ck_str(NOENT "\ne.type()", "err");
  ck_str("42 | type()", "num");                   /* the same method through a pipe */
  /* An ERR is not run through: it propagates as a value (§1.5/§1.6), so type(e)
   * hands back the error instead of the word "err" - the method form is the one
   * SPEC 1.4 promises, and it works above. */
  ck_code(NOENT "\ntype(e)", "NOENT");
  /* LEFT FAILING (implementation defect, not a doc dispute): `null`, `true` and
   * `false` are literals (§1.1), yet the evaluator's namespace probe in
   * unbound_id() reads them as namespaces, so `null.type()` is resolved as the
   * builtin call null.type() and refused. SPEC 1.4 says EVERY value answers
   * .type(). Fix: treat the three keyword literals as values, not namespaces. */
  ck_str("(null).type()", "null");
  ck_str("(true).type()", "bool");

  T("SPEC 1.4 list methods the doc lists");
  ck_str("[3,1,2].sort()", "[1,2,3]");
  ck_str("[1,1,2].uniq()", "[1,2]");
  ck_str("[[1],[2]].flat()", "[1,2]");
  ck_str("[1,2,3].first", "1");
  ck_str("[1,2,3].last", "3");
  ck_str("[1,2,3,4].slice(1,3)", "[2,3]");
  ck_true("[1,2].has(2)");
  ck_false("[1,2].has(9)");
  ck_txt("[1,2].join(\"-\")", "1-2");
  ck_num("[1,2,3].each(x => x).len", 3);          /* each returns the list unchanged */
  ck_txt("[[1,2],[3]].join(\",\")", "[1,2],[3]"); /* nested keeps its delimiters (§2.1) */
  ck_num("[1,2,3].count()", 3);

  T("SPEC 1.4 str methods the doc lists");
  ck_num("\"abc\".len", 3);                        /* §9: a string's length is its bytes */
  ck_str("\"a,b\".split(\",\")", "[a,b]");
  ck_txt("\" x \".trim()", "x");
  ck_txt("\"ab\".upper()", "AB");
  ck_txt("\"AB\".lower()", "ab");
  ck_true("\"abc\".contains(\"b\")");
  ck_true("\"abc\".starts(\"ab\")");
  ck_true("\"abc\".ends(\"bc\")");
  ck_txt("\"aXa\".replace(\"X\",\"y\")", "aya");
  ck_len("\"a\\nb\".lines", 2);
  ck_len("\"abc\".hash", 12);                      /* §4.1: a 12-char content fingerprint */
  ck_txt("\"ab\".fmt()", "ab");
  ck_num("\"a1b22\".find(\"b\")[0].n", 1);
  ck_true("\"abc\" ~= \"abc\"");
  /* LEFT FAILING: §1.4 lists `.re(sep)` for str and §9 lists `.ulen` for code
   * points. Neither name resolves - the build answers .chars - so an agent that
   * follows SPEC.md hits "str has no method". */
  ck_num("\"世界\".ulen", 2);
  ck_len("\"a,b\".re(\",\")", 2);

  T("SPEC 1.4 rec methods the doc lists");
  ck_str("{a:1,b:2}.values", "[1,2]");
  ck_true("{a:1}.has(\"a\")");
  ck_false("{a:1}.has(\"z\")");
  ck_str("{a:1}.get(\"z\", 9)", "9");
  ck_str("{a:1,b:2}.del(\"a\")", "b=2");
  ck_str("{a:1}.merge({b:2})", "a=1,b=2");
  ck_str("{a:1,b:2,c:3}.pick([\"a\",\"c\"])", "a=1,c=3");
  ck_str("{a:1,b:2,c:3}.omit([\"b\"])", "a=1,c=3");
  ck_str("{a:1,b:2}.keys", "[a,b]");              /* insertion order, never a hash walk */

  T("SPEC 9 encoding: bytes as length, code points on request");
  ck_num("\"世界\".len", 6);
  ck_num("\"世界\".chars", 2);
}

/* ===================================================================== */
/* SPEC 1.1 / 1.2 / 1.3 / 1.6                                             */
/* ===================================================================== */
static void spec11_lex(void) {
  T("SPEC 1.1 literals and strings");
  ck_num("12", 12); ck_num("3.5", 3.5); ck_num("-2", -2); ck_num("0x1F", 31);
  ck_str("3.5 + 0.5", "4");                        /* integers print without .0 */
  ck_txt("x = 2\n\"a{x}b\"", "a2b");               /* {expr} interpolation */
  ck_txt("\"a{1+2}c\"", "a3c");
  ck_txt("\"a\\{x}b\"", "a{x}b");                  /* a literal { needs \{ */
  ck_txt("'a{x}\\n'", "a{x}\\n");                  /* raw: never interpolates, never unescapes */
  ck_str("```a{x}y```", "\"a{x}y\"");              /* block: three quotes, never interpolates */
  ck_num("```a {x} b```.len", 7);
  ck_num("\"a\\nb\".len", 3);
  ck_num("\"a\\tb\".len", 3);
  ck_num("\"a\\\"b\".len", 3);
  ck_num("[1, 2, 3,].len", 3);                     /* trailing comma allowed in a list */
  ck_txt("{name:\"a\", n:1}.name", "a");
  ck_str("{1:2}[\"1\"]", "2");                     /* a number is a key, looked up as text */
  ck_str("{k:1}.k", "1");                          /* {k:1} is the KEY k, not the variable k */
  ck_refused("k = 5\n{{k}:1}");                    /* LEARN: {{k}:1} is refused */
  ck_str("# comment only\n1", "1");                /* §1.1: # runs to the end of the line */
}

static void spec12_operators(void) {
  T("SPEC 1.2 precedence: 1 + 2 * 3 is 7, never left-to-right");
  ck_num("1 + 2 * 3", 7);
  ck_num("2 * 3 - 4", 2);
  ck_num("(1 + 2) * 3", 9);
  ck_num("7 / 2", 3.5);
  ck_num("6 % 4", 2);
  ck_num("-2 + 3", 1);
  ck_num("2 + 3 * 4 - 6 / 3", 12);
  ck_false("!1");
  ck_true("!0");
  ck_false("not true");
  ck_true("not null");
  ck_true("\"a\" < \"b\"");
  ck_false("3 >= 4");
  ck_true("2 <= 2");
  ck_true("1 == 1");
  ck_true("1 != 2");
  ck_true("null == null");
  ck_false("[1,2] == [2,1]");
  ck_true("{a:1} == {a:1}");
  ck_true("true and 1");
  ck_true("false or 2");
  ck_false("0 or false");
  ck_str("null or \"dflt\"", "dflt");              /* and/or short-circuit to a value */
  ck_code("1 / 0", "RANGE");                       /* §1.2 has no silent infinity */

  T("SPEC 1.2 ~= similarity, 0..1 normalised Levenshtein");
  ck_num("\"abc\" ~= \"abc\"", 1);
  {
    V v = EV("\"kitten\" ~= \"sitting\"");
    double want = 4.0 / 7.0;                       /* three edits over seven code points */
    CHECK(v.t == V_NUM && fabs(v.u.n - want) < 1e-4,
          "SPEC ~=  \"kitten\" ~= \"sitting\" want ~%g got %g", want, v.t == V_NUM ? v.u.n : 0.0 / 0.0);
    V w = EV("tx.similar(\"kitten\", \"sitting\")");
    CHECK(w.t == V_NUM && fabs(w.u.n - want) < 1e-9,
          "SPEC 4.2 tx.similar is ~= as a function: want %g", want);
  }

  T("SPEC 1.2 pipe: x | f(a,b) == f(x,a,b), the value is ALWAYS the first argument");
  ck_txt("\"hello\" | tx.subst(\"l\",\"L\")", "heLLo");    /* arg0 = the piped text */
  ck_str("\"a,b\" | tx.split(\",\")", "[a,b]");
  ck_txt("[1,2,3] | tx.join(\"-\")", "1-2-3");
  ck_len("\"ab\" | tx.pad(4)", 4);                 /* the piped value is arg 0, 4 is arg 1 */
  ck_txt("\"a\" + \"b\" | tx.upper()", "AB");       /* | binds looser than + */
  ck_str("[1,2] | map(x => x + 1)", "[2,3]");      /* bare name: the pipe fills the receiver */
  ck_num("0 - 5 | abs()", 5);                      /* the pipe adds no argument of its own */
  ck_refused("[1,2] | len");                       /* LEARN: a pipe "needs a CALL with parens" */

  T("SPEC 1.2 a..b, and . as field / index / namespace call");
  ck_str("1..4", "[1,2,3,4]");
  ck_str("3..3", "[3]");
  ck_txt("\"abc\"[1..2]", "b");                    /* LEARN: s = "abc"[1..2] is a slice */
  ck_txt("\"abcd\"[2..]", "cd");                   /* s[2..] runs to the end */
  ck_refused("s = \"abcd\"\ns[..2]");              /* LEARN: s[..2] is not a form */
  ck_str("{a:{b:1}}.a.b", "1");                    /* a.b is a record field */
  ck_str("[7,8,9][1]", "8");
  ck_true("fs.exists(\"spec15.txt\")");            /* fs.read is a namespace call (§1.2) */
  ck_code("fs.bogus(1)", "UNDEF");                 /* an unknown namespace call is refused */
  /* LEFT FAILING: §1.2 documents `a.0` as the list-index spelling of `.`. The
   * parser refuses a number after a dot, so only a[0] works. */
  ck_num("[7,8,9].0", 7);
}

static void spec13_statements(void) {
  T("SPEC 1.3 assignment declares");
  ck_num("x = 1\nx", 1);
  ck_num("x = 1\nx = x + 1\nx", 2);
  ck_num("x = 1; y = 2\nx + y", 3);                /* ; separates statements too */

  T("SPEC 1.3 def: the block's last expression is the value, return exits early");
  ck_num("def add(a, b) { a + b }\nadd(2, 3)", 5);
  ck_num("def early(n) { if n > 1 { return 9 }\n7 }\nearly(0)", 7);
  ck_num("def early(n) { if n > 1 { return 9 }\n7 }\nearly(5)", 9);
  ck_str("def f() { return }\nf().type()", "null");   /* return expr? -> a null value */
  ck_num("def id(a) { a }\nid(4)", 4);
  ck_num("f = x => x * 2\nf(4)", 8);
  ck_num("g = (a, b) -> a + b\ng(2, 3)", 5);
  ck_code("def add(a, b) { a + b }\nadd(1)", "ARITY");   /* §2.2: arity is a script defect */

  T("SPEC 1.3 recursion, and the guard the doc promises");
  ck_num("def fib(n) { if n < 2 { n } else { fib(n-1) + fib(n-2) } }\nfib(10)", 55);
  ck_num("def sumto(n) { if n == 0 { 0 } else { n + sumto(n - 1) } }\nsumto(100)", 5050);
  ck_code("def f(n) { f(n + 1) }\nf(0)", "GUARD");
  CKI(err_exit(E_GUARD), 2);
  {
    V v = EV("def f(n) { f(n + 1) }\nf(0)");
    Str g = bytes_of(v);
    CK(is_err(v) && g.p && strstr(g.p, "512"));    /* §1.3: 递归深度 512, named in the line */
    /* LEFT FAILING: §1.3 names the code GUARD_LIMIT; the table prints GUARD. */
    const char *got = is_err(v) ? err_name(v_errcode(v)) : "(no error)";
    CHECK(is_err(v) && !strcmp(got, "GUARD_LIMIT"),
          "SPEC 1.3 the guard trip is `ERR GUARD_LIMIT`, the build says \"%s\"", got);
  }

  T("SPEC 1.3 if / elif / else");
  ck_str("if 1 > 2 { \"a\" } else { \"b\" }", "b");
  ck_str("if 0 { \"a\" } elif 1 { \"b\" } else { \"c\" }", "b");
  ck_str("a = 0\nfor k in range(4) { if k == 0 { a = a } elif k == 1 { a = a + 10 } "
         "elif k == 2 { a = a + 20 } else { a = a + 30 } }\na", "60");
  ck_str("if false { 1 }", "-");                   /* no else: the value is null */
  ck_refused("if 1 { 2 } else if 3 { 4 }");        /* LEARN: "else if" is not a form */

  T("SPEC 1.3 for, over a list and over a record with two names");
  ck_num("t = 0\nfor v in [1,2,3] { t = t + v }\nt", 6);
  ck_str("s = []\nfor k, v in {a:1,b:2} { s[s.len] = [k, v] }\ns", "[[a,1],[b,2]]");
  ck_str("s = []\nfor k, v in [9,8] { s[s.len] = [k, v] }\ns", "[[9,0],[8,1]]");
  ck_num("for v in {a:1,b:2} { v }", 2);           /* one name over a rec walks the values */
  ck_num("s = []\nfor c in \"ab\" { s[s.len] = c }\ns.len", 2);
  ck_str("for k, v in {a:1} { k }", "a");
  ck_num("t = 0\nfor v in range(2) { t = t + 1 }\nt", 2);

  T("SPEC 1.3 while, break, continue");
  ck_str("i = 0\nn = 0\nwhile i < 5 { i = i + 1\n if i == 3 { continue }\n n = n + i }\n[n, i]", "[12,5]");
  ck_str("i = 0\nwhile true { i = i + 1\n if i == 4 { break } }\ni", "4");
  ck_str("t = 0\nn = 0\nwhile t < 6 { t = t + 1\n if t == 2 { continue }\n n = n + t }\n[n, t]", "[19,6]");
  ck_num("n = 0\nfor v in [1,2,3,4] { n = n + v }\nn", 10);
  /* LEFT FAILING (implementation defect, not a doc dispute): §1.3 lists `break
   * continue` as statements, and they work inside `while`. Inside `for` they are
   * handled by the loop over the BODY'S STATEMENTS instead of the loop over the
   * items, so `break` only aborts the rest of one body and `continue` skips
   * nothing: the list below comes out [1,2,3] (break ignored) with a 2 in it
   * (continue ignored). Fix in ev_for: evaluate the body as one node and test
   * c->flow at the item level, the way ev_while already does. */
  ck_str("s = []\nfor v in [1,2,3,4] { if v == 2 { continue }\n if v == 4 { break }\n s[s.len] = v }\ns",
         "[1,3]");

  T("SPEC 1.3 scoping: {} is a new scope, outer names write through");
  ck_num("a = 1\ndef bump() { a = a + 1 }\nbump()\nbump()\na", 3);
  ck_num("def loc() { inner = 7\n inner }\nloc()", 7);
  ck_code("def loc() { inner = 7\n inner }\nloc()\ninner", "UNDEF");
  ck_code("if true { y = 5 }\ny", "UNDEF");
  ck_num("z = 1\nif true { z = 2 }\nz", 2);
}

static void spec16_truthy(void) {
  T("SPEC 1.6 null false 0 \"\" [] {} are falsy, everything else truthy");
  ck_false("null"); ck_false("false"); ck_false("0"); ck_false("\"\""); ck_false("[]"); ck_false("{}");
  ck_true("true"); ck_true("1"); ck_true("\"x\""); ck_true("[0]"); ck_true("{a:0}");
  ck_true("0.5"); ck_false("0.0");
  /* §2.2: a command's code is 0 on success and 0 is falsy, so `if r.code` is the
   * failure branch - the same truthiness rule, on data (§5 of LEARN) */
  ck_false("0"); ck_true("1");
}

/* ===================================================================== */
/* SPEC 2.1 the compact form, compared as EXACT BYTES                     */
/* ===================================================================== */
static void spec21_compact_bytes(void) {
  T("SPEC 2.1 scalars");
  ck_str("42", "42");
  ck_str("3.5", "3.5");
  ck_str("true", "t");                             /* bool as t/f */
  ck_str("false", "f");
  ck_str("null", "-");                             /* null as '-' */
  ck_str("\"bare\"", "bare");                      /* safe strings are written bare */
  ck_str("\"needs quoting\"", "\"needs quoting\"");
  ck_str("\"k=v\"", "\"k=v\"");                    /* '=' is structural, so it must be quoted */
  ck_str("\"a,b\"", "\"a,b\"");
  ck_str("[1,2,3]", "[1,2,3]");                    /* a list of scalars */
  ck_str("[]", "[]");
  ck_str("-2", "-2");

  T("SPEC 2.1 a record: the top level drops its outer braces, nesting keeps them");
  ck_str("{path:\"src/main.c\",hash:\"7c19f3a1b2c3\",lines:\"10-40\",total:312}",
         "path=src/main.c,hash=7c19f3a1b2c3,lines=10-40,total=312");
  ck_str("{a:{b:1},c:[1,2]}", "a={b=1},c=[1,2]"); /* never flattened to a.b=1 (§2.1) */
  ck_str("{}", "{}");
  ck_str("{a:1}.keys", "[a]");
  ck_str("{k:{j:{i:1}}}", "k={j={i=1}}");

  T("SPEC 2.1 a list of records, and null/bool inside a list");
  ck_str("[{path:\"a.c\",kind:\"modify\"},{path:\"b.c\",kind:\"delete\"}]",
         "[{path=a.c,kind=modify},{path=b.c,kind=delete}]");
  ck_str("[1,2,\"x\",null]", "[1,2,x,-]");
  ck_str("[true,false,null]", "[t,f,-]");
  ck_str("[[1],[2]]", "[[1],[2]]");

  T("SPEC 2.1 the error line, byte for byte");
  {
    /* docs/LEARN.md prints this under "These are real emitted bytes". */
    V e = v_err_hint(E_NOENT, s_lit(&A, "no such file nope.txt"),
                     s_lit(&A, "path missing: run fs.ls/fs.glob to confirm the real path"));
    Str g = bytes_of(e);
    const char *want = "!ERR code=NOENT msg=\"no such file nope.txt\" "
                       "hint=\"path missing: run fs.ls/fs.glob to confirm the real path\"";
    size_t wl = strlen(want);
    CHECK(g.p && (size_t)g.len == wl && !memcmp(g.p, want, wl),
          "SPEC 2.1 error line\n      want \"%s\"\n      got  \"%.*s\"", want, g.len, g.p ? g.p : "");
  }
  /* LEFT FAILING: SPEC 2.1's NEED_CONFIRM line carries no msg and puts op/token
   * before the hint; the serializer always emits msg=, and flat data rides after
   * it. What the rule actually is, is pinned by the second check. */
  {
    V e = v_err_hint(E_NEED_CONFIRM, s_null(), s_lit(&A, "rerun with --confirm 9f2c1a"));
    Rec *d = rec_new(&A);
    rec_set(&A, d, s_wrap("op"), v_strz(&A, "fs.write"));
    rec_set(&A, d, s_wrap("token"), v_strz(&A, "9f2c1a"));
    e.u.e->data = rec_to_v(d);
    Str g = bytes_of(e);
    const char *want = "!ERR code=NEED_CONFIRM op=fs.write token=9f2c1a hint=\"rerun with --confirm 9f2c1a\"";
    size_t wl = strlen(want);
    CHECK(g.p && (size_t)g.len == wl && !memcmp(g.p, want, wl),
          "SPEC 2.1 NEED_CONFIRM line\n      want \"%s\"\n      got  \"%.*s\"", want, g.len, g.p ? g.p : "");
    V f = v_err_hint(E_NEED_CONFIRM, s_lit(&A, "rerun with --confirm 9f2c1a"),
                     s_lit(&A, err_hint(E_NEED_CONFIRM)));
    Rec *d2 = rec_new(&A);
    rec_set(&A, d2, s_wrap("op"), v_strz(&A, "fs.write"));
    rec_set(&A, d2, s_wrap("token"), v_strz(&A, "9f2c1a"));
    f.u.e->data = rec_to_v(d2);
    Str h = bytes_of(f);
    const char *hant = "!ERR code=NEED_CONFIRM msg=\"rerun with --confirm 9f2c1a\" "
                       "hint=\"rerun the same script with --confirm <token> from this plan\" "
                       "op=fs.write token=9f2c1a";
    size_t hl = strlen(hant);
    CHECK(h.p && (size_t)h.len == hl && !memcmp(h.p, hant, hl),
          "SPEC 2.1 flat data rides on the error line\n      want \"%s\"\n      got  \"%.*s\"",
          hant, h.len, h.p ? h.p : "");
  }

  T("SPEC 2.1 the one-line invariant");
  {
    V v = EV("[\"a\\nb\", {k:\"c\\rd\"}, \"x\\ty\"]");
    Str g = bytes_of(v);
    int nl = 0;
    for (int i = 0; i < g.len; i++) if (g.p[i] == '\n' || g.p[i] == '\r') nl++;
    CHECK(nl == 0, "SPEC 2.1 a compact value is EXACTLY one line: %d raw breaks in \"%.*s\"",
          nl, g.len, g.p ? g.p : "");
  }
  ck_txt("tostr([1,2,\"a b\",null,true])", "[1,2,\"a b\",-,t]");
  ck_txt("tostr({a:1})", "a=1");                   /* the top-level brace rule survives tostr */

  T("SPEC 0.4 determinism: the same value, the same bytes");
  {
    Str s1 = bytes_of(EV("x = {b:2,a:[1,{z:\"q\"}],c:null}\nx"));
    Str s2 = bytes_of(EV("x = {b:2,a:[1,{z:\"q\"}],c:null}\nx"));
    CHECK(s1.len == s2.len && !memcmp(s1.p, s2.p, (size_t)s1.len),
          "identical programs must print identical bytes");
    ck_str("x = {b:2,a:[1,{z:\"q\"}],c:null}\nx", "b=2,a=[1,{z=q}],c=-"); /* insertion order kept */
  }
}

/* ===================================================================== */
/* SPEC 2.2 exit codes - the whole table, so it cannot rot                */
/* ===================================================================== */
static void spec22_exit_codes(void) {
  T("SPEC 2.2 every documented code maps to its documented exit status");
  CKI(err_exit(E_NONE), 0);                        /* 0 成功 */
  /* 2 脚本解析或执行错误: parse, type, arity, undef, guard, limit (LEARN's list) */
  CKI(err_exit(E_PARSE), 2);
  CKI(err_exit(E_SYNTAX), 2);
  CKI(err_exit(E_UNDEF), 2);
  CKI(err_exit(E_TYPE), 2);
  CKI(err_exit(E_ARITY), 2);
  CKI(err_exit(E_RANGE), 2);
  CKI(err_exit(E_GUARD), 2);
  CKI(err_exit(E_LIMIT), 2);
  CKI(err_exit(E_BAD_INPUT), 2);
  CKI(err_exit(E_REGEX), 2);
  CKI(err_exit(E_UNSUPPORTED), 2);
  /* 3 需要二次确认 */
  CKI(err_exit(E_NEED_CONFIRM), 3);
  /* 4 策略拒绝（越界、禁用） */
  CKI(err_exit(E_OUTSIDE_JAIL), 4);
  CKI(err_exit(E_DENIED), 4);
  CKI(err_exit(E_POLICY), 4);
  /* 5 IO 错误: NOENT IO NOTDIR ISDIR ANCHOR_MISS ANCHOR_DUP STALE_PLAN BAD_TOKEN SH */
  CKI(err_exit(E_NOENT), 5);
  CKI(err_exit(E_IO), 5);
  CKI(err_exit(E_NOTDIR), 5);
  CKI(err_exit(E_ISDIR), 5);
  CKI(err_exit(E_ANCHOR_MISS), 5);
  CKI(err_exit(E_ANCHOR_DUP), 5);
  CKI(err_exit(E_STALE_PLAN), 5);
  CKI(err_exit(E_BAD_TOKEN), 5);
  CKI(err_exit(E_SH), 5);
  /* 6 内部错误 / 内存, 7 超时 */
  CKI(err_exit(E_OOM), 6);
  CKI(err_exit(E_TIMEOUT), 7);
  /* LEFT FAILING: SPEC 2.2 row 6 is "内部错误 / 报障", so E_INTERNAL must exit 6.
   * It exits 2, which means nothing but OOM ever reaches 6 - and docs/LEARN.md's
   * table hands row 6 to OOM alone. The two documents disagree; SPEC.md wins. */
  CKI(err_exit(E_INTERNAL), 6);

  T("SPEC 2.2 the stable code NAMES an agent branches on (SPEC 0: 稳定错误码)");
  CKS(err_name(E_NOENT), "NOENT");
  CKS(err_name(E_NEED_CONFIRM), "NEED_CONFIRM");
  CKS(err_name(E_OUTSIDE_JAIL), "OUTSIDE_JAIL");
  CKS(err_name(E_ANCHOR_MISS), "ANCHOR_MISS");
  CKS(err_name(E_ANCHOR_DUP), "ANCHOR_DUP");
  CKS(err_name(E_STALE_PLAN), "STALE_PLAN");
  CKS(err_name(E_BAD_TOKEN), "BAD_TOKEN");
  CKS(err_name(E_IO), "IO");
  CKS(err_name(E_NOTDIR), "NOTDIR");
  CKS(err_name(E_ISDIR), "ISDIR");
  CKS(err_name(E_SH), "SH");
  CKS(err_name(E_POLICY), "POLICY");
  CKS(err_name(E_DENIED), "DENIED");
  CKS(err_name(E_UNDEF), "UNDEF");
  CKS(err_name(E_TYPE), "TYPE");
  CKS(err_name(E_ARITY), "ARITY");
  CKS(err_name(E_RANGE), "RANGE");
  CKS(err_name(E_GUARD), "GUARD");
  CKS(err_name(E_LIMIT), "LIMIT");
  CKS(err_name(E_OOM), "OOM");
  CKS(err_name(E_TIMEOUT), "TIMEOUT");
  CKS(err_name(E_SYNTAX), "SYNTAX");
  CKS(err_name(E_PARSE), "PARSE");
  CKS(err_name(E_BAD_INPUT), "BAD_INPUT");
  CKS(err_name(E_REGEX), "REGEX");
  CKS(err_name(E_UNSUPPORTED), "UNSUPPORTED");
  CKS(err_name(E_INTERNAL), "INTERNAL");
  CKS(err_name(E_EXISTS), "EXISTS");
  /* every code carries one actionable hint line (SPEC 0: `hint=` 可执行修复建议) */
  for (int c = E_NONE + 1; c <= E_SH; c++) {
    const char *h = err_hint((ErrCode)c);
    CHECK(h && *h, "code %d has no hint", c);
    CHECK(h && !strchr(h, '\n'), "code %d hint is not one line", c);
    CHECK(h && h[0] != ' ', "code %d hint starts with a space", c);
  }
  /* An ERR propagates through a call as a value instead of running it (§1.5), so
   * tostr(err) hands the error back: the code is still what the agent branches on. */
  ck_code(NOENT "\ntostr(e)", "NOENT");
  ck_str(NOENT "\ntostr(e)", "!ERR code=NOENT msg=\"read: 'nope-not-here.txt' - path missing: "
         "run fs.ls/fs.glob to confirm the real path\" hint=\"path missing: "
         "run fs.ls/fs.glob to confirm the real path\"");
  {                                                /* --fmt json, the other documented shape */
    V e = EV(NOENT);
    CHECK(is_err(e), "SPEC 1.5 the failed read is an error value");
    Str j = v_tojson(&A, e);
    const char *want = "{\"__err\":\"NOENT\",\"msg\":\"read: 'nope-not-here.txt' - path missing: "
                       "run fs.ls/fs.glob to confirm the real path\",\"hint\":\"path missing: "
                       "run fs.ls/fs.glob to confirm the real path\"}";
    size_t wl = strlen(want);
    CHECK(j.p && (size_t)j.len == wl && !memcmp(j.p, want, wl),
          "SPEC 2 --fmt json of an ERR\n      want \"%s\"\n      got  \"%.*s\"", want, j.len, j.p ? j.p : "");
  }
}

/* ===================================================================== */
/* SPEC 3 - PLAN shape, the token law, the jail, receipt, replay, TOCTOU  */
/* ===================================================================== */
static void spec3_plan(void) {
  T("SPEC 3.1 the PLAN shape: op, confirm, token, files");
  ck_named("fs.write(\"spec3-new.txt\", \"hello\").op", "fs.write", "3.1 op");
  ck_named("fs.write(\"spec3-new.txt\", \"hello\").confirm", "jail", "3.3 fs.write defaults to jail");
  {
    V t = EV("p = fs.write(\"spec3-new.txt\", \"hello\")\np.token");
    CHECK(t.t == V_STR && t.u.s.len == 12 && hex_only(t.u.s.p, t.u.s.len),
          "SPEC 3.2 token = base16(sha256(canonical_plan))[:12]");
  }
  ck_num("fs.write(\"spec3-new.txt\", \"hello\").files.len", 1);   /* files IS the impact surface */
  ck_named("fs.write(\"spec3-new.txt\", \"hello\").files[0].kind", "create", "3.1 kind");
  ck_num("fs.write(\"spec3-new.txt\", \"hello\").files[0].bytes", 5);
  ck_named("fs.write(\"spec3-new.txt\", \"hello\").files[0].to", "2cf24dba5fb0",
           "3.1 to is sha256[:12] of the new bytes");
  ck_true("fs.write(\"spec3-new.txt\", \"hello\").files[0].to == tx.hash(\"hello\")");
  ck_str("fs.write(\"spec3-new.txt\", \"hello\").files[0].from", "-");  /* a create replaces nothing */
  ck_str("fs.write(\"spec3-new.txt\", \"hello\").files[0].keys", "[path,kind,to,bytes]");
  {
    V p = EV("fs.write(\"spec3-new.txt\", \"hello\").files[0].path");
    Str g = p.t == V_STR ? p.u.s : s_null();
    const char *want = "spec3-new.txt";
    size_t wl = strlen(want);
    CHECK(p.t == V_STR && (size_t)g.len >= wl && !memcmp(g.p + g.len - wl, want, wl),
          "SPEC 3.1 the plan names the file it will touch, got \"%.*s\"", g.len, g.p ? g.p : "");
  }

  T("SPEC 3.1 a modify plan carries the OLD hash, which is what the token binds");
  mkfile("spec3-has.txt", "old-content");
  ck_named("fs.write(\"spec3-has.txt\", \"new-content\").files[0].kind", "modify", "3.1 kind=modify");
  ck_true("fs.write(\"spec3-has.txt\", \"new-content\").files[0].from == fs.hash(\"spec3-has.txt\")");
  ck_true("fs.write(\"spec3-has.txt\", \"new-content\").files[0].to == tx.hash(\"new-content\")");
  ck_num("fs.write(\"spec3-has.txt\", \"new-content\").files[0].bytes", 11);
  ck_str("fs.write(\"spec3-has.txt\", \"new-content\").files[0].keys", "[path,kind,from,to,bytes]");

  T("SPEC 3.2 / 0.3 the token is a function of the plan and nothing else");
  ck_true("a = fs.write(\"spec3-tok.txt\", \"one\")\nb = fs.write(\"spec3-tok.txt\", \"one\")\n"
          "a.token == b.token");                        /* same script + same input = same token */
  ck_false("a = fs.write(\"spec3-tok.txt\", \"one\")\nb = fs.write(\"spec3-tok.txt\", \"two\")\n"
           "a.token == b.token");                       /* only the target hash changed => new token */
  ck_false("a = fs.write(\"spec3-tok.txt\", \"one\")\nb = fs.write(\"spec3-tok2.txt\", \"one\")\n"
           "a.token == b.token");                       /* a different path is a different plan */
  ck_len("fs.write(\"spec3-tok.txt\", \"one\").token", 12);
  /* bound to the workspace: the same script run under another root mints another token */
  {
    ensure_dir_for("spec3-sub");
    char sub[4096];
    snprintf(sub, sizeof sub, "%s/spec3-sub", ROOT);
    Ctx *c2 = ctx_new(&A, sub, "test_spec.vxa");
    V t1 = EV("p = fs.write(\"spec3-root.txt\", \"same-content\")\np.token");
    if (chdir("spec3-sub") == 0) {
      V t2 = ev_in(c2, "q = fs.write(\"spec3-root.txt\", \"same-content\")\nq.token");
      chdir("..");
      CHECK(t1.t == V_STR && t2.t == V_STR && t2.u.s.len && !s_eq(t1.u.s, t2.u.s),
            "SPEC 3.2 the token is bound to the workspace root: %.*s vs %.*s",
            t1.t == V_STR ? t1.u.s.len : 1, t1.t == V_STR ? t1.u.s.p : "?",
            t2.t == V_STR ? t2.u.s.len : 1, t2.t == V_STR ? t2.u.s.p : "?");
    } else CHECK(0, "cannot enter spec3-sub for the root-binding check");
  }

  T("SPEC 3.4b a PLAN that is never applied does nothing");
  {
    (void)EV("fs.write(\"spec3-never.txt\", \"never-written\")");
    (void)EV("fs.append(\"spec3-never.txt\", \"also-never\")");
    (void)EV("fs.delete(\"spec3-has.txt\")");
    CHECK(rdfile("spec3-never.txt") == NULL, "SPEC 3.4b an unapplied plan must not create the file");
    const char *kept = rdfile("spec3-has.txt");
    CHECK(kept && !strcmp(kept, "old-content"),
          "SPEC 3.4b an unapplied fs.delete must leave the target alone (got \"%s\")", kept ? kept : "(gone)");
  }

  T("SPEC 3.3 jail: a create inside the root runs at once and gives a receipt");
  {
    V r = EV("p = fs.write(\"spec3-jail.txt\", \"fresh\")\nr = p.apply()\nr.code");
    CHECK(r.t == V_NULL, "SPEC 3.3 a jail create receipt is not an error, got %s", v_typename(r));
    const char *on = rdfile("spec3-jail.txt");
    CHECK(on && !strcmp(on, "fresh"), "SPEC 3.3 the create was applied, file holds \"%s\"",
          on ? on : "(absent)");
    ck_num("p = fs.write(\"spec3-jail2.txt\", \"fresh\")\nr = p.apply()\nr.bytes", 5);
    ck_true("p = fs.write(\"spec3-jail3.txt\", \"abcde\")\nr = p.apply()\nr.bytes == 5");
    ck_true("p = fs.write(\"spec3-jail4.txt\", \"fresh\")\nr = p.apply()\nr.token == p.token");
    ck_true("p = fs.write(\"spec3-jail5.txt\", \"fresh\")\nr = p.apply()\nr.path == p.files[0].path");
  }

  T("SPEC 3.3 jail: replacing what exists needs the token");
  {
    mkfile("spec3-replace.txt", "first");
    ck_code("q = fs.write(\"spec3-replace.txt\", \"second\")\nq.apply()", "NEED_CONFIRM");
    ck_field("q = fs.write(\"spec3-replace.txt\", \"second\")\ne = q.apply()", "code", "NEED_CONFIRM");
    ck_field("q = fs.write(\"spec3-replace.txt\", \"second\")\ne = q.apply()", "hint", err_hint(E_NEED_CONFIRM));
    const char *still = rdfile("spec3-replace.txt");
    CHECK(still && !strcmp(still, "first"), "SPEC 3.4 NEED_CONFIRM must not write: file is \"%s\"",
          still ? still : "(absent)");
    /* with the token it applies (SPEC 3.5: p.apply(p.token) is the self-test spelling) */
    ck_true("p = fs.write(\"spec3-replace.txt\", \"second\")\nq = p.apply(p.token)\nq.code == null");
    const char *now = rdfile("spec3-replace.txt");
    CHECK(now && !strcmp(now, "second"), "SPEC 3.4 the confirmed plan applied: file is \"%s\"",
          now ? now : "(absent)");
    /* a consumed token replays the cached result instead of writing twice (§3.4) */
    ck_true("p = fs.write(\"spec3-replace.txt\", \"third\")\nq = p.apply(p.token)\n"
            "w = p.apply(p.token)\nw.replayed == true and w.applied == false");
    const char *after = rdfile("spec3-replace.txt");
    CHECK(after && !strcmp(after, "third"), "SPEC 3.4 a replay never rewrites: file is \"%s\"",
          after ? after : "(absent)");
    /* a token from another plan is refused, and nothing is written */
    ck_code("p = fs.write(\"spec3-replace.txt\", \"fourth\")\np.apply(\"deadbeefcafe\")", "BAD_TOKEN");
    /* TOCTOU: the target moved after the plan was minted, so its token is stale.
     * The plan object has to survive the change, so the interloper write happens
     * inside the same program - and it is itself a plan, applied with its own token. */
    {
      mkfile("spec3-stale.txt", "planned-from");
      V a = EV("g = fs.write(\"spec3-stale.txt\", \"planned-to\")\ng.token");  /* binds planned-from */
      ck_code("p = fs.write(\"spec3-stale.txt\", \"planned-to\")\nt = p.token\n"
              "d = fs.write(\"spec3-stale.txt\", \"someone-else-wrote-this\")\nx = d.apply(d.token)\n"
              "p.apply(t)", "STALE_PLAN");
      const char *theirs = rdfile("spec3-stale.txt");
      CHECK(theirs && !strcmp(theirs, "someone-else-wrote-this"),
            "SPEC 3.2 STALE_PLAN must not clobber the change: file is \"%s\"", theirs ? theirs : "(absent)");
      /* the from-hash is part of the canonical text, so the same content planned
       * against the changed file mints a different token (§3.2) */
      V b = EV("g = fs.write(\"spec3-stale.txt\", \"planned-to\")\ng.token");
      CHECK(a.t == V_STR && b.t == V_STR && !s_eq(a.u.s, b.u.s),
            "SPEC 3.2 the token binds the OLD content hash: %.*s vs %.*s",
            a.t == V_STR ? a.u.s.len : 0, a.t == V_STR ? a.u.s.p : "?",
            b.t == V_STR ? b.u.s.len : 0, b.t == V_STR ? b.u.s.p : "?");
    }
  }

  T("SPEC 3.3 / 4.3 the documented default policy per op");
  ck_named("fs.delete(\"spec3-has.txt\").confirm", "ask", "3.3 fs.delete always asks");
  ck_named("sh.run([\"echo\",\"hi\"]).confirm", "ask", "4.3 sh.run always asks");
  ck_named("fs.write(\"spec3-new.txt\", \"x\").confirm", "jail", "3.3 fs.write defaults to jail");
  ck_named("fs.append(\"spec3-has.txt\", \"x\").confirm", "jail", "3.3 fs.append is a write");
  ck_named("fs.move(\"spec3-has.txt\", \"spec3-moved.txt\").op", "fs.move", "4.1 fs.move returns a PLAN");
  ck_named("fs.delete(\"spec3-has.txt\").op", "fs.delete", "4.1 fs.delete returns a PLAN");
  ck_named("cfg.save().op", "cfg.save", "4.4 cfg.save returns a PLAN");
  ck_true("type(fs.delete(\"spec3-has.txt\")) == \"plan\" and type(sh.run([\"true\"])) == \"plan\"");
  /* LEFT FAILING: SPEC 3.3 gives confirm=none to writes under .vxa/tmp/ (the zero-risk
   * zone: act at once, no token). Every fs.write gets the jail policy instead. */
  ck_named("fs.write(\".vxa/tmp/none-zone.txt\", \"tmp\").confirm", "none", "3.3 the tmp zone is confirm=none");

  T("SPEC 3.4 the script is idempotent, so the plan is too");
  {
    V a = EV("i = fs.write(\"spec3-idem.txt\", \"same\")\ni.token");
    V b = EV("i = fs.write(\"spec3-idem.txt\", \"same\")\ni.token");
    CHECK(a.t == V_STR && b.t == V_STR && s_eq(a.u.s, b.u.s), "SPEC 3.4 two runs, one token");
  }
}

/* ===================================================================== */
/* SPEC 4 - the round-trip laws the doc promises                          */
/* ===================================================================== */
static void spec4_round_trip(void) {
  T("SPEC 4.1 fs.read -> fs.write leaves fs.hash unchanged");
  mkfile("spec4-a.txt", "alpha\nbravo\ncharlie\n");
  ck_true("r = fs.read(\"spec4-a.txt\")\nr.hash == fs.hash(\"spec4-a.txt\")");
  ck_true("r = fs.read(\"spec4-a.txt\")\ntx.hash(r.text) == fs.hash(\"spec4-a.txt\")");
  ck_true("r = fs.read(\"spec4-a.txt\")\nw = fs.write(\"spec4-copy.txt\", r.text)\n"
          "q = w.apply(w.token)\nfs.hash(\"spec4-copy.txt\") == r.hash");
  {
    const char *a = rdfile("spec4-a.txt"), *b = rdfile("spec4-copy.txt");
    CHECK(a && b && !strcmp(a, b), "SPEC 4.1 read->write is byte preserving: \"%s\" vs \"%s\"",
          a ? a : "(absent)", b ? b : "(absent)");
  }
  ck_named("fs.read(\"spec4-a.txt\").path", "spec4-a.txt", "4.1 the read echoes the path you typed");
  ck_num("fs.read(\"spec4-a.txt\").total", 3);
  ck_false("fs.read(\"spec4-a.txt\").truncated");
  ck_true("r = fs.read(\"spec4-a.txt\", {lines:\"2-2\"})\nr.lines == \"2-2\" and r.shown == 1");
  ck_true("r = fs.read(\"spec4-a.txt\", {lines:\"1-40\"})\nr.lines == \"1-3\"");
  ck_true("fs.exists(\"spec4-a.txt\")");
  ck_false("fs.exists(\"spec4-none.txt\")");
  ck_len("fs.hash(\"spec4-a.txt\")", 12);          /* §4.1 sha256[:12] */
  ck_true("type(fs.read(\"spec4-a.txt\").bytes) == \"num\" and "
          "type(fs.read(\"spec4-a.txt\").cursor) == \"num\" and "
          "type(fs.read(\"spec4-a.txt\").est) == \"num\"");
  ck_field("e = fs.read(\"spec4-none.txt\")", "code", "NOENT");
  /* LEFT FAILING: §4.1 types the read record as {path,hash,lines,total,match,tok,
   * text} and §2.1's example line prints `tok=214`. The build reports
   * bytes/shown/truncated/est/cursor (docs/STATUS.md decision 7 dropped `tok` as
   * unspeakable), and a grep read carries no `match` count at all. */
  ck_true("type(fs.read(\"spec4-a.txt\").tok) == \"num\"");
  ck_true("type(fs.read(\"spec4-a.txt\", {grep:\"bravo\"}).match) == \"num\"");

  T("SPEC 4.1 fs.ls / fs.glob / fs.outline shapes");
  ck_true("l = fs.ls(\".\", {glob:\"spec4-*.txt\", max:50})\ntype(l) == \"list\" and l.len >= 1");
  ck_true("e0 = fs.ls(\".\", {glob:\"spec4-a.txt\", max:1})[0]\ntype(e0.bytes) == \"num\" and type(e0.lines) == \"num\"");
  /* LEFT FAILING: §4.1 documents fs.ls -> [{path,bytes,hash,lines}]. The entries the
   * build emits are {path,bytes,lines}: no hash, so "did this file change" still
   * costs one fs.hash per entry. */
  ck_str("fs.ls(\".\", {glob:\"spec4-a.txt\", max:1})[0].keys", "[path,bytes,hash,lines]");
  ck_true("g = fs.glob(\"**/spec4-a.txt\", \".\")\ntype(g) == \"list\" and len(g) >= 1");
  ck_code("fs.outline(\"spec4-no-such.c\")", "NOENT");

  T("SPEC 4.1 fs.outline -> {path,hash,symbols:[{kind,name,line,hash,sig}],lang,tok}");
  {
    mkfile("spec4-sym.c", "int alpha(void) { return 1; }\nint beta(void) { return 2; }\n");
    V o = EV("fs.outline(\"spec4-sym.c\")");
    CHECK(o.t == V_REC, "fs.outline returns a record, got %s", v_typename(o));
    ck_num("fs.outline(\"spec4-sym.c\").symbols.len", 2);
    ck_named("fs.outline(\"spec4-sym.c\").symbols[0].name", "alpha", "4.1 symbols[].name");
    ck_named("fs.outline(\"spec4-sym.c\").symbols[0].kind", "fn", "4.1 symbols[].kind");
    ck_named("fs.outline(\"spec4-sym.c\").lang", "c", "4.1 lang");
    ck_true("o = fs.outline(\"spec4-sym.c\")\no.hash == fs.hash(\"spec4-sym.c\")");
    ck_true("s = fs.outline(\"spec4-sym.c\").symbols[0]\nlen(s.sig) > 0 and type(s.line) == \"num\"");
    /* LEFT FAILING: §4.1 names the symbol fingerprint `hash`; the build emits `at`. */
    ck_len("fs.outline(\"spec4-sym.c\").symbols[0].hash", 8);
    ck_true("type(fs.outline(\"spec4-sym.c\").tok) == \"num\"");
    /* §4.1: with anchors:true every line is prefixed `L12:d6f21459| ` */
    ck_true("r = fs.read(\"spec4-sym.c\", {anchors:true})\n"
            "a = tx.anchors(fs.read(\"spec4-sym.c\").text, {path:\"spec4-sym.c\"})\n"
            "l = tx.lines(r.text)[0]\n"
            "l.starts(\"L1:\") and l[3..11] == a[0].at and l[11] == \"|\" and l[12] == \" \" and "
            "l[13..] == 'int alpha(void) { return 1; }'");
  }
  ck_true("b = fs.bundle([\"spec4-sym.c\"], {max_bytes:2000, only:\"symbols\"})\ntype(b.bundle) == \"list\"");

  T("SPEC 4.2 the anchor hash is base16(sha256(vxa1 prev cur next))[:8], with no line number");
  {
    V a = EV("tx.anchors(\"alpha\\nbeta\\ngamma\", {path:\"spec4-anchor.txt\"})");
    ck_num("tx.anchors(\"alpha\\nbeta\\ngamma\", {path:\"spec4-anchor.txt\"}).len", 3);
    CHECK(a.t == V_LIST && a.u.l->len == 3, "SPEC 4.2 one anchor per line, got %s", v_typename(a));
    if (a.t == V_LIST && a.u.l->len == 3) {
      /* independent recomputation of the definition, separators included */
      const struct { const char *prev, *cur, *next; } cx[3] = {
        { "", "alpha", "beta" }, { "alpha", "beta", "gamma" }, { "beta", "gamma", "" }
      };
      for (int i = 0; i < 3; i++) {
        Sha256 s; uint8_t d[32]; char hx[9];
        const char nul = '\0';
        sha256_init(&s);
        sha256_update(&s, "vxa1", 4); sha256_update(&s, &nul, 1);
        if (*cx[i].prev) sha256_update(&s, cx[i].prev, strlen(cx[i].prev));
        sha256_update(&s, &nul, 1);
        sha256_update(&s, cx[i].cur, strlen(cx[i].cur));
        sha256_update(&s, &nul, 1);
        if (*cx[i].next) sha256_update(&s, cx[i].next, strlen(cx[i].next));
        sha256_final(&s, d);
        base16(d, 4, hx); hx[8] = 0;
        V at = fld(item(a, i), "at");
        CHECK(at.t == V_STR && at.u.s.len == 8 && hex_only(at.u.s.p, at.u.s.len) &&
              !memcmp(at.u.s.p, hx, 8),
              "SPEC 4.2 anchor %d: want %s (8 hex of sha256 over vxa1|prev|cur|next), got %.*s",
              i, hx, at.t == V_STR ? at.u.s.len : 0, at.t == V_STR ? at.u.s.p : "(none)");
      }
      /* LEFT FAILING: §4.2 types tx.anchors -> [{n,at,hash}]; the build's third
       * field is `text`, so the anchor's own hash is not what you get back. */
      V h = fld(item(a, 0), "hash");
      CHECK(h.t != V_NULL, "SPEC 4.2 tx.anchors -> [{n,at,hash}]: the `hash` field is absent");
      ck_num("tx.anchors(\"alpha\\nbeta\\ngamma\", {path:\"spec4-anchor.txt\"})[0].n", 0);
    }
    /* the law the section exists FOR: an anchor is a content address, so inserting
     * a line upstream does not move the anchors whose context did not change (§4.2) */
    V b = EV("tx.anchors(\"intro\\nalpha\\nbeta\\ngamma\", {path:\"spec4-anchor.txt\"})");
    if (a.t == V_LIST && b.t == V_LIST && a.u.l->len == 3 && b.u.l->len == 4) {
      int same = 1;
      for (int i = 1; i < 3; i++) {
        V x = fld(item(a, i), "at"), y = fld(item(b, i + 1), "at");
        if (x.t != V_STR || y.t != V_STR || !s_eq(x.u.s, y.u.s)) same = 0;
      }
      CK(same);                          /* every anchor but the first survived the insert */
      V x0 = fld(item(a, 0), "at"), y1 = fld(item(b, 1), "at");
      CHECK(x0.t == V_STR && y1.t == V_STR && !s_eq(x0.u.s, y1.u.s),
            "SPEC 4.2 the first line's prev is the empty string, so its anchor DOES move");
    } else CHECK(0, "anchor stability needs both tables");
    /* LEFT FAILING: §4.2 prints the signature as tx.anchors(text, path); the build
     * takes an options record and refuses a positional str. */
    ck_num("tx.anchors(\"alpha\\nbeta\\ngamma\", \"spec4-anchor.txt\").len", 3);
  }

  T("SPEC 4.2 tx.patch -> {text, applied, checks}, the checks re-read for you");
  {
    ck_true("t = \"one\\ntwo\\nthree\"\na = tx.anchors(t, {path:\"p\"})\n"
            "p = tx.patch(t, [{at:a[1].at, op:\"set\", text:\"TWO\"}], {path:\"p\"})\n"
            "p.text == \"one\\nTWO\\nthree\"");
    ck_num("t = \"one\\ntwo\\nthree\"\na = tx.anchors(t, {path:\"p\"})\n"
           "tx.patch(t, [{at:a[1].at, op:\"set\", text:\"TWO\"}], {path:\"p\"}).applied", 1);
    ck_true("t = \"one\\ntwo\\nthree\"\na = tx.anchors(t, {path:\"p\"})\n"
            "tx.patch(t, [{at:a[1].at, op:\"set\", text:\"TWO\"}], {path:\"p\"}).checks[0].ok == true");
    ck_true("t = \"one\\ntwo\\nthree\"\na = tx.anchors(t, {path:\"p\"})\n"
            "p = tx.patch(t, [{at:a[1].at, op:\"set\", text:\"TWO\"}], {path:\"p\"})\n"
            "p.checks[0].at == a[1].at");
    ck_true("t = \"one\\ntwo\\nthree\"\na = tx.anchors(t, {path:\"p\"})\n"
            "p = tx.patch(t, [{at:a[0].at, op:\"del\"}, {at:a[2].at, op:\"ins_after\", text:\"end\"}], {path:\"p\"})\n"
            "p.text == \"two\\nthree\\nend\" and p.applied == 2");
    ck_code("tx.patch(\"one\\ntwo\", [{at:\"00000000\", op:\"set\", text:\"x\"}], {path:\"p\"})", "ANCHOR_MISS");
  }

  T("SPEC 4.2 the diff/patch round-trip law: replay diff(a,b) on a and you get b");
  {
    static const char *TA = "alpha\nbravo\ncharlie\ndelta\n";
    static const char *TB = "alpha\nBRAVO\ncharlie\ndelta\necho\n";
    ck_str("tx.diff(\"a\\nb\\n\", \"a\\nc\\n\", {unified:true}).keys", "[text,adds,dels,hunks]");
    ck_num("d = tx.diff(\"a\\nb\\n\", \"a\\nc\\n\", {unified:true})\nd.adds", 1);
    ck_num("d = tx.diff(\"a\\nb\\n\", \"a\\nc\\n\", {unified:true})\nd.dels", 1);
    ck_num("d = tx.diff(\"a\\nb\\n\", \"a\\nc\\n\", {unified:true})\nd.hunks", 1);
    ck_true("d = tx.diff(\"same\\n\", \"same\\n\", {unified:true})\nd.adds == 0 and d.dels == 0 and d.hunks == 0");
    ck_true("a = \"alpha\\nbravo\\ncharlie\\ndelta\\n\"\nb = \"alpha\\nBRAVO\\ncharlie\\ndelta\\necho\\n\"\n"
            "d = tx.diff(a, b, {unified:false})\n"
            "p = tx.patch(a, d.ops, {path:\"rt.txt\"})\n"
            "p.text == b");
    ck_true("a = \"alpha\\nbravo\\ncharlie\\ndelta\\n\"\nb = \"bravo\\ncharlie\\n\"\n"            /* pure deletions */
            "d = tx.diff(a, b, {unified:false})\ntx.patch(a, d.ops, {path:\"rt\"}).text == b");
    ck_true("tx.patch(\"a\\n\", tx.diff(\"a\\n\", \"a\\nb\\n\", {unified:false}).ops, {path:\"rt\"}).text == \"a\\nb\\n\"");
    ck_true("tx.patch(\"one\\n\", tx.diff(\"one\\n\", \"\", {unified:false}).ops, {path:\"rt\"}).text == \"\"");
    ck_true("tx.patch(\"\", tx.diff(\"\", \"one\", {unified:false}).ops, {path:\"rt\"}).text == \"one\"");
    /* LEFT FAILING (implementation gap, not a doc dispute): the law is "replay
     * diff(a,b) on a and you get b", byte for byte. In the empty-source case the
     * added lines are joined without the final newline, so a target that ends in
     * LF cannot be reproduced onto an empty text. The fix site is xdiff.c's
     * ops_list/join_lines, which is outside the files this suite may change. */
    ck_true("tx.patch(\"\", tx.diff(\"\", \"one\\n\", {unified:false}).ops, {path:\"rt\"}).text == \"one\\n\"");
    /* the same law through the C entry point the API table names */
    {
      V a = v_strz(&A, TA), b = v_strz(&A, TB);
      V d = tx_diff(&A, a.u.s, b.u.s, 3, false);
      V ops = fld(d, "ops");
      CHECK(d.t == V_REC && ops.t == V_LIST, "SPEC 4.2 the compact diff carries the ops that replay it");
      if (ops.t == V_LIST) {
        V r = tx_patch_apply(&A, a.u.s, "rt.c", ops);
        V t = fld(r, "text");
        CHECK(t.t == V_STR && t.u.s.len == b.u.s.len && !memcmp(t.u.s.p, b.u.s.p, (size_t)b.u.s.len),
              "SPEC 4.2 round trip: want \"%s\" got \"%.*s\"", TB,
              t.t == V_STR ? t.u.s.len : 0, t.t == V_STR ? t.u.s.p : "(none)");
      }
    }
    /* on a real file: the write lands the hash the plan promised */
    {
      mkfile("spec4-rt.txt", TA);
      V ok = EV("r = fs.read(\"spec4-rt.txt\")\n"
                "d = tx.diff(r.text, \"alpha\\nBRAVO\\ncharlie\\ndelta\\necho\\n\", {unified:false})\n"
                "p = tx.patch(r.text, d.ops, {path:\"spec4-rt.txt\"})\n"
                "w = fs.write(\"spec4-rt.txt\", p.text)\n"
                "q = w.apply(w.token)\n"
                "fs.hash(\"spec4-rt.txt\") == w.files[0].to and q.code == null");
      Buf bb; describe(ok, &bb);
      CHECK(!is_err(ok), "the file round trip errors out: %s", bb.p ? bb.p : "(?)");
      CHECK(ok.t == V_BOOL && ok.u.b, "SPEC 4.1/3.1 the applied file matches the promised to-hash");
      const char *on = rdfile("spec4-rt.txt");
      CHECK(on && !strcmp(on, TB), "SPEC 3.1/4.2 the file now holds the diff target: \"%s\"",
            on ? on : "(absent)");
    }
  }

  T("SPEC 4.2 the text tools the doc shows");
  /* LEFT FAILING: §4.2 types tx.subst/tx.find with a `re` (and promises `${1}` in
   * the replacement). The build has no regex engine: tx.subst is a literal replace
   * and a pattern is passed through unchanged, so the documented call below does
   * not do what the doc says it does. */
  ck_txt("tx.subst(\"a1b22\", \"[0-9]+\", \"N\")", "aNbN");
  ck_txt("tx.subst(\"hello world\", \"l\", \"L\")", "heLLo worLd");
  ck_txt("tx.join([\"a\",\"b\"], \"|\")", "a|b");
  ck_num("tx.find(\"one\\ntwo one\\n\", \"one\").len", 2);
  ck_num("tx.find(\"one\\ntwo one\\n\", \"one\")[1].n", 2);
  ck_true("len(tx.find(\"one\\ntwo three\\n\", \"zzz\")) == 0");
  ck_true("tx.word_wrap(\"aa bb cc\", 5) == \"aa bb\\ncc\"");
  ck_true("tx.unlines(tx.lines(\"a\\nb\\n\")) == \"a\\nb\\n\"");
}

/* ===================================================================== */
/* SPEC 4.4 out / cfg and the globals §1.4 lists                          */
/* ===================================================================== */
static void spec44_out_cfg(void) {
  T("SPEC 4.4 cfg / out and the global functions");
  ck_txt("cfg.get(\"nope-key\", \"dflt\")", "dflt");
  ck_true("cfg.set(\"spec4-k\", 7)\ncfg.get(\"spec4-k\") == 7");
  ck_true("type(cfg.keys()) == \"list\"");
  ck_true("cfg.merge({m:1})\ncfg.get(\"m\") == 1");
  ck_str("keys({a:1,b:2})", "[a,b]");              /* globals in call position (§1.4) */
  ck_str("vals({a:1})", "[1]");
  ck_num("sum([1,2,3])", 6);
  ck_num("len([1,2])", 2);
  ck_num("range(3).len", 3);
  ck_num("max([1,7,3])", 7);
  ck_num("min([1,7,3])", 1);
  ck_num("abs(0 - 5)", 5);
  ck_num("num(\"42\")", 42);
  ck_txt("tostr([1,2])", "[1,2]");
  ck_len("hash([1,2])", 12);
  ck_true("type(now().epoch) == \"num\" and len(now().iso) == 19");
  ck_str("out.emit({a:1})", "emitted=t");          /* §4.4 out.emit answers with an ack */
  ck_str("out.kv(\"bytes\", 12)", "emitted=t");      /* one key=value line on stdout, an ack back */
  ck_str("out.json([1,2])", "emitted=t");
  ck_str("out.raw(\"s\")", "emitted=t");
  /* SPEC 2: out.log is progress on stderr; it answers with an ack, never stdout text */
  ck_str("out.log(\"progress\")", "logged=t");
  ck_code("out.die({code:\"NOHIT\", path:\"p\"})", "LIMIT");
  ck_str("cfg.get(\"nope-key\")", "-");              /* §4.4: no default is a null, never a guess */
}

/* ===================================================================== */
int main(void) {
  arena_init(&A, 0);
  enter_scratch();
  wipe_here();                       /* the scratch dir holds only what this suite makes */

  spec15_err_idiom();
  spec14_values();
  spec11_lex();
  spec12_operators();
  spec13_statements();
  spec16_truthy();
  spec21_compact_bytes();
  spec22_exit_codes();
  spec3_plan();
  spec4_round_trip();
  spec44_out_cfg();

  /* leave the scratch (three levels below the repo root) and remove only it */
  if (chdir("../../..") == 0) rmrf("build/scratch/spec");
  T_REPORT("spec");
}
