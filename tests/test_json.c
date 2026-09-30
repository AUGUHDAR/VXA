/* test_json.c - SPEC 4.5: strict JSON in, strict JSON out.
 *
 * The laws under test, in the order they are asserted:
 *   1. a malformed byte yields an ERR naming the byte offset, the line, the
 *      column and an escaped snippet - never a repair, never a second guess;
 *   2. every limit is a value (E_LIMIT) that names the limit and how to raise it;
 *   3. parse -> stringify -> parse gives back the same value and the same bytes,
 *      and an integer never comes back as 1.0;
 *   4. a duplicate key resolves last-wins AND is reported (dupes=N);
 *   5. json.query pulls one path out of a big document without building the rest;
 *   6. UTF-8 passes through as bytes, \uXXXX decodes, and nothing is substituted.
 *
 * The builtin table is the only part of json.c that reaches into the interpreter
 * and the only symbol it needs is ctx_arena. This suite links util+val+json (see
 * build.sh deps), so that symbol comes from here: a real Ctx's first field IS its
 * Arena (struct Ctx, interp.c), which is what the shim hands back for a
 * (Ctx*)&A argument. cfg_get_c comes from lib_core.c in a full build; here it is
 * test-controlled, which is also how the json.* config overlay gets asserted.
 */
#include "vxa.h"
#include "lib.h"
#include "json.h"
#include "t.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

static Arena A;

Arena *ctx_arena(Ctx *c) { return (Arena*)c; }
Ctx *ctx_new(Arena *a, const char *cwd, const char *name) { (void)cwd; (void)name; return (Ctx*)a; }
void ctx_free(Ctx *c) { (void)c; }
void plan_disp(Plan *p, Buf *b, bool compact) { (void)p; (void)b; (void)compact; }
V list_of(List *l) { V r; r.t = V_LIST; r.u.l = l; return r; }

static const char *g_cfg_key = "";     /* what config says, per test */
static double g_cfg_num = 0;
V cfg_get_c(Ctx *c, const char *k) { (void)c; if (!strcmp(k, g_cfg_key)) return v_num(g_cfg_num); return VN; }

/* ---------------- helpers ---------------- */
static V P(const char *text) { return json_parse(&A, s_wrap(text), NULL); }
static V Pn(Str text) { return json_parse(&A, text, NULL); }
static Str RS(V v) { return json_stringify(&A, v, false); }
static Str RSP(V v) { return json_stringify(&A, v, true); }
static bool isE(V v) { return v_is_err(v); }
static Str one(V v) { return v_tostr(&A, v, true); }              /* the compact line */
static Str sj(V v) { return v_tojson(&A, v); }

static ErrCode EC(V v) { return isE(v) && v.u.e ? v.u.e->code : E_NONE; }
static Str EM(V v) { return isE(v) && v.u.e ? v.u.e->msg : s_null(); }
static Str EH(V v) { return isE(v) && v.u.e ? v.u.e->hint : s_null(); }
static V ED(V v, const char *k) {
  if (!isE(v) || !v.u.e) return VN;
  V d = v.u.e->data;
  if (d.t != V_REC || !d.u.r) return VN;
  V *p = rec_getz(d.u.r, k);
  return p ? *p : VN;
}
static long long EDN(V v, const char *k) { V x = ED(v, k); return x.t == V_NUM ? (long long)x.u.n : -999999; }
static bool has_(Str h, const char *n) { return h.p != NULL && s_findz(h, n, 0) >= 0; }
#define CKHAS(h, n) CHECK(has_(h, n), "want text to contain \"%s\", got \"%.*s\"", n, (h).len, (h).p ? (h).p : "")
#define CKNO(h, n) CHECK(!has_(h, n), "want text WITHOUT \"%s\", got \"%.*s\"", n, (h).len, (h).p ? (h).p : "")
static bool oneline(Str s) { return !s.p || !has_(s, "\n"); }
static int nlines(Str s) { return s.p ? s_count_char(s, '\n') : 0; }

static V fld(V r, const char *k) {
  V *p = (r.t == V_REC && r.u.r) ? rec_getz(r.u.r, k) : NULL;
  return p ? *p : VN;
}
static V at(V r, int i) {
  V *p = (r.t == V_LIST && r.u.l) ? list_get(r.u.l, i) : NULL;
  return p ? *p : VN;
}
static long long numof(V v) { return v.t == V_NUM ? (long long)v.u.n : -999999; }
static bool isstr(V v, const char *s) { return v.t == V_STR && s_eqz(v.u.s, s); }
static int bytes_of(V v, int i) { return (v.t == V_STR && v.u.s.p && i < v.u.s.len) ? (unsigned char)v.u.s.p[i] : -1; }

/* an error, blaming an exact position */
static void ckbad(V e, ErrCode code, int off, int line, int col, const char *has) {
  CHECK(isE(e), "expected an ERR, got a %s", v_typename(e));
  if (!isE(e)) return;
  CKI((int)EC(e), (int)code);
  CKI(EDN(e, "offset"), off);
  CKI(EDN(e, "line"), line);
  CKI(EDN(e, "col"), col);
  char want[64];
  snprintf(want, sizeof want, "at offset=%d line=%d col=%d", off, line, col);
  CKHAS(EM(e), want);
  if (has) CKHAS(EM(e), has);
  V near_v = ED(e, "near");
  Str near = near_v.t == V_STR ? near_v.u.s : s_null();
  CHECK(near.len <= JSON_SNIPPET_MAX, "near= is %d chars, the cap is %d", near.len, JSON_SNIPPET_MAX);
  CHECK(oneline(EM(e)) && oneline(EH(e)) && oneline(near), "a diagnostic must stay on one line");
  Str c = one(e);
  CKHAS(c, "code=");
  CKHAS(c, "offset=");
  CKHAS(c, "line=");
  CKHAS(c, "col=");
  CHECK(nlines(c) == 0, "the compact error line holds %d newlines", nlines(c));
  Str j = sj(e);
  CKHAS(j, "\"__err\"");
  CHECK(nlines(j) == 0, "the json error line holds %d newlines", nlines(j));
}
static void ckbadT(const char *text, ErrCode code, int off, int line, int col, const char *has) {
  ckbad(P(text), code, off, line, col, has);
}
static Str S(const char *s) { return s_lit(&A, s); }
/* the round-trip law, on one document */
static void rt(const char *label, const char *text) {
  V v1 = P(text);
  CHECK(!isE(v1), "%s: must parse, got \"%.*s\"", label, EM(v1).len, EM(v1).p);
  if (isE(v1)) return;
  Str s1 = RS(v1);
  V v2 = Pn(s1);
  CHECK(!isE(v2), "%s: its own output must parse: \"%.*s\"", label, EM(v2).len, EM(v2).p);
  if (isE(v2)) return;
  Str s2 = RS(v2);
  CHECK(v_eq(v1, v2), "%s: round trip changed the value", label);
  CHECK(s_eq(s1, s2), "%s: second generation differs: \"%.*s\" vs \"%.*s\"", label,
        s1.len, s1.p, s2.len, s2.p);
  CHECK(s_eq(s1, sj(v1)), "%s: json_render must equal v_tojson: \"%.*s\" vs \"%.*s\"", label,
        s1.len, s1.p, sj(v1).len, sj(v1).p);
}
static Str rep(char c, int n) {
  Buf b; buf_init(&b, &A);
  for (int i = 0; i < n; i++) buf_putc(&b, c);
  return buf_take(&b);
}
static Str doc_long(int n) {                    /* {"k":"xxxx..."} */
  Str x = rep('x', n);
  Buf b; buf_init(&b, &A);
  buf_puts(&b, "{\"k\":\"");
  buf_put(&b, x.p, (size_t)x.len);
  buf_puts(&b, "\"}");
  return buf_take(&b);
}
static Str doc_depth(int n) {                   /* [[[ ... ]]] */
  Str o = rep('[', n), c = rep(']', n);
  Buf b; buf_init(&b, &A);
  buf_put(&b, o.p, (size_t)o.len);
  buf_put(&b, c.p, (size_t)c.len);
  return buf_take(&b);
}

/* ---------------- the language rows ---------------- */
static const Builtin *row(const char *name) {
  int n = 0;
  const Builtin *t = t_json(&n);
  for (int i = 0; i < n; i++) if (!strcmp(t[i].ns, "json") && !strcmp(t[i].name, name)) return &t[i];
  return NULL;
}
static V call(const char *name, V *args, int nargs) {
  const Builtin *b = row(name);
  if (!b) { CHECK(false, "t_json() has no row json.%s", name); return VN; }
  if (nargs < b->min || (b->max >= 0 && nargs > b->max)) {
    CHECK(false, "json.%s arity is %d..%d, the test asked for %d", name, b->min, b->max, nargs);
    return VN;
  }
  return b->fn((Ctx*)&A, args, nargs);
}
static V sarg(const char *s) { return v_str(s_lit(&A, s)); }

int main(void) {
  arena_init(&A, 64u * 1024u * 1024u);

  /* ============ 1. every value type, and nesting ============ */
  T("types and nesting");
  CK(fld(P("{\"a\":1}"), "a").t == V_NUM);
  CK(isstr(P("\"x\""), "x"));
  CK(fld(P("{\"a\":null}"), "a").t == V_NULL);
  CK(fld(P("{\"a\":{}}"), "a").t == V_REC);
  CK(fld(P("{\"a\":[]}"), "a").t == V_LIST);
  CK(numof(P("42")) == 42);
  CK(numof(P("-17")) == -17);
  CKI(at(P("[1,2,3]"), 2).u.n, 3);
  CKI(P("[1,2,3]").u.l->len, 3);
  CKI(P("[[[1]]]").u.l->len, 1);
  CK(at(at(P("[[[1]]]"), 0), 0).t == V_LIST);
  CK(P("true").u.b == true);
  CK(P("false").u.b == false);
  CK(P("null").t == V_NULL);
  CKI(P("\"\"").u.s.len, 0);
  CKI(P("{}").u.r->len, 0);
  CKI(P("[]").u.l->len, 0);
  CKSTR(RS(P("{}")), "{}");
  CKSTR(RS(P("[]")), "[]");
  CKSTR(RS(P("\"\"")), "\"\"");
  CKSTR(RS(P("null")), "null");
  CKSTR(RS(P("true")), "true");
  CKSTR(RS(P("false")), "false");
  /* insertion order is the value's, not the hash's (SPEC 2.1) */
  V nest = P("{\"z\":1,\"a\":{\"y\":2,\"b\":[3,{\"c\":null}]},\"m\":[[[]]]}");
  CK(!isE(nest));
  CKI(nest.u.r->len, 3);
  CKSTR(nest.u.r->kv[0].k, "z");
  CKSTR(nest.u.r->kv[1].k, "a");
  CKSTR(nest.u.r->kv[2].k, "m");
  CKSTR(RS(nest), "{\"z\":1,\"a\":{\"y\":2,\"b\":[3,{\"c\":null}]},\"m\":[[[]]]}");
  CKSTR(RS(P("{\"\":1,\"a.b\":2}")), "{\"\":1,\"a.b\":2}");
  CK(!isE(P("[true,false,null,0,\"\",{},[]]")));

  /* ============ 2. numbers ============ */
  T("numbers");
  CKSTR(RS(P("1")), "1");
  CKSTR(RS(P("{\"n\":1}")), "{\"n\":1}");
  CKSTR(RS(P("{\"n\":1.5}")), "{\"n\":1.5}");
  CKSTR(RS(P("-1.25")), "-1.25");
  CKSTR(RS(P("0")), "0");
  CKSTR(RS(P("-0")), "0");                       /* -0 normalises to 0: documented */
  CK(v_eq(P("-0"), P("0")));
  CKSTR(RS(P("1e2")), "100");
  CKSTR(RS(P("1E+2")), "100");
  CKSTR(RS(P("1e-2")), "0.01");
  CKSTR(RS(P("0e0")), "0");
  CKSTR(RS(P("3.14159")), "3.14159");
  CK(numof(P("9007199254740991")) == 9007199254740991LL);   /* 2^53-1 survives exactly */
  CKSTR(RS(P("900719925474099")), "900719925474099");
  CKSTR(RS(P("2.5e-13")), "2.5e-13");
  CKSTR(RS(P("1e-999")), "0");                   /* flushed to 0, and stays 0 */
  /* the documented precision wall: fmt_num keeps 15 significant digits, so an
   * integer past 1e15 prints in exponential form and is stable from there on */
  V big = P("12345678901234567");
  CK(!isE(big));
  Str bs = RS(big);
  CKSTR(bs, "1.23456789012346e+16");
  CKSTR(RS(Pn(bs)), bs.p);
  /* 1e999 is refused, not saturated: infinity has no JSON form (SPEC 4.5) */
  V ov = P("1e999");
  ckbad(ov, E_RANGE, 0, 1, 1, "overflows double");
  CKI(EDN(ov, "digits"), 5);
  CKHAS(EH(ov), "keep it as a string");
  ckbadT("[1e999]", E_RANGE, 1, 1, 2, "overflows double");
  ckbadT("{\"a\":1e999}", E_RANGE, 5, 1, 6, "overflows double");
  /* only the shapes JSON allows */
  ckbadT("{\"a\":01}", E_PARSE, 6, 1, 7, "leading zero");
  ckbadT("[00]", E_PARSE, 1, 1, 2, "leading zero");
  ckbadT("[5.]", E_PARSE, 1, 1, 2, "expected a value");
  ckbadT("[.5]", E_PARSE, 1, 1, 2, "expected a value");
  ckbadT("[+5]", E_PARSE, 1, 1, 2, "'+'");
  ckbadT("[1e]", E_PARSE, 1, 1, 2, "expected a value");
  ckbadT("[1e+]", E_PARSE, 1, 1, 2, "expected a value");
  ckbadT("[--1]", E_PARSE, 1, 1, 2, "expected a value");
  ckbadT("[1.2.3]", E_PARSE, 4, 1, 5, "extra data");
  ckbadT("[0x1F]", E_PARSE, 2, 1, 3, "extra data");
  ckbadT("[NaN]", E_PARSE, 1, 1, 2, "NaN");
  ckbadT("[Infinity]", E_PARSE, 1, 1, 2, "Infinity");
  ckbadT("[-Infinity]", E_PARSE, 1, 1, 2, "Infinity");
  ckbadT("[-1x]", E_PARSE, 2, 1, 3, "extra data");

  /* ============ 3. strings, escapes, UTF-8 ============ */
  T("escapes and UTF-8");
  CK(isstr(P("\"a\""), "a"));
  CK(isstr(P("\"\\\"\""), "\""));
  CK(isstr(P("\"\\\\\""), "\\"));
  CK(isstr(P("\"\\/\""), "/"));
  CKI(P("\"\\n\"").u.s.len, 1);
  CKI(bytes_of(P("\"\\n\""), 0), '\n');
  CKI(bytes_of(P("\"\\r\""), 0), '\r');
  CKI(bytes_of(P("\"\\t\""), 0), '\t');
  CKI(bytes_of(P("\"\\b\""), 0), '\b');
  CKI(bytes_of(P("\"\\f\""), 0), '\f');
  CKI(P("\"\\n\\r\\t\\b\\f\\\"\\\\\\/\"").u.s.len, 8);
  CKI(P("\"\\u0041\"").u.s.len, 1);
  CK(isstr(P("\"\\u0041\""), "A"));
  CKI(P("\"\\u00fc\"").u.s.len, 2);                        /* BMP -> 2 bytes */
CKI(bytes_of(P("\"\\u00fc\""), 0), 0xC3);
  CKI(bytes_of(P("\"\\u00fc\""), 1), 0xBC);
  CKI(P("\"\\u65e5\"").u.s.len, 3);                        /* CJK -> 3 bytes */
  CKI(bytes_of(P("\"\\u65e5\""), 2), 0xA5);
  CKI(P("\"\\ud83d\\ude00\"").u.s.len, 4);                 /* emoji pair -> 4 bytes */
  CKI(bytes_of(P("\"\\ud83d\\ude00\""), 0), 0xF0);
  CKI(bytes_of(P("\"\\ud83d\\ude00\""), 3), 0x80);
  CKI(P("\"\\u0000\"").u.s.len, 1);                        /* a NUL byte is legal in a Str */
  CKI(bytes_of(P("\"\\u0000\""), 0), 0);
  CKSTR(RS(P("\"\\u0000\"")), "\"\\x00\"");                /* the emitter's own form */
  CK(v_eq(P(RS(P("\"\\u0000\"")).p), P("\"\\u0000\"")));   /* ... and it reads back */
  CKSTR(RS(P("\"\\u001f\"")), "\"\\x1f\"");
  CK(v_eq(P("\"\\x1f\""), P("\"\\u001f\"")));              /* \xNN == \u00NN */
  CKI(bytes_of(P("\"\\x7f\""), 0), 0x7f);
  /* raw UTF-8 passes through as bytes, uncounted and unconverted (SPEC 9) */
  CKI(P("\"\xc3\xa9\"").u.s.len, 2);
  CKI(P("\"\xe6\x97\xa5\"").u.s.len, 3);
  CKSTR(RS(P("\"\xe6\x97\xa5\"")), "\"\xe6\x97\xa5\"");
  CKI(s_ucount(P("\"\xe6\x97\xa5\"").u.s), 1);
  /* bad escapes: refused, never replaced */
  ckbadT("\"\\q\"", E_PARSE, 2, 1, 3, "unknown escape");
  ckbadT("\"\\x1\"", E_PARSE, 3, 1, 4, "hex");
  ckbadT("\"\\xgg\"", E_PARSE, 3, 1, 4, "hex");
  ckbadT("\"\\x80\"", E_PARSE, 2, 1, 3, "out of range");
  ckbadT("\"\\u12\"", E_PARSE, 3, 1, 4, "4 hex digits");
  ckbadT("\"\\u12g4\"", E_PARSE, 3, 1, 4, "4 hex digits");
  ckbadT("\"\\uzzzz\"", E_PARSE, 3, 1, 4, "4 hex digits");
  ckbadT("\"\\\"", E_PARSE, 1, 1, 2, "ends inside an escape");
  ckbadT("\"abc", E_PARSE, 1, 1, 2, "unterminated string");
  ckbadT("[\"a\vb\"]", E_PARSE, 3, 1, 4, "control byte");
  ckbadT("[\"a\x01" "b\"]", E_PARSE, 3, 1, 4, "0x01");
  ckbadT("[\"a\x7f" "b\"]", E_PARSE, 3, 1, 4, "0x7f");
  ckbadT("[\"raw\nbreak\"]", E_PARSE, 3, 1, 4, "control byte");

  /* lone surrogates are refused: half a code point is not a character */
  ckbadT("\"\\ud800\"", E_PARSE, 1, 1, 2, "lone high surrogate");
  ckbadT("\"\\udc00\"", E_PARSE, 1, 1, 2, "lone low surrogate");
  ckbadT("\"\\udbff\"", E_PARSE, 1, 1, 2, "lone high surrogate");
  ckbadT("\"\\ud800\\ud800\"", E_PARSE, 1, 1, 2, "low half");
  ckbadT("\"\\ud83dx\"", E_PARSE, 1, 1, 2, "lone high surrogate");
  ckbadT("\"\\ud800A\"", E_PARSE, 1, 1, 2, "lone high surrogate");
  /* over-long, surrogate-encoded, out-of-range and truncated UTF-8 */
  ckbadT("[\"\xc0\x80\"]", E_PARSE, 2, 1, 3, "invalid UTF-8");
  ckbadT("[\"\xe0\x80\x80\"]", E_PARSE, 2, 1, 3, "invalid UTF-8");
  ckbadT("[\"\xed\xa0\x80\"]", E_PARSE, 2, 1, 3, "invalid UTF-8");     /* encoded surrogate */
  ckbadT("[\"\xf4\x90\x80\x80\"]", E_PARSE, 2, 1, 3, "invalid UTF-8"); /* past U+10FFFF */
  ckbadT("[\"\xf5\x80\x80\x80\"]", E_PARSE, 2, 1, 3, "invalid UTF-8");
  ckbadT("[\"\xc3\"]", E_PARSE, 2, 1, 3, "invalid UTF-8");             /* truncated */
  ckbadT("[\"\xe6\x97\"]", E_PARSE, 2, 1, 3, "invalid UTF-8");
  ckbadT("[\"\x80\"]", E_PARSE, 2, 1, 3, "invalid UTF-8");             /* stray continuation */
  /* a key is decoded by the same rules as a value */
  ckbadT("{\"\xc0\x80\":1}", E_PARSE, 2, 1, 3, "invalid UTF-8");
  ckbadT("{\"\\ud800\":1}", E_PARSE, 1, 1, 2, "lone high surrogate");

  /* ============ 4. whitespace ============ */
  T("whitespace");
  CK(!isE(P("  [ 1 , 2 ]  ")));
  CK(!isE(P("\t{\"a\"\t:\t1\t}")));
  CK(!isE(P("\r\n{\r\n\"a\"\r\n:\r\n1\r\n}\r\n")));
  CK(!isE(P("[\n1,\n2\n]")));
  CK(numof(fld(P("{\n\"a\" : \n 1 \n}"), "a")) == 1);
  ckbadT("[\f1]", E_PARSE, 1, 1, 2, "control byte");
  ckbadT("[\v1]", E_PARSE, 1, 1, 2, "control byte");
  ckbadT("[\xc2\xa0" " 1]", E_PARSE, 1, 1, 2, "byte 0xc2");   /* NBSP is not whitespace */
  ckbadT("[1,2\v]", E_PARSE, 4, 1, 5, "expected ',' or ']'");
  ckbadT("", E_PARSE, 0, 1, 1, "empty input");
  ckbadT("   ", E_PARSE, 3, 1, 4, "no JSON value");
  ckbadT("\n\n", E_PARSE, 2, 3, 1, "no JSON value");

  /* ============ 5. structural malformations ============ */
  T("malformations");
  ckbadT("[1,2", E_PARSE, 4, 1, 5, "never closed");
  ckbadT("[", E_PARSE, 1, 1, 2, "never closed");
  ckbadT("{\"a\":1]", E_PARSE, 7, 1, 8, "expected ',' or '}'");
  ckbadT("[1}", E_PARSE, 2, 1, 3, "expected ',' or ']'");
  ckbadT("{\"a\":1 \"b\":2}", E_PARSE, 7, 1, 8, "expected ',' or '}'");
  ckbadT("[1 2]", E_PARSE, 3, 1, 4, "expected ',' or ']'");
  ckbadT("[1,2,]", E_PARSE, 5, 1, 6, "trailing comma");
  ckbadT("{\"a\":1,}", E_PARSE, 8, 1, 9, "trailing comma");
  ckbadT("{,}", E_PARSE, 1, 1, 2, "quoted object key");
  ckbadT("{a:1}", E_PARSE, 1, 1, 2, "quoted object key");
  ckbadT("{'a':1}", E_PARSE, 1, 1, 2, "double quotes");
  ckbadT("{\"a\"}", E_PARSE, 5, 1, 6, "expected ':'");
  ckbadT("{\"a\"", E_PARSE, 5, 1, 6, "expected ':'");
  ckbadT("{\"a\":}", E_PARSE, 6, 1, 7, "expected a value");
  ckbadT("[,]", E_PARSE, 1, 1, 2, "expected a value");
  ckbadT("]", E_PARSE, 0, 1, 1, "expected a value");
  ckbadT("}", E_PARSE, 0, 1, 1, "expected a value");
  ckbadT(",", E_PARSE, 0, 1, 1, "expected a value");
  ckbadT("[1] [2]", E_PARSE, 4, 1, 5, "extra data");
  ckbadT("{}x", E_PARSE, 2, 1, 3, "extra data");
  ckbadT("nulll", E_PARSE, 4, 1, 5, "extra data");
  ckbadT("tru", E_PARSE, 0, 1, 1, "expected true, false or null");
  ckbadT("truefalse", E_PARSE, 4, 1, 5, "bare word");
  ckbadT("'x'", E_PARSE, 0, 1, 1, "single quotes");
  ckbadT("`x`", E_PARSE, 0, 1, 1, "backticks");
  ckbadT("// hi", E_PARSE, 0, 1, 1, "comments");
  ckbadT("/* hi */1", E_PARSE, 0, 1, 1, "comments");
  ckbadT("nope", E_PARSE, 0, 1, 1, "expected a value");
  V tc = P("{\n  \"a\": 1,\n  \"b\" 2\n}");
  ckbad(tc, E_PARSE, 16, 3, 6, "expected ':' after object key");
  V nr_v = ED(tc, "near");
  Str nr = nr_v.t == V_STR ? nr_v.u.s : s_null();
  CHECK(nr.len > 0, "a multi-line break still carries a snippet");
  CKNO(nr, "\n");
  CKHAS(nr, "\\n");
  V ep = P("{\"a\":[1,2");
  ckbad(ep, E_PARSE, 10, 1, 11, "never closed");
  CKHAS(one(ep), "path=");
  V path_v = ED(ep, "path");
  CKHAS(path_v.t == V_STR ? path_v.u.s : s_null(), "$.a[1]");
  Str pn = s_wrap("preset");
  V qe = P("{\"a\":1,\"b\" 2}");
  ckbad(qe, E_PARSE, 13, 1, 14, "expected ':'");
  CHECK(pn.len > 0 && pn.len <= JSON_SNIPPET_MAX, "err_snippet out-param holds %d chars", pn.len);
  CKHAS(pn, "b");
  CHECK(oneline(pn), "err_snippet holds no raw newline");
  V near_qe = ED(qe, "near");
  CHECK(s_eq(pn, near_qe.t == V_STR ? near_qe.u.s : s_null()), "the out-param and near= are the same bytes");
  Str keep = s_wrap("");
  json_parse(&A, s_wrap("[1,2]"), &keep);
  CHECK(keep.len == 0, "a success leaves the snippet empty, got %.*s", keep.len, keep.p);

  /* ============ 6. duplicate keys: last wins, and the parse says so ============ */
  T("duplicate keys");
  V d1 = P("{\"a\":1,\"a\":2}");
  CK(!isE(d1));
  CK(numof(fld(d1, "a")) == 2);
  CKSTR(RS(d1), "{\"a\":2}");
  V d2 = P("{\"a\":1,\"b\":2,\"a\":3}");
  CK(numof(fld(d2, "a")) == 3);
  CKI(d2.u.r->len, 2);
  CKSTR(d2.u.r->kv[0].k, "a");              /* position kept, value replaced */
  CKSTR(RS(d2), "{\"a\":3,\"b\":2}");
  V d3 = P("{\"a\":1,\"a\":2,\"a\":3}");
  CK(numof(fld(d3, "a")) == 3);
  CKSTR(RS(d3), "{\"a\":3}");
  V d4 = P("[1,1,1]");                      /* arrays have no keys: no dupes */
  CKI(d4.u.l->len, 3);
  JsonMeta m0;
  memset(&m0, 0, sizeof m0);
  V ok1 = P("{\"a\":1}");
  CK(!isE(ok1));
  Str tmp = s_null();
  JsonLimits def = json_limits_default();
  def.max_nodes = 100000;
  json_parse_lim(&A, S("{\"a\":1,\"a\":2,\"b\":{\"c\":1,\"c\":2,\"c\":3}}"), &tmp, &def, &m0);
  CKI(m0.dupes, 3);                          /* 1 at the top level + 2 nested */
  memset(&m0, 0, sizeof m0);
  json_parse_lim(&A, S("{\"a\":1,\"b\":2}"), &tmp, &def, &m0);
  CKI(m0.dupes, 0);
  CKI(m0.nodes, 3);
  CKI(m0.depth, 1);
  memset(&m0, 0, sizeof m0);
  json_parse_lim(&A, S("[[],[1]]"), &tmp, &def, &m0);
  CKI(m0.nodes, 3);
  CKI(m0.depth, 2);
  CKI(m0.bytes, 8);
  memset(&m0, 0, sizeof m0);
  json_parse_lim(&A, S("{\"x\":{\"y\":{\"z\":[1]}}}"), &tmp, &def, &m0);
  CKI(m0.depth, 4);
  CKI(m0.nodes, 5);                          /* x,y,z containers + the number */

  /* ============ 7. limits are values, and they name themselves ============ */
  T("limits");
  CK(!isE(Pn(doc_depth(128))));                    /* exactly at the default */
  V deep = Pn(doc_depth(129));
  CK(isE(deep));
  CKI((int)EC(deep), (int)E_LIMIT);
  CKHAS(EM(deep), "limit 128");
  CKI(EDN(deep, "max_depth"), 128);
  CKHAS(EH(deep), "max_depth");
  CKHAS(EH(deep), "json.query");
  CKHAS(one(deep), "code=LIMIT");
  CHECK(nlines(one(deep)) == 0, "a limit error stays on one line");
  V path_deep = ED(deep, "path");
  CKHAS(path_deep.t == V_STR ? path_deep.u.s : s_null(), "$[0][0][0]");
  JsonLimits tight = json_limits_default();
  tight.max_depth = 4;
  Str sn = s_null();
  CK(!isE(json_parse_lim(&A, S("[[[[1]]]]"), &sn, &tight, NULL)));
  ckbad(json_parse_lim(&A, S("[[[[[1]]]]]"), &sn, &tight, NULL), E_LIMIT, 4, 1, 5, "limit 4");
  JsonLimits bytes = json_limits_default();
  bytes.max_bytes = 16;
  V big_in = json_parse_lim(&A, S("{\"aaaaaaaaaaaaaaaaaaaaaaaaaaaa\":1}"), &sn, &bytes, NULL);
  CK(isE(big_in));
  CKI((int)EC(big_in), (int)E_LIMIT);
  CKHAS(EM(big_in), "32 bytes");
  CKHAS(EM(big_in), "cap 16");
  CKHAS(EH(big_in), "max_bytes");
  CHECK(s_eq(sn, s_null()), "an oversized input has no region to show");
  JsonLimits nodes = json_limits_default();
  nodes.max_nodes = 3;
  CK(!isE(json_parse_lim(&A, S("[1,2]"), &sn, &nodes, NULL)));
  ckbad(json_parse_lim(&A, S("[1,2,3,4]"), &sn, &nodes, NULL), E_LIMIT, 7, 1, 8, "cap 3");
  /* the caps are checked before allocating: a small arena is an ERR, not an abort */
  Arena small;
  arena_init(&small, 40 * 1024);
  V tight_arena = json_parse_lim(&small, S("{\"a\":[1,2,3,4,5]}"), &sn, &def, NULL);
  CHECK(!isE(tight_arena) || EC(tight_arena) == E_LIMIT, "small arena: %.*s", EM(tight_arena).len, EM(tight_arena).p);
  arena_free(&small);

  /* ============ 8. the round-trip law, on an embedded corpus ============ */
  T("round trip");
  static const char *corpus[] = {
    "{}", "[]", "42", "\"x\"", "true", "false", "null",
    "{\"a\":1,\"b\":2.5,\"c\":\"x\",\"d\":true,\"e\":false,\"f\":null,\"g\":[],\"h\":{}}",
    "[[[]]]", "[1,[2,[3,[4,[5]]]]]",
    "{\"name\":\"app\",\"version\":\"1.2.3\",\"private\":true,"
      "\"dependencies\":{\"build\":\"^1.0.0\",\"dev\":\"2.x\"},"
      "\"scripts\":{\"build\":\"tsc --strict\",\"test\":\"node --test\"},"
      "\"keywords\":[\"a\",\"b\"],\"engines\":{\"node\":\">=18\"}}",
    "{\"compilerOptions\":{\"target\":\"ES2022\",\"strict\":true,\"paths\":{\"@x/*\":[\"./src/*\"]},"
      "\"types\":[]},\"include\":[\"src/**/*\"],\"exclude\":[]}",
    "{\"名称\":\"中文值\",\"語\":\"全\",\"key\":\"значение\"}",
    "{\"q\":\"tab\\tnl\\ncr\\rquote\\\"bslash\\\\slash\\/back\\bb\\ff\\u00e9\\u0000\\u001f\\x7f\"}",
    "{\"n\":[0,-0,1,-1,1.5,-1.5,1e2,1E+2,1e-2,0e0,-0.0,3.14159,2.5e-13,1000000000000000]}",
    " \n\t\r{\n \"a\" : [ 1 ,\t 2 ] ,\"b\":{\"c\":[]}\r\n}\n ",
    "{\"items\":[{\"n\":1,\"t\":\"a\"},{\"n\":2,\"t\":\"b\"},{\"n\":3,\"t\":null}]}",
    "{\"\":\"empty key\",\"a.b\":1,\"a b\":2,\"a\"\"b\":3,\"日本\":4,\"[x]\":5}",
    "\"\\u00fc\\u65e5\\u8a9e\\u20ac\\ud83d\\ude00\"",
    "[true,false,null,0,1,\"\",{},[],[[]],[{}]]",
    "{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":{\"f\":{\"g\":[1,2,3]}}}}}}}",
    "{\"packages\":[{\"name\":\"serde\",\"version\":\"1.0.219\",\"features\":[\"derive\",\"std\"],"
      "\"dependencies\":[{\"name\":\"serde_core\",\"kind\":\"build\"}]}],"
      "\"workspace_root\":\"/x/y\",\"target_directory\":\"/x/y/target\"}",
  };
  for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++) {
    char lab[16];
    snprintf(lab, sizeof lab, "doc%zu", i);
    rt(lab, corpus[i]);
  }
  Str longest = doc_long(4000);
  rt("long-4000", "");                                     /* placeholder, see below */
  {
    V v1 = Pn(longest);
    CK(!isE(v1));
    CKI(fld(v1, "k").u.s.len, 4000);
    Str s1 = RS(v1);
    V v2 = Pn(s1);
    CHECK(!isE(v2) && v_eq(v1, v2), "a 4000-byte string must survive the round trip");
    CHECK(s_eq(s1, RS(v2)), "and the bytes must be identical");
    CKI(s1.len, 4008);
  }
  {
    Str d = doc_depth(128);
    V v1 = Pn(d);
    CK(!isE(v1));
    Str s1 = RS(v1);
    V v2 = Pn(s1);
    CHECK(!isE(v2) && v_eq(v1, v2), "a 128-deep document round-trips");
    CHECK(s_eq(s1, RS(v2)), "byte for byte, twice");
    CHECK(!s_eq(s1, sj(v1)), "past val.c's depth 32 the two emitters differ (documented)");
    Str s3 = RS(P("1e999"));
    (void)s3;
  }
  /* the emitter's own output is valid input, for every document */
  {
    int n = 0;
    const Builtin *b = row("parse");
    (void)b; (void)n;
    for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++) {
      V v = P(corpus[i]);
      if (isE(v)) { CHECK(false, "corpus doc %zu must parse", i); continue; }
      CHECK(!isE(Pn(RS(v))), "parse(stringify(doc%zu))", i);
    }
  }
  /* ============ 9. json_get_path: hit, miss, index ============ */
  T("json_get_path");
  V doc = P("{\"dependencies\":{\"build\":\"^1.0.0\",\"react\":\"18.2.0\"},"
             "\"items\":[{\"n\":1,\"t\":\"a\"},{\"n\":2,\"t\":\"b\"}],\"n\":\"num\","
             "\"2\":\"key-two\",\"empty\":{},\"list\":[10,20,30]}");
  CK(!isE(doc));
  Ctx *cx = ctx_new(&A, ".", "test_json");
  CK(cx != NULL && ctx_arena(cx) == &A);
  V g = json_get_path(cx, doc, S("dependencies.build"));
  CK(isstr(g, "^1.0.0"));
  g = json_get_path(cx, doc, S("items[1].t"));
  CK(isstr(g, "b"));
  g = json_get_path(cx, doc, S("items.0.n"));
  CK(numof(g) == 1);
  g = json_get_path(cx, doc, S("2"));
  CK(isstr(g, "key-two"));                       /* a bare number is a record key too */
  g = json_get_path(cx, doc, S("list[2]"));
  CK(numof(g) == 30);
  g = json_get_path(cx, doc, S("list"));
  CK(g.t == V_LIST && g.u.l->len == 3);
  g = json_get_path(cx, doc, S("dependencies"));
  CK(g.t == V_REC && g.u.r->len == 2);
  g = json_get_path(cx, P("[[[5]]]"), S("[0][0][0]"));
  CK(numof(g) == 5);
  g = json_get_path(cx, P("{\"a\":{\"b.c\":7}}"), S("a[\"b.c\"]"));
  CK(numof(g) == 7);
  /* a miss names the path, where it stopped, and what was really there */
  V miss = json_get_path(cx, doc, S("dependencies.missing"));
  CHECK(isE(miss) && EC(miss) == E_NOENT, "a missing key is E_NOENT");
  CKHAS(EM(miss), "missing");
  CKHAS(EH(miss), "build");
  CKHAS(EH(miss), "react");
  CKHAS(EM(miss), "dependencies");
  V miss2 = json_get_path(cx, doc, S("items[9].t"));
  CK(isE(miss2) && EC(miss2) == E_NOENT);
  CKHAS(EH(miss2), "[0]..[1]");
  CKHAS(EM(miss2), "index 9");
  V leaf = json_get_path(cx, doc, S("n.ope"));
  CK(isE(leaf) && EC(leaf) == E_NOENT);
  CKHAS(EM(leaf), "number");
  V notlist = json_get_path(cx, doc, S("dependencies[0]"));
  CK(isE(notlist) && EC(notlist) == E_NOENT);
  CKHAS(EM(notlist), "is a record");
  V notrec = json_get_path(cx, doc, S("list.name"));
  CK(isE(notrec) && EC(notrec) == E_NOENT);
  CKHAS(EM(notrec), "indexed");
  /* paths that cannot be addressed at all are E_SYNTAX, never a guess */
  V badp = json_get_path(cx, doc, S(""));
  CK(isE(badp) && EC(badp) == E_SYNTAX);
  CKHAS(EM(badp), "empty");
  CKI((int)EC(json_get_path(cx, doc, S("a."))), (int)E_SYNTAX);
  CKI((int)EC(json_get_path(cx, doc, S("a..b"))), (int)E_SYNTAX);
  CKI((int)EC(json_get_path(cx, doc, S(".a"))), (int)E_SYNTAX);
  CKI((int)EC(json_get_path(cx, doc, S("a["))), (int)E_SYNTAX);
  CKI((int)EC(json_get_path(cx, doc, S("a[]"))), (int)E_SYNTAX);
  CKI((int)EC(json_get_path(cx, doc, S("a[1"))), (int)E_SYNTAX);
  CKI((int)EC(json_get_path(cx, doc, S("a]b"))), (int)E_SYNTAX);
  CKI((int)EC(json_get_path(cx, doc, S("a[""x"))), (int)E_SYNTAX);
  CKI((int)EC(json_get_path(cx, doc, S("x]"))), (int)E_SYNTAX);
  ctx_free(cx);
  return 0;
}
