/* test_xdiff.c - SPEC 4.2 anchors/patch/diff.
 * The module's reason to exist is tested first and explicitly: an anchor over
 * neighbouring context must survive an upstream insert, and every patch op must
 * come back verified against the NEW text.
 *
 * The entry points are Arena-first, exactly as vxa.h's xdiff block declares them.
 */
#include "vxa.h"
#include "t.h"
#include <stdlib.h>
#include <stdarg.h>

static Arena AR;

/* ---------- helpers ---------- */
static Str joinv(const char **v, int n, const char *eol, bool trail) {
  Buf b;
  buf_init(&b, &AR);
  for (int i = 0; i < n; i++) { if (i) buf_puts(&b, eol); buf_puts(&b, v[i]); }
  if (trail && n > 0) buf_puts(&b, eol);
  return buf_take(&b);
}
#define TXT(a, eol, trail) joinv((a), (int)(sizeof(a) / sizeof(*(a))), (eol), (trail))
#define TJ(a) TXT(a, "\n", false)
#define TJN(a) TXT(a, "\n", true)

static V fld(V r, const char *k) {
  V *p = (r.t == V_REC && r.u.r) ? rec_getz(r.u.r, k) : NULL;
  return p ? *p : VN;
}
static int ifld(V r, const char *k) { V x = fld(r, k); return x.t == V_NUM ? (int)x.u.n : -999; }
static Str sfld(V r, const char *k) { V x = fld(r, k); return x.t == V_STR ? x.u.s : s_null(); }
static bool bfld(V r, const char *k) { V x = fld(r, k); return x.t == V_BOOL ? x.u.b : false; }
static bool hasfld(V r, const char *k) { return fld(r, k).t != V_NULL; }
static V opat(V lst, int i) { V *p = (lst.t == V_LIST && lst.u.l) ? list_get(lst.u.l, i) : NULL; return p ? *p : VN; }
static Str emsg(V v) { return (v.t == V_ERR && v.u.e) ? v.u.e->msg : s_null(); }
static bool inStr(Str h, const char *n) { return h.p != NULL && s_findz(h, n, 0) >= 0; }
static int cntstr(Str h, const char *n) { int c = 0, at = 0; while ((at = s_findz(h, n, at)) >= 0) { c++; at += (int)strlen(n); } return c; }
static bool is_hex(Str s) {
  if (s.len <= 0) return false;
  for (int i = 0; i < s.len; i++) {
    char c = s.p[i];
    if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
  }
  return true;
}
static V op(const char *at, const char *kind, const char *text, int n) {
  Rec *r = rec_new(&AR);
  rec_setz(&AR, r, "at", v_str(s_lit(&AR, at)));
  rec_setz(&AR, r, "op", v_str(s_lit(&AR, kind)));
  if (text) rec_setz(&AR, r, "text", v_str(s_lit(&AR, text)));
  if (n > 0) rec_setz(&AR, r, "n", v_num(n));
  return rec_to_v(r);
}
static V opl(int n, ...) {
  List *l = list_new(&AR);
  V v;
  va_list ap;
  va_start(ap, n);
  for (int i = 0; i < n; i++) list_push(&AR, l, va_arg(ap, V));
  va_end(ap);
  v.t = V_LIST; v.u.l = l;
  return v;
}
static Anch *anc(Str text, int *n) {
  Anch *A = anchors_of(&AR, "t.c", text, n, NULL);
  return A;
}
static const char *atc(Str text, int i) {
  int n = 0;
  Anch *A = anc(text, &n);
  return (i >= 0 && i < n) ? A[i].at.p : "";
}
static char LB[24][8];
static const char *V20[20];
static const char *V5[5] = { "alpha", "beta", "gamma", "delta", "epsilon" };
static const char *V12[12] = { "L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8", "L9", "L10", "L11", "L12" };
static void mk20(void) {
  for (int i = 0; i < 20; i++) { snprintf(LB[i], sizeof LB[i], "l%02d", i + 1); V20[i] = LB[i]; }
}

int main(void) {
  arena_init(&AR, 0);
  mk20();

  /* ================= anchor_at: no line number in the hash ================= */
  T("anchor_at excludes path and index");
  {
    Str t = TJN(V20);
    int n = -1;
    ErrCode e = E_BAD_INPUT;
    Anch *A = anchors_of(&AR, "t.c", t, &n, &e);
    Str p = A[4].text, c = A[5].text, x = A[6].text;
    Str one = anchor_at(&AR, "some/dir/a.c", 5, p, c, x);
    Str two = anchor_at(&AR, "totally/other.py", 9991, p, c, x);
    CK(A != NULL);
    CKI(n, 20);
    CKI(e, E_NONE);
    CKI(one.len, 8);
    CK(is_hex(one));
    CK(s_eq(one, two));                                  /* path/idx not hashed */
    CK(!s_eq(one, anchor_at(&AR, "a.c", 5, s_null(), c, x)));   /* prev matters */
    CK(!s_eq(one, anchor_at(&AR, "a.c", 5, p, s_lit(&AR, "zzz"), x)));
    CK(!s_eq(one, anchor_at(&AR, "a.c", 5, p, c, s_null())));   /* next matters */
    CK(s_eq(A[5].at, anchor_at(&AR, "t.c", 5, A[4].text, A[5].text, A[6].text)));
    CK(s_eq(A[0].at, anchor_at(&AR, "t.c", 0, s_null(), A[0].text, A[1].text)));  /* first: prev empty */
    CK(s_eq(A[19].at, anchor_at(&AR, "t.c", 19, A[18].text, A[19].text, s_null()))); /* last: next empty */
    CKI(A[0].n, 0);
    CKI(A[19].n, 19);
    CKSTR(A[0].text, "l01");
    CKSTR(A[19].text, "l20");
    CK(s_eq(anchors_of(&AR, "t.c", t, &n, NULL)[7].at, A[7].at));  /* deterministic */
    CK(s_eq(anchor_for(&AR, "t.c", 5, s_null(), t), A[5].at));         /* vxa.h helper */
    CKI(anchor_for(&AR, "t.c", 99, s_null(), t).len, 0);
  }

  /* ================= line splitting ================= */
  T("split rules");
  {
    int n = -1;
    Anch *A;
    ErrCode e = E_BAD_INPUT;
    A = anchors_of(&AR, "x", s_lit(&AR, ""), &n, &e);
    CK(A == NULL);
    CKI(n, 0);
    CKI(e, E_NONE);
    A = anchors_of(&AR, "x", s_lit(&AR, "a"), &n, NULL);
    CKI(n, 1);
    CKSTR(A[0].text, "a");
    CKI(anchors_of(&AR, "x", s_lit(&AR, "a\n"), &n, NULL) != NULL, n);
    CKI(n, 1);
    (void)anchors_of(&AR, "x", s_lit(&AR, "a\n\n"), &n, NULL);
    CKI(n, 2);
    A = anchors_of(&AR, "x", s_lit(&AR, "\n"), &n, NULL);
    CKI(n, 1);
    CKI(A[0].text.len, 0);
    A = anchors_of(&AR, "x", s_lit(&AR, "a\r\nb"), &n, NULL);
    CKI(n, 2);
    CKSTR(A[0].text, "a");
    CKSTR(A[1].text, "b");
    (void)anchors_of(&AR, "x", s_lit(&AR, "a\rb"), &n, NULL);
    CKI(n, 2);
    (void)anchors_of(&AR, "x", s_lit(&AR, "a\r\r\nb"), &n, NULL);
    CKI(n, 3);
    A = anchors_of(&AR, "x", s_lit(&AR, "\xe4\xbd\xa0\nok"), &n, NULL);
    CKI(n, 2);
    CKI(A[0].text.len, 3);      /* 3 UTF-8 bytes, 1 code point */
    CKI(A[1].text.len, 2);
    A = anchors_of(&AR, "x", s_lit(&AR, "\n\n\n"), &n, NULL);
    CKI(n, 3);
    CK(s_eq(A[0].at, A[1].at));      /* identical 3-line contexts hash alike */
    CK(s_eq(A[1].at, A[2].at));
  }

  /* ================= THE point of the module ================= */
  T("anchor stability across upstream insert");
  {
    Str t = TJN(V20);
    int n0 = 0, n1 = 0;
    Anch *A = anchors_of(&AR, "t.c", t, &n0, NULL);
    const char *want = A[14].at.p;         /* anchor of line 15 */
    static const char *ins3[3] = { "new1", "new2", "new3" };
    Buf b;
    Str t2;
    Anch *B;
    V f;
    buf_init(&b, &AR);
    for (int i = 0; i < 3; i++) { buf_puts(&b, ins3[i]); buf_putc(&b, '\n'); }
    buf_put(&b, t.p, (size_t)t.len);
    t2 = buf_take(&b);
    B = anchors_of(&AR, "t.c", t2, &n1, NULL);
    CKI(n0, 20);
    CKI(n1, 23);
    CK(s_eq(B[17].at, s_wrap(want)));       /* line 15 kept its anchor */
    CKSTR(B[17].text, "l15");
    CK(s_eq(B[17].text, A[14].text));
    CK(!s_eq(A[0].at, B[3].at));            /* line 1: its prev went from "" to new3 */
    CK(s_eq(A[1].at, B[4].at));             /* its own neighbours are untouched */
    CK(s_eq(A[19].at, B[22].at));
    {
      bool all = true;
      for (int i = 1; i < 20; i++) if (!s_eq(A[i].at, B[i + 3].at)) all = false;
      CK(all);                                              /* lines 2..20 all stable */
    }
    f = tx_find_anchor(&AR, B, n1, A[14].at, 0);
    CK(f.t == V_REC);
    CKI(ifld(f, "idx"), 17);
    CKI(ifld(f, "count"), 1);
    CKI(ifld(f, "n"), 0);
  }
  T("anchor changes when the line or a neighbour changes");
  {
    Str t = TJN(V20);
    int n = 0;
    Anch *A = anchors_of(&AR, "t.c", t, &n, NULL);
    const char *repl[20];
    Str t2;
    Anch *B;
    for (int i = 0; i < 20; i++) repl[i] = LB[i];
    memcpy(LB[14], "L15!", 5);                     /* change line 15 in place */
    t2 = TJN(repl);
    B = anchors_of(&AR, "t.c", t2, &n, NULL);
    CK(n == 20);
    CK(!s_eq(A[14].at, B[14].at));
    CK(!s_eq(A[13].at, B[13].at));        /* prev neighbour changed -> new anchor */
    CK(!s_eq(A[15].at, B[15].at));        /* next neighbour changed -> new anchor */
    CK(s_eq(A[12].at, B[12].at));
    CK(s_eq(A[16].at, B[16].at));
    CK(s_eq(A[0].at, B[0].at));
    memcpy(LB[14], "l15", 4);                      /* restore */
  }

  /* ================= duplicate contexts ================= */
  T("duplicate 3-line context");
  {
    const char *bl[7] = { "x", "", "", "", "", "", "y" };
    Str t = TJ(bl);
    int n = 0;
    Anch *A = anchors_of(&AR, "t.c", t, &n, NULL);
    Str at;
    V f;
    CKI(n, 7);
    CK(s_eq(A[2].at, A[3].at) && s_eq(A[3].at, A[4].at));
    CK(!s_eq(A[1].at, A[2].at));
    CK(!s_eq(A[4].at, A[5].at));
    at = A[3].at;
    f = tx_find_anchor(&AR, A, n, at, 0);
    CK(f.t == V_ERR);
    CKI(v_errcode(f), E_ANCHOR_DUP);
    CK(inStr(emsg(f), "matches 3 lines"));
    CK(inStr(emsg(f), "L3"));
    CK(inStr(emsg(f), "L4"));
    CK(inStr(emsg(f), "L5"));
    CK(inStr(emsg(f), "n:1..3"));
    f = tx_find_anchor(&AR, A, n, at, 2);
    CK(f.t == V_REC);
    CKI(ifld(f, "idx"), 3);
    CKI(ifld(f, "count"), 3);
    CKI(ifld(f, "n"), 2);
    f = tx_find_anchor(&AR, A, n, at, 4);
    CK(v_errcode(f) == E_ANCHOR_MISS);
    CK(inStr(emsg(f), "occurrence 4"));
    { /* prefix form of an anchor is accepted */
      V g = tx_find_anchor(&AR, A, n, s_slice(at, 0, 4), 0);
      CK(g.t == V_ERR && v_errcode(g) == E_ANCHOR_DUP);
      g = tx_find_anchor(&AR, A, n, s_slice(A[6].at, 0, 5), 0);
      CK(g.t == V_REC && ifld(g, "idx") == 6);
      g = tx_find_anchor(&AR, A, n, s_lit(&AR, ""), 0);
      CK(v_errcode(g) == E_BAD_INPUT);
      g = tx_find_anchor(&AR, A, n, s_lit(&AR, "deadbeef00"), 0);
      CK(v_errcode(g) == E_BAD_INPUT);
    }
  }
  T("missing anchor lists candidates");
  {
    Str t = TJN(V20);
    int n = 0;
    Anch *A = anchors_of(&AR, "t.c", t, &n, NULL);
    V f = tx_find_anchor(&AR, A, n, s_lit(&AR, "00112233"), 0);
    CK(f.t == V_ERR);
    CKI(v_errcode(f), E_ANCHOR_MISS);
    CK(inStr(emsg(f), "nearest"));
    CK(inStr(emsg(f), "L1: l01"));
    CK(inStr(emsg(f), "L2: l02"));
    CK(inStr(emsg(f), "L3: l03"));
    CK(cntstr(emsg(f), "L") == 3);                  /* at most 3 candidates */
    f = tx_find_anchor(&AR, NULL, 0, s_lit(&AR, "00112233"), 0);
    CK(v_errcode(f) == E_ANCHOR_MISS);
    CK(inStr(emsg(f), "no lines"));
  }

  /* ================= patch: the four ops ================= */
  T("patch set/del/ins");
  {
    Str t = TJN(V20);
    V r, c0, c1;
    r = tx_patch_apply(&AR, t, "t.c", opl(1, op(atc(t, 4), "set", "REPLACED", 0)));
    CK(r.t == V_REC);
    CKI(ifld(r, "applied"), 1);
    CKSTR(sfld(r, "eol"), "lf");
    CK(s_eq(sfld(r, "text"), s_lit(&AR, "l01\nl02\nl03\nl04\nREPLACED\nl06\nl07\nl08\nl09\nl10\nl11\nl12\nl13\nl14\nl15\nl16\nl17\nl18\nl19\nl20\n")));
    c0 = opat(fld(r, "checks"), 0);
    CK(c0.t == V_REC);
    CK(bfld(c0, "ok"));
    CKI(ifld(c0, "line"), 5);
    CKSTR(sfld(c0, "preview"), "L5: REPLACED");
    CK(inStr(sfld(c0, "at"), "l01") == false);       /* at is the hash, not text */
    CKI(fld(r, "checks").u.l->len, 1);

    r = tx_patch_apply(&AR, t, "t.c", opl(1, op(atc(t, 2), "del", NULL, 0)));
    CK(inStr(sfld(r, "text"), "l01\nl02\nl04\nl05\nl06\nl07\n"));   /* the line is gone */
    CK(!inStr(sfld(r, "text"), "\nl03\n"));
    CK(cntstr(sfld(r, "text"), "\n") == 19);
    c0 = opat(fld(r, "checks"), 0);
    CK(bfld(c0, "ok"));
    CKI(ifld(c0, "line"), 3);
    CKSTR(sfld(c0, "preview"), "L3: l04");           /* what took its place */

    r = tx_patch_apply(&AR, t, "t.c", opl(1, op(atc(t, 0), "ins_before", "TOP", 0)));
    CK(s_ni(sfld(r, "text"), "TOP\nl01\n"));
    c0 = opat(fld(r, "checks"), 0);
    CK(bfld(c0, "ok"));
    CKI(ifld(c0, "line"), 1);
    CKSTR(sfld(c0, "preview"), "L1: TOP");
    CK(s_eq(sfld(c0, "at"), s_wrap(atc(t, 0))));       /* the op's at is echoed back */
    CKSTR(sfld(r, "path"), "t.c");

    r = tx_patch_apply(&AR, t, "t.c", opl(1, op(atc(t, 19), "ins_after", "BOTTOM", 0)));
    CK(inStr(sfld(r, "text"), "l19\nl20\nBOTTOM\n"));               /* tail check */
    CK(cntstr(sfld(r, "text"), "\n") == 21);
    c0 = opat(fld(r, "checks"), 0);
    CKI(ifld(c0, "line"), 21);
    CK(bfld(c0, "ok"));

    /* multi-line insert keeps its own line structure */
    r = tx_patch_apply(&AR, t, "t.c", opl(1, op(atc(t, 1), "ins_after", "m1\nm2\nm3", 0)));
    c0 = opat(fld(r, "checks"), 0);
    CKI(ifld(r, "applied"), 1);
    CKI(ifld(c0, "line"), 3);
    CK(inStr(sfld(c0, "preview"), "L3: m1"));
    CK(inStr(sfld(r, "text"), "l01\nl02\nm1\nm2\nm3\nl03\n"));
    CK(cntstr(sfld(r, "text"), "\n") == 23);

    /* multi-line set */
    r = tx_patch_apply(&AR, t, "t.c", opl(1, op(atc(t, 0), "set", "s1\ns2", 0)));
    c0 = opat(fld(r, "checks"), 0);
    CK(bfld(c0, "ok"));
    CK(inStr(sfld(r, "text"), "s1\ns2\nl02\n"));

    /* checks report NEW line numbers after upstream insertions */
    r = tx_patch_apply(&AR, t, "t.c", opl(2,
       op(atc(t, 0), "ins_before", "TOP", 0),
       op(atc(t, 4), "set", "NEW5", 0)));
    CKI(ifld(r, "applied"), 2);
    c0 = opat(fld(r, "checks"), 0);
    c1 = opat(fld(r, "checks"), 1);
    CKI(ifld(c0, "line"), 1);
    CKI(ifld(c1, "line"), 6);                        /* was 5, shifted by the insert */
    CKSTR(sfld(c1, "preview"), "L6: NEW5");
    CK(bfld(c1, "ok"));
    CK(inStr(sfld(r, "text"), "TOP\nl01\nl02\nl03\nl04\nNEW5\nl06\n"));

    /* two insertions on one line do not conflict; order is the ops order */
    r = tx_patch_apply(&AR, t, "t.c", opl(2,
       op(atc(t, 0), "ins_after", "X", 0), op(atc(t, 0), "ins_after", "Y", 0)));
    CK(r.t == V_REC);
    CK(inStr(sfld(r, "text"), "l01\nX\nY\nl02\n"));
    /* insertion attached to a deleted line */
    r = tx_patch_apply(&AR, t, "t.c", opl(2,
       op(atc(t, 1), "del", NULL, 0), op(atc(t, 1), "ins_after", "KEPT", 0)));
    CK(r.t == V_REC);
    CK(inStr(sfld(r, "text"), "l01\nKEPT\nl03\n"));
  }
  T("checks.ok goes false when the edit cannot be proven");
  { /* deleting one of two identical adjacent lines: the content is still there,
     * so the self-verification reports ok=false but still returns a result */
    const char *dd[2] = { "dup", "dup" };
    Str t = TJN(dd);
    V r = tx_patch_apply(&AR, t, "d.c", opl(1, op(atc(t, 0), "del", NULL, 0)));
    V c;
    CK(r.t == V_REC);
    CKI(ifld(r, "applied"), 1);
    CK(s_eq(sfld(r, "text"), s_lit(&AR, "dup\n")));
    c = opat(fld(r, "checks"), 0);
    CK(!bfld(c, "ok"));
    CKSTR(sfld(c, "preview"), "L1: dup");
    CKI(ifld(c, "line"), 1);
    { /* a long line is previewed, not dumped */
      static char longbuf[200];
      const char *lv[1] = { longbuf };
      Str lt;
      V q;
      memset(longbuf, 'z', sizeof longbuf - 1);
      longbuf[sizeof longbuf - 1] = 0;
      lt = TJ(lv);
      q = tx_patch_apply(&AR, lt, "l", opl(1, op(atc(lt, 0), "set", "short", 0)));
      CK(bfld(opat(fld(q, "checks"), 0), "ok"));
      CKSTR(sfld(opat(fld(q, "checks"), 0), "preview"), "L1: short");
      q = tx_patch_apply(&AR, lt, "l", opl(1, op(atc(lt, 0), "del", NULL, 0)));
      CKSTR(sfld(opat(fld(q, "checks"), 0), "preview"), "L1: <none>");
    }
  }
  T("patch failures are all-or-nothing");
  {
    Str t = TJN(V20);
    int n = 0;
    Anch *A = anchors_of(&AR, "t.c", t, &n, NULL);
    V r;
    r = tx_patch_apply(&AR, t, "t.c", opl(3,
       op(A[0].at.p, "set", "one", 0), op(A[1].at.p, "del", NULL, 0),
       op("ffffffff", "set", "nope", 0)));
    CK(r.t == V_ERR);
    CKI(v_errcode(r), E_ANCHOR_MISS);
    CK(inStr(emsg(r), "ffffffff"));
    r = tx_patch_apply(&AR, t, "t.c", opl(2,
       op(A[3].at.p, "set", "a", 0), op(A[3].at.p, "del", NULL, 0)));
    CK(v_errcode(r) == E_BAD_INPUT);
    CK(inStr(emsg(r), "op #1"));
    CK(inStr(emsg(r), "op #2"));
    CK(inStr(emsg(r), "line 4"));
    r = tx_patch_apply(&AR, t, "t.c", opl(1, op(A[5].at.p, "replace", "x", 0)));
    CK(v_errcode(r) == E_BAD_INPUT);
    CK(inStr(emsg(r), "set|del|ins_before|ins_after"));
    r = tx_patch_apply(&AR, t, "t.c", opl(1, op(A[5].at.p, "set", NULL, 0)));
    CK(v_errcode(r) == E_BAD_INPUT);
    CK(inStr(emsg(r), "requires a text field"));
    r = tx_patch_apply(&AR, t, "t.c", opl(1, v_num(4)));
    CK(v_errcode(r) == E_BAD_INPUT);
    CK(inStr(emsg(r), "op #1"));
    r = tx_patch_apply(&AR, t, "t.c", v_str(s_lit(&AR, "nope")));
    CK(v_errcode(r) == E_BAD_INPUT);
    { Rec *x = rec_new(&AR); rec_setz(&AR, x, "op", v_strz(&AR, "del"));
      r = tx_patch_apply(&AR, t, "t.c", opl(1, rec_to_v(x)));
      CK(v_errcode(r) == E_BAD_INPUT);
      CK(inStr(emsg(r), "missing at")); }
    /* duplicate anchor inside a batch */
    r = tx_patch_apply(&AR, t, "t.c", opl(1, op(A[3].at.p, "set", "d", 0)));
    CK(r.t == V_REC);
    CKSTR(sfld(opat(fld(r, "checks"), 0), "preview"), "L4: d");
    r = tx_patch_apply(&AR, t, "t.c", opl(1, op(A[3].at.p, "set", "d", 2)));
    CK(v_errcode(r) == E_ANCHOR_MISS);       /* occurrence 2 of a unique anchor */
    CK(inStr(emsg(r), "occurrence 2"));
  }

  /* ================= line endings ================= */
  T("eol detection and preservation");
  {
    const char *cv[4] = { "one", "two", "three", "four" };
    Str t = TXT(cv, "\r\n", true);
    V r = tx_patch_apply(&AR, t, "w.c", opl(2,
        op(atc(t, 1), "set", "TWO", 0), op(atc(t, 2), "ins_after", "NEAR", 0)));
    CK(r.t == V_REC);
    CKSTR(sfld(r, "eol"), "crlf");
    CK(s_eq(sfld(r, "text"), s_lit(&AR, "one\r\nTWO\r\nthree\r\nNEAR\r\nfour\r\n")));
    CK(cntstr(sfld(r, "text"), "\r\n") == 5);
    CK(!inStr(sfld(r, "text"), "one\n"));
    { /* mixed file: strict majority wins, untouched lines keep their own break */
      Str mx = s_lit(&AR, "a\r\nb\nc\r\nd\r\n");
      V q = tx_patch_apply(&AR, mx, "m", opl(1, op(atc(mx, 0), "ins_after", "NEW", 0)));
      CKSTR(sfld(q, "eol"), "crlf");
      CK(s_eq(sfld(q, "text"), s_lit(&AR, "a\r\nNEW\r\nb\nc\r\nd\r\n")));
      q = tx_patch_apply(&AR, s_lit(&AR, "a\r\nb\nc\n"), "m", opl(1, op(atc(s_lit(&AR, "a\r\nb\nc\n"), 0), "set", "A", 0)));
      CKSTR(sfld(q, "eol"), "lf");            /* 2 of 3 breaks are bare lf */
      CK(s_eq(sfld(q, "text"), s_lit(&AR, "A\nb\nc\n")));  /* new line takes lf */
    }
    { /* no trailing newline in, none out */
      Str nn = s_lit(&AR, "a\nb\nc");
      V q = tx_patch_apply(&AR, nn, "n", opl(1, op(atc(nn, 2), "set", "C", 0)));
      CK(s_eq(sfld(q, "text"), s_lit(&AR, "a\nb\nC")));
      CKSTR(sfld(q, "eol"), "lf");
      q = tx_patch_apply(&AR, nn, "n", opl(1, op(atc(nn, 2), "ins_after", "D", 0)));
      CK(s_eq(sfld(q, "text"), s_lit(&AR, "a\nb\nc\nD")));   /* appended, still unterminated */
      CK(!inStr(sfld(q, "text"), "D\n"));
      CKI(ifld(opat(fld(q, "checks"), 0), "line"), 4);
      CK(bfld(opat(fld(q, "checks"), 0), "ok"));
    }
    { /* deleting every line empties the file */
      Str only = s_lit(&AR, "a\n");
      V q = tx_patch_apply(&AR, only, "o", opl(1, op(atc(only, 0), "del", NULL, 0)));
      CKSTR(sfld(q, "text"), "");
      CKI(ifld(q, "applied"), 1);
      CKSTR(sfld(opat(fld(q, "checks"), 0), "preview"), "L1: <none>");
      CK(bfld(opat(fld(q, "checks"), 0), "ok"));
    }
    { /* empty input: no anchors, so only the empty-text position is resolvable */
      Str empty = s_lit(&AR, "");
      Str nat = anchor_at(&AR, NULL, 0, s_null(), s_null(), s_null());
      Rec *r = rec_new(&AR);
      V q;
      rec_setz(&AR, r, "at", v_str(nat));
      rec_setz(&AR, r, "op", v_strz(&AR, "ins_before"));
      rec_setz(&AR, r, "text", v_strz(&AR, "p\nq"));
      { V o; o.t = V_LIST; o.u.l = list_new(&AR); list_push(&AR, o.u.l, rec_to_v(r)); q = tx_patch_apply(&AR, empty, "e", o); }
      CK(q.t == V_REC);
      CKSTR(sfld(q, "text"), "p\nq");
      CKSTR(sfld(q, "eol"), "lf");
      CK(bfld(opat(fld(q, "checks"), 0), "ok"));
      q = tx_patch_apply(&AR, empty, "e", opl(1, op("eeeeeeee", "set", "x", 0)));
      CK(v_errcode(q) == E_ANCHOR_MISS);
    }
    { /* empty text for set means one blank line */
      Str t = TJN(V5);
      V q = tx_patch_apply(&AR, t, "b", opl(1, op(atc(t, 2), "set", "", 0)));
      CK(q.t == V_REC);
      CK(s_eq(sfld(q, "text"), s_lit(&AR, "alpha\nbeta\n\ndelta\nepsilon\n")));
      CKSTR(sfld(opat(fld(q, "checks"), 0), "preview"), "L3: <empty>");
      q = tx_patch_apply(&AR, t, "b", opl(1, op(atc(t, 0), "ins_after", "", 0)));
      CK(cntstr(sfld(q, "text"), "\n") == 6);
      /* empty ops list is a no-op that preserves the bytes */
      q = tx_patch_apply(&AR, t, "b", opl(0));
      CKI(ifld(q, "applied"), 0);
      CK(s_eq(sfld(q, "text"), t));
      CKI(fld(q, "checks").u.l->len, 0);
      CKSTR(sfld(q, "path"), "b");
    }
  }

  /* ================= diff ================= */
  T("diff stats and hunks");
  {
    Str t5 = TJN(V5);
    const char *chg[5] = { "alpha", "beta", "GAMMA", "delta", "epsilon" };
    Str u = TJN(chg);
    const char *ins[7] = { "alpha", "beta", "X", "Y", "gamma", "delta", "epsilon" };
    Str w = TJ(ins);
    const char *del[4] = { "alpha", "beta", "delta", "epsilon" };
    Str x = TJ(del);
    V d;
    d = tx_diff(&AR, t5, t5, 3, true);
    CK(d.t == V_REC);
    CKI(ifld(d, "adds"), 0);
    CKI(ifld(d, "dels"), 0);
    CKI(ifld(d, "hunks"), 0);
    CKSTR(sfld(d, "text"), "");
    CK(!hasfld(d, "ops"));
    CK(!hasfld(d, "approx"));
    d = tx_diff(&AR, s_lit(&AR, ""), s_lit(&AR, ""), 3, true);
    CKI(ifld(d, "adds"), 0);
    CKI(ifld(d, "hunks"), 0);
    d = tx_diff(&AR, t5, u, 3, true);
    CKI(ifld(d, "adds"), 1);
    CKI(ifld(d, "dels"), 1);
    CKI(ifld(d, "hunks"), 1);
    CK(inStr(sfld(d, "text"), "@@ -1,5 +1,5 @@"));
    CK(inStr(sfld(d, "text"), "-gamma"));
    CK(inStr(sfld(d, "text"), "+GAMMA"));
    CK(inStr(sfld(d, "text"), " beta"));
    d = tx_diff(&AR, t5, w, 3, true);                 /* pure insert into 5 lines */
    CKI(ifld(d, "adds"), 2);
    CKI(ifld(d, "dels"), 0);
    CKI(ifld(d, "hunks"), 1);
    CK(inStr(sfld(d, "text"), "@@ -1,5 +1,7 @@"));
    d = tx_diff(&AR, t5, x, 3, true);                 /* pure delete */
    CKI(ifld(d, "adds"), 0);
    CKI(ifld(d, "dels"), 1);
    CKI(ifld(d, "hunks"), 1);
    CK(inStr(sfld(d, "text"), "@@ -1,5 +1,4 @@"));
    { V a = tx_diff(&AR, t5, u, 0, true), b = tx_diff(&AR, t5, u, -7, true), e = tx_diff(&AR, t5, u, 3, true);
      CK(s_eq(sfld(a, "text"), sfld(b, "text")));     /* ctxl<=0 means 3 */
      CK(s_eq(sfld(a, "text"), sfld(e, "text")));       /* and so is the explicit 3 */       /* and that is the explicit 3 */
      V c = tx_diff(&AR, t5, u, 1, true);
      CK(inStr(sfld(c, "text"), "@@ -2,3 +2,3 @@"));  /* ctxl=1 shrinks the hunk */
      CKI(ifld(c, "hunks"), 1);
    }
    { /* two distant changes -> 2 hunks, close changes -> merged into 1 */
      const char *far[20];
      Str tf = TJN(V20), tf2;
      V d1, d2;
      for (int i = 0; i < 20; i++) far[i] = LB[i];
      memcpy(LB[1], "CH2", 4);
      Str m1 = TJN(far);
      memcpy(LB[17], "CH18", 5);
      tf2 = TJN(far);
      d1 = tx_diff(&AR, tf, tf2, 3, true);
      CKI(ifld(d1, "hunks"), 2);
      CKI(ifld(d1, "adds"), 2);
      CKI(ifld(d1, "dels"), 2);
      CK(cntstr(sfld(d1, "text"), "@@ -") == 2);
      CK(inStr(sfld(d1, "text"), "@@ -1,5 +1,5 @@"));
      CK(inStr(sfld(d1, "text"), "@@ -15,6 +15,6 @@"));
      memcpy(LB[1], "l02", 4); memcpy(LB[17], "l18", 4);
      d2 = tx_diff(&AR, tf, m1, 3, true);              /* only the l02 change left */
      CKI(ifld(d2, "hunks"), 1);
    }
    { const char *near[12] = { "L1", "L2", "L3", "L4", "Y5", "L6", "L7", "Z8", "L9", "L10", "L11", "L12" };
      Str a12 = TJN(V12), b12 = TJN(near);
      V d3 = tx_diff(&AR, a12, b12, 3, true);
      CKI(ifld(d3, "hunks"), 1);                      /* gap smaller than context */
      CKI(ifld(d3, "adds"), 2);
      CKI(ifld(d3, "dels"), 2);
      CK(inStr(sfld(d3, "text"), "@@ -2,10 +2,10 @@"));
    }
  }
  T("diff -> patch round trip");
  {
    Str t5 = TJN(V5);
    const char *chg[5] = { "alpha", "beta", "GAMMA", "delta", "epsilon" };
    const char *ins[8] = { "TOP", "alpha", "beta", "gamma", "delta", "epsilon", "BOT1", "BOT2" };
    const char *blk[3] = { "alpha", "beta", "epsilon" };
    const char *dup[4] = { "same", "same", "same", "z" };
    const char *dup2[4] = { "same", "same", "SAME2", "z" };
    const char *bl[7] = { "x", "", "", "", "", "", "y" };
    const char *blm[7] = { "x", "", "", "M", "", "", "y" };
    const char *aa[2] = { "a", "b" };
    const char *ab[3] = { "a", "", "b" };
    struct { Str a, b; } cs[8];
    int ncs = 0;
    Str tnn = TJN(chg), tin = TJN(ins), tdup = TJ(dup), tM = TJ(blm);
    cs[ncs].a = t5;              cs[ncs++].b = tnn;                 /* one change */
    cs[ncs].a = t5;              cs[ncs++].b = tin;                 /* head + tail insert */
    cs[ncs].a = t5;              cs[ncs++].b = TJN(blk);            /* block delete */
    cs[ncs].a = tdup;            cs[ncs++].b = TJ(dup2);             /* same, same, SAME2, z */
    cs[ncs].a = tM;              cs[ncs++].b = tdup;                /* duplicate anchors */
    cs[ncs].a = TJ(bl);          cs[ncs++].b = tM;                  /* set an ambiguous blank */
    cs[ncs].a = TJ(aa);          cs[ncs++].b = TJ(ab);              /* insert a blank line */
    cs[ncs].a = TJN(chg);        cs[ncs++].b = s_lit(&AR, "beta\n"); /* rewrite + delete */
    for (int i = 0; i < ncs; i++) {
      V d = tx_diff(&AR, cs[i].a, cs[i].b, 3, false);
      V o = fld(d, "ops");
      V p = tx_patch_apply(&AR, cs[i].a, "rt", o);
      CK(o.t == V_LIST);
      static const char *nm[8] = { "one change", "head+tail insert", "block delete", "change dup-context",
                                   "undo M", "insert ambiguous blank", "insert a blank line", "shrink to 1" };
      if (p.t != V_REC) printf("  RT[%s] err: %.*s\n", nm[i], emsg(p).len, emsg(p).p);
      else if (!s_eq(sfld(p, "text"), cs[i].b))
        printf("  RT[%s] got=[%.*s] want=[%.*s]\n", nm[i],
               sfld(p, "text").len, sfld(p, "text").p, cs[i].b.len, cs[i].b.p);
      CK(s_eq(sfld(p, "text"), cs[i].b));
    }
    { /* compact result shape */
      V d = tx_diff(&AR, t5, TJN(chg), 3, false);
      V o0;
      CK(!hasfld(d, "text"));
      CKI(ifld(d, "adds"), 1);
      CKI(ifld(d, "dels"), 1);
      CKI(fld(d, "ops").u.l->len, 1);
      o0 = opat(fld(d, "ops"), 0);
      CKSTR(sfld(o0, "op"), "set");
      CKSTR(sfld(o0, "text"), "GAMMA");
      CK(s_eq(sfld(o0, "at"), s_wrap(atc(t5, 2))));
      CK(!hasfld(o0, "n"));                    /* unambiguous: no n emitted */
      /* the ambiguous case carries n so patch never sees a false DUP */
      d = tx_diff(&AR, TJ(bl), tM, 3, false);
      o0 = opat(fld(d, "ops"), 0);
      CKI(ifld(o0, "n"), 2);
      CKSTR(sfld(o0, "text"), "M");
      /* pure insert becomes one ins_after/ins_before op */
      d = tx_diff(&AR, t5, tin, 3, false);
      CKI(fld(d, "ops").u.l->len, 2);
      CK(s_eqz(sfld(opat(fld(d, "ops"), 0), "op"), "ins_before"));
      CK(s_eqz(sfld(opat(fld(d, "ops"), 1), "op"), "ins_after"));
      CKI(ifld(d, "hunks"), 1);              /* 5 keeps apart is inside 2*ctxl */
      /* empty a -> b uses the empty-text position anchor */
      d = tx_diff(&AR, s_lit(&AR, ""), s_lit(&AR, "p\nq"), 3, false);
      CKI(fld(d, "ops").u.l->len, 1);
      CKSTR(sfld(opat(fld(d, "ops"), 0), "op"), "ins_before");
      CKI(ifld(d, "adds"), 2);
      CKI(ifld(d, "dels"), 0);
      { V p = tx_patch_apply(&AR, s_lit(&AR, ""), "e", fld(d, "ops"));
        CKSTR(sfld(p, "text"), "p\nq"); }
      /* a -> empty deletes everything */
      d = tx_diff(&AR, TJ(V5), s_lit(&AR, ""), 3, false);
      CKI(ifld(d, "dels"), 5);
      { V p = tx_patch_apply(&AR, TJ(V5), "e", fld(d, "ops"));
        CKSTR(sfld(p, "text"), ""); }
    }
    { /* a bigger real edit: 300 lines, 40 changed */
      static char big[400][8];
      static const char *bv[400];
      static const char *cv[400];
      int n2 = 0;
      for (int i = 0; i < 400; i++) { snprintf(big[i], 8, "x%03d", i); bv[i] = big[i]; cv[i] = big[i]; }
      Str ta = joinv(bv, 400, "\n", true);              /* copied before mutating */
      for (int i = 50; i < 90; i++) { snprintf(big[i], 8, "y%03d", i); cv[i] = big[i]; }
      for (int i = 200; i < 210; i++) { snprintf(big[i], 8, "z%03d", i); cv[i] = big[i]; }
      Str tb = joinv(cv, 400, "\n", true);
      V d = tx_diff(&AR, ta, tb, 3, false);
      V p;
      CKI(ifld(d, "dels"), 50);
      CKI(ifld(d, "adds"), 50);
      CKI(ifld(d, "hunks"), 2);              /* 110 keeps apart is past 2*ctxl */
      CK(!hasfld(d, "approx"));
      p = tx_patch_apply(&AR, ta, "big", fld(d, "ops"));
      CK(p.t == V_REC);
      CK(s_eq(sfld(p, "text"), tb));
      n2 = ifld(p, "applied");
      CKI(n2, 50);
      /* unified form of the same pair agrees with the stats */
      d = tx_diff(&AR, ta, tb, 3, true);
      CK(cntstr(sfld(d, "text"), "\n-") >= 50);
      CK(inStr(sfld(d, "text"), "--- a\n+++ b\n"));
    }
    { /* bounded-work guard: past 4000x4000 cells fall back to a whole replace */
      static char b1[4001][8], b2[4001][8];
      static const char *v1[4001], *v2[4001];
      for (int i = 0; i < 4001; i++) {
        snprintf(b1[i], 8, "a%04d", i); snprintf(b2[i], 8, "b%04d", i);
        v1[i] = b1[i]; v2[i] = b2[i];
      }
      Str ta = joinv(v1, 4001, "\n", false), tb = joinv(v2, 4001, "\n", false);
      V d = tx_diff(&AR, ta, tb, 3, true);
      CK(bfld(d, "approx"));
      CKI(ifld(d, "adds"), 4001);
      CKI(ifld(d, "dels"), 4001);
      CKI(ifld(d, "hunks"), 1);
      d = tx_diff(&AR, ta, tb, 3, false);
      CK(bfld(d, "approx"));
      CKI(fld(d, "ops").u.l->len, 4001);
    }
  }
  T("diff is deterministic, stats helper agrees");
  {
    Str t5 = TJN(V5);
    const char *chg[5] = { "alpha", "beta", "GAMMA", "delta", "epsilon" };
    Str u = TJN(chg);
    V a1 = tx_diff(&AR, t5, u, 3, true);
    V a2 = tx_diff(&AR, t5, u, 3, true);
    /* verify the stats by counting the diff's own +/- lines, not by re-calling
     * the helper that produced them */
    Str dt = sfld(a1, "text");
    int padds = 0, pdels = 0;
    for (int i = 0; i < dt.len; i++) {
      if ((dt.p[i] == '+' || dt.p[i] == '-') && (i == 0 || dt.p[i-1] == 10) &&
          !(i + 1 < dt.len && (dt.p[i+1] == '+' || dt.p[i+1] == '-'))) {
        if (dt.p[i] == '+') padds++; else pdels++;
      }
    }
    CKI(padds, ifld(a1, "adds"));
    CKI(pdels, ifld(a1, "dels"));
    CKI(ifld(a1, "hunks"), 1);
    /* an empty op list is a successful no-op, not an error */
    V nop = tx_patch_apply(&AR, t5, "p", opl(0));
    CK(!v_is_err(nop));
    CKI(ifld(nop, "applied"), 0);
    CK(s_eq(sfld(nop, "text"), t5));
  }
  T("no pointer into caller storage escapes");
  {
    char stackbuf[64];
    Str st;
    Anch *A;
    int n = 0;
    V r;
    strcpy(stackbuf, "aa\nbb\ncc");
    st.p = stackbuf;
    st.len = (int)strlen(stackbuf);
    A = anchors_of(&AR, "s", st, &n, NULL);
    CKI(n, 3);
    CKSTR(A[1].text, "bb");
    strcpy(stackbuf, "XY\nYZ\nWV");                 /* scribble over the input */
    CKSTR(A[1].text, "bb");
    strcpy(stackbuf, "aa\nbb\ncc");
    r = tx_patch_apply(&AR, st, "s", opl(1, op(A[1].at.p, "set", "BB", 0)));
    CK(r.t == V_REC);
    strcpy(stackbuf, "00\n00\n00");
    CK(s_eq(sfld(r, "text"), s_lit(&AR, "aa\nBB\ncc")));
    CKSTR(sfld(opat(fld(r, "checks"), 0), "preview"), "L2: BB");
    CK(!inStr(sfld(r, "text"), "00"));
  }
  T_REPORT("xdiff");
}
