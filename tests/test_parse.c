/* test_parse.c - lex() + parse_program() over every form VXA admits, and every
 * form it must refuse. The parser is the one part of the language an agent
 * cannot work around: a rejected token costs a whole round trip, so the shapes,
 * the precedence and the error positions are all pinned here.
 *
 * Runs on the parse-only link line (build.sh test parse -> tests/stubs.c plus
 * util val lex parse); the stubs file covers plan.c's side of the seam. */
#include "ast.h"
#include "t.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Arena A;

/* ---------- drivers ---------- */
static Node *prog(const char *src, char **err) {
  Lexer lx; memset(&lx, 0, sizeof lx); lx.a = &A;
  lex(&A, src, strlen(src), &lx);
  if (lx.err) { if (err) *err = arena_strdup(&A, lx.err); return NULL; }
  int el = 0;
  return parse_program(&A, lx.toks, lx.n, "t.vxa", err, &el);
}
static Node *Q(const char *src) { return prog(src, NULL); }
/* parses, or the test fails */
static Node *P(const char *src) {
  char *err = NULL;
  Node *n = prog(src, &err);
  CHECK(n != NULL, "should parse: %.56s -> %s", src, err ? err : "(no error)");
  if (!n && err) printf("   note: %s\n", err);
  return n;
}
/* refuses, and the message has this shape */
static void X(const char *src, const char *needle) {
  char *err = NULL;
  Node *n = prog(src, &err);
  CHECK(n == NULL && err && strstr(err, needle) != NULL,
        "should refuse with \"%s\": got %s", needle, err ? err : "(parsed clean)");
}
/* refuses with an error anchored at file:line:col */
static void XPOS(const char *src, int line, int col) {
  char *err = NULL;
  Node *n = prog(src, &err);
  if (n || !err) { CHECK(0, "should refuse at %d:%d: %s", line, col, err ? err : "(parsed clean)"); return; }
  char want[64]; snprintf(want, sizeof want, "t.vxa:%d:%d:", line, col);
  CHECK(strstr(err, want) != NULL, "error position: got \"%s\" want prefix \"%s\"", err, want);
}
static const char *dump(Node *n) {
  Buf b; buf_init(&b, &A); ast_dump(&A, n, &b); return b.p ? b.p : "";
}

static Node *k0(Node *n) { return n ? n->kids[0] : NULL; }
static Node *k1(Node *n) { return n ? n->kids[1] : NULL; }
static Node *k2(Node *n) { return n ? n->kids[2] : NULL; }
/* parse_program returns a BLOCK: its statements live in items[], never in kids[] */
static Node *s0(Node *prog) { return prog ? prog->items[0] : NULL; }
static Node *val(Node *prog) { Node *a = s0(prog); return a ? k1(a) : NULL; }  /* "NAME = <here>" */

static bool isop(Node *n, const char *o) { return n && n->k == NK_BIN && !strcmp(n->s.p, o); }
static int kind(Node *n) { return n ? n->k : -1; }
static int items(Node *n) { return n ? n->nitems : -1; }

/* ---------- the doc budget is part of the contract ---------- */
static long doc_read(char **out) {
  static const char *paths[] = { "docs/LEARN.md", "../docs/LEARN.md" };
  for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) {
    FILE *f = fopen(paths[i], "rb");
    if (!f) continue;
    fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f);
    if (out) {
      f = fopen(paths[i], "rb");
      char *b = (char*)malloc((size_t)n + 1);
      size_t rd = fread(b, 1, (size_t)n, f); fclose(f);
      b[rd] = 0; *out = b;
    }
    return n;
  }
  return -1;
}
int main(void) {
  arena_init(&A, 0);
  setvbuf(stdout, NULL, _IONBF, 0);

  T("every statement form parses");
  CK(Q("x = 1"));
  CK(Q("def add(a, b) { a + b }"));
  CK(Q("if c { } elif d { } else { }"));
  CK(Q("while c { break }"));
  CK(Q("for v in list { continue }"));
  CK(Q("for k, v in rec { out.kv(k, v) }"));
  CK(Q("return\n"));
  CK(Q("return 1 + 2"));
  CK(Q("a = 1; b = 2"));
  CK(Q("#!/usr/bin/env vxa\nx = 1\n# trailing comment"));
  CK(Q("break"));
  CK(Q("continue"));

  T("every value form parses");
  CK(Q("x = 12"));
  CK(Q("x = 3.5"));
  CK(Q("x = 0x1F"));
  CK(Q("x = 1e-2"));
  CK(Q("x = -2"));
  CK(Q("x = \"a{b}c\""));
  CK(Q("x = 'raw {no-interp}'"));
  CK(Q("x = ```\nblock\n```"));
  CK(Q("x = [1, 2, 3]"));
  CK(Q("x = {name:\"a\", n:1}"));
  CK(Q("x = true"));
  CK(Q("x = false"));
  CK(Q("x = null"));
  CK(Q("x = def(a, b) { a + b }"));
  CK(Q("x = (a, b) -> a + b"));
  CK(Q("x = a -> a * 2"));
  CK(Q("x = () -> 1"));
  CK(Q("x = a => a * 2"));                       /* SPEC/CHEAT spelling is also accepted */

  T("postfix chains");
  CK(Q("x = a.b.c.d"));
  CK(Q("x = a[0][1].c(1)(2)"));
  CK(Q("x = fs.read(\"a.c\", {lines:\"10-40\"})"));
  CK(Q("x = p.files.len"));
  CK(Q("x = r.get(\"k\", \"-\")"));
  CK(Q("x = [1,2] | map(y -> y * 2)"));
  CK(Q("x = s[2..5]"));
  CK(Q("x = s[2..]"));

  T("the forms the language refuses");
  X("x = {a:1", "expected '}' to close the record");
  X("x = [1, 2", "expected ']'");
  X("else { 1 }", "unexpected KW_ELSE");
  X("for v xs { }", "expected 'in'");
  X("x = = 1", "expected a value, a name or '(' here");
  X("!\n", "expected a value");
  X("x = 1.2.3", "malformed number");
  X("x = \"a{b\"", "unterminated interpolation");
  X("x = \"unterminated", "unterminated string");
  X("def 1f() { }", "malformed number");
  X("x = 1 y = 2", "statements must be separated");
  X("if a { b() c() }", "statements must be separated");
  X("if r.code? { }", "unexpected QUEST");       /* CHEAT teaches "r.code?": not a production */
  X("global x", "statements must be separated"); /* interp has NK_GLOBAL, parse cannot emit it */
  X("p | out.emit", "after '|' a call is required");
  X("m = {{k}: 1}", "expected a record key");
  X("x = (1, 2)", "must be one value");
  X("x = s[..5]", "expected a value");           /* only a leading index is allowed */
  X("x = [k..]", "expected a value");            /* SPEC 4.2 "omit([k..])" is unwritable */
  X("def f(a,) { a }", "expected a parameter name");
  X("x = a =~ b", "unexpected character '~'");
  X("x = 1 @ 2", "unexpected character '@'");
  X("if a { 1 } else if b { 2 }", "expected '{'");
  X("x = {a:1, ,b:2}", "expected a record key");
  X("x = [1, 2,,]", "expected a value");
  X("x = \"{ 1 +}\"", "in string interpolation");
  X("x = \"{ @ }\"", "unexpected character");
  X("x = def f { 1 }", "not an expression");

  T("error position is file:line:col:");
  XPOS("x = 1\ny = 2\nz = = 3\n", 3, 5);
  XPOS("a = 1\nb = [\n1, 2\n", 4, 1);            /* EOF is reported where it sits */
  XPOS("x = {a:1,\ny = 2\n", 2, 3);
  XPOS("a = 1\nb = 2\nc = !\n", 4, 1);
  XPOS("\nx = 1 2\n", 2, 7);
  XPOS("a = 1\nb = 2\n2 3\n", 3, 3);
  { char *e = NULL; prog("x = = 1", &e);
    CHECK(e && !strncmp(e, "t.vxa:1:", 8), "error starts with <file>:<line>: %s", e ? e : "(null)"); }
  { char *e = NULL; prog("x = {a", &e);
    CHECK(e && strstr(e, ":1:") != NULL && strstr(e, "unexpected end of file") != NULL,
          "EOF errors name the token: %s", e ? e : "(null)"); }

  T("precedence: * over +, left associative");
  { Node *p = P("x = 1 + 2 * 3"); CK(isop(val(p), "+")); CK(isop(k1(val(p)), "*")); }
  { Node *p = P("x = 1 * 2 + 3"); CK(isop(val(p), "+")); CK(isop(k0(val(p)), "*")); }
  { Node *p = P("x = 1 - 2 - 3"); CK(isop(val(p), "-")); CK(isop(k0(val(p)), "-")); }
  { Node *p = P("x = 2 * 3 % 4"); CK(isop(val(p), "%")); CK(isop(k0(val(p)), "*")); }
  { Node *p = P("x = 1 + 2 | f(3)");
    CK(kind(val(p)) == NK_PIPE); CK(isop(k0(val(p)), "+")); }

  T("precedence: cmp over and, and over or, not highest");
  { Node *p = P("x = a and b or c"); CK(isop(val(p), "or")); CK(isop(k0(val(p)), "and")); }
  { Node *p = P("x = not a and b"); CK(isop(val(p), "and")); CK(kind(k0(val(p))) == NK_NOT); }
  { Node *p = P("x = a == b and c"); CK(isop(val(p), "and")); CK(isop(k0(val(p)), "==")); }
  { Node *p = P("x = a ~= b and c"); CK(isop(val(p), "and")); CK(isop(k0(val(p)), "~=")); }
  { Node *p = P("x = a < b == c"); CK(isop(val(p), "==")); CK(isop(k0(val(p)), "<")); }
  { Node *p = P("x = a < b and c < d"); CK(isop(val(p), "and")); CK(isop(k0(val(p)), "<")); }

  T("precedence: .. sits between + and |");
  { Node *p = P("x = 1 + 2..3 + 4"); CK(kind(val(p)) == NK_RANGE); CK(isop(k1(val(p)), "+")); }
  { Node *p = P("x = 1..3 | f(2)");  CK(kind(val(p)) == NK_PIPE); CK(kind(k0(val(p))) == NK_RANGE); }
  { Node *p = P("x = 1 | f(2) | g(3)");
    CK(kind(val(p)) == NK_PIPE); CK(kind(k0(val(p))) == NK_PIPE); CK(kind(k1(val(p))) == NK_CALL); }
  { Node *p = P("x = a | b(c)");
    CK(kind(val(p)) == NK_PIPE);
    CK(kind(k1(val(p))) == NK_CALL);
    CKI(items(k1(val(p))), 1);                   /* the piped value becomes arg 0 */
    CK(strstr(dump(k0(val(p))), "ID@1(a)") != NULL); }

  T("unary minus and not");
  { Node *p = P("x = -2 * 3"); CK(isop(val(p), "*")); CK(kind(k0(val(p))) == NK_INT);
    CK(k0(val(p))->num == -2.0); }
  { Node *p = P("x = -a.b"); CK(kind(val(p)) == NK_NEG); CK(kind(k0(val(p))) == NK_FIELD); }
  { Node *p = P("x = not not a"); CK(kind(val(p)) == NK_NOT); CK(kind(k0(val(p))) == NK_NOT); }
  { Node *p = P("x = !a"); CK(kind(val(p)) == NK_NOT); }         /* '!' is unary not */
  { Node *p = P("x = a != b"); CK(isop(val(p), "!=")); }

  T("postfix binds tighter than any binary operator");
  { Node *p = P("x = a.b(1).c"); CK(kind(val(p)) == NK_FIELD); CK(kind(k0(val(p))) == NK_CALL); }
  { Node *p = P("x = [1,2][0]"); CK(kind(val(p)) == NK_INDEX); CK(kind(k0(val(p))) == NK_LIST); }
  { Node *p = P("x = f(1)(2)");  CK(kind(val(p)) == NK_CALL);  CK(kind(k0(val(p))) == NK_CALL); }
  { Node *p = P("x = 1 + f(2) * 3"); CK(isop(val(p), "+")); CK(isop(k1(val(p)), "*")); }
  { Node *p = P("x = a[1..2][0]"); CK(kind(val(p)) == NK_INDEX); CK(kind(k0(val(p))) == NK_INDEX); }

  T("slices are INDEX with three child slots");
  { Node *p = P("x = s[2..5]"); Node *i = val(p);
    CK(kind(i) == NK_INDEX); CKI(i->nkids, 3); CK(i->kids[2] != NULL); }
  { Node *p = P("x = s[2..]"); Node *i = val(p);
    CK(kind(i) == NK_INDEX); CKI(i->nkids, 3); CK(i->kids[2] == NULL); }
  { Node *p = P("x = s[2]"); CK(kind(val(p)) == NK_INDEX); CKI(val(p)->nkids, 2); }
  { Node *p = P("x = l[0..len(l)]"); CK(kind(k2(val(p))) == NK_CALL); }

  T("blocks vs records: { is decided by position, never guessed");
  { Node *p = P("x = {a:1, b:2}"); CK(kind(val(p)) == NK_REC); CKI(items(val(p)), 2); }
  { Node *p = P("x = {}"); CK(kind(val(p)) == NK_REC); CKI(items(val(p)), 0); }
  { Node *p = P("if a { x = 1 }");
    CK(kind(s0(p)) == NK_IF); CK(kind(k1(s0(p))) == NK_BLOCK); }
  { Node *p = P("def f() { x = 1 }"); CK(kind(s0(p)) == NK_FUNC); }
  { Node *p = P("f = (a, b) -> { a + b }"); CK(kind(val(p)) == NK_LAMBDA);
    CK(kind(k0(val(p))) == NK_BLOCK); }
  { Node *p = P("x = {a:{b:{c:[1,2,{f:{g:2}}]}}}"); CK(kind(val(p)) == NK_REC);
    CK(strstr(dump(s0(p)), "REC@") != NULL); }

  T("trailing commas and multi-line collections");
  { Node *p = P("x = [1, 2, 3,]"); CKI(items(val(p)), 3); }
  { Node *p = P("x = {a:1, b:2,}"); CKI(items(val(p)), 2); }
  { Node *p = P("x = f(1, 2,)"); CKI(items(val(p)), 2); }
  { Node *p = P("x = [\n 1,\n 2,\n]"); CKI(items(val(p)), 2); }
  { Node *p = P("x = {\n a: [\n  1,\n ],\n b: 2,\n}"); CKI(items(val(p)), 2); }
  { Node *p = P("x = f(\n 1,\n 2\n)"); CKI(items(val(p)), 2); }
  { Node *p = P("x = [\n];"); CKI(items(val(p)), 0); }

  T("nesting depth is bounded, not fatal");
  { Buf b; buf_init(&b, &A);
    for (int i = 0; i < 190; i++) buf_puts(&b, "[");
    buf_puts(&b, "1");
    for (int i = 0; i < 190; i++) buf_puts(&b, "]");
    Str s = buf_take(&b);
    CK(Q(s.p)); }
  { Buf b; buf_init(&b, &A);
    for (int i = 0; i < 260; i++) buf_puts(&b, "(");
    buf_puts(&b, "1");
    for (int i = 0; i < 260; i++) buf_puts(&b, ")");
    Str s = buf_take(&b);
    char *e = NULL; Node *n = prog(s.p, &e);
    CHECK(n == NULL && e && strstr(e, "nested too deeply") != NULL,
          "260-deep must hit the depth guard, got %s", e ? e : "(parsed)"); }
  { CK(Q("x = {a:{b:{c:{d:[1,2,{f:{g:2}}]}}}}")); }

  T("if/elif/else chain shape");
  { Node *p = P("if a { 1 } elif b { 2 } elif c { 3 } else { 4 }");
    Node *i = s0(p);
    CK(kind(i) == NK_IF); CK(kind(k2(i)) == NK_IF); CK(kind(k2(k2(i))) == NK_IF);
    CK(kind(k2(k2(k2(i)))) == NK_BLOCK);
    CKI(i->line, 1); CKI(items(k1(i)), 1); }
  { Node *p = P("if a { 1 }\n\nelse { 2 }"); CK(kind(k2(s0(p))) == NK_BLOCK); }
  { Node *p = P("if a { 1 }"); CK(k2(s0(p)) == NULL); }
  { Node *p = P("if a { 1 } elif b { 2 }"); CK(kind(k2(s0(p))) == NK_IF); }

  T("for and def signatures");
  { Node *p = P("for k, v in rec { x }");
    Node *f = s0(p); CK(kind(f) == NK_FOR); CKI(f->nnames, 2);
    CK(!strcmp(f->names[0].p, "k") && !strcmp(f->names[1].p, "v")); }
  { Node *p = P("for i in 1..3 { x = i }");
    Node *f = s0(p); CK(kind(f) == NK_FOR); CK(kind(k0(f)) == NK_RANGE); }
  { Node *p = P("def f(a, b, c) { a }");
    Node *fn = s0(p); CK(kind(fn) == NK_FUNC); CK(!strcmp(fn->s.p, "f"));
    CK(kind(k0(fn)) == NK_LAMBDA); CKI(k0(fn)->nnames, 3); }
  { Node *p = P("def f() { }"); CKI(k0(s0(p))->nnames, 0); }
  { Node *p = P("x = def(a, b) { a + b }"); CK(kind(val(p)) == NK_LAMBDA); CKI(val(p)->nnames, 2); }
  { Node *p = P("x = a => a * 2"); CK(kind(val(p)) == NK_LAMBDA); CKI(val(p)->nnames, 1); }
  { Node *p = P("y = x.map(a => a * 2)"); CK(kind(k0(val(p))) == NK_FIELD); CKI(items(val(p)), 1); }
  { Node *p = P("def f { 1 }"); CK(kind(s0(p)) == NK_FUNC); }   /* parens may be left out */

  T("interpolation is parsed as an expression, in place");
  { Node *p = P("x = \"a{b + 1}c\"");
    CK(kind(val(p)) == NK_INTERP); CKI(items(val(p)), 3);
    CK(kind(val(p)->items[1]) == NK_BIN); }
  { Node *p = P("x = \"{ f(1) }\""); CK(kind(val(p)->items[1]) == NK_CALL); }
  { Node *p = P("x = \"{a}{b}{c}\""); CKI(items(val(p)), 7); }
  { Node *p = P("x = \"s={ f(\"y\") }\""); CK(kind(val(p)) == NK_INTERP); }
  { Node *p = P("x = \"a{ f(g) | h(1) }b\""); CK(kind(val(p)->items[1]) == NK_PIPE); }
  { Node *p = P("x = \"no interp here\""); CK(kind(val(p)) == NK_STR); }
  X("x = \"{}\"", "unexpected end of file");

  T("an operator may end a line; a pipe may not start one");
  { Node *p = P("x =\n1"); CK(kind(val(p)) == NK_INT); }
  { Node *p = P("x = 1 +\n2"); CK(isop(val(p), "+")); }
  X("x = a\n| f(1)", "expected a value");               /* a pipe cannot start a line */
  { Node *p = P("f(\n1,\n2\n)"); CK(kind(s0(p)) == NK_CALL); }
  X("x = 1\n2 3\n", "statements must be separated");

  T("CRLF sources lex and parse identically");
  { char src[] = "x = 1\r\nif x {\r\n  y = 2\r\n}\r\n";
    Node *n = P(src); CKI(items(n), 2); }
  { char src[] = "a = 1\r\nb = = 2\r\n"; XPOS(src, 2, 5); }
  { char src[] = "a = [\r\n1,\r\n2\r\n]\r\n"; Node *n = P(src);
    CK(kind(val(n)) == NK_LIST); CKI(items(val(n)), 2); }
  { char src[] = "def f(a)\r\n{\r\n a\r\n}\r\n"; CK(Q(src)); }

  T("comments and the shebang are not statements");
  { Node *p = P("# lead\nx = 1 # trail\n"); CKI(items(p), 1); }
  { Node *p = P("#!/usr/bin/env vxa\nx = 1\n"); CKI(items(p), 1); }
  CK(Q("#!/usr/bin/env vxa"));
  { Node *p = P("x = 1 # }\n"); CK(kind(val(p)) == NK_INT); }
  { Node *p = P("# a \"quoted\" { and } inside a comment\nx = 1\n"); CKI(items(p), 1); }
  CK(Q("x = \"a#b\" # not a comment"));

  T("whitespace and separators");
  { Node *p = P("x\t=\t1"); CK(kind(val(p)) == NK_INT); }
  { Node *p = P("a=1;b=2;c=3"); CKI(items(p), 3); }
  { Node *p = P("a=1;;b=2"); CKI(items(p), 2); }
  { Node *p = P("\n\n\n"); CK(Q("\n\n\n")); }
  { Node *p = P("def f() {\n a = 1\n b = 2\n}\n"); CKI(items(k0(k0(s0(p)))), 2); }

  T("an AST walk sees every child");
  { Node *p = P("x = 1 + 2"); Node *a = s0(p);
    CK(a && a->k == NK_ASSIGN); CKI(a->nkids, 2); CK(a->kids[1] != NULL);
    CK(strstr(dump(a), "ID@1(x)") != NULL); }
  { Node *p = P("f(1)"); Node *c = s0(p);
    CKI(c->nkids, 1); CK(strstr(dump(c), "ID@1(f)") != NULL);
    CKI(items(c), 1); }
  { Node *p = P("a.b"); CKI(s0(p)->nkids, 1); }
  { Node *p = P("if a { 1 } else { 2 }"); Node *i = s0(p); CKI(i->nkids, 3); }
  { Node *p = P("for k, v in r { k }"); Node *f = s0(p); CKI(f->nkids, 2); }

  T("the doc must fit the context budget and teach what parses");
  { char *body = NULL; long b = doc_read(&body);
    if (b <= 0) {
      printf("   SKIP: docs/LEARN.md not found from the test's cwd - size guard not applied\n");
    } else {
      CHECK(b / 4 < 2800, "docs/LEARN.md is %ld bytes = %ld est-tokens, budget is 2800", b, b / 4);
      printf("   docs/LEARN.md bytes=%ld est_tokens=%ld budget=2800 headroom=%ld\n", b, b / 4, 2799 - b / 4);
      CHECK(body && strstr(body, "NEED_CONFIRM") != NULL, "doc must cover NEED_CONFIRM");
      CHECK(body && strstr(body, "STALE_PLAN") != NULL, "doc must cover STALE_PLAN");
      CHECK(body && strstr(body, "OUTSIDE_JAIL") != NULL, "doc must cover OUTSIDE_JAIL");
      CHECK(body && strstr(body, "LOOP_DETECTED") != NULL, "doc must cover LOOP_DETECTED");
      CHECK(body && strstr(body, "--confirm") != NULL, "doc must show --confirm");
      CHECK(body && strstr(body, "fs.outline") != NULL, "doc must cover find/outline");
      CHECK(body && strstr(body, "tx.patch") != NULL, "doc must cover anchored edits");
      CHECK(body && strstr(body, "anchors") != NULL, "doc must cover anchors:true");
      CHECK(body && strstr(body, "sh.ro") != NULL, "doc must cover sh.ro");
      CHECK(body && strstr(body, "cfg.get") != NULL, "doc must cover cfg.get");
      CHECK(body && strstr(body, "truncated") != NULL, "doc must cover the truncated contract");
      CHECK(body && strstr(body, "vxa lang") != NULL, "doc must point at vxa lang");
      CHECK(body && strstr(body, "->") != NULL, "doc must teach the arrow that parses");
      CHECK(body && strstr(body, "max_bytes") != NULL, "doc must use the real fs.read option");
      CHECK(body && strstr(body, "exit 3") != NULL, "doc must give the confirm exit code");
      /* never teach a form the parser refuses */
      CHECK(body && strstr(body, "r.code? {") == NULL, "doc must not teach the unimplemented r.code?");
      CHECK(body && strstr(body, "| out.plan\n") == NULL, "doc must not teach a bare pipe target");
    } }

  T_REPORT("parse");
}
