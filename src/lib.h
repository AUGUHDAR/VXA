/* lib.h - the single source of truth for builtins.
 * Every builtin registers its function, signature, one-line doc and one example
 * in the SAME row, so `vxa doc` can never drift from the implementation. */
#ifndef VXA_LIB_H
#define VXA_LIB_H
#include "vxa.h"

typedef V (*BFn)(Ctx *c, V *args, int nargs);

typedef struct Builtin {
  const char *ns;      /* "fs", "tx", "sh", "out", "cfg", "sim", "task", "" for globals */
  const char *name;    /* "read" -> fs.read */
  BFn fn;
  int min, max;        /* arity; max < 0 means variadic */
  const char *sig;     /* fs.read(path, {lines, grep, max_tok, anchors}) -> {path,hash,lines,total,tok,text} */
  const char *doc;     /* one line: what it is for, from the agent's point of view */
  const char *ex;      /* one runnable example, <= 80 chars */
  int flags;           /* BF_PURE: no side effects, safe in a plan pass */
} Builtin;

enum { BF_PURE = 1 };

/* each namespace exports its table */
const Builtin *t_fs(int *n);
const Builtin *t_tx(int *n);
const Builtin *t_sh(int *n);
const Builtin *t_out(int *n);
const Builtin *t_cfg(int *n);
const Builtin *t_misc(int *n);   /* len, keys, vals, type, str, num, abs, min, max, ... */

const Builtin *lib_find(const char *ns, const char *name);   /* exact match only */
const Builtin *lib_suggest(const char *ns, const char *name); /* for error text, never dispatch */
const Builtin *lib_all(int *count);        /* flattened, stable order */
V lib_call(Ctx *c, const Builtin *b, V *args, int nargs);

/* methods: x.len, x.map(...), s.split(...). Dispatched by value type. */
typedef struct Method {
  const char *type;    /* "str" "list" "rec" "num" "any" */
  const char *name;
  BFn fn;
  int min, max;
  const char *sig;
  const char *doc;
} Method;
const Method *meth_find(const char *type, const char *name);
const Method *meth_all(int *count);

/* doc rendering (used by `vxa doc`, `vxa lang` and docs/GENERATED.md) */
void doc_namespace(Arena *a, Buf *b, const char *ns, bool full);
void doc_one(Arena *a, Buf *b, const char *qualified);
void doc_lang(Arena *a, Buf *b, int level);   /* level 1 = cheat sheet, 2 = +grammar, 3 = +all builtins */
#endif
