/* interp.h - what the evaluator offers to builtins and the CLI.
 * Builtin authors: use the Args helpers instead of hand-checking V tags; that
 * is what keeps error messages uniform (stable codes, actionable hints). */
#ifndef VXA_INTERP_H
#define VXA_INTERP_H
#include "vxa.h"
#include "ast.h"

struct Ctx;

Ctx *ctx_new(Arena *a, const char *root, const char *script_path);
void ctx_free(Ctx *c);
void ctx_set_confirm(Ctx *c, const char *token);
void ctx_set_argstr(Ctx *c, const char *s);
void ctx_set_strict(Ctx *c, bool on);
void ctx_set_steps(Ctx *c, long long max);
void ctx_set_fmt(Ctx *c, Fmt f);
Fmt  ctx_fmt(Ctx *c);
void ctx_set_anchors(Ctx *c, bool on);
bool ctx_anchors(Ctx *c);
const char *ctx_root(Ctx *c);
const char *ctx_script(Ctx *c);
Arena *ctx_arena(Ctx *c);
char *ctx_confirm(Ctx *c);
void ctx_note_applied(Ctx *c, const char *token);  /* this ctx spent `token` on an apply */
bool ctx_applied(Ctx *c, Str token);              /* did it, or was it replayed? */
Str  ctx_argstr(Ctx *c);
Rec *ctx_cfg(Ctx *c);
long long ctx_steps(Ctx *c);

/* error state */
void set_error(Ctx *c, ErrCode code, const char *fmt, ...);
V    at_err(Ctx *c, Node *n, ErrCode code, const char *fmt, ...);
bool ctx_has_err(Ctx *c);
V    ctx_err(Ctx *c);
void ctx_clear_err(Ctx *c);
Node *cur_node(Ctx *c);                  /* for error position inside builtins */

/* execution */
Node *parse_script(Ctx *c, const char *src, size_t len, const char *name, V *err_out);
V  eval_node(Ctx *c, Node *n);           /* evaluates with a fresh child scope for blocks */
V  eval_in_env(Ctx *c, Node *n, Env *e);
V  call_fn(Ctx *c, V fn, V *args, int nargs);
V  run_builtin(Ctx *c, const char *ns, const char *name, V *args, int nargs);
V  lib_method(Ctx *c, V self, const char *name, V *args, int nargs);

/* environment */
Env *env_new(Arena *a, Env *parent);
V   *env_get(Env *e, const char *name);
void env_set(Arena *a, Env *e, const char *name, V v);
bool env_assign(Env *e, const char *name, V v);
Str  env_dump(Arena *a, Env *e);
V   *env_own(Env *e, const char *name);

/* ---------- argument checking for builtins ---------- */
typedef struct { Ctx *c; V *a; int n; Node *at; } Args;
#define ARGS() (Args){ c, args, nargs, NULL }
V      arg_at(Args *x, int i);                          /* VN when absent */
bool   arg_present(Args *x, int i);
Str    arg_str(Args *x, int i, const char *what);        /* "" and error on bad type */
bool   arg_has_err(Args *x);
double arg_num(Args *x, int i, const char *what);
bool   arg_bool(Args *x, int i, bool dflt);
Rec   *arg_rec(Args *x, int i, const char *what);
List  *arg_list(Args *x, int i, const char *what);
V      arg_opt(Args *x, int i, const char *name, V dflt); /* field of a trailing options record */
int    arg_opt_int(Args *x, int i, const char *name, int dflt);
Str    arg_opt_str(Args *x, int i, const char *name, const char *dflt);
bool   arg_opt_bool(Args *x, int i, const char *name, bool dflt);
Str    as_text(Ctx *c, V v);                            /* string form for display/interp */

/* convenience constructors */
V   ok_rec(Arena *a);
V   v_ok(Arena *a, int n, ...);           /* pairs: const char* key, V value */
Str q(Arena *a, const char *s);           /* owned literal string value */
#endif
