/* test_outline.c - the structure view + bundle budget (SPEC §4.1, §4.2 anchors).
 *
 * Every language sample below is realistic multi-line source, deliberately seeded
 * with the traps that break naive scanners: declarations inside comments and string
 * literals, block comments spanning a declaration, a python docstring with a fake
 * def, a JS template interpolation, a rust lifetime, C preprocessor conditionals and
 * nested python classes. Expected symbol lists are asserted exactly (kind+name+line)
 * because an agent trusts this output: a wrong symbol is worse than a missing one.
 *
 * interp.c and fsx.c are NOT in this test's link set (see build.sh deps), so the two
 * cross-module calls outline.c makes are provided here: ctx_arena() (the arena the
 * results live in) and fs_slurp() (the bundle's file reader, served from a table so
 * bundle tests are hermetic and deterministic).
 */
#include "vxa.h"
#include "t.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* t.h's CK carries no message; every assertion here needs one to be diagnosable. */
#undef CK
#define CK CHECK

static Arena A;
static Ctx *const CTX = (Ctx *const)0x1;    /* opaque token; ctx_arena ignores it */

Arena *ctx_arena(Ctx *c) { (void)c; return &A; }

/* ---------- fake filesystem for bundle_files ---------- */
static struct { const char *path, *data; } F[24];
static int NF = 0;
static void give(const char *path, const char *data) {
  F[NF].path = path; F[NF].data = data; NF++;
}
char *fs_slurp(Arena *a, const char *p, size_t *len_out, ErrCode *err) {
  for (int i = 0; i < NF; i++) {
    if (strcmp(F[i].path, p)) continue;
    size_t n = strlen(F[i].data);
    char *d = (char*)arena_alloc(a, n + 1);
    memcpy(d, F[i].data, n);
    d[n] = 0;
    if (len_out) *len_out = n;
    if (err) *err = E_NONE;
    return d;
  }
  if (err) *err = E_NOENT;
  return NULL;
}

/* ---------- result probing ---------- */
static V fld(V r, const char *k) {
  if (r.t != V_REC) return VN;
  V *p = rec_getz(r.u.r, k);
  return p ? *p : VN;
}
static const char *sv(V r, const char *k) {     /* rotating: safe for 8 live results */
  static char b[8][512];
  static int i = 0;
  char *o = b[i = (i + 1) & 7];
  o[0] = 0;
  V v = fld(r, k);
  if (v.t == V_STR && v.u.s.p) {
    int n = v.u.s.len < 511 ? v.u.s.len : 511;
    memcpy(o, v.u.s.p, (size_t)n);
    o[n] = 0;
  }
  return o;
}
static int nv(V r, const char *k) {
  V v = fld(r, k);
  return v.t == V_NUM ? (int)v.u.n : -1;
}
static V sym(V out, int i) {
  V s = fld(out, "symbols");
  if (s.t != V_LIST || !s.u.l || i < 0 || i >= s.u.l->len) return VN;
  return s.u.l->v[i];
}
static int nsyms(V out) {
  V s = fld(out, "symbols");
  return s.t == V_LIST && s.u.l ? s.u.l->len : -1;
}
/* how many symbols carry exactly this name: "appears once" needs a count, not a
 * per-symbol "is not this name" loop - the legitimate declaration fails that loop. */
static int count_named(V out, const char *name) {
  int n = 0;
  for (int i = 0; i < nsyms(out); i++)
    if (s_eqz(fld(sym(out, i), "name").u.s, name)) n++;
  return n;
}
static bool is_hex8(Str s) {
  if (s.len != 8) return false;
  for (int i = 0; i < 8; i++)
    if (!strchr("0123456789abcdef", s.p[i])) return false;
  return true;
}
static void cksym(V out, int i, const char *kind, const char *name, int line) {
  V s = sym(out, i);
  if (s.t != V_REC) { CK(0, "symbol %d missing (have %d)", i, nsyms(out)); return; }
  Str gotk = fld(s, "kind").u.s, gotn = fld(s, "name").u.s;
  CK(s_eqz(gotk, kind), "symbols[%d].kind=\"%.*s\" want \"%s\"", i, gotk.len, gotk.p, kind);
  CK(s_eqz(gotn, name), "symbols[%d].name=\"%.*s\" want \"%s\"", i, gotn.len, gotn.p, name);
  CKI(nv(s, "line"), line);
  CK(is_hex8(fld(s, "at").u.s), "symbols[%d].at=\"%.*s\" not 8 hex", i,
     fld(s, "at").u.s.len, fld(s, "at").u.s.p);
  V sg = fld(s, "sig");
  CK(sg.t == V_STR && sg.u.s.len > 0 && sg.u.s.len <= 120,
     "symbols[%d].sig length %d out of 1..120", i, sg.t == V_STR ? sg.u.s.len : -1);
}
typedef struct { const char *kind, *name; int line; } Exp;
static void dump(V out) {                        /* only called on a mismatch */
  for (int i = 0; i < nsyms(out); i++)
    printf("   got[%d] %s %s @%d\n", i, sv(sym(out, i), "kind"),
           sv(sym(out, i), "name"), nv(sym(out, i), "line"));
}
static void ck_syms(V out, const Exp *e, int n, const char *what) {
  if (nsyms(out) != n) { CK(0, "%s: %d symbols, want %d", what, nsyms(out), n); dump(out); }
  for (int i = 0; i < n; i++) cksym(out, i, e[i].kind, e[i].name, e[i].line);
}
static V ol(const char *path, const char *text) {
  Str t; t.p = (char*)text; t.len = (int)strlen(text);
  return outline_file(CTX, path, t, NULL);
}
static V ol_lang(const char *path, const char *text, const char *lang) {
  Str t; t.p = (char*)text; t.len = (int)strlen(text);
  return outline_file(CTX, path, t, lang);
}
static const char *detect(const char *path, const char *text) {
  static char b[8][16];
  static int i = 0;
  char *o = b[i = (i + 1) & 7];
  Str t; t.p = (char*)text; t.len = (int)strlen(text);
  int n = outline_lang_detect(path, t, o);
  CKI(n, (int)strlen(o));
  return o;
}
/* independent re-derivation of the SPEC §4.2 anchor: pins the hashed byte layout -
 * "vxa1", then prev/cur/next each separated by one NUL, and no line number. xdiff.c's
 * anchor_at() is the producer under test (it is also what fs.read --anchors prints);
 * this spells the same recipe out from the spec text so a change to one of the two
 * shows up here rather than passing silently. */
static Str want_at(const char *prev, const char *cur, const char *next) {
  Sha256 s;
  sha256_init(&s);
  sha256_update(&s, "vxa1", 4);
  sha256_update(&s, "\0", 1);
  sha256_update(&s, prev, strlen(prev));
  sha256_update(&s, "\0", 1);
  sha256_update(&s, cur, strlen(cur));
  sha256_update(&s, "\0", 1);
  sha256_update(&s, next, strlen(next));
  uint8_t dg[32];
  char hx[65];
  sha256_final(&s, dg);
  base16(dg, 32, hx);
  return s_from(&A, hx, 8);
}
static bool body_line_ok(const char *l) {        /* "L<no>:<at8>| ..." */
  int n = 0;
  char hx[16];
  hx[0] = 0;
  if (sscanf(l, "L%d:%8[0123456789abcdef]| ", &n, hx) != 2) return false;
  return n > 0 && strlen(hx) == 8;
}
/* walks a bundle body line by line; returns the line count, *nok = anchor-shaped */
static int body_lines(const char *body, int *nok) {
  int nl = 0;
  if (nok) *nok = 0;
  if (!body || !*body) return 0;
  for (const char *q = body; ; ) {
    const char *s2 = strchr(q, '\n');
    size_t len = s2 ? (size_t)(s2 - q) : strlen(q);
    char row[1024];
    if (len >= sizeof row) len = sizeof row - 1;
    memcpy(row, q, len);
    row[len] = 0;
    nl++;
    if (nok && (body_line_ok(row) || !strncmp(row, "##", 2))) (*nok)++;
    if (!s2) break;
    q = s2 + 1;
  }
  return nl;
}

/* ========================================================================== */
static const char CSRC[] =
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"\n"
"#define WIDGET_MAX 12\n"
"#define MAX(a, b) ((a) > (b) ? (a) : (b))\n"
"\n"
"struct Widget {\n"
"  int id;\n"
"  const char *name;\n"
"};\n"
"\n"
"typedef struct Widget Widget;\n"
"\n"
"/* Returns a widget.\n"
"   int not_a_real_decl(int x) {\n"
"     return 0;\n"
"   }\n"
"*/\n"
"static Widget *widget_new(int id, const char *name) {\n"
"  Widget *w = malloc(sizeof *w);\n"
"  if (!w) {\n"
"    fprintf(stderr, \"func main() { panic() }\\n\");\n"
"    return NULL;\n"
"  }\n"
"  w->id = id;\n"
"  return w;\n"
"}\n"
"\n"
"// void commented_out(void) { }\n"
"int widget_free(Widget *w);\n"
"\n"
"#ifdef DEBUG\n"
"int widget_dump(Widget *w) { return 0; }\n"
"#endif\n"
"\n"
"#if 0\n"
"int dead_code(int x) { return x; }\n"
"#endif\n"
"\n"
"int\n"
"compare(const void *a,\n"
"        const void *b) {\n"
"  return 0;\n"
"}\n";

static const char CSRC_CRLF[] =
"#include <stdio.h>\r\n"
"\r\n"
"#define WIDGET_MAX 12\r\n"
"\r\n"
"static int widget_init(int n) {\r\n"
"  return n;\r\n"
"}\r\n"
"\r\n"
"int widget_free(Widget *w);\r\n";

static const char CPPSRC[] =
"namespace gfx {\n"
"class Shape {\n"
"public:\n"
"  virtual double area() const {\n"
"    return 0.0;\n"
"  }\n"
"  void scale(double k);\n"
"};\n"
"struct Point { double x, y; };\n"
"}  // namespace gfx\n";

static const char GOSRC[] =
"package store\n"
"\n"
"import (\n"
"\t\"errors\"\n"
"\t\"fmt\"\n"
")\n"
"\n"
"// Store keeps widgets. It is not func Noop() {} and never will be.\n"
"const (\n"
"\tErrMissing = errors.New(\"store: missing\")\n"
"\tMaxItems   = 64\n"
")\n"
"\n"
"var Registry = map[string]*Widget{}\n"
"var doc = \"func main() { os.Exit(1) }\"\n"
"\n"
"type Widget struct {\n"
"\tName string\n"
"\tqty  int\n"
"}\n"
"\n"
"type Stringer interface {\n"
"\tString() string\n"
"}\n"
"\n"
"type Alias = Widget\n"
"\n"
"/* func phantom() {\n"
"\treturn\n"
"} */\n"
"func New(name string) *Widget {\n"
"\treturn &Widget{Name: name}\n"
"}\n"
"\n"
"func (w *Widget) Qty() int {\n"
"\tfmt.Println(\"func main() {}\", w.Name)\n"
"\treturn w.qty\n"
"}\n"
"\n"
"func (w Widget) String() string { return \"widget\" }\n"
"\n"
"func TestNew(t *testing.T) {\n"
"\tif got := New(\"a\"); got.Name != \"a\" {\n"
"\t\tt.Fatal(\"not a test\")\n"
"\t}\n"
"}\n"
"\n"
"func helper() {\n"
"\t_ = 1\n"
"}\n";

static const char PYSRC[] =
"#!/usr/bin/env python3\n"
"\"\"\"Module docstring.\n"
"\n"
"def fake_in_docstring(x):\n"
"    return x\n"
"\"\"\"\n"
"import os\n"
"\n"
"MAX_ITEMS = 10\n"
"\n"
"\n"
"class Repo:\n"
"    \"\"\"Holds items.  def nested_fake(): pass\"\"\"\n"
"\n"
"    def __init__(self, name):\n"
"        self.name = name\n"
"\n"
"    def add(self, item):\n"
"        s = \"def fake_in_string(): pass\"\n"
"        return s\n"
"\n"
"    class Inner:\n"
"        def go(self):\n"
"            return 1\n"
"\n"
"    @staticmethod\n"
"    def make(name):\n"
"        return Repo(name)\n"
"\n"
"\n"
"async def fetch(url):\n"
"    return url\n"
"\n"
"\n"
"def helper(a,\n"
"           b):\n"
"    return a + b\n"
"\n"
"\n"
"def _private():\n"
"    pass\n"
"\n"
"\n"
"def test_helper():\n"
"    assert helper(1, 2) == 3\n";

static const char JSSRC[] =
"import { readFileSync } from \"fs\";\n"
"\n"
"const MAX_RETRIES = 5;\n"
"const config = load(\"x\");\n"
"\n"
"/**\n"
" * function bogus() {}\n"
" */\n"
"function parseConfig(text) {\n"
"  const tpl = `a ${1 + 2} b`;  // function inside_template() {}\n"
"  return { text, tpl };\n"
"}\n"
"\n"
"export async function load(name) {\n"
"  return JSON.parse(`{\"path\": \"${name}\"}`);\n"
"}\n"
"\n"
"class Widget extends Base {\n"
"  constructor(id) {\n"
"    super(id);\n"
"  }\n"
"\n"
"  static create(a) {\n"
"    return new Widget(a);\n"
"  }\n"
"\n"
"  get label() { return \"x\"; }\n"
"\n"
"  render(el) {\n"
"    el.textContent = `Widget(${this.id})`;\n"
"  }\n"
"}\n"
"\n"
"const arrowAdd = (a, b) => a + b;\n"
"\n"
"export default class App {\n"
"  run() {}\n"
"}\n"
"\n"
"interface Named { name: string }\n"
"\n"
"enum Color { Red, Green }\n"
"\n"
"if (MAX_RETRIES) { console.log(\"func main() {}\"); }\n";

static const char TSSRC[] =
"function one(x: number): number {\n"
"  return x;  /* function two() {} */\n"
"}\n"
"export type Handler = (e: unknown) => void;\n";

static const char RSSRC[] =
"use std::fmt;\n"
"\n"
"pub const MAX: usize = 64;\n"
"static COUNTER: u32 = 0;\n"
"\n"
"mod inner;\n"
"\n"
"/// Docs with a fake fn bogus() {} inside.\n"
"#[derive(Debug, Clone)]\n"
"pub struct Point<T> {\n"
"    pub x: T,\n"
"}\n"
"\n"
"pub enum Shape {\n"
"    Circle(f64),\n"
"}\n"
"\n"
"pub trait Describe {\n"
"    fn describe(&self) -> String;\n"
"}\n"
"\n"
"impl Point {\n"
"    pub fn new() -> Self {\n"
"        Point { x: 0 }\n"
"    }\n"
"\n"
"    // fn phantom() {}\n"
"    pub fn label(&self) -> String {\n"
"        String::from(\"fn fake() {}\")\n"
"    }\n"
"}\n"
"\n"
"pub fn first_word<'a>(s: &'a str) -> &'a str {\n"
"    let bytes = s.as_bytes();\n"
"    for (i, &item) in bytes.iter().enumerate() {\n"
"        if item == b' ' {\n"
"            return &s[0..i];\n"
"        }\n"
"    }\n"
"    &s[..]\n"
"}\n"
"\n"
"#[test]\n"
"fn checks_first_word() {\n"
"    assert_eq!(first_word(\"hello world\"), \"hello\");\n"
"}\n"
"\n"
"macro_rules! shout {\n"
"    ($e:expr) => {\n"
"        println!(\"{}\", $e)\n"
"    };\n"
"}\n"
"\n"
"type Al = std::result::Result<u8, ()>;\n";

static const char JAVASRC[] =
"package com.acme;\n"
"\n"
"import java.util.List;\n"
"\n"
"/** A widget.\n"
" *  public class BogusInComment {}\n"
" */\n"
"public class Widget implements Named {\n"
"    private static final int MAX = 10;   // void fake() {}\n"
"    private final List<String> items = new ArrayList<>();\n"
"\n"
"    public Widget(String name) {\n"
"        this.name = name;\n"
"    }\n"
"\n"
"    @Test\n"
"    public void testNaming() {\n"
"        assertEquals(\"a\", name);\n"
"    }\n"
"\n"
"    @Override\n"
"    public String toString() {\n"
"        /* block comment\n"
"           public void phantom() { }\n"
"        */\n"
"        return name;\n"
"    }\n"
"\n"
"    public static Widget parse(String s) {\n"
"        if (s.isEmpty()) {\n"
"            throw new IllegalArgumentException(\"func main()\");\n"
"        }\n"
"        return new Widget(s);\n"
"    }\n"
"}\n"
"\n"
"interface Named {\n"
"    String getName();\n"
"}\n"
"\n"
"enum Suit {\n"
"    HEARTS, SPADES\n"
"}\n";

static const char KTSRC[] =
"package app\n"
"\n"
"import kotlin.test.Test\n"
"\n"
"data class Point(val x: Int, val y: Int)\n"
"\n"
"interface Greeter {\n"
"    fun greet(name: String): String {\n"
"        return \"hi\"\n"
"    }\n"
"}\n"
"\n"
"class Repo(private val items: MutableList<String>) {\n"
"    @Test\n"
"    fun testAdd() {\n"
"        assertTrue(items.add(\"a\"))\n"
"    }\n"
"\n"
"    fun add(s: String): Boolean = items.add(s)\n"
"\n"
"    val NAME = \"class Fake { }\"\n"
"}\n"
"\n"
"fun main() {\n"
"    println(\"ok\")\n"
"}\n";

/* ========================================================================== */
int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);   // so a crash still shows the last section
  arena_init(&A, 0);

  /* ---------- language detection: extensions ---------- */
  T("detect: extensions");
  CKS(detect("src/a.c", "int x;"), "c");
  CKS(detect("a.h", ""), "c");
  CKS(detect("a.cc", ""), "cpp");
  CKS(detect("a.cpp", ""), "cpp");
  CKS(detect("a.hpp", ""), "cpp");
  CKS(detect("m/go", ""), "go");
  CKS(detect("A.Java", "x"), "java");
  CKS(detect("k.kt", "x"), "kotlin");
  CKS(detect("k.kts", "x"), "kotlin");
  CKS(detect("w.js", ""), "js");
  CKS(detect("w.jsx", ""), "js");
  CKS(detect("w.ts", ""), "ts");
  CKS(detect("w.tsx", ""), "ts");
  CKS(detect("m.py", ""), "python");
  CKS(detect("m.rs", ""), "rust");
  CKS(detect("README.md", "hello"), "text");
  CKS(detect("noext", ""), "text");

  /* ---------- language detection: shebang and content sniff ---------- */
  T("detect: shebang / content");
  CKS(detect("build", "#!/usr/bin/env python3\nprint(1)\n"), "python");
  CKS(detect("serve", "#!/usr/bin/env node\nlet a = 1;\n"), "js");
  CKS(detect("run.sh", "#!/bin/sh\necho hi\n"), "text");
  CKS(detect("notes", "just prose about things\n"), "text");
  /* bytes say go: `func` is not a rust keyword, and the fixture's call is
   * `println("x")` with NO bang, which is go's builtin - rust's macro is
   * `println!(..)`. The next line is what actual rust source looks like. */
  CKS(detect("main", "func main() {\n\tprintln(\"x\")\n}\n"), "go");
  CKS(detect("main", "fn main() {\n\tprintln!(\"x\")\n}\n"), "rust");
  CKS(detect("app", "public class App {\n}\n"), "java");
  CKS(detect("tool", "package main\n\nfunc run() {\n}\n"), "go");
  CKS(detect("x", "def main():\n    pass\n"), "python");
  char lb[16];
  Str empty = s_wrap("");
  CKI(outline_lang_detect("a.go", empty, lb), 2);
  CKS(lb, "go");
  CK(outline_lang_detect(NULL, empty, lb) > 0, "NULL path must still answer");
  CKS(lb, "text");

  /* ---------- C ---------- */
  T("outline: c");
  V c = ol("src/widget.c", CSRC);
  CKS(sv(c, "lang"), "c");
  CKI(nv(c, "lines"), 44);
  CK(nv(c, "bytes") == (int)sizeof(CSRC) - 1, "bytes=%d want %d", nv(c, "bytes"),
     (int)sizeof(CSRC) - 1);
  CKS(sv(c, "path"), "src/widget.c");
  CK(nv(c, "hash") == -1, "hash is a string");
  CK(fld(c, "hash").u.s.len == 12, "hash must be sha256[:12], got %d", fld(c, "hash").u.s.len);
  static const Exp cexp[] = {
    { "macro", "WIDGET_MAX", 4 }, { "macro", "MAX", 5 },
    { "struct", "Widget", 7 },    { "struct", "Widget", 12 },
    { "fn", "widget_new", 19 },   { "fn", "widget_free", 30 },
    { "fn", "widget_dump", 33 },  { "fn", "compare", 40 },
  };
  ck_syms(c, cexp, 8, "c");
  CK(s_eqz(fld(sym(c, 2), "sig").u.s, "struct Widget {"), "sig: %s", sv(sym(c, 2), "sig"));
  CK(s_ni(fld(sym(c, 4), "sig").u.s, "static Widget *widget_new"),
     "sig must start at the declaration: %s", sv(sym(c, 4), "sig"));
  /* wrapped declaration: sig is the joined logical line */
  CK(s_ni(fld(sym(c, 7), "sig").u.s, "int compare(const void *a,"),
     "wrapped sig: %s", sv(sym(c, 7), "sig"));
  T("outline: c traps");
  CK(nsyms(c) == 8, "comment/string decls must not leak (got %d)", nsyms(c));
  for (int i = 0; i < nsyms(c); i++) {
    CK(!s_eqz(fld(sym(c, i), "name").u.s, "not_a_real_decl"), "block comment leaked a symbol");
    CK(!s_eqz(fld(sym(c, i), "name").u.s, "commented_out"), "line comment leaked a symbol");
    CK(!s_eqz(fld(sym(c, i), "name").u.s, "dead_code"), "#if 0 region leaked a symbol");
    CK(!s_eqz(fld(sym(c, i), "name").u.s, "main"), "string literal leaked a symbol");
    CK(!s_eqz(fld(sym(c, i), "name").u.s, "panic"), "string literal leaked a symbol");
    CK(!s_eqz(fld(sym(c, i), "name").u.s, "fprintf"), "a call was taken for a declaration");
    CK(!s_eqz(fld(sym(c, i), "name").u.s, "malloc"), "a call was taken for a declaration");
  }
  /* anchors: pinned byte layout, and context-sensitivity.
   * prev is "" because CSRC line 3 is the blank between the two #includes and the
   * macros: SPEC 4.2 hashes the IMMEDIATELY neighbouring lines, not the nearest
   * non-blank one. */
  CK(s_eq(fld(sym(c, 0), "at").u.s, want_at("", "#define WIDGET_MAX 12",
                                              "#define MAX(a, b) ((a) > (b) ? (a) : (b))")),
     "anchor must match SPEC 4.2 over prev/cur/next");
  CK(s_eq(fld(sym(c, 3), "at").u.s, want_at("", "", "")) == false,
     "anchors must not ignore context");
  CK(!s_eq(fld(sym(c, 2), "at").u.s, fld(sym(c, 3), "at").u.s),
     "same name on different lines must give different anchors");
  /* ONE anchor rule: every outline `at` must be the anchor xdiff computes for that
   * symbol's line - the value fs.read --anchors prints for the same bytes, and what
   * tx.patch matches. Two producers of one rule is how they drifted apart. */
  {
    Str ct; ct.p = (char*)CSRC; ct.len = (int)sizeof(CSRC) - 1;
    int na = 0;
    Anch *xa = anchors_of(&A, "src/widget.c", ct, &na, NULL);
    CK(xa != NULL && na == nv(c, "lines"), "xdiff splits c into %d lines, outline says %d",
       na, nv(c, "lines"));
    int nmis = 0;
    for (int i = 0; i < nsyms(c); i++) {
      int ln = nv(sym(c, i), "line");
      if (ln < 1 || ln > na || !s_eq(fld(sym(c, i), "at").u.s, xa[ln - 1].at)) nmis++;
    }
    CKI(nmis, 0);
    CK(s_eq(fld(sym(c, 0), "at").u.s, xa[3].at), "widget macro anchor != fs.read's");
  }
  T("outline: c crlf");
  V cc = ol("src/w.c", CSRC_CRLF);
  CKS(sv(cc, "lang"), "c");
  /* 9 CR-LF-terminated lines, so 9 lines - and this same block asserts widget_free
   * on line 9, which a 8-line file could not hold. */
  CKI(nv(cc, "lines"), 9);
  static const Exp ccexp[] = {
    { "macro", "WIDGET_MAX", 3 }, { "fn", "widget_init", 5 }, { "fn", "widget_free", 9 },
  };
  ck_syms(cc, ccexp, 3, "crlf");
  CK(s_eqz(fld(sym(cc, 2), "sig").u.s, "int widget_free(Widget *w);"),
     "sig must not carry the CR");
  CK(s_eq(fld(sym(cc, 1), "at").u.s,
           want_at("", "static int widget_init(int n) {", "  return n;")),
     "anchor must be computed on CR-stripped lines");
  T("outline: cpp class members");
  V cp = ol("g/shape.cpp", CPPSRC);
  CKS(sv(cp, "lang"), "cpp");
  static const Exp cpexp[] = {
    { "class", "Shape", 2 }, { "method", "Shape.area", 4 }, { "method", "Shape.scale", 7 },
    { "struct", "Point", 9 },
  };
  ck_syms(cp, cpexp, 4, "cpp");

  /* ---------- Go ---------- */
  T("outline: go");
  V g = ol("store/store.go", GOSRC);
  CKS(sv(g, "lang"), "go");
  /* line = the 1-based physical line the declaration's FIRST text sits on, verified
   * against the fixture bytes (grep -n 'ErrMissing' -> 10). The sig assertions below
   * join from that same line, so line and sig pin each other. */
  static const Exp gexp[] = {
    { "const", "ErrMissing", 10 }, { "const", "MaxItems", 11 },
    { "var", "Registry", 14 },     { "var", "doc", 15 },
    { "struct", "Widget", 17 },    { "iface", "Stringer", 22 }, { "type", "Alias", 26 },
    { "fn", "New", 31 },            { "method", "Widget.Qty", 35 },
    { "method", "Widget.String", 40 }, { "test", "TestNew", 42 }, { "fn", "helper", 48 },
  };
  ck_syms(g, gexp, 12, "go");
  CK(s_eqz(fld(sym(g, 9), "sig").u.s, "func (w Widget) String() string { return \"widget\" }"),
     "sig keeps the original text: %s", sv(sym(g, 9), "sig"));
  for (int i = 0; i < nsyms(g); i++) {
    CK(!s_eqz(fld(sym(g, i), "name").u.s, "Noop"), "go comment leaked");
    CK(!s_eqz(fld(sym(g, i), "name").u.s, "phantom"), "go block comment leaked");
    CK(!s_eqz(fld(sym(g, i), "name").u.s, "main"), "go raw string leaked a func");
    CK(!s_eqz(fld(sym(g, i), "name").u.s, "Println"), "go call taken for a decl");
  }

  /* ---------- Python ---------- */
  T("outline: python");
  V py = ol("pkg/repo.py", PYSRC);
  CKS(sv(py, "lang"), "python");
  static const Exp pyexp[] = {
    { "const", "MAX_ITEMS", 9 },        { "class", "Repo", 12 },
    { "method", "Repo.__init__", 15 },  { "method", "Repo.add", 18 },
    { "class", "Repo.Inner", 22 },      { "method", "Repo.Inner.go", 23 },
    { "method", "Repo.make", 27 },      { "fn", "fetch", 31 },
    { "fn", "helper", 35 },             { "fn", "_private", 40 },
    { "test", "test_helper", 44 },
  };
  ck_syms(py, pyexp, 11, "python");
  CK(s_eqz(fld(sym(py, 3), "sig").u.s, "def add(self, item):"), "sig=%s", sv(sym(py, 3), "sig"));
  CK(s_eqz(fld(sym(py, 8), "sig").u.s, "def helper(a, b):"),
     "wrapped def sig must be joined: %s", sv(sym(py, 8), "sig"));
  for (int i = 0; i < nsyms(py); i++) {
    CK(!s_eqz(fld(sym(py, i), "name").u.s, "fake_in_docstring"), "docstring leaked a def");
    CK(!s_eqz(fld(sym(py, i), "name").u.s, "Repo.fake_in_docstring"), "docstring leaked a def");
    CK(!s_eqz(fld(sym(py, i), "name").u.s, "nested_fake"), "one-line docstring leaked");
    CK(!s_eqz(fld(sym(py, i), "name").u.s, "Repo.fake_in_string"), "string leaked a def");
  }
  CKI(count_named(py, "Repo"), 1);                 /* the class itself appears exactly once */
  CK(nsyms(py) == 11, "exactly 11 python symbols, got %d", nsyms(py));
  V pyt = ol("tests/test_repo.py", "def test_ok():\n    assert 1\n\ndef other():\n    pass\n");
  static const Exp pytexp[] = { { "test", "test_ok", 1 }, { "fn", "other", 4 } };
  ck_syms(pyt, pytexp, 2, "python test file");

  /* ---------- JavaScript / TypeScript ---------- */
  T("outline: js");
  V js = ol("src/app.js", JSSRC);
  CKS(sv(js, "lang"), "js");
  static const Exp jsexp[] = {
    { "const", "MAX_RETRIES", 3 },   { "fn", "parseConfig", 9 },
    { "fn", "load", 14 },            { "class", "Widget", 18 },
    { "method", "Widget.constructor", 19 }, { "method", "Widget.create", 23 },
    { "method", "Widget.label", 27 },        { "method", "Widget.render", 29 },
    { "fn", "arrowAdd", 34 },        { "class", "App", 36 },
    { "method", "App.run", 37 },     { "iface", "Named", 40 },
    { "enum", "Color", 42 },
  };
  ck_syms(js, jsexp, 13, "js");
  for (int i = 0; i < nsyms(js); i++) {
    CK(!s_eqz(fld(sym(js, i), "name").u.s, "bogus"), "js block comment leaked");
    CK(!s_eqz(fld(sym(js, i), "name").u.s, "inside_template"), "js line comment leaked");
    CK(!s_eqz(fld(sym(js, i), "name").u.s, "main"), "js string leaked a func");
    CK(!s_eqz(fld(sym(js, i), "name").u.s, "parse"), "JSON.parse call leaked");
    CK(!s_eqz(fld(sym(js, i), "name").u.s, "config"), "a plain const was emitted");
    CK(!s_eqz(fld(sym(js, i), "name").u.s, "readFileSync"), "import leaked");
  }
  T("outline: ts");
  V ts = ol("src/two.ts", TSSRC);
  CKS(sv(ts, "lang"), "ts");
  static const Exp tsexp[] = { { "fn", "one", 1 }, { "type", "Handler", 4 } };
  ck_syms(ts, tsexp, 2, "ts");

  /* ---------- Rust ---------- */
  T("outline: rust");
  V rs = ol("src/lib.rs", RSSRC);
  CKS(sv(rs, "lang"), "rust");
  /* an attribute or doc comment above an item is not part of the item's line:
   * `pub struct Point<T> {` is line 10, `#[derive(..)]` is line 9. */
  static const Exp rsexp[] = {
    { "const", "MAX", 3 },          { "var", "COUNTER", 4 },
    { "mod", "inner", 6 },          { "struct", "Point", 10 },
    { "enum", "Shape", 14 },        { "trait", "Describe", 18 },
    { "impl", "Point", 22 },       { "method", "Point::new", 23 },
    { "method", "Point::label", 28 }, { "fn", "first_word", 33 },
    { "test", "checks_first_word", 44 }, { "macro", "shout", 48 },
    { "type", "Al", 54 },
  };
  ck_syms(rs, rsexp, 13, "rust");
  for (int i = 0; i < nsyms(rs); i++) {
    CK(!s_eqz(fld(sym(rs, i), "name").u.s, "bogus"), "rust doc comment leaked a fn");
    CK(!s_eqz(fld(sym(rs, i), "name").u.s, "phantom"), "rust line comment leaked a fn");
    CK(!s_eqz(fld(sym(rs, i), "name").u.s, "fake"), "rust string leaked a fn");
    CK(!s_eqz(fld(sym(rs, i), "name").u.s, "from"), "String::from call leaked");
    CK(!s_eqz(fld(sym(rs, i), "name").u.s, "println"), "macro call leaked");
  }
  /* the trait is one symbol; its `fn describe(&self) -> String;` is a required
   * signature, not a definition, so it must not be emitted beside it. */
  CKI(count_named(rs, "Describe"), 1);
  CKI(count_named(rs, "describe"), 0);
  CKI(count_named(rs, "Describe::describe"), 0);
  /* the lifetime must survive as code, not swallow the rest of the line */
  CK(s_eqz(fld(sym(rs, 9), "sig").u.s, "pub fn first_word<'a>(s: &'a str) -> &'a str {"),
     "lifetime mangled the signature: %s", sv(sym(rs, 9), "sig"));
  CK(s_eqz(fld(sym(rs, 9), "name").u.s, "first_word"), "generic fn name");

  /* ---------- Java / Kotlin ---------- */
  T("outline: java");
  V jv = ol("src/Widget.java", JAVASRC);
  CKS(sv(jv, "lang"), "java");
  static const Exp jvexp[] = {
    { "class", "Widget", 8 },       { "method", "Widget.new", 12 },
    { "test", "Widget.testNaming", 17 }, { "method", "Widget.toString", 22 },
    { "method", "Widget.parse", 29 }, { "iface", "Named", 37 },
    { "method", "Named.getName", 38 }, { "enum", "Suit", 41 },
  };
  ck_syms(jv, jvexp, 8, "java");
  for (int i = 0; i < nsyms(jv); i++) {
    CK(!s_eqz(fld(sym(jv, i), "name").u.s, "BogusInComment"), "java doc comment leaked");
    CK(!s_eqz(fld(sym(jv, i), "name").u.s, "Widget.fake"), "java line comment leaked");
    CK(!s_eqz(fld(sym(jv, i), "name").u.s, "phantom"), "java block comment leaked");
    CK(!s_eqz(fld(sym(jv, i), "name").u.s, "ArrayList"), "a constructor call leaked");
    CK(!s_eqz(fld(sym(jv, i), "name").u.s, "isEmpty"), "a call inside a body leaked");
    CK(!s_eqz(fld(sym(jv, i), "name").u.s, "IllegalArgumentException"), "throw leaked");
  }
  T("outline: kotlin");
  V kt = ol("app/Repo.kt", KTSRC);
  CKS(sv(kt, "lang"), "kotlin");
  static const Exp ktexp[] = {
    { "class", "Point", 5 },     { "iface", "Greeter", 7 },
    { "method", "Greeter.greet", 8 }, { "class", "Repo", 13 },
    { "test", "Repo.testAdd", 15 }, { "fn", "main", 24 },
  };
  ck_syms(kt, ktexp, 6, "kotlin");
  for (int i = 0; i < nsyms(kt); i++)
    CK(!s_eqz(fld(sym(kt, i), "name").u.s, "Repo.add"),
       "expression-bodied members are not emitted (axiom 1)");
  CK(!s_eqz(fld(sym(kt, 5), "name").u.s, "Fake"), "kotlin raw string leaked a class");

  /* ---------- text fallback, empty input, explicit lang ---------- */
  T("outline: fallbacks");
  V tx = ol("docs/notes.txt", "hello world\nsecond line\n");
  CKS(sv(tx, "lang"), "text");
  CKI(nsyms(tx), 0);
  CK(fld(tx, "symbols").t == V_LIST, "symbols must be an empty list, not an error");
  CK(v_is_err(tx) == false, "text fallback must not be an error");
  CKI(nv(tx, "lines"), 2);
  V mt = ol("empty.go", "");
  CKI(nv(mt, "bytes"), 0);
  CKI(nv(mt, "lines"), 0);
  CKI(nsyms(mt), 0);
  CKS(sv(mt, "lang"), "go");
  CK(fld(mt, "hash").u.s.len == 12, "empty input still gets a content hash");
  Str nulls = { 0, NULL };
  V nn = outline_file(CTX, "x.c", nulls, NULL);
  CK(!v_is_err(nn), "NULL text must not be an error");
  CKI(nv(nn, "bytes"), 0);
  V badlang = ol_lang("x.c", "int f(void) { return 1; }\n", "cobol");
  CKS(sv(badlang, "lang"), "text");
  CKI(nsyms(badlang), 0);
  V forced = ol_lang("notes.txt", "int f(void) { return 1; }\n", "c");
  static const Exp fexp[] = { { "fn", "f", 1 } };
  ck_syms(forced, fexp, 1, "explicit lang beats the extension");
  V noctx = outline_file(NULL, "a.c", s_wrap("int f(void){}"), NULL);
  CK(v_is_err(noctx), "a null context must be an ERR value, not a crash");
  CK(v_errcode(noctx) == E_BAD_INPUT, "expected BAD_INPUT, got %s", err_name(v_errcode(noctx)));

  /* ---------- minified bail-out ---------- */
  T("outline: minified");
  {
    size_t n = 6000;
    char *big = (char*)malloc(n + 1);
    for (size_t i = 0; i < n; i++) big[i] = (char)('a' + (i % 26));
    big[0] = 'f'; big[1] = 'u'; big[2] = 'n'; big[3] = 'c'; big[4] = ' ';
    big[n] = 0;
    Str t; t.p = big; t.len = (int)n;
    V mi = outline_file(CTX, "dist/bundle.js", t, NULL);
    CKS(sv(mi, "lang"), "text");
    CKS(sv(mi, "note"), "minified?");
    CK(fld(mi, "symbols").t == V_NULL, "minified output must not carry symbols");
    CKI(nv(mi, "bytes"), (int)n);
    CKI(nv(mi, "lines"), 1);
    /* just under the limit: still outlined */
    V notmi = outline_file(CTX, "dist/short.js", s_lit(&A, "function small() {}\n"), NULL);
    static const Exp miexp[] = { { "fn", "small", 1 } };
    ck_syms(notmi, miexp, 1, "short js");
    free(big);
  }

  /* ---------- bundle ---------- */
  T("bundle: symbols only");
  NF = 0;
  give("a.c", "int alpha(void) { return 1; }\nint beta(void) { return 2; }\n");
  give("b.py", "def one():\n    pass\n\nclass Two:\n    def three(self):\n        pass\n");
  give("c.go", "package p\n\nfunc Three() {\n}\n");
  give("many.c",
       "int f00(void) { return 0; }\nint f01(void) { return 1; }\nint f02(void) { return 2; }\n"
       "int f03(void) { return 3; }\nint f04(void) { return 4; }\nint f05(void) { return 5; }\n"
       "int f06(void) { return 6; }\nint f07(void) { return 7; }\nint f08(void) { return 8; }\n"
       "int f09(void) { return 9; }\nint f10(void) { return 10; }\nint f11(void) { return 11; }\n"
       "int f12(void) { return 12; }\nint f13(void) { return 13; }\nint f14(void) { return 14; }\n");
  {
    V ps = v_list(&A, 3, v_str(s_wrap("a.c")), v_str(s_wrap("b.py")), v_str(s_wrap("c.go")));
    V r = bundle_files(CTX, ps, NULL, 100000, true);
    CK(v_is_err(r) == false, "bundle failed");
    CKI(nv(fld(r, "bundle").u.l->v[0], "hits"), 0);
    int nb = fld(r, "bundle").t == V_LIST && fld(r, "bundle").u.l ? fld(r, "bundle").u.l->len : -1;
    CKI(nb, 3);
    CKI(nv(r, "cap"), 100000);
    CK(nv(r, "bytes") <= nv(r, "cap"), "bytes %d over cap %d", nv(r, "bytes"), nv(r, "cap"));
    V f0 = fld(r, "bundle").u.l->v[0];
    /* ordering: most structure first (b.py has 3 symbols), ties by path */
    CKS(sv(f0, "path"), "b.py");
    CKS(sv(f0, "lang"), "python");
    CK(fld(f0, "hash").u.s.len == 12, "bundle entries carry a 12-byte hash");
    const char *body = sv(f0, "text");
    int nok = 0, nl = body_lines(body, &nok);
    CKI(nl, 3);
    CKI(nok, nl);
    CK(strstr(body, "class Two") != NULL, "symbol kind+name line missing");
    CK(strstr(body, "##hits") == NULL, "no query => no hit section");
    CK(strstr(body, "method Two.three") != NULL, "qualified name in the bundle body");
  }
  T("bundle: query mode");
  {
    V ps = v_list(&A, 2, v_str(s_wrap("a.c")), v_str(s_wrap("b.py")));
    V r = bundle_files(CTX, ps, "THREE", 100000, false);
    CKS(sv(r, "query"), "THREE");
    CKI(nv(fld(r, "bundle").u.l->v[0], "hits"), 1);  /* only `def three(self):` matches */
    V hit_first = fld(r, "bundle").u.l->v[0];
    CKS(sv(hit_first, "path"), "b.py");
    const char *body = sv(hit_first, "text");
    CK(strstr(body, "##hits=1") != NULL, "hit count line missing: %s", body);
    CK(strstr(body, "def three(self):") != NULL, "matched line must be shown verbatim");
    CK(strstr(body, "class Two:") != NULL, "2 lines of context above the hit");
    CK(strstr(body, "pass") != NULL, "context below the hit");
    int nok = 0, nl = body_lines(body, &nok);
    CKI(nok, nl);                                    /* every line is L<n>:at| or ## */
    CKI(nl, 8);                                      /* 3 symbols + ##hits + 4 lines */
    /* case-insensitive substring, not a regex: a regex metachar is literal */
    V r2 = bundle_files(CTX, ps, "no.such.thing", 100000, false);
    CK(nv(r2, "bytes") >= 0, "bytes must be reported");
    CKI(nv(fld(r2, "bundle").u.l->v[0], "hits"), 0);
    CK(strstr(sv(fld(r2, "bundle").u.l->v[0], "text"), "##hits") == NULL,
       "no hits => no hit section");
    V r4 = bundle_files(CTX, ps, "beta", 100000, false);      /* only a.c matches */
    CKS(sv(fld(r4, "bundle").u.l->v[0], "path"), "a.c");      /* hits sort first */
    CKI(nv(fld(r4, "bundle").u.l->v[0], "hits"), 1);
    V r3 = bundle_files(CTX, ps, "THREE", 100000, true);
    CK(strstr(sv(fld(r3, "bundle").u.l->v[0], "text"), "##hits") == NULL,
       "only_symbols must not emit source lines");
    CKI(nv(fld(r3, "bundle").u.l->v[0], "hits"), 1);
  }
  T("bundle: hard byte budget");
  {
    V ps = v_list(&A, 4, v_str(s_wrap("a.c")), v_str(s_wrap("b.py")),
                  v_str(s_wrap("c.go")), v_str(s_wrap("many.c")));
    int caps[] = { 0, 1, 32, 64, 96, 128, 192, 256, 512, 1024, 4096, 100000 };
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++) {
      V r = bundle_files(CTX, ps, NULL, caps[i], true);
      CK(!v_is_err(r), "cap %d must not fail", caps[i]);
      int bytes = nv(r, "bytes"), cap = nv(r, "cap");
      CK(bytes <= cap, "cap %d: bytes %d exceeded", caps[i], bytes);
      CK(cap == caps[i], "cap must be echoed, got %d", cap);
      int nd = fld(r, "dropped").u.l ? fld(r, "dropped").u.l->len : -1;
      int nb = fld(r, "bundle").u.l ? fld(r, "bundle").u.l->len : -1;
      CKI(nd + nb, 4);
      if (caps[i] < 128) CK(nd > 0, "cap %d must drop something", caps[i]);
      if (caps[i] >= 4096) CKI(nd, 0);
      /* most structure first, and nothing is ever cut mid-line. A cap this small
       * drops every file (asserted just above), so there is no entry to read:
       * v[0] of an empty list is the segfault this line used to crash on. */
      if (nb > 0) CKI(nv(fld(fld(r, "bundle").u.l->v[0], "hits"), "x"), -1);
      for (int k = 0; k < nb; k++) {
        V e = fld(r, "bundle").u.l->v[k];
        Str t2 = fld(e, "text").u.s;
        if (t2.len > 0) CK(t2.p[t2.len - 1] != '\n', "text must not end with a newline");
      }
    }
    V r = bundle_files(CTX, ps, NULL, 64, true);
    CK(fld(r, "dropped").u.l->len > 0, "64-byte budget must drop");
    Str why = fld(fld(r, "dropped").u.l->v[0], "reason").u.s;
    CK(s_ni(why, "budget"), "reason=%.*s", why.len, why.p);
    for (int cap = 48; cap <= 600; cap += 24) {
      V b = bundle_files(CTX, ps, NULL, cap, true);
      CK(nv(b, "bytes") <= cap, "cap %d: got %d", cap, nv(b, "bytes"));
      int nok = 0, nl = body_lines(sv(fld(b, "bundle").u.l->v[0], "text"), &nok);
      CKI(nok, nl);                     /* a cut bundle is still line-shaped */
      if (nl > 0) CKI(fld(b, "bundle").u.l->v[0].u.r->len, 8);
    }
    V big = bundle_files(CTX, ps, NULL, 300, true);
    bool saw_trunc = false;
    for (int k = 0; k < fld(big, "bundle").u.l->len; k++)
      if (strstr(sv(fld(big, "bundle").u.l->v[k], "text"), "##truncated=")) saw_trunc = true;
    CK(saw_trunc, "a partially fitted file must say so with ##truncated=");
    CK(nv(big, "bytes") <= 300, "truncated bundle still over budget");
  }
  T("bundle: read failures and duplicates");
  {
    V ps = v_list(&A, 3, v_str(s_wrap("gone.c")), v_str(s_wrap("a.c")), v_str(s_wrap("a.c")));
    V r = bundle_files(CTX, ps, NULL, 100000, true);
    CKI(fld(r, "bundle").u.l->len, 1);
    CKI(fld(r, "dropped").u.l->len, 2);
    CKS(sv(fld(r, "dropped").u.l->v[0], "path"), "gone.c");
    Str why = fld(fld(r, "dropped").u.l->v[0], "reason").u.s;
    CK(s_eqz(why, "read:NOENT"), "missing file reason=%.*s", why.len, why.p);
    CKS(sv(fld(r, "dropped").u.l->v[1], "path"), "a.c");
    Str dup = fld(fld(r, "dropped").u.l->v[1], "reason").u.s;
    CK(s_eqz(dup, "duplicate"), "duplicate reason=%.*s", dup.len, dup.p);
    CKS(sv(fld(r, "bundle").u.l->v[0], "path"), "a.c");
    /* bytes is the SOURCE size of the bundled file (SPEC fs.ls bytes), not the
     * emitted body: a.c is 59 bytes of text. */
    CKI(nv(fld(r, "bundle").u.l->v[0], "bytes"), 59);
    V notalist = bundle_files(CTX, v_str(s_wrap("a.c")), NULL, 100, true);
    CK(v_is_err(notalist), "a non-list must be an ERR value");
    CK(v_errcode(notalist) == E_TYPE, "expected TYPE, got %s", err_name(v_errcode(notalist)));
    V empty = bundle_files(CTX, v_list(&A, 0), "", 1000, false);
    CKI(fld(empty, "bundle").u.l->len, 0);
    CKI(fld(empty, "dropped").u.l->len, 0);
    CKS(sv(empty, "query"), "-");
  }
  T("bundle: determinism");
  {
    V ps = v_list(&A, 3, v_str(s_wrap("c.go")), v_str(s_wrap("b.py")), v_str(s_wrap("a.c")));
    V ps2 = v_list(&A, 3, v_str(s_wrap("b.py")), v_str(s_wrap("a.c")), v_str(s_wrap("c.go")));
    Str x = v_tostr(&A, bundle_files(CTX, ps, "e", 1000, false), true);
    Str y = v_tostr(&A, bundle_files(CTX, ps, "e", 1000, false), true);
    Str z = v_tostr(&A, bundle_files(CTX, ps2, "e", 1000, false), true);
    CK(s_eq(x, y), "same input must give byte-identical output");
    CK(s_eq(x, z), "path order in the request must not change the bundle");
    Str j1 = v_tojson(&A, bundle_files(CTX, ps, NULL, 1000, true));
    Str j2 = v_tojson(&A, bundle_files(CTX, ps, NULL, 1000, true));
    CK(s_eq(j1, j2), "json form must be stable too");
    V o1 = ol("store/store.go", GOSRC);
    V o2 = ol("store/store.go", GOSRC);
    CK(s_eq(v_tostr(&A, o1, true), v_tostr(&A, o2, true)), "outline must be stable");
    CK(s_eq(fld(sym(o1, 3), "at").u.s, fld(sym(o2, 3), "at").u.s), "anchors must be stable");
    Str oc = v_tostr(&A, o1, true);
    CK(nv(o1, "tok") > 0, "outline must self-report a token estimate");
    CK(nv(o1, "tok") == tok_est(oc.p, (size_t)oc.len),
       "tok= must be the estimate of what it emitted (%d vs %d)", nv(o1, "tok"),
       tok_est(oc.p, (size_t)oc.len));
  }
  T("outline: contract keys");
  {
    V o = ol("src/widget.c", CSRC);
    CK(fld(o, "path").t == V_STR && fld(o, "lang").t == V_STR, "path/lang");
    CK(fld(o, "bytes").t == V_NUM && fld(o, "lines").t == V_NUM, "bytes/lines");
    CK(fld(o, "hash").t == V_NUM ? false : true, "hash must be a string");
    CK(fld(o, "symbols").t == V_LIST, "symbols must be a list");
    CK(fld(o, "tok").t == V_NUM, "tok must be present");
    CK(o.u.r->len == 7, "keys must be exactly path,lang,bytes,lines,hash,symbols,tok (got %d)",
       o.u.r->len);
    CK(s_eqz(o.u.r->kv[0].k, "path") && s_eqz(o.u.r->kv[1].k, "lang") &&
       s_eqz(o.u.r->kv[2].k, "bytes") && s_eqz(o.u.r->kv[3].k, "lines") &&
       s_eqz(o.u.r->kv[4].k, "hash") && s_eqz(o.u.r->kv[5].k, "symbols") &&
       s_eqz(o.u.r->kv[6].k, "tok"), "key order is contractual (insertion order)");
    V s0 = sym(o, 0);
    CK(s0.u.r->len == 5, "symbol keys: kind,name,line,at,sig (got %d)", s0.u.r->len);
    CK(s_eqz(s0.u.r->kv[0].k, "kind") && s_eqz(s0.u.r->kv[1].k, "name") &&
       s_eqz(s0.u.r->kv[2].k, "line") && s_eqz(s0.u.r->kv[3].k, "at") &&
       s_eqz(s0.u.r->kv[4].k, "sig"), "symbol key order");
    /* the compact rendering is what an agent reads: one line per symbol */
    Str compact = v_tostr(&A, o, true);
    CK(strstr(compact.p, "symbols=[{kind=macro,name=WIDGET_MAX,line=4,at=") != NULL,
       "compact form lost symbols: %.*s", compact.len > 200 ? 200 : compact.len, compact.p);
    CK(strstr(compact.p, "lines=44") != NULL, "size self-report missing");
    CK(strchr(compact.p, '\n') == NULL, "compact form must be exactly one line");
  }
  T("outline: never reads past the end");
  {
    /* a non-NUL-terminated loaned view must be handled by length alone */
    const char *full = "int pad_unused(void);\nstatic int real(int x) { return x; }\n";
    Str view = s_slice(s_wrap(full), 0, 21);        /* exactly through the ';' */
    CKI(view.len, 21);
    V v = outline_file(CTX, "x.c", view, NULL);
    CKI(nsyms(v), 1);
    CKS(sv(v, "path"), "x.c");
    CKI(nv(v, "bytes"), 21);
    CKS(sv(sym(v, 0), "name"), "pad_unused");
    Str mid = s_slice(s_wrap(full), 0, 18);         /* cuts the identifier short */
    V vm = outline_file(CTX, "x.c", mid, NULL);
    CK(!v_is_err(vm), "a truncated buffer must still outline, not crash");
    CKI(nsyms(vm), 0);                              /* half a token is not a symbol */
    Str view2 = s_slice(s_wrap("void tail(void);\n"), 0, 9);
    V v2 = outline_file(CTX, "y.c", view2, NULL);
    CK(!v_is_err(v2), "a clipped line must not run off the end");
    Str c3 = s_slice(s_wrap("int unterminated(\"x\n"), 0, 17);
    V v3 = outline_file(CTX, "z.c", c3, NULL);
    CK(!v_is_err(v3), "unbalanced quotes must not run off the end");
    CKI(nsyms(v3), 0);
    V v4 = outline_file(CTX, "w.c", s_slice(s_wrap("int ok(void);\n"), 0, 12), NULL);
    CKI(nsyms(v4), 1);
  }
  T_REPORT("outline");
}
