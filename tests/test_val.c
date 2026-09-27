/* test_val.c - values + the compact output protocol (SPEC §2.1).
 * The exact byte strings below are contractual: plan tokens hash them.
 * §2.1 v2: structure is carried by {}/[] delimiters and ',' separators, never
 * by line position, and a compact value is always exactly one line. */
#include "vxa.h"
#include "t.h"
#include <stdlib.h>

static Arena A;

/* plan.c is not in this test's link set; the stub only proves delegation. */
void plan_disp(Plan *p, Buf *b, bool compact) {
  (void)p;
  buf_puts(b, compact ? "<plan>" : "{\"plan\":\"stub\"}");
}

static V mkfn(const char *name, int arity) {
  static Fn f;
  f.kind = F_SCRIPT; f.body = NULL; f.names = NULL; f.env = NULL;
  f.nparams = arity; f.name = name; f.native = NULL; f.ud = NULL;
  V v; v.t = V_FN; v.u.f = &f; return v;
}
static V mkplan(void) {
  static int fake;
  V v; v.t = V_PLAN; v.u.pl = (Plan*)&fake; return v;
}
static V mknat(void *p) { V v; v.t = V_NAT; v.u.nat = p; return v; }
static V mkerr(ErrCode c, const char *msg, const char *hint, V data) {
  PErr *e = (PErr*)arena_alloc(&A, sizeof(PErr));
  e->code = c; e->msg = s_lit(&A, msg); e->hint = s_lit(&A, hint); e->data = data;
  V v; v.t = V_ERR; v.u.e = e; return v;
}
static void ckcompact(V v, const char *want) { CKSTR(v_tostr(&A, v, true), want); }
static void ckjson(V v, const char *want) { CKSTR(v_tojson(&A, v), want); }
/* same value, built twice: used to prove the serializers are byte-stable */
static V mk_ordered(void) {
  return v_rec(&A, 3,
               "b", v_list(&A, 2, v_num(1), v_strz(&A, "s p")),
               "a", v_rec(&A, 2, "z", v_num(0), "y", VF),
               "c", v_num(2.5));
}
/* the load-bearing invariant: compact output never contains a raw CR or LF */
static int linebreaks(Str s) {
  int n = 0;
  for (int i = 0; i < s.len; i++) { char c = s.p[i]; if (c == '\n' || c == '\r') n++; }
  return n;
}
static void ckoneline(V v) {
  Str s = v_tostr(&A, v, true);
  CKI(linebreaks(s), 0);
  CK(s.len > 0 && s.p[s.len] == 0);                      /* NUL-terminated, greppable */
}

int main(void) {
  arena_init(&A, 0);

  T("typename");
  CKS(v_typename(v_nil()), "null");
  CKS(v_typename(VT), "bool");
  CKS(v_typename(v_bool(false)), "bool");
  CKS(v_typename(v_num(1)), "num");
  CKS(v_typename(v_strz(&A, "x")), "str");
  CKS(v_typename(v_list(&A, 0)), "list");
  CKS(v_typename(rec_to_v(rec_new(&A))), "rec");
  CKS(v_typename(mkfn("f", 0)), "fn");
  CKS(v_typename(mkplan()), "plan");
  CKS(v_typename(v_err(&A, E_IO, "x")), "err");
  CKS(v_typename(mknat(NULL)), "nat");
  { V bad; bad.t = 200; bad.u.n = 0; CKS(v_typename(bad), "unknown"); }

  T("ctors");
  CK(v_bool(true).u.b && !v_bool(false).u.b);
  CKD(v_num(2.5).u.n, 2.5);
  CK(v_nil().t == V_NULL);
  CKSTR(v_strz(&A, "abc").u.s, "abc");
  CKSTR(v_str(s_lit(&A, "q")).u.s, "q");
  CK(rec_to_v(rec_new(&A)).u.r != NULL);

  T("list");
  List *l = list_new(&A);
  CKI(l->len, 0);
  CK(list_get(l, 0) == NULL);
  CK(list_get(NULL, 0) == NULL);
  for (int i = 0; i < 10; i++) list_push(&A, l, v_num(i));
  CKI(l->len, 10);
  CK(l->cap >= 10);
  CKD(list_get(l, 0)->u.n, 0);
  CKD(list_get(l, 9)->u.n, 9);
  CK(list_get(l, 10) == NULL);
  CK(list_get(l, -1) == NULL);
  *list_get(l, 3) = v_num(99);
  CKD(list_get(l, 3)->u.n, 99);
  { V lv = v_list(&A, 3, v_num(1), v_num(2), v_num(3));
    CKI(lv.u.l->len, 3);
    CKI(lv.u.l->cap, 4);
    ckcompact(lv, "[1,2,3]");
    ckjson(lv, "[1,2,3]"); }
  ckcompact(v_list(&A, 0), "[]");

  T("rec order");
  Rec *r = rec_new(&A);
  rec_setz(&A, r, "a", v_num(1));
  rec_setz(&A, r, "b", v_num(2));
  rec_setz(&A, r, "c", v_num(3));
  CKI(r->len, 3);
  CK(s_eqz(r->kv[0].k, "a") && s_eqz(r->kv[1].k, "b") && s_eqz(r->kv[2].k, "c"));
  ckcompact(rec_to_v(r), "a=1,b=2,c=3");
  rec_setz(&A, r, "a", v_num(7));                       /* overwrite keeps position */
  CKI(r->len, 3);
  CK(s_eqz(r->kv[0].k, "a"));
  CKD(r->kv[0].v.u.n, 7);
  ckcompact(rec_to_v(r), "a=7,b=2,c=3");
  CK(rec_getz(r, "b") != NULL);
  CK(rec_get(r, s_lit(&A, "c")) != NULL);
  CK(rec_getz(r, "zz") == NULL);
  CK(rec_get(NULL, s_wrap("a")) == NULL);
  CK(rec_del(&A, r, s_wrap("b")));
  CKI(r->len, 2);
  CK(s_eqz(r->kv[1].k, "c"));
  CK(!rec_del(&A, r, s_wrap("b")));
  ckcompact(rec_to_v(r), "a=7,c=3");
  rec_setz(&A, r, "b", v_num(9));                        /* new key appends */
  CK(s_eqz(r->kv[2].k, "b"));
  ckcompact(rec_to_v(r), "a=7,c=3,b=9");
  { Rec *big = rec_new(&A);
    for (int i = 0; i < 20; i++) rec_set(&A, big, s_fmt(&A, "k%02d", i), v_num(i));
    CKI(big->len, 20); CK(big->cap >= 20);
    CK(s_eqz(big->kv[19].k, "k19"));
    CKD(rec_get(big, s_lit(&A, "k07"))->u.n, 7); }
  { V rv = v_rec(&A, 2, "k1", v_num(1), "k2", v_strz(&A, "s"));
    CKI(rv.u.r->len, 2);
    CK(s_eqz(rv.u.r->kv[0].k, "k1") && s_eqz(rv.u.r->kv[1].k, "k2"));
    ckcompact(rv, "k1=1,k2=s"); }

  T("key");
  { size_t before = A.total; Str k = key(&A, "path");
    CK(A.total == before);                               /* static key: no allocation */
    CKI(k.len, 4); CK(s_eqz(k, "path")); }

  T("truthy");
  CK(!v_truthy(v_nil()));
  CK(v_truthy(VT));
  CK(!v_truthy(VF));
  CK(!v_truthy(v_num(0)));
  CK(!v_truthy(v_num(-0.0)));
  CK(v_truthy(v_num(NAN)));
  CK(v_truthy(v_num(1e-300)));
  CK(!v_truthy(v_strz(&A, "")));
  CK(v_truthy(v_strz(&A, "0")));
  CK(!v_truthy(v_list(&A, 0)));
  CK(v_truthy(v_list(&A, 1, v_nil())));
  CK(!v_truthy(rec_to_v(rec_new(&A))));
  CK(v_truthy(v_rec(&A, 1, "a", v_nil())));
  CK(v_truthy(mkfn(NULL, 1)));
  CK(v_truthy(mkplan()));
  CK(v_truthy(v_err(&A, E_IO, "x")));

  T("eq");
  CK(v_eq(v_num(3), v_num(3.0)));
  CK(!v_eq(v_num(3), v_num(4)));
  CK(!v_eq(v_num(1), v_bool(true)));                     /* types differ */
  CK(!v_eq(v_nil(), v_num(0)));
  CK(v_eq(v_nil(), v_nil()));
  CK(v_eq(VT, v_bool(true)) && !v_eq(VT, VF));
  CK(v_eq(v_strz(&A, "ab"), v_strz(&A, "ab")));
  CK(!v_eq(v_strz(&A, "ab"), v_strz(&A, "AB")));
  CK(!v_eq(v_strz(&A, "ab"), v_strz(&A, "a")));
  CK(v_eq(v_str(s_null()), v_strz(&A, "")));              /* empty == empty */
  { V x = v_list(&A, 2, v_num(1), v_strz(&A, "s"));
    V y = v_list(&A, 2, v_num(1), v_strz(&A, "s"));
    V z = v_list(&A, 2, v_num(1), v_strz(&A, "t"));
    V w = v_list(&A, 1, v_num(1));
    CK(v_eq(x, y)); CK(!v_eq(x, z)); CK(!v_eq(x, w)); }
  { V p = v_rec(&A, 2, "a", v_num(1), "b", v_num(2));
    V q = v_rec(&A, 2, "b", v_num(2), "a", v_num(1));    /* order-insensitive */
    V m = v_rec(&A, 2, "a", v_num(1), "b", v_num(3));
    V n = v_rec(&A, 1, "a", v_num(1));
    CK(v_eq(p, q)); CK(!v_eq(p, m)); CK(!v_eq(p, n)); }
  { Fn f1, f2; f1.name = "x"; f1.nparams = 1; f2.name = "x"; f2.nparams = 1;
    V a; a.t = V_FN; a.u.f = &f1; V b; b.t = V_FN; b.u.f = &f2;
    CK(!v_eq(a, b));                                     /* fn by identity */
    CK(v_eq(a, a)); }
  { void *q = (void*)0x1234; CK(v_eq(mknat(q), mknat(q))); CK(!v_eq(mknat(q), mknat(NULL))); }
  CK(!v_eq(v_err(&A, E_IO, "same"), v_err(&A, E_IO, "same")));
  CK(v_eq(v_list(&A, 0), v_list(&A, 0)));
  CK(v_eq(rec_to_v(rec_new(&A)), rec_to_v(rec_new(&A))));

  T("cmp");
  { bool ok = false;
    CKI(v_cmp(v_num(1), v_num(2), &ok), -1); CK(ok);
    ok = false; CKI(v_cmp(v_num(2), v_num(1), &ok), 1); CK(ok);
    ok = false; CKI(v_cmp(v_num(2), v_num(2), &ok), 0); CK(ok);
    ok = false; CKI(v_cmp(v_strz(&A, "abc"), v_strz(&A, "abd"), &ok), -1); CK(ok);
    ok = false; CKI(v_cmp(v_strz(&A, "b"), v_strz(&A, "ab"), &ok), 1); CK(ok);
    ok = false; CKI(v_cmp(v_strz(&A, "ab"), v_strz(&A, "ab"), &ok), 0); CK(ok);
    ok = true; CKI(v_cmp(v_list(&A, 0), v_list(&A, 0), &ok), 0); CK(!ok);
    ok = true; CKI(v_cmp(v_num(1), v_strz(&A, "1"), &ok), 0); CK(!ok);
    ok = true; CKI(v_cmp(v_nil(), v_nil(), &ok), 0); CK(!ok);
    ok = true; CKI(v_cmp(VT, VF, &ok), 0); CK(!ok);
    CKI(v_cmp(v_num(1), v_num(2), NULL), -1); }

  T("err");
  { V e = v_err(&A, E_NOENT, "no such file: %s (%d)", "a.txt", 7);
    CK(v_is_err(e));
    CKI(v_errcode(e), E_NOENT);
    CKSTR(e.u.e->msg, "no such file: a.txt (7)");
    CK(s_eq(e.u.e->hint, s_lit(&A, err_hint(E_NOENT)))); /* hint auto-filled */
    CK(v_eq(e.u.e->data, v_nil()));
    CK(!v_is_err(v_num(1)));
    CKI(v_errcode(v_num(1)), E_NONE);
    ckcompact(e, "!ERR code=NOENT msg=\"no such file: a.txt (7)\" "
                 "hint=\"path missing: run fs.ls/fs.glob to confirm the real path\"");
    ckjson(e, "{\"__err\":\"NOENT\",\"msg\":\"no such file: a.txt (7)\","
              "\"hint\":\"path missing: run fs.ls/fs.glob to confirm the real path\"}"); }
  { char big[401]; memset(big, 'a', 400); big[400] = 0;
    V e = v_err(&A, E_BAD_INPUT, "%s", big);
    CKI(e.u.e->msg.len, 400);
    CK(e.u.e->msg.p[399] == 'a'); }
  { V e = v_err_hint(E_NEED_CONFIRM, s_lit(&A, "needs confirm"), s_lit(&A, "use --confirm"));
    CKI(v_errcode(e), E_NEED_CONFIRM);
    CKSTR(e.u.e->msg, "needs confirm");
    CKSTR(e.u.e->hint, "use --confirm");
    CKSTR(v_repr(&A, e), "ERR(NEED_CONFIRM)");
    ckcompact(e, "!ERR code=NEED_CONFIRM msg=\"needs confirm\" hint=\"use --confirm\""); }
  { V e = mkerr(E_NEED_CONFIRM, "rerun now", "-",
                 v_rec(&A, 2, "op", v_strz(&A, "fs.write"), "token", v_strz(&A, "9f2c1a")));
    ckcompact(e, "!ERR code=NEED_CONFIRM msg=\"rerun now\" hint=- op=fs.write token=9f2c1a");
    ckjson(e, "{\"__err\":\"NEED_CONFIRM\",\"msg\":\"rerun now\",\"hint\":\"-\","
              "\"op\":\"fs.write\",\"token\":\"9f2c1a\"}"); }
  { /* the §2.1 err example, as this code emits it */
    V e = mkerr(E_NEED_CONFIRM, "needs confirm", "rerun with --confirm 9f2c1a",
                v_rec(&A, 2, "op", v_strz(&A, "fs.write"), "token", v_strz(&A, "9f2c1a")));
    ckcompact(e, "!ERR code=NEED_CONFIRM msg=\"needs confirm\" "
                 "hint=\"rerun with --confirm 9f2c1a\" op=fs.write token=9f2c1a");
    ckoneline(e); }
  { /* ERR whose data carries containers: braces/brackets survive, one line stays */
    V e = mkerr(E_ANCHOR_MISS, "anchor not found", "re-read the file",
                v_rec(&A, 3, "at", v_strz(&A, "c3f2"), "n", v_num(2),
                      "nearby", v_list(&A, 2, v_strz(&A, "line one"),
                                             v_rec(&A, 2, "ln", v_num(7), "txt", v_strz(&A, "a,b")))));
    ckcompact(e, "!ERR code=ANCHOR_MISS msg=\"anchor not found\" hint=\"re-read the file\" "
                 "at=c3f2 n=2 nearby=[\"line one\",{ln=7,txt=\"a,b\"}]");
    ckoneline(e); }
  { V e = mkerr(E_IO, "boom", "b", v_num(5));
    ckcompact(e, "!ERR code=IO msg=boom hint=b data=5");
    ckjson(e, "{\"__err\":\"IO\",\"msg\":\"boom\",\"hint\":\"b\",\"data\":5}"); }
  { V e = mkerr(E_IO, "boom", "b", rec_to_v(rec_new(&A)));
    ckcompact(e, "!ERR code=IO msg=boom hint=b data={}"); }
  { V e = mkerr(E_TYPE, "x", "y", v_list(&A, 1, v_nil()));
    ckcompact(e, "!ERR code=TYPE msg=x hint=y data=[-]"); }
  { V rv = v_rec(&A, 1, "err", mkerr(E_STALE_PLAN, "file changed", "rerun", v_nil()));
    ckcompact(rv, "err=!ERR code=STALE_PLAN msg=\"file changed\" hint=rerun"); }

  T("scalars compact");
  ckcompact(v_num(3.0), "3");
  ckcompact(v_num(-2), "-2");
  ckcompact(v_num(3.5), "3.5");
  ckcompact(VT, "t");
  ckcompact(VF, "f");
  ckcompact(v_nil(), "-");
  ckcompact(v_strz(&A, "src/main.c"), "src/main.c");
  ckcompact(v_strz(&A, "a b"), "\"a b\"");
  ckcompact(v_strz(&A, ""), "\"\"");
  ckcompact(v_strz(&A, "a=b"), "\"a=b\"");
  ckcompact(v_strz(&A, "a,b"), "\"a,b\"");
  ckcompact(v_strz(&A, "a:b"), "\"a:b\"");
  ckcompact(v_strz(&A, "a[b]"), "\"a[b]\"");
  ckcompact(v_strz(&A, "a{b}"), "\"a{b}\"");
  ckcompact(v_strz(&A, "a#b"), "\"a#b\"");
  ckcompact(v_strz(&A, "a\"b"), "\"a\\\"b\"");
  ckcompact(v_strz(&A, "a\\b"), "\"a\\\\b\"");
  ckcompact(v_strz(&A, "a\nb"), "\"a\\nb\"");
  ckcompact(v_strz(&A, "a\rb"), "\"a\\rb\"");
  ckcompact(v_strz(&A, "a\tb\vb"), "\"a\\tb\\x0bb\"");
  ckcompact(v_strz(&A, "\xe4\xbd\xa0"), "\"\xe4\xbd\xa0\"");
  ckjson(v_strz(&A, "a\nb"), "\"a\\nb\"");
  ckjson(v_num(3.0), "3");
  ckjson(VT, "true");
  ckjson(VF, "false");
  ckjson(v_nil(), "null");
  ckjson(v_num(NAN), "null");                                /* stays valid JSON */
  ckjson(v_num(1e20), "1e+20");
  { /* ':' is not part of the compact grammar, and neither is a newline */
    Str s = v_tostr(&A, v_rec(&A, 2, "a", v_rec(&A, 1, "b", v_num(1)),
                                        "c", v_list(&A, 2, v_num(1), v_num(2))), true);
    CKSTR(s, "a={b=1},c=[1,2]");
    CKI(s_count_char(s, ':'), 0);
    CKI(linebreaks(s), 0); }
  CKI(linebreaks(v_tostr(&A, v_strz(&A, "a\nb\r"), true)), 0);
  ckoneline(v_strz(&A, "a\nb\r"));

  T("fn + plan");
  ckcompact(mkfn("add", 2), "fn(add)");
  ckcompact(mkfn(NULL, 3), "fn/3");
  ckjson(mkfn("add", 2), "null");
  ckjson(mkplan(), "{\"plan\":\"stub\"}");
  ckcompact(v_rec(&A, 1, "p", mkplan()), "p=<plan>");
  ckcompact(v_list(&A, 1, mkplan()), "[<plan>]");           /* delegation keeps brackets */
  ckcompact(v_rec(&A, 1, "f", mkfn("neg", 1)), "f=fn(neg)");
  ckcompact(v_rec(&A, 1, "n", mknat(NULL)), "n=<nat>");
  CKSTR(v_repr(&A, mkfn("add", 2)), "FN(add)");
  CKSTR(v_repr(&A, mkfn(NULL, 2)), "FN/2");
  CKSTR(v_repr(&A, mkplan()), "PLAN");
  CKSTR(v_repr(&A, mknat(NULL)), "NAT");

  T("nested compact: delimiters, not line position");
  ckcompact(v_rec(&A, 1, "a", v_rec(&A, 1, "b", v_rec(&A, 2, "c", v_num(1), "d", v_num(2)))),
             "a={b={c=1,d=2}}");
  ckcompact(v_rec(&A, 2, "a", v_rec(&A, 2, "b", v_num(1), "c", v_num(2)), "z", v_num(9)),
             "a={b=1,c=2},z=9");
  ckcompact(v_rec(&A, 2, "k", rec_to_v(rec_new(&A)), "l", v_list(&A, 0)), "k={},l=[]");
  ckcompact(v_rec(&A, 1, "k", v_list(&A, 3, v_num(1), v_num(2), v_strz(&A, "x"))), "k=[1,2,x]");
  ckcompact(v_rec(&A, 1, "k", v_list(&A, 1, v_rec(&A, 1, "a", v_num(1)))), "k=[{a=1}]");
  ckcompact(v_rec(&A, 1, "k", v_list(&A, 2, v_num(1), rec_to_v(rec_new(&A)))), "k=[1,{}]");
  ckcompact(v_list(&A, 1, v_list(&A, 2, v_num(1), v_num(2))), "[[1,2]]");
  ckcompact(rec_to_v(rec_new(&A)), "{}");
  ckjson(rec_to_v(rec_new(&A)), "{}");
  ckjson(v_rec(&A, 1, "a", v_rec(&A, 2, "b", v_num(1), "c", v_num(2))), "{\"a\":{\"b\":1,\"c\":2}}");
  { V inner = v_list(&A, 2, v_rec(&A, 1, "v", v_num(1)), v_rec(&A, 1, "v", v_num(2)));
    V row = v_rec(&A, 1, "k", inner);
    ckcompact(v_rec(&A, 1, "rows", v_list(&A, 1, row)), "rows=[{k=[{v=1},{v=2}]}]"); }
  { /* the shape that made flattening ambiguous: one record, TWO lists */
    V two = v_rec(&A, 2,
                  "files", v_list(&A, 2, v_rec(&A, 1, "path", v_strz(&A, "a.c")),
                                        v_rec(&A, 1, "path", v_strz(&A, "b.c"))),
                  "deps",  v_list(&A, 2, v_rec(&A, 1, "path", v_strz(&A, "x.h")),
                                        v_rec(&A, 1, "path", v_strz(&A, "y.h"))));
    ckcompact(two, "files=[{path=a.c},{path=b.c}],deps=[{path=x.h},{path=y.h}]");
    ckoneline(two); }
  { /* §2.1 grammar table, one assertion per line of the spec */
    ckcompact(v_rec(&A, 4, "a", v_num(42), "b", v_num(3.5), "c", VT, "d", v_bool(false)),
              "a=42,b=3.5,c=t,d=f");
    ckcompact(v_rec(&A, 1, "k", v_nil()), "k=-");
    ckcompact(v_rec(&A, 2, "k", v_num(1), "k2", v_num(2)), "k=1,k2=2");
    ckcompact(v_rec(&A, 1, "k", v_rec(&A, 2, "a", v_num(1), "b", v_num(2))), "k={a=1,b=2}");
    ckcompact(v_rec(&A, 1, "k", v_list(&A, 3, v_num(1), v_num(2), v_num(3))), "k=[1,2,3]");
    ckcompact(v_rec(&A, 1, "k", v_list(&A, 2, v_rec(&A, 2, "a", v_num(1), "b", v_num(2)),
                                         v_rec(&A, 1, "a", v_num(3)))), "k=[{a=1,b=2},{a=3}]");
    ckcompact(v_rec(&A, 1, "k", rec_to_v(rec_new(&A))), "k={}");
    ckcompact(v_rec(&A, 1, "k", v_list(&A, 0)), "k=[]");
    ckcompact(v_rec(&A, 1, "k", v_list(&A, 2, rec_to_v(rec_new(&A)), v_list(&A, 0))), "k=[{},[]]"); }

  { /* the plan.files shape: exact bytes are load-bearing */
    V r5 = v_rec(&A, 5,
                 "a", v_rec(&A, 1, "b", v_num(1)),
                 "files", v_list(&A, 2,
                     v_rec(&A, 2, "path", v_strz(&A, "x y"), "bytes", v_num(10)),
                     v_rec(&A, 2, "path", v_strz(&A, "z"), "bytes", v_num(2))),
                 "n", v_num(3.0), "ok", VT, "e", v_nil());
    ckcompact(r5, "a={b=1},files=[{path=\"x y\",bytes=10},{path=z,bytes=2}],n=3,ok=t,e=-");
    ckoneline(r5);
    ckjson(r5, "{\"a\":{\"b\":1},\"files\":[{\"path\":\"x y\",\"bytes\":10},{\"path\":\"z\",\"bytes\":2}],"
               "\"n\":3,\"ok\":true,\"e\":null}");

    /* determinism: same insertion order -> byte-identical serialization */
    V again = v_rec(&A, 5,
                    "a", v_rec(&A, 1, "b", v_num(1)),
                    "files", v_list(&A, 2,
                        v_rec(&A, 2, "path", v_strz(&A, "x y"), "bytes", v_num(10)),
                        v_rec(&A, 2, "path", v_strz(&A, "z"), "bytes", v_num(2))),
                    "n", v_num(3.0), "ok", VT, "e", v_nil());
    Str j1 = v_tojson(&A, r5), j2 = v_tojson(&A, again);
    CK(s_eq(j1, j2));
    CK(s_eq(v_tostr(&A, r5, true), v_tostr(&A, again, true)));
    CK(v_eq(r5, again));
    CKI(j1.len, j2.len);
    /* same content, different insertion order -> equal value, different bytes */
    V perm = v_rec(&A, 5,
                   "files", v_list(&A, 2,
                       v_rec(&A, 2, "path", v_strz(&A, "x y"), "bytes", v_num(10)),
                       v_rec(&A, 2, "path", v_strz(&A, "z"), "bytes", v_num(2))),
                   "a", v_rec(&A, 1, "b", v_num(1)),
                   "n", v_num(3.0), "ok", VT, "e", v_nil());
    CK(v_eq(r5, perm));
    CK(!s_eq(v_tojson(&A, perm), j1));
    ckcompact(perm, "files=[{path=\"x y\",bytes=10},{path=z,bytes=2}],a={b=1},n=3,ok=t,e=-");

    /* list of records at top level: brackets kept, one line, no prefix */
    V fl = v_list(&A, 2,
                  v_rec(&A, 2, "path", v_strz(&A, "x y"), "bytes", v_num(10)),
                  v_rec(&A, 2, "path", v_strz(&A, "z"), "bytes", v_num(2)));
    ckcompact(fl, "[{path=\"x y\",bytes=10},{path=z,bytes=2}]");
    ckjson(fl, "[{\"path\":\"x y\",\"bytes\":10},{\"path\":\"z\",\"bytes\":2}]");

    /* plan-shaped record as SPEC §3.1 */
    V p = v_rec(&A, 4, "op", v_strz(&A, "fs.write"), "confirm", v_strz(&A, "required"),
                "token", v_strz(&A, "9f2c1a"),
                "files", v_list(&A, 2,
                    v_rec(&A, 4, "path", v_strz(&A, "src/a.c"), "from", v_strz(&A, "e1b2.."),
                          "to", v_strz(&A, "77aa.."), "kind", v_strz(&A, "modify")),
                    v_rec(&A, 2, "path", v_strz(&A, "src/b.c"), "kind", v_strz(&A, "delete"))));
    ckcompact(p, "op=fs.write,confirm=required,token=9f2c1a,"
                 "files=[{path=src/a.c,from=e1b2..,to=77aa..,kind=modify},"
                 "{path=src/b.c,kind=delete}]");
    ckoneline(p); }

  T("budget: compact < json");
  { /* budget sanity: the compact form must be strictly smaller than JSON,
       so the format change is measurable and not just aesthetic */
    V p = v_rec(&A, 4, "op", v_strz(&A, "fs.write"), "confirm", v_strz(&A, "required"),
                "token", v_strz(&A, "9f2c1a"),
                "files", v_list(&A, 2,
                    v_rec(&A, 4, "path", v_strz(&A, "src/a.c"), "from", v_strz(&A, "e1b2.."),
                          "to", v_strz(&A, "77aa.."), "kind", v_strz(&A, "modify")),
                    v_rec(&A, 2, "path", v_strz(&A, "src/b.c"), "kind", v_strz(&A, "delete"))));
    Str c = v_tostr(&A, p, true), j = v_tojson(&A, p);
    CKI(c.len, 125); CKI(j.len, 165);
    CK(c.len < j.len);                                     /* strictly smaller */
    CK(c.len * 5 < j.len * 4);                             /* >= 20% saved here */
    CKI(v_tok_est(p), tok_est(c.p, (size_t)c.len));
    CK(v_tok_est(p) < tok_est(j.p, (size_t)j.len));
    /* bool/null-heavy shape: SPEC §2.1 claims ~35% vs JSON, t/f and - carry it */
    V q = v_rec(&A, 5, "a", v_rec(&A, 1, "b", v_num(1)),
                "files", v_list(&A, 2, v_rec(&A, 2, "path", v_strz(&A, "x y"), "bytes", v_num(10)),
                                      v_rec(&A, 2, "path", v_strz(&A, "z"), "bytes", v_num(2))),
                "n", v_num(3.0), "ok", VT, "e", v_nil());
    Str c2 = v_tostr(&A, q, true), j2 = v_tojson(&A, q);
    CKI(c2.len, 67); CKI(j2.len, 97);
    CK(c2.len < j2.len);
    CK(c2.len * 4 < j2.len * 3);                           /* >= 25% saved */
    CKI(v_tok_est(q), tok_est(c2.p, (size_t)c2.len));
    CK(v_tok_est(q) < tok_est(j2.p, (size_t)j2.len)); }

  T("single-line invariant");
  { /* single-line invariant over deeply mixed values */
    V deep = v_rec(&A, 3,
        "rows", v_list(&A, 3,
            v_rec(&A, 3, "path", v_strz(&A, "a b\nc"), "tags", v_list(&A, 2, v_strz(&A, "x,y"), v_strz(&A, "{z}")),
                  "meta", v_rec(&A, 2, "k", v_list(&A, 1, v_rec(&A, 1, "n", v_num(1))), "h", v_strz(&A, "a\r\nb"))),
            v_rec(&A, 2, "path", v_strz(&A, "d.c"), "tags", v_list(&A, 0),
                  "meta", v_rec(&A, 1, "e", rec_to_v(rec_new(&A)))),
            v_list(&A, 2, rec_to_v(rec_new(&A)), v_list(&A, 1, v_list(&A, 0)))),
        "err", mkerr(E_NEED_CONFIRM, "line1\nline2", "use --confirm\t9f2c1a",
                     v_rec(&A, 3, "op", v_strz(&A, "fs.write"), "token", v_strz(&A, "9f2c1a"),
                           "files", v_list(&A, 2, v_rec(&A, 1, "path", v_strz(&A, "p q")),
                                                 v_rec(&A, 1, "path", v_strz(&A, ""))))),
        "plain", v_strz(&A, "tab\there"));
    ckoneline(deep);
    ckoneline(v_rec(&A, 1, "k", v_strz(&A, "\n")));
    ckoneline(v_rec(&A, 1, "k", v_strz(&A, "\r\n")));
    ckoneline(v_list(&A, 2, v_strz(&A, "a\nb"), mkerr(E_IO, "x\ny", "z\n", v_list(&A, 1, v_strz(&A, "\n")))));
    ckoneline(mkerr(E_IO, "m\nm", "h\nh", v_rec(&A, 1, "d", v_list(&A, 1, v_strz(&A, "q\nq")))));
    ckoneline(v_rec(&A, 1, "k", v_list(&A, 2, rec_to_v(rec_new(&A)), v_list(&A, 0))));
    ckoneline(v_rec(&A, 1, "p", mkplan()));
    { Rec *wk = rec_new(&A); rec_setz(&A, wk, "bad\nkey", v_num(1)); rec_setz(&A, wk, "bad\rkey", v_num(2));
      ckoneline(rec_to_v(wk));                              /* keys escape too */
      ckcompact(rec_to_v(wk), "\"bad\\nkey\"=1,\"bad\\rkey\"=2"); }
    CKI(linebreaks(v_tostr(&A, deep, true)), 0);
    CK(linebreaks(v_tojson(&A, deep)) == 0);                /* JSON escapes as well */ }

  T("distinguishability");
  { /* distinguishability: structurally different values must not collide */
    V x = v_rec(&A, 2, "a", v_rec(&A, 1, "b", v_num(1)), "c", v_num(2));
    V y = v_rec(&A, 1, "a", v_rec(&A, 2, "b", v_num(1), "c", v_num(2)));
    ckcompact(x, "a={b=1},c=2");
    ckcompact(y, "a={b=1,c=2}");
    CK(!s_eq(v_tostr(&A, x, true), v_tostr(&A, y, true)));
    CK(!v_eq(x, y));
    CK(!s_eq(v_tojson(&A, x), v_tojson(&A, y)));           /* and JSON still differs */
    V l1 = v_list(&A, 2, v_rec(&A, 1, "a", v_num(1)), v_rec(&A, 1, "b", v_num(2)));
    V l2 = v_list(&A, 1, v_rec(&A, 2, "a", v_num(1), "b", v_num(2)));
    ckcompact(l1, "[{a=1},{b=2}]");
    ckcompact(l2, "[{a=1,b=2}]");
    CK(!s_eq(v_tostr(&A, l1, true), v_tostr(&A, l2, true)));
    CK(!v_eq(l1, l2));
    /* the old flattened form collapsed exactly these two to the same bytes */
    V f1 = v_rec(&A, 1, "k", l1), f2 = v_rec(&A, 1, "k", l2);
    CK(!s_eq(v_tostr(&A, f1, true), v_tostr(&A, f2, true)));
    ckcompact(f1, "k=[{a=1},{b=2}]");
    ckcompact(f2, "k=[{a=1,b=2}]");
    CK(v_eq(v_rec(&A, 2, "a", v_num(1), "b", v_num(2)), v_rec(&A, 2, "b", v_num(2), "a", v_num(1)))); }

  T("determinism");
  { /* determinism: rebuild the same value twice -> byte-identical, order kept */
    Str s1 = v_tostr(&A, mk_ordered(), true), s2 = v_tostr(&A, mk_ordered(), true);
    CK(s_eq(s1, s2));
    CKI(s1.len, s2.len);
    CK(!memcmp(s1.p, s2.p, (size_t)s1.len));
    ckcompact(mk_ordered(), "b=[1,\"s p\"],a={z=0,y=f},c=2.5");   /* insertion order, not key order */
    CKI(s_findz(s1, "b=", 0), 0);
    CK(s_findz(s1, "a=", 0) < s_findz(s1, "c=", 0));
    Str j1 = v_tojson(&A, mk_ordered()), j2 = v_tojson(&A, mk_ordered());
    CK(s_eq(j1, j2)); }

  { /* token estimate is the compact length */
    V r = v_rec(&A, 6, "path", v_strz(&A, "src/main.c"), "hash", v_strz(&A, "7c19f3"),
                "lines", v_strz(&A, "41-71"), "total", v_num(312), "tok", v_num(214),
                "text", v_strz(&A, "int main(void) {\n  return 0;\n}"));
    ckcompact(r, "path=src/main.c,hash=7c19f3,lines=41-71,total=312,tok=214,"
                 "text=\"int main(void) {\\n  return 0;\\n}\"");
    ckoneline(r);                                          /* multi-line text stays one line */
    Str s = v_tostr(&A, r, true);
    CKI(v_tok_est(r), tok_est(s.p, (size_t)s.len));
    CK(v_tok_est(r) < s.len);
    CKI(v_tok_est(v_num(3)), 1);
    CKI(v_tok_est(v_rec(&A, 1, "abcdef", v_num(1))), 2);
    CKI(v_tok_est(v_rec(&A, 1, "abcdefgh", v_num(1))), 3); }

  T("repr");
  CKSTR(v_repr(&A, v_nil()), "NIL");
  CKSTR(v_repr(&A, VT), "BOOL(t)");
  CKSTR(v_repr(&A, VF), "BOOL(f)");
  CKSTR(v_repr(&A, v_num(3.0)), "NUM(3)");
  CKSTR(v_repr(&A, v_strz(&A, "a b")), "STR(\"a b\")");
  CKSTR(v_repr(&A, v_list(&A, 3, v_num(1), v_num(2), v_num(3))), "LIST[3]");
  CKSTR(v_repr(&A, v_list(&A, 0)), "LIST[0]");
  CKSTR(v_repr(&A, v_rec(&A, 2, "a", v_num(1), "b", v_num(2))), "REC[2]");
  CKSTR(v_repr(&A, v_err(&A, E_NEED_CONFIRM, "x")), "ERR(NEED_CONFIRM)");

  T("depth guard");
  { V d = v_num(1);
    for (int i = 0; i < 40; i++) { List *ll = list_new(&A); list_push(&A, ll, d); d.t = V_LIST; d.u.l = ll; }
    Str s = v_tostr(&A, d, true);
    CKI(s.len, 33 + 3 + 33);                              /* [[[.. ... ..]]] */
    CK(s_findz(s, "...", 0) >= 0);
    CKI(s_count_char(s, '['), 33); CKI(s_count_char(s, ']'), 33);
    CKI(linebreaks(s), 0);
    Str j = v_tojson(&A, d);
    CKI(j.len, 33 + 4 + 33);
    CK(s_findz(j, "null", 0) >= 0); }
  { V d = v_num(1);
    for (int i = 0; i < 40; i++) { Rec *rr = rec_new(&A); rec_setz(&A, rr, "k", d); d = rec_to_v(rr); }
    Str s = v_tostr(&A, d, true);                          /* braces kept: k={k={.. ... }} */
    CKI(s_count_char(s, '{'), 32);
    CKI(s_count_char(s, '}'), 32);
    CKI(s_count_char(s, '='), 33);                         /* 33 keys, then the collapse */
    CK(s_findz(s, "...", 0) >= 0);
    CK(s_ni(s, "k={k={"));
    CKI(s.len, 33 * 2 + 32 + 3 + 32);
    CKI(linebreaks(s), 0);
    ckcompact(d, s.p);                                     /* same bytes on a second render */
    Str j = v_tojson(&A, d);
    CKI(j.len, 33 * 5 + 4 + 33);
    CK(s_ni(j, "{\"k\":{\"k\":")); }

  T("v_disp plumbing");
  { Buf b; buf_init(&b, &A);
    buf_puts(&b, "pre:");
    v_disp(&b, v_num(3), 0, true);
    CKSTR(buf_str(&b), "pre:3");                           /* appends, never clears */
    buf_puts(&b, "|");
    v_disp(&b, v_rec(&A, 2, "a", v_num(1), "b", v_list(&A, 1, v_num(2))), 0, true);
    CKSTR(buf_str(&b), "pre:3|a=1,b=[2]");                 /* top level: no outer braces */
    buf_clear(&b);
    v_disp(&b, v_rec(&A, 1, "a", v_num(1)), 0, false);
    CKSTR(buf_str(&b), "{\"a\":1}");
    v_disp(NULL, v_num(1), 0, true);
    buf_clear(&b);
    v_disp(&b, v_nil(), 33, true);
    CKSTR(buf_str(&b), "...");
    buf_clear(&b);
    v_disp(&b, v_rec(&A, 1, "k", v_num(1)), 32, true);     /* brace budget matches JSON */
    CKSTR(buf_str(&b), "k=...");
    buf_clear(&b);
    v_disp(&b, v_list(&A, 1, v_num(1)), 31, true);
    CKSTR(buf_str(&b), "[1]");
    buf_clear(&b);
    v_disp(&b, rec_to_v(rec_new(&A)), 0, true);
    CKSTR(buf_str(&b), "{}"); }
  { V v = v_rec(&A, 1, "a", v_num(1));
    CK(s_eq(v_tostr(&A, v, false), v_tojson(&A, v)));
    CKI(v_tostr(&A, v_nil(), true).len, 1); }

  T_REPORT("val");
}
