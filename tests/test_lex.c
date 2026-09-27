/* test_lex.c - the parser inherits every mistake the lexer makes, so the
 * token stream is asserted exactly: kinds, positions, spans, string parts. */
#include "ast.h"
#include "t.h"
#include <stdlib.h>

static Arena A;
static Lexer lx;

static void L(const char *s) { lex(&A, s, strlen(s), &lx); }

static Str kinds_str(void) {
  Buf b; buf_init(&b, &A);
  for (int i = 0; i < lx.n; i++) { if (i) buf_putc(&b, ' '); buf_puts(&b, tok_name(lx.toks[i].k)); }
  return buf_take(&b);
}
static Tok *tk(int i) {
  static Tok bad;
  if (i < lx.n) return &lx.toks[i];
  memset(&bad, 0, sizeof bad); bad.k = T_INVALID; return &bad;
}
#define KINDS(w) CKSTR(kinds_str(), w)

int main(void) {
  arena_init(&A, 0);

  T("tok_name");
  CKS(tok_name(T_ID), "ID");
  CKS(tok_name(T_EEQ), "EQ");
  CKS(tok_name(T_EOF), "EOF");
  CKS(tok_name(T_NL), "NL");
  CKS(tok_name(T_DOTS), "DOTS");
  CKS(tok_name(T_INVALID), "INVALID");

  T("empty");
  L("");
  CKI(lx.n, 1);
  KINDS("EOF");
  CK(lx.err == NULL);
  CKI(tk(0)->line, 1);
  CKI(tk(0)->col, 1);

  T("numbers");
  L("123 1.5 1e3 0x1F 0xff 1e-2 1..2");
  KINDS("NUM NUM NUM NUM NUM NUM NUM DOTS NUM EOF");
  CK(lx.err == NULL);
  CKD(tk(0)->num, 123);
  CKD(tk(1)->num, 1.5);
  CKD(tk(2)->num, 1000);
  CKD(tk(3)->num, 31);
  CKD(tk(4)->num, 255);
  CKD(tk(5)->num, 0.01);
  CKD(tk(6)->num, 1);
  CKI(tk(0)->len, 3);
  CKI(tk(3)->len, 4);
  CKI(tk(5)->len, 4);
  CKI(tk(3)->col, 13);
  CKI(tk(5)->col, 23);
  CKI(tk(7)->col, 29);
  CKI(tk(7)->len, 2);
  CKI(tk(8)->col, 31);
  L("0");
  CKD(tk(0)->num, 0);
  L("007");
  CKD(tk(0)->num, 7);

  T("numbers are never signed by the lexer");
  L("a -1\nb");
  KINDS("ID SUB NUM NL ID EOF");
  CKD(tk(2)->num, 1);
  L("1 - 2");
  KINDS("NUM SUB NUM EOF");

  T("bad numbers");
  L("1.2.3");
  CKS(lx.err, "lex:1:1: malformed number");
  CKI(lx.err_line, 1); CKI(lx.err_col, 1);
  L("x = 1.2.3\n");
  CKS(lx.err, "lex:1:5: malformed number");
  L("1e");
  CK(lx.err != NULL && strstr(lx.err, "exponent") != NULL);
  L("0x");
  CK(lx.err != NULL && strstr(lx.err, "hex") != NULL);
  L("1abc");
  CKS(lx.err, "lex:1:1: malformed number");
  L("0x1FG");
  CK(lx.err != NULL);

  T("identifiers and keywords");
  L("def add if elif else while for in return break continue true false null and or not _x9 A_b");
  KINDS("KW_DEF ID KW_IF KW_ELIF KW_ELSE KW_WHILE KW_FOR KW_IN KW_RETURN KW_BREAK "
        "KW_CONTINUE KW_TRUE KW_FALSE KW_NULL KW_AND KW_OR KW_NOT ID ID EOF");
  CK(lx.err == NULL);
  CKSTR(tk(0)->s, "def");
  CKSTR(tk(1)->s, "add");
  CKI(tk(1)->len, 3);
  CKSTR(tk(17)->s, "_x9");
  CKSTR(tk(18)->s, "A_b");
  CKI(tk(18)->col, 88);
  L("Def IF Null AND");          /* keywords are case-sensitive */
  KINDS("ID ID ID ID EOF");
  L("_");
  KINDS("ID EOF");

  T("operators");
  L("+ - * / % < > <= >= == != ~= = : ? .. , ; . | -> ( ) [ ] { }");
  KINDS("ADD SUB MUL DIV MOD LT GT LE GE EQ NE SIM ASSIGN COLON QUEST DOTS COMMA SEMI "
        "DOT PIPE ARROW LPAREN RPAREN LBRACK RBRACK LBRACE RBRACE EOF");
  CK(lx.err == NULL);
  CKI(tk(9)->len, 2);
  CKI(tk(10)->len, 2);
  CKI(tk(20)->len, 2);
  CKI(tk(21)->len, 1);
  CKI(tk(21)->k, T_LPAREN);

  T("comments");
  L("x # c\ny");
  KINDS("ID NL ID EOF");
  CKI(tk(2)->line, 2);
  L("#only comment, no newline at eof");
  KINDS("EOF");
  CKI(lx.n, 1);
  L("1 #x");
  KINDS("NUM EOF");
  L("\"a#b\" # not a comment inside a string");
  KINDS("STR EOF");
  CKSTR(tk(0)->s, "a#b");
  L("'a#b'");
  KINDS("SQSTR EOF");

  T("shebang");
  L("#!/usr/bin/env vxa\n1");
  KINDS("NUM EOF");
  CKI(lx.n, 2);
  CKI(tk(0)->line, 2);
  L("#x\n1");                    /* plain comment on line 1 still ends the statement */
  KINDS("NL NUM EOF");
  CKI(lx.n, 3);

  T("line breaks");
  L("1\r\n2");
  KINDS("NUM NL NUM EOF");
  CKI(tk(1)->line, 1);
  CKI(tk(1)->len, 2);            /* the CRLF is one NL token, two bytes wide */
  CKI(tk(2)->line, 2);
  CKI(tk(2)->col, 1);
  L("1\r2");
  KINDS("NUM NL NUM EOF");
  CKI(tk(2)->line, 2);
  L("1\n\n\n2");
  KINDS("NUM NL NL NL NUM EOF");
  L("\n");
  KINDS("NL EOF");
  L("a\n\nb");
  KINDS("ID NL NL ID EOF");
  CKI(tk(3)->line, 3);

  T("newlines vanish inside [] and () but survive inside {}");
  L("[1,\n2]");
  KINDS("LBRACK NUM COMMA NUM RBRACK EOF");
  L("f(\n1\n)");
  KINDS("ID LPAREN NUM RPAREN EOF");
  L("[[1,\n2],\n[3]]");
  KINDS("LBRACK LBRACK NUM COMMA NUM RBRACK COMMA LBRACK NUM RBRACK RBRACK EOF");
  L("1]\n2");                    /* stray closer must not drive depth negative */
  KINDS("NUM RBRACK NL NUM EOF");
  CKI(tk(3)->line, 2);
  /* a brace is either a record or a block, so NLs stay: the parser skipnls
   * inside records and needs the breaks to separate block statements */
  L("{\na:1,\nb:2\n}");
  KINDS("LBRACE NL ID COLON NUM COMMA NL ID COLON NUM NL RBRACE EOF");
  L("{a:[1,\n2],\nb:3}");
  KINDS("LBRACE ID COLON LBRACK NUM COMMA NUM RBRACK COMMA NL ID COLON NUM RBRACE EOF");
  L("def f() {\n a = 1\n b = 2\n}\n");
  KINDS("KW_DEF ID LPAREN RPAREN LBRACE NL ID ASSIGN NUM NL ID ASSIGN NUM NL RBRACE NL EOF");
  L("[1]\n[2]");
  KINDS("LBRACK NUM RBRACK NL LBRACK NUM RBRACK EOF");
  L("a; b\n");                   /* ; separates too, and does not reset depth */
  KINDS("ID SEMI ID NL EOF");
  L("1\t+\f2\v");                /* every non-break whitespace is skipped */
  KINDS("NUM ADD NUM EOF");

  T("string escapes");
  { const char *p = "\"a\\nb\\tc\\\\d\\\"e\\{f\\}g\\x41\\x7f\"";
    L(p);
    KINDS("STR EOF");
    CKI(tk(0)->len, (int)strlen(p));
    CKSTR(tk(0)->s, "a\nb\tc\\d\"e{f}gA\x7f");
    CKI(tk(0)->nparts, 0);
    CK(tk(0)->parts == NULL); }
  L("\"\"");
  KINDS("STR EOF");
  CKSTR(tk(0)->s, "");
  CKI(tk(0)->len, 2);
  L("\"tab\\there\"");
  CKSTR(tk(0)->s, "tab\there");

  T("interpolation splits into parts");
  L("\"a{x}b\"");
  KINDS("STR EOF");
  CKI(tk(0)->nparts, 3);
  CKSTR(tk(0)->parts[0], "a");
  CKSTR(tk(0)->parts[1], "x");
  CKSTR(tk(0)->parts[2], "b");
  CKSTR(tk(0)->s, "ab");
  CK(tk(0)->pnode == NULL);      /* the expression stays unlexed for the parser */
  L("\"{a}{b}\"");
  CKI(tk(0)->nparts, 5);
  CKSTR(tk(0)->parts[0], "");
  CKSTR(tk(0)->parts[1], "a");
  CKSTR(tk(0)->parts[2], "");
  CKSTR(tk(0)->parts[3], "b");
  CKSTR(tk(0)->parts[4], "");
  L("\"v={ f({k:1}) }\"");        /* nested braces belong to the expression */
  CKI(tk(0)->nparts, 3);
  CKSTR(tk(0)->parts[0], "v=");
  CKSTR(tk(0)->parts[1], " f({k:1}) ");
  CKSTR(tk(0)->parts[2], "");
  CKSTR(tk(0)->s, "v=");
  L("\"s={ f(\"y\") }\"");        /* nested string must not end the outer one */
  CKI(tk(0)->nparts, 3);
  CKSTR(tk(0)->parts[1], " f(\"y\") ");
  CKSTR(tk(0)->s, "s=");
  L("\"a\\{b{x}c\"");             /* escaped brace is data, not interpolation */
  CKI(tk(0)->nparts, 3);
  CKSTR(tk(0)->parts[0], "a{b");
  CKSTR(tk(0)->parts[2], "c");
  L("\"{a}{a}{a}{a}{a}{a}{a}{a}{a}{a}\"");
  CKI(tk(0)->nparts, 21);
  CKSTR(tk(0)->parts[19], "a");
  CKSTR(tk(0)->parts[20], "");

  T("bad strings stop the lexer");
  L("\"abc");
  CKS(lx.err, "lex:1:1: unterminated string");
  CKI(lx.n, 1);
  KINDS("EOF");
  L("x = 1\n\"abc");
  CKS(lx.err, "lex:2:1: unterminated string");
  CKI(lx.err_line, 2); CKI(lx.err_col, 1);
  L("\"a\nb\"");
  CKS(lx.err, "lex:1:1: unterminated string");
  L("'abc");
  CKS(lx.err, "lex:1:1: unterminated string");
  L("\"a\\qb\"");
  CKS(lx.err, "lex:1:3: unknown escape '\\q'");
  L("\"\\x4\"");
  CKS(lx.err, "lex:1:2: bad \\x escape");
  L("\"a{b\"");
  CKS(lx.err, "lex:1:3: unterminated interpolation");
  L("```abc");
  CKS(lx.err, "lex:1:1: unterminated raw string");
  L("1 + \"x\n");
  CK(lx.err != NULL);

  T("raw single-quoted strings");
  L("'a\\nb'");
  KINDS("SQSTR EOF");
  CKSTR(tk(0)->s, "a\\nb");
  L("'it\\'s'");
  KINDS("SQSTR EOF");
  CKSTR(tk(0)->s, "it's");
  L("'a\\\\b'");
  CKSTR(tk(0)->s, "a\\b");
  L("''");
  CKSTR(tk(0)->s, "");
  CKI(tk(0)->len, 2);
  L("'no {x} or \\t here'");
  CKI(tk(0)->nparts, 0);
  CKSTR(tk(0)->s, "no {x} or \\t here");
  L("'C:\\x\\y'");
  CKSTR(tk(0)->s, "C:\\x\\y");

  T("triple backtick raw strings");
  { const char *p = "```\na\nb\n```";
    L(p);
    KINDS("TICKS EOF");
    CKI(tk(0)->nparts, 0);
    CKI(tk(0)->len, (int)strlen(p));
    CKSTR(tk(0)->s, "\na\nb\n"); }
  L("```\r\na\r\n```");
  CKSTR(tk(0)->s, "\na\n");      /* CRLF inside a raw string is normalized */
  L("```a\\nb```");
  CKSTR(tk(0)->s, "a\\nb");
  L("```\"x\" # y```");
  CKSTR(tk(0)->s, "\"x\" # y");
  L("```a`b```");
  CKSTR(tk(0)->s, "a`b");
  L("``````");
  CKSTR(tk(0)->s, "");
  L("```\nx\n```");
  CKI(tk(0)->line, 1);
  CKI(tk(1)->line, 3);

  T("bad characters");
  L("a = 1\n  @\n");
  CKS(lx.err, "lex:2:3: unexpected character '@'");
  CKI(lx.err_line, 2); CKI(lx.err_col, 3);
  L("\xc3\xa9=1");
  CKS(lx.err, "lex:1:1: unexpected character '\xc3\xa9'");
  CKI(lx.err_col, 1);
  L("1 & 2");
  CKS(lx.err, "lex:1:3: unexpected character '&'");
  L("!x");                      /* '!' is unary not (SPEC 1.2) */
  CHECK(!lx.err, "bang should lex, got %s", lx.err ? lx.err : "-");
  CKI(lx.n, 3);
  CKI(lx.toks[0].k, T_KW_NOT);
  L("~ =");
  CKS(lx.err, "lex:1:1: unexpected character '~'");
  L("`");
  CKS(lx.err, "lex:1:1: unexpected character '`'");
  L("``");
  CK(lx.err != NULL);
  L("```x``` = 1");
  CK(lx.err == NULL);

  T("whole program");
  L("def add(a, b) { a + b }\n"
    "x = [1, 2] | add(1)   # pipe\n"
    "if x.len == 2 { out = \"n={x.len}\" }\n");
  KINDS("KW_DEF ID LPAREN ID COMMA ID RPAREN LBRACE ID ADD ID RBRACE NL "
        "ID ASSIGN LBRACK NUM COMMA NUM RBRACK PIPE ID LPAREN NUM RPAREN NL "
        "KW_IF ID DOT ID EQ NUM LBRACE ID ASSIGN STR RBRACE NL EOF");
  CK(lx.err == NULL);
  CKI(lx.n, 39);
  CKI(tk(0)->line, 1);
  CKI(tk(13)->line, 2);
  CKI(tk(26)->line, 3);
  CKI(tk(26)->col, 1);
  CKD(tk(31)->num, 2);
  CKSTR(tk(35)->s, "n=");
  CKI(tk(35)->nparts, 3);
  CKSTR(tk(35)->parts[1], "x.len");
  CKI(tk(38)->k, T_EOF);

  T("token array grows past its first block");
  { Buf b; buf_init(&b, &A);
    for (int i = 0; i < 200; i++) buf_puts(&b, "x ");
    Str src = buf_take(&b);
    lex_run(&A, src.p, (size_t)src.len, &lx);   /* ast.h's second spelling */
    CKI(lx.n, 201);
    CKI(tk(0)->col, 1);
    CKI(tk(1)->col, 3);
    CKI(tk(199)->col, 399);
    CKI(tk(200)->k, T_EOF);
    CKSTR(tk(0)->s, "x"); }

  T("everything lives in the arena");
  { char *heap;
    const char *p1 = "\"ab\" + 'raw{1}'\n";
    const char *p2 = "\"v={ a + 1 }\"";
    heap = (char*)malloc(strlen(p1) + 1);
    strcpy(heap, p1);
    lex(&A, heap, strlen(p1), &lx);
    free(heap);
    KINDS("STR ADD SQSTR NL EOF");
    CKSTR(lx.toks[0].s, "ab");
    CKSTR(lx.toks[2].s, "raw{1}");
    heap = (char*)malloc(strlen(p2) + 1);
    strcpy(heap, p2);
    lex(&A, heap, strlen(p2), &lx);
    free(heap);
    CKI(tk(0)->nparts, 3);
    CKSTR(tk(0)->parts[0], "v=");
    CKSTR(tk(0)->parts[1], " a + 1 ");
    CKSTR(tk(0)->parts[2], "");
    CKSTR(tk(0)->s, "v=");
    CKI(tk(0)->len, (int)strlen(p2)); }

  T_REPORT("lex");
}
