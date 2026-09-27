/* ast.h - tokens, AST nodes, lexer API. Grammar is strict on purpose:
 * VXA never guesses what the author meant (SPEC §1). */
#ifndef VXA_AST_H
#define VXA_AST_H
#include "vxa.h"

typedef enum {
  T_EOF, T_NL, T_NUM, T_STR, T_SQSTR, T_TICKS,   /* "..." '...' ```...``` */
  T_ID,
  /* keywords */
  T_KW_DEF, T_KW_IF, T_KW_ELIF, T_KW_ELSE, T_KW_WHILE, T_KW_FOR, T_KW_IN,
  T_KW_RETURN, T_KW_BREAK, T_KW_CONTINUE, T_KW_TRUE, T_KW_FALSE, T_KW_NULL,
  T_KW_AND, T_KW_OR, T_KW_NOT,
  /* punctuation */
  T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_LBRACE, T_RBRACE,
  T_COMMA, T_SEMI, T_DOT, T_EQ, T_ASSIGN,
  T_ADD, T_SUB, T_MUL, T_DIV, T_MOD, T_LT, T_LE, T_GT, T_GE, T_EEQ, T_NE,
  T_ARROW, T_PIPE, T_SIM, T_QUEST, T_COLON, T_DOTS,
  T_INVALID
} TokKind;

typedef struct {
  TokKind k;
  Str s;              /* identifier name / string literal value */
  double num;         /* T_NUM */
  int line, col;
  int len;            /* source span, for error reporting */
  Str *parts;         /* interpolated string pieces (T_STR only) */
  int nparts;
  Node *pnode;        /* placeholder for interpolated sub-expression */
} Tok;

typedef struct {
  const char *src;
  size_t len;
  size_t pos;
  int line, col;
  Arena *a;
  Tok *toks; int n, cap;
  char *err;          /* non-NULL on lexical error */
  int err_line, err_col;
} Lexer;


/* AST node kinds */
enum {
  NK_INT, NK_FLOAT, NK_STR, NK_INTERP, NK_ID, NK_TRUE, NK_FALSE, NK_NULL,
  NK_BLOCK, NK_LOCAL, NK_FUNC, NK_ASSIGN, NK_IF, NK_WHILE, NK_FOR,
  NK_RETURN, NK_BREAK, NK_CONTINUE, NK_CALL, NK_INDEX, NK_FIELD,
  NK_BIN, NK_NEG, NK_NOT, NK_PAREN, NK_LIST, NK_REC, NK_LAMBDA, NK_PIPE,
  NK_PAIR, NK_RANGE, NK_GLOBAL
};

struct Node {
  int k;                    /* NK_* */
  int line, col;
  Str s;                    /* identifier, literal text, or binary op name */
  double num;               /* NK_INT / NK_FLOAT */
  Node *kids[4]; int nkids; /* up to 4 fixed children */
  Node **items; int nitems; /* call args, block stmts, rec pairs, interp parts */
  Str *names; int nnames;   /* def/for names */
  int nn;                   /* declared arity */
};

void lex_run(Arena *a, const char *src, size_t len, Lexer *lx);
void lex(Arena *a, const char *src, size_t len, Lexer *lx); /* implemented by lex.c */
const char *tok_name(TokKind k);
Node *parse_program(Arena *a, Tok *toks, int n, const char *name, char **err_out, int *eline);
void ast_dump(Arena *a, Node *n, Buf *b);
#endif
