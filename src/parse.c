/* parse.c - tokens -> AST. Recursive descent, strict grammar (SPEC §1).
 * Errors are reported as "<file>:<line>:<col>: <message>" and stop the run.
 * No repair, no fuzzy recovery: an agent must never act on a program we
 * silently "guessed" it meant. */
#include "vxa.h"
#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#define MAX_DEPTH 200

typedef struct {
  Tok *t; int n, i;
  Arena *a;
  const char *name;
  char err[512];
  int depth;
} P;

/* ---------- node helpers ---------- */
static Node *nnew(Arena *a, int k, int line, int col) {
  Node *n = (Node*)arena_zalloc(a, sizeof(Node));
  n->k = k; n->line = line; n->col = col;
  return n;
}
static void pushkid(Node *n, Node *k) { if (n->nkids < 4) n->kids[n->nkids++] = k; }
/* Fixed-layout child slot. nkids must stay truthful: interp.c reads it to tell a
 * slice from an index and a bare 'return' from 'return expr', and ast_dump walks
 * it. Assigning kids[i] by hand without it leaves the node half-built. */
static void setkid(Node *n, int i, Node *k) {
  n->kids[i] = k;
  if (i + 1 > n->nkids) n->nkids = i + 1;
}

/* growable node list kept on the arena */
typedef struct { Node **v; int len, cap; } NList;
static void nl_push(Arena *a, NList *l, Node *n) {
  if (l->len == l->cap) {
    int nc = l->cap ? l->cap * 2 : 4;
    Node **nv = (Node**)arena_zalloc(a, sizeof(Node*) * (size_t)nc);
    if (l->cap) memcpy(nv, l->v, sizeof(Node*) * (size_t)l->cap);
    l->v = nv; l->cap = nc;
  }
  l->v[l->len++] = n;
}
typedef struct { Str *v; int len, cap; } SList;
static void sl_push(Arena *a, SList *l, Str s) {
  if (l->len == l->cap) {
    int nc = l->cap ? l->cap * 2 : 4;
    Str *nv = (Str*)arena_zalloc(a, sizeof(Str) * (size_t)nc);
    if (l->cap) memcpy(nv, l->v, sizeof(Str) * (size_t)l->cap);
    l->v = nv; l->cap = nc;
  }
  l->v[l->len++] = s;
}
static void attach_items(Arena *a, Node *n, NList *l) {
  n->nitems = l->len;
  if (l->len) {
    n->items = (Node**)arena_zalloc(a, sizeof(Node*) * (size_t)l->len);
    memcpy(n->items, l->v, sizeof(Node*) * (size_t)l->len);
  }
}
static void attach_names(Arena *a, Node *n, SList *l) {
  n->nnames = l->len;
  if (l->len) {
    n->names = (Str*)arena_zalloc(a, sizeof(Str) * (size_t)l->len);
    memcpy(n->names, l->v, sizeof(Str) * (size_t)l->len);
  }
  n->nn = l->len;
}

/* ---------- errors ---------- */
static void perr(P *p, Tok *t, const char *fmt, ...) {
  if (p->err[0]) return;
  char msg[320];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  const char *got = t->k == T_EOF ? "end of file" : tok_name(t->k);
  char disp[64];
  if (t->k == T_ID || t->k == T_STR || t->k == T_SQSTR) {
    snprintf(disp, sizeof disp, " '%.*s'", t->s.len > 24 ? 24 : t->s.len, t->s.p ? t->s.p : "");
  } else disp[0] = 0;
  snprintf(p->err, sizeof p->err, "%s:%d:%d: unexpected %s%s: %s",
           p->name, t->line, t->col, got, disp, msg);
}
static Tok *peek(P *p) { return (p->i < p->n) ? &p->t[p->i] : &p->t[p->n ? p->n - 1 : 0]; }
static Tok *peek_at(P *p, int off) {
  int j = p->i + off;
  return (j >= 0 && j < p->n) ? &p->t[j] : peek(p);
}
static Tok *adv(P *p) { Tok *t = peek(p); if (p->i < p->n) p->i++; return t; }
static bool at(P *p, TokKind k) { return peek(p)->k == k; }
static bool eat(P *p, TokKind k) { if (at(p, k)) { p->i++; return true; } return false; }
static void skipnl(P *p) { while (at(p, T_NL)) p->i++; }
/* The lexer only silences line breaks inside [] and (), so a T_NL can show up
 * anywhere inside a {} - block or record, at the start, at the end, in runs.
 * Both spellings of a statement separator may repeat, and either may sit right
 * before '}' or end of file. This is used only where a statement boundary is
 * legal (block bodies, program body): inside an expression just newlines are
 * skipped, so a ';' never disappears into a value. What stays an error is two
 * statements with nothing between them - VXA does not guess where one ends. */
static void skip_sep(P *p) { while (at(p, T_NL) || at(p, T_SEMI)) p->i++; }
static bool at_sep(P *p) { return at(p, T_NL) || at(p, T_SEMI); }
/* A lambda arrow: the lexer's spelling is '->' (T_ARROW); the spelling SPEC
 * uses is '=>', which arrives as T_ASSIGN T_GT. Accept both, and let either end
 * a line: an operator may always be the last thing on a line (SPEC 1.1). */
static bool at_arrow(P *p) {
  if (at(p, T_ARROW)) return true;
  return at(p, T_ASSIGN) && peek_at(p, 1)->k == T_GT;
}
static void eat_arrow(P *p) {
  if (at(p, T_ARROW)) p->i++;
  else p->i += 2;                    /* T_ASSIGN then T_GT, never reversed */
  skipnl(p);
}
static bool expect(P *p, TokKind k, const char *what) {
  if (at(p, k)) { p->i++; return true; }
  perr(p, peek(p), "expected %s", what);
  return false;
}

/* ---------- precedence ladder ---------- */
enum { LVL_OR, LVL_AND, LVL_CMP, LVL_SIM, LVL_ADD, LVL_MUL, LVL_UNARY };

static Node *p_expr(P *p);
static Node *p_bin(P *p);
static Node *p_block(P *p);
static Node *p_primary(P *p);
static Node *p_stmt(P *p);
static Node *p_unary(P *p);
static Node *p_postfix(P *p);

static const char *op_for(TokKind k, int lvl) {
  switch (lvl) {
    case LVL_MUL:
      if (k == T_MUL) return "*";
      if (k == T_DIV) return "/";
      if (k == T_MOD) return "%";
      break;
    case LVL_ADD:
      if (k == T_ADD) return "+";
      if (k == T_SUB) return "-";
      break;
    case LVL_SIM:
      if (k == T_SIM) return "~=";
      break;
    case LVL_CMP:
      if (k == T_EEQ) return "==";
      if (k == T_NE) return "!=";
      if (k == T_LT) return "<";
      if (k == T_LE) return "<=";
      if (k == T_GT) return ">";
      if (k == T_GE) return ">=";
      break;
    case LVL_AND:
      if (k == T_KW_AND) return "and";
      break;
    case LVL_OR:
      if (k == T_KW_OR) return "or";
      break;
    default: break;
  }
  return NULL;
}

static Node *p_lvl(P *p, int lvl) {
  if (lvl >= LVL_UNARY) return p_unary(p);
  Node *lhs = p_lvl(p, lvl + 1);
  if (!lhs || p->err[0]) return lhs;
  for (;;) {
    Tok *t = peek(p);
    const char *op = op_for(t->k, lvl);
    if (!op) break;
    adv(p);
    skipnl(p);            /* an operator may end a line */
    Node *rhs = p_lvl(p, lvl + 1);
    if (!rhs || p->err[0]) return NULL;
    Node *n = nnew(p->a, NK_BIN, t->line, t->col);
    n->s = s_lit(p->a, op);
    pushkid(n, lhs); pushkid(n, rhs);
    lhs = n;
  }
  return lhs;
}
static Node *p_unary(P *p) {
  if (at(p, T_SUB)) {
    Tok *t = adv(p);
    skipnl(p);                 /* an operator may end a line: '-' too */
    Node *e = p_unary(p);
    if (!e || p->err[0]) return NULL;
    if (e->k == NK_INT || e->k == NK_FLOAT) {
      e->num = -e->num;
      Buf b; buf_init(&b, p->a); fmt_num(&b, e->num);
      e->s = buf_take(&b);
      return e;
    }
    Node *n = nnew(p->a, NK_NEG, t->line, t->col);
    pushkid(n, e);
    return n;
  }
  if (at(p, T_KW_NOT)) {
    Tok *t = adv(p);
    skipnl(p);                 /* ... and so may 'not' */
    Node *e = p_unary(p);
    if (!e || p->err[0]) return NULL;
    Node *n = nnew(p->a, NK_NOT, t->line, t->col);
    pushkid(n, e);
    return n;
  }
  return p_postfix(p);
}

/* ---------- literals ---------- */
static Node *mk_num(P *p, Tok *t) {
  double d = t->num;
  bool integ = (d == (double)(long long)d) && d > -1e15 && d < 1e15;
  Node *n = nnew(p->a, integ ? NK_INT : NK_FLOAT, t->line, t->col);
  n->num = d;
  Buf b; buf_init(&b, p->a); fmt_num(&b, d);
  n->s = buf_take(&b);
  return n;
}

static Node *mk_interp(P *p, Tok *t) {
  Node *n = nnew(p->a, NK_STR, t->line, t->col);
  n->s = t->s;
  if (t->nparts <= 0) return n;
  NList l; memset(&l, 0, sizeof l);
  n->k = NK_INTERP;
  for (int i = 0; i < t->nparts; i++) {
    if (i % 2 == 0) {
      Node *lit = nnew(p->a, NK_STR, t->line, t->col);
      lit->s = t->parts[i];
      nl_push(p->a, &l, lit);
    } else {
      Lexer sub; memset(&sub, 0, sizeof sub);
      sub.a = p->a;
      lex(p->a, t->parts[i].p, (size_t)t->parts[i].len, &sub);
      if (sub.err) { perr(p, t, "in string interpolation: %s", sub.err); return NULL; }
      P sp; memset(&sp, 0, sizeof sp);
      sp.a = p->a; sp.name = p->name; sp.t = sub.toks; sp.n = sub.n;
      Node *e = p_expr(&sp);
      if (sp.err[0]) { perr(p, t, "in string interpolation: %s", sp.err); return NULL; }
      if (!e) { perr(p, t, "empty interpolation {}"); return NULL; }
      nl_push(p->a, &l, e);
    }
  }
  attach_items(p->a, n, &l);
  return n;
}

/* ---------- postfix ---------- */
static Node *p_call(P *p, Node *fn) {
  Tok *t = adv(p);   /* '(' */
  Node *call = nnew(p->a, NK_CALL, fn->line, fn->col);
  setkid(call, 0, fn);
  NList l; memset(&l, 0, sizeof l);
  skipnl(p);
  if (!at(p, T_RPAREN)) {
    for (;;) {
      skipnl(p);
      Node *arg = p_expr(p);
      if (!arg || p->err[0]) return NULL;
      nl_push(p->a, &l, arg);
      skipnl(p);
      if (eat(p, T_COMMA)) { skipnl(p); if (at(p, T_RPAREN)) break; continue; }
      break;
    }
  }
  if (!expect(p, T_RPAREN, "')' to close the call")) return NULL;
  attach_items(p->a, call, &l);
  (void)t;
  return call;
}

static Node *p_postfix(P *p) {
  Node *cur = p_primary(p);
  if (!cur || p->err[0]) return cur;
  for (;;) {
    if (at(p, T_LPAREN)) {
      cur = p_call(p, cur);
      if (!cur || p->err[0]) return NULL;
    } else if (at(p, T_LBRACK)) {
      Tok *t = adv(p);
      Node *idx = nnew(p->a, NK_INDEX, cur->line, cur->col);
      setkid(idx, 0, cur);
      skipnl(p);
      /* p_bin, not p_expr: 'a[1..2]' is a slice, so the '..' must be left for the
       * slice branch below instead of being eaten as a range by the operand. */
      setkid(idx, 1, p_bin(p));
      if (!idx->kids[1] || p->err[0]) return NULL;
      skipnl(p);
      if (eat(p, T_DOTS)) {
        idx->nkids = 3;           /* a slice; kids[2] stays null for 'a[2..]' */
        skipnl(p);
        if (!at(p, T_RBRACK)) {
          setkid(idx, 2, p_expr(p));
          if (!idx->kids[2] || p->err[0]) return NULL;
        }
      }
      skipnl(p);
      if (!expect(p, T_RBRACK, "']'")) return NULL;
      cur = idx;
      (void)t;
    } else if (at(p, T_DOT)) {
      adv(p);
      if (!at(p, T_ID)) { perr(p, peek(p), "expected a field or method name after '.'"); return NULL; }
      Tok *f = adv(p);
      Node *fd = nnew(p->a, NK_FIELD, f->line, f->col);
      setkid(fd, 0, cur);
      fd->s = f->s;
      cur = fd;
    } else break;
  }
  return cur;
}

/* ---------- collections ---------- */
static Node *p_list(P *p) {
  Tok *t = adv(p);
  Node *n = nnew(p->a, NK_LIST, t->line, t->col);
  NList l; memset(&l, 0, sizeof l);
  skipnl(p);
  if (!at(p, T_RBRACK)) {
    for (;;) {
      skipnl(p);
      Node *e = p_expr(p);
      if (!e || p->err[0]) return NULL;
      nl_push(p->a, &l, e);
      skipnl(p);
      if (eat(p, T_COMMA)) { skipnl(p); if (at(p, T_RBRACK)) break; continue; }
      break;
    }
  }
  if (!expect(p, T_RBRACK, "']'")) return NULL;
  attach_items(p->a, n, &l);
  return n;
}

static Node *p_record(P *p) {
  Tok *t = adv(p);
  Node *n = nnew(p->a, NK_REC, t->line, t->col);
  NList l; memset(&l, 0, sizeof l);
  skipnl(p);
  if (!at(p, T_RBRACE)) {
    for (;;) {
      skipnl(p);
      Tok *kt = peek(p);
      Node *pair = nnew(p->a, NK_PAIR, kt->line, kt->col);
      if (kt->k == T_ID) { adv(p); pair->s = kt->s; }
      else if (kt->k == T_STR || kt->k == T_SQSTR) { adv(p); pair->s = kt->s; }
      else if (kt->k == T_NUM) { adv(p); pair->s = kt->s.len ? kt->s : s_fmt(p->a, "%g", kt->num); }
      else { perr(p, kt, "expected a record key (name or \"quoted\")"); return NULL; }
      if (!expect(p, T_COLON, "':' after the record key")) return NULL;
      skipnl(p);
      Node *v = p_expr(p);
      if (!v || p->err[0]) return NULL;
      setkid(pair, 0, v);
      nl_push(p->a, &l, pair);
      skipnl(p);
      if (eat(p, T_COMMA)) { skipnl(p); if (at(p, T_RBRACE)) break; continue; }
      break;
    }
  }
  if (!expect(p, T_RBRACE, "'}' to close the record")) return NULL;
  attach_items(p->a, n, &l);
  return n;
}

/* ---------- lambdas ---------- */
static Node *mk_lambda_body(P *p, Node *lam) {
  skipnl(p);                      /* the head and its body may sit on separate lines */
  if (at(p, T_LBRACE)) {
    Node *b = p_block(p);
    if (!b || p->err[0]) return NULL;
    pushkid(lam, b);
    return lam;
  }
  Node *e = p_expr(p);
  if (!e || p->err[0]) return NULL;
  Node *b = nnew(p->a, NK_BLOCK, e->line, e->col);
  NList l; memset(&l, 0, sizeof l);
  nl_push(p->a, &l, e);
  attach_items(p->a, b, &l);
  pushkid(lam, b);
  return lam;
}

static bool p_param_names(P *p, SList *l, const char *what) {
  if (at(p, T_RPAREN)) return true;
  for (;;) {
    if (!at(p, T_ID)) { perr(p, peek(p), "expected %s", what); return false; }
    sl_push(p->a, l, adv(p)->s);
    if (!eat(p, T_COMMA)) break;
    skipnl(p);
  }
  return true;
}

/* paren form: grouping, or a lambda head "(a,b)=>..." */
static Node *p_paren(P *p) {
  Tok *t = adv(p);            /* '(' */
  skipnl(p);
  if (at(p, T_RPAREN)) {      /* () => body */
    adv(p);
    if (at_arrow(p)) {
      eat_arrow(p);
      Node *lam = nnew(p->a, NK_LAMBDA, t->line, t->col);
      return mk_lambda_body(p, lam);
    }
    Node *nil = nnew(p->a, NK_ID, t->line, t->col);
    nil->s = s_lit(p->a, "null");
    return nil;
  }
  NList l; memset(&l, 0, sizeof l);
  for (;;) {
    Node *e = p_expr(p);
    if (!e || p->err[0]) return NULL;
    nl_push(p->a, &l, e);
    skipnl(p);
    if (eat(p, T_COMMA)) { skipnl(p); continue; }
    break;
  }
  if (!expect(p, T_RPAREN, "')'")) return NULL;
  if (at_arrow(p)) {
    eat_arrow(p);
    Node *lam = nnew(p->a, NK_LAMBDA, t->line, t->col);
    SList ns; memset(&ns, 0, sizeof ns);
    for (int i = 0; i < l.len; i++) {
      if (l.v[i]->k != NK_ID) { perr(p, peek(p), "a lambda parameter must be a plain name"); return NULL; }
      sl_push(p->a, &ns, l.v[i]->s);
    }
    attach_names(p->a, lam, &ns);
    return mk_lambda_body(p, lam);
  }
  if (l.len != 1) { perr(p, peek(p), "a parenthesised expression must be one value (use a list [...] for several)"); return NULL; }
  return l.v[0];
}

static Node *p_primary(P *p) {
  int saved_depth = p->depth + 1;
  if (saved_depth > MAX_DEPTH) { perr(p, peek(p), "expression nested too deeply (limit %d)", MAX_DEPTH); return NULL; }
  p->depth = saved_depth;
  Tok *t = peek(p);
  Node *r = NULL;
  bool allow_arrow = true;
  switch (t->k) {
    case T_NUM: adv(p); r = mk_num(p, t); break;
    case T_STR: case T_SQSTR: case T_TICKS: adv(p); r = mk_interp(p, t); break;
    case T_ID: {
      adv(p);
      r = nnew(p->a, NK_ID, t->line, t->col);
      r->s = t->s;
      break;
    }
    case T_KW_TRUE: case T_KW_FALSE: case T_KW_NULL: {
      adv(p);
      r = nnew(p->a, NK_ID, t->line, t->col);
      r->s = s_lit(p->a, t->k == T_KW_TRUE ? "true" : t->k == T_KW_FALSE ? "false" : "null");
      allow_arrow = false;
      break;
    }
    case T_LPAREN: r = p_paren(p); allow_arrow = false; break;
    case T_LBRACK: r = p_list(p); allow_arrow = false; break;
    case T_LBRACE: r = p_record(p); allow_arrow = false; break;
    case T_KW_DEF: {
      if (peek_at(p, 1)->k == T_LPAREN) {
        adv(p); adv(p);
        Node *lam = nnew(p->a, NK_LAMBDA, t->line, t->col);
        SList l; memset(&l, 0, sizeof l);
        if (!p_param_names(p, &l, "a parameter name")) return NULL;
        if (!expect(p, T_RPAREN, "')'")) return NULL;
        attach_names(p->a, lam, &l);
        if (at_arrow(p)) eat_arrow(p);
        r = mk_lambda_body(p, lam);
        allow_arrow = false;
        break;
      }
      perr(p, t, "'def' with a name is a statement, not an expression");
      break;
    }
    default:
      perr(p, t, "expected a value, a name or '(' here");
      break;
  }
  if (r && allow_arrow && at_arrow(p) && r->k == NK_ID) {
    eat_arrow(p);
    Node *lam = nnew(p->a, NK_LAMBDA, t->line, t->col);
    SList l; memset(&l, 0, sizeof l);
    sl_push(p->a, &l, r->s);
    attach_names(p->a, lam, &l);
    r = mk_lambda_body(p, lam);
  }
  p->depth = saved_depth - 1;
  return r;
}

/* ---------- expression entry: range then pipe ---------- */
static Node *p_bin(P *p) { return p_lvl(p, LVL_OR); }

/* '..' is a single, non-associative operator between two binary expressions:
 * it binds tighter than ',' (list items, call args) and than '|', so the whole
 * of 'lo' and the whole of 'hi' are decided before the range node closes and
 * nothing after 'hi' can leak into it. A second '..' is a shape the grammar
 * does not have, so it is named here rather than left over for the statement
 * loop to misreport as a missing separator. */
static Node *p_range(P *p) {
  Node *a = p_bin(p);
  if (!a || p->err[0]) return a;
  if (at(p, T_DOTS)) {
    Tok *dt = adv(p);
    Node *n = nnew(p->a, NK_RANGE, a->line, a->col);
    pushkid(n, a);
    skipnl(p);
    Node *b = p_bin(p);
    if (!b || p->err[0]) return NULL;
    pushkid(n, b);
    if (at(p, T_DOTS)) {
      perr(p, dt, "'..' takes one value on each side and does not chain: write a list ([1..3, 4..6]) or a call for that");
      return NULL;
    }
    return n;
  }
  return a;
}

static Node *p_pipe(P *p) {
  Node *lhs = p_range(p);
  if (!lhs || p->err[0]) return lhs;
  while (at(p, T_PIPE)) {
    Tok *t = adv(p);
    skipnl(p);
    Node *rhs = p_range(p);
    if (!rhs || p->err[0]) return NULL;
    if (rhs->k != NK_CALL) {
      perr(p, t, "after '|' a call is required (pipe inserts the left side as the first argument)");
      return NULL;
    }
    Node *n = nnew(p->a, NK_PIPE, lhs->line, lhs->col);
    pushkid(n, lhs); pushkid(n, rhs);
    lhs = n;
  }
  return lhs;
}

static Node *p_expr(P *p) { return p_pipe(p); }

/* ---------- statements ---------- */
/* One statement, then a run of separators, until the closing '}'. The lexer
 * emits T_NL inside braces, so leading, trailing and repeated separators are
 * all normal here; what stays an error is two statements with nothing between
 * them (SPEC axiom: never guess where one statement ends). */
static Node *p_block(P *p) {
  Tok *t = peek(p);
  if (!expect(p, T_LBRACE, "'{'")) return NULL;
  Node *b = nnew(p->a, NK_BLOCK, t->line, t->col);
  NList l; memset(&l, 0, sizeof l);
  skip_sep(p);
  while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
    Node *s = p_stmt(p);
    if (p->err[0]) return NULL;
    if (s) nl_push(p->a, &l, s);
    if (at_sep(p)) { skip_sep(p); continue; }
    if (at(p, T_RBRACE) || at(p, T_EOF)) break;
    perr(p, peek(p), "statements must be separated by a newline or ';'");
    return NULL;
  }
  if (!expect(p, T_RBRACE, "'}' to close the block")) return NULL;
  attach_items(p->a, b, &l);
  return b;
}

static Node *p_if(P *p) {
  Tok *t = adv(p);
  skipnl(p);
  Node *n = nnew(p->a, NK_IF, t->line, t->col);
  setkid(n, 0, p_expr(p));
  if (!n->kids[0] || p->err[0]) return NULL;
  skipnl(p);
  setkid(n, 1, p_block(p));
  if (!n->kids[1] || p->err[0]) return NULL;
  /* 'else' / 'elif' may sit on the line after the closing '}' of this branch:
   * a run of newlines there is a continuation of this statement, and anything
   * else on the next line still ends it (the position is rewound below). */
  int save = p->i;
  skipnl(p);
  if (at(p, T_KW_ELIF)) {
    Node *e = p_if(p);
    if (!e || p->err[0]) return NULL;
    setkid(n, 2, e);
  } else if (at(p, T_KW_ELSE)) {
    adv(p);
    skipnl(p);
    setkid(n, 2, p_block(p));
    if (!n->kids[2] || p->err[0]) return NULL;
  } else {
    p->i = save;
  }
  return n;
}

static Node *p_def(P *p) {
  Tok *t = adv(p);
  if (at(p, T_LPAREN)) return p_primary(p);      /* def(a,b){...} = an anonymous lambda */
  if (!at(p, T_ID)) { perr(p, peek(p), "expected a function name after 'def'"); return NULL; }
  Tok *nt = adv(p);
  Node *lam = nnew(p->a, NK_LAMBDA, t->line, t->col);
  SList l; memset(&l, 0, sizeof l);
  if (at(p, T_LPAREN)) {
    adv(p);
    if (!p_param_names(p, &l, "a parameter name")) return NULL;
    if (!expect(p, T_RPAREN, "')' after the parameter list")) return NULL;
  }
  attach_names(p->a, lam, &l);
  skipnl(p);
  if (!at(p, T_LBRACE)) { perr(p, peek(p), "expected '{' for the function body"); return NULL; }
  Node *b = p_block(p);
  if (!b || p->err[0]) return NULL;
  pushkid(lam, b);
  Node *n = nnew(p->a, NK_FUNC, t->line, t->col);
  n->s = nt->s;
  setkid(n, 0, lam);
  return n;
}

static Node *p_for(P *p) {
  Tok *t = adv(p);
  skipnl(p);
  Node *n = nnew(p->a, NK_FOR, t->line, t->col);
  SList l; memset(&l, 0, sizeof l);
  for (;;) {
    if (!at(p, T_ID)) { perr(p, peek(p), "expected a loop variable"); return NULL; }
    sl_push(p->a, &l, adv(p)->s);
    if (!eat(p, T_COMMA)) break;
    skipnl(p);
  }
  if (!expect(p, T_KW_IN, "'in'")) return NULL;
  skipnl(p);
  setkid(n, 0, p_expr(p));
  if (!n->kids[0] || p->err[0]) return NULL;
  attach_names(p->a, n, &l);
  skipnl(p);
  setkid(n, 1, p_block(p));
  if (!n->kids[1] || p->err[0]) return NULL;
  return n;
}

static Node *p_stmt(P *p) {
  Tok *t = peek(p);
  switch (t->k) {
    case T_KW_IF: return p_if(p);
    case T_KW_DEF: return p_def(p);
    case T_KW_WHILE: {
      adv(p); skipnl(p);
      Node *n = nnew(p->a, NK_WHILE, t->line, t->col);
      setkid(n, 0, p_expr(p));
      if (!n->kids[0] || p->err[0]) return NULL;
      skipnl(p);
      setkid(n, 1, p_block(p));
      if (!n->kids[1] || p->err[0]) return NULL;
      return n;
    }
    case T_KW_FOR: return p_for(p);
    case T_KW_RETURN: {
      adv(p);
      Node *n = nnew(p->a, NK_RETURN, t->line, t->col);
      /* a value on the same line is the result; a separator, '}' or EOF means
       * bare 'return' and the value belongs to the next statement */
      if (!at_sep(p) && !at(p, T_RBRACE) && !at(p, T_EOF)) {
        skipnl(p);
        setkid(n, 0, p_expr(p));
        if (!n->kids[0] || p->err[0]) return NULL;
      }
      return n;
    }
    case T_KW_BREAK: adv(p); return nnew(p->a, NK_BREAK, t->line, t->col);
    case T_KW_CONTINUE: adv(p); return nnew(p->a, NK_CONTINUE, t->line, t->col);
    default: break;
  }
  Node *e = p_expr(p);
  if (!e || p->err[0]) return e;
  if (at(p, T_ASSIGN)) {
    Tok *at_t = adv(p);
    skipnl(p);
    Node *v = p_expr(p);
    if (!v || p->err[0]) return NULL;
    if (e->k != NK_ID && e->k != NK_INDEX && e->k != NK_FIELD) {
      perr(p, at_t, "the left side of '=' must be a name, an [index] or a .field");
      return NULL;
    }
    Node *n = nnew(p->a, NK_ASSIGN, e->line, e->col);
    setkid(n, 0, e); setkid(n, 1, v);
    return n;
  }
  return e;
}

/* ---------- entry points ---------- */
Node *parse_program(Arena *a, Tok *toks, int n, const char *name, char **err_out, int *eline) {
  P p; memset(&p, 0, sizeof p);
  p.a = a; p.name = name ? name : "<script>";
  if (!toks || n <= 0) {
    if (err_out) *err_out = arena_strdup(a, "empty input");
    return NULL;
  }
  p.t = toks; p.n = n;
  if (toks[n - 1].k != T_EOF) {
    if (err_out) *err_out = arena_strdup(a, "internal: token stream not terminated");
    return NULL;
  }
  Node *prog = nnew(a, NK_BLOCK, 1, 1);
  NList l; memset(&l, 0, sizeof l);
  skip_sep(&p);
  while (!at(&p, T_EOF)) {
    Node *s = p_stmt(&p);
    if (p.err[0]) {
      if (err_out) *err_out = arena_strdup(a, p.err);
      if (eline) *eline = s ? s->line : 0;
      return NULL;
    }
    if (s) nl_push(a, &l, s);
    if (at_sep(&p)) { skip_sep(&p); continue; }
    if (at(&p, T_EOF)) break;
    perr(&p, peek(&p), "statements must be separated by a newline or ';'");
    if (err_out) *err_out = arena_strdup(a, p.err);
    return NULL;
  }
  attach_items(a, prog, &l);
  return prog;
}

static const char *kindname(int k) {
  static const char *names[] = {
    "INT", "FLOAT", "STR", "INTERP", "ID", "TRUE", "FALSE", "NULL", "BLOCK", "LOCAL",
    "FUNC", "ASSIGN", "IF", "WHILE", "FOR", "RETURN", "BREAK", "CONTINUE", "CALL",
    "INDEX", "FIELD", "BIN", "NEG", "NOT", "PAREN", "LIST", "REC", "LAMBDA", "PIPE",
    "PAIR", "RANGE", "GLOBAL"
  };
  return (k >= 0 && k < (int)(sizeof(names)/sizeof(names[0]))) ? names[k] : "?";
}

void ast_dump(Arena *a, Node *n, Buf *b) {
  (void)a;
  if (!n) { buf_puts(b, "-"); return; }
  buf_fmt(b, "%s@%d", kindname(n->k), n->line);
  if (n->k == NK_ID || n->k == NK_STR || n->k == NK_FIELD || n->k == NK_BIN || n->k == NK_PAIR) {
    buf_putc(b, '('); buf_add_escaped(b, n->s); buf_putc(b, ')');
  }
  if (n->k == NK_INT || n->k == NK_FLOAT) buf_fmt(b, "(%g)", n->num);
  if (n->nnames) {
    buf_puts(b, "[");
    for (int i = 0; i < n->nnames; i++) { if (i) buf_putc(b, ','); buf_add_escaped(b, n->names[i]); }
    buf_puts(b, "]");
  }
  if (n->nkids) {
    buf_puts(b, "{");
    for (int i = 0; i < n->nkids; i++) { if (i) buf_putc(b, ' '); ast_dump(a, n->kids[i], b); }
    buf_puts(b, "}");
  }
  if (n->nitems) {
    buf_puts(b, "<");
    for (int i = 0; i < n->nitems; i++) { if (i) buf_putc(b, ' '); ast_dump(a, n->items[i], b); }
    buf_puts(b, ">");
  }
}
