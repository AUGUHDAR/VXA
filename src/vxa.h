/* vxa.h - VXA agent-native language: core types + utility API. C17, stdlib only.
 * Contract for all modules. Semantics documented in ../SPEC.md */
#ifndef VXA_H
#define VXA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define VXA_VERSION "0.1.0"

/* ---------- process exit codes (SPEC §2.2) ---------- */
enum {
  X_OK = 0, X_USAGE = 1, X_SCRIPT = 2, X_CONFIRM = 3,
  X_POLICY = 4, X_IO = 5, X_INTERNAL = 6, X_TIMEOUT = 7
};

/* ---------- stable error codes: agents branch on these, never on prose ---------- */
typedef enum {
  E_NONE = 0,
  E_PARSE, E_SYNTAX, E_UNDEF, E_TYPE, E_ARITY, E_RANGE, E_GUARD, E_OOM,
  E_NOENT, E_DENIED, E_EXISTS, E_IO, E_OUTSIDE_JAIL, E_NOTDIR, E_ISDIR,
  E_NEED_CONFIRM, E_STALE_PLAN, E_BAD_TOKEN, E_POLICY,
  E_ANCHOR_MISS, E_ANCHOR_DUP, E_BAD_INPUT, E_REGEX, E_TIMEOUT,
  E_UNSUPPORTED, E_LIMIT, E_INTERNAL, E_SH
} ErrCode;

const char *err_name(ErrCode c);   /* "NEED_CONFIRM" */
const char *err_hint(ErrCode c);   /* one actionable line */
int  err_exit(ErrCode c);          /* mapped process exit code */

/* ---------- arena: script-lifetime allocation, no GC ---------- */
typedef struct Blk { struct Blk *next; size_t off, cap; } Blk;
typedef struct Arena { Blk *cur; size_t total, limit, nalloc; } Arena;

void  arena_init(Arena *a, size_t limit);
void *arena_alloc(Arena *a, size_t n);
void *arena_zalloc(Arena *a, size_t n);
char *arena_strdup(Arena *a, const char *s);
char *arena_strndup(Arena *a, const char *s, size_t n);
void  arena_free(Arena *a);

/* ---------- Str: counted; owned strings are NUL-terminated at p[len] ---------- */
typedef struct { int len; char *p; } Str;

Str  s_null(void);
Str  s_lit(Arena *a, const char *cstr);
Str  s_wrap(const char *cstr);
Str  s_from(Arena *a, const char *p, size_t n);
Str  s_fmt(Arena *a, const char *fmt, ...);
bool s_eq(Str a, Str b);
bool s_eqz(Str a, const char *b);
bool s_ni(Str a, const char *prefix);
int  s_cmp(Str a, Str b);
Str  s_concat(Arena *a, Str x, Str y);
Str  s_slice(Str s, int from, int to);            /* loaned view, NOT NUL-terminated */
Str  s_cut(Arena *a, Str s, int from, int to);      /* owned, NUL-terminated copy */
Str  s_lower(Arena *a, Str s);
Str  s_upper(Arena *a, Str s);
Str  s_trim(Arena *a, Str s);
int  s_find(Str hay, Str needle, int from);
int  s_findz(Str hay, const char *needle, int from);
int  s_rfind(Str hay, Str needle);
int  s_count_char(Str s, char c);
bool s_is_int(Str s, long long *out);
int  s_ucount(Str s);
Str  s_replace(Arena *a, Str s, Str from, Str to);
Str  s_ellide(Arena *a, Str s, int max);
Str  s_char_at(Arena *a, Str s, int i);            /* one UTF-8 code point */

/* ---------- Buf: growable sink ---------- */
typedef struct { char *p; size_t len, cap; Arena *a; } Buf;
void buf_init(Buf *b, Arena *a);
void buf_put(Buf *b, const void *p, size_t n);
void buf_putc(Buf *b, char c);
void buf_puts(Buf *b, const char *s);
void buf_fmt(Buf *b, const char *fmt, ...);
void buf_clear(Buf *b);
Str  buf_str(Buf *b);
Str  buf_take(Buf *b);
void buf_json_str(Buf *b, Str s);
void buf_c_escape(Buf *b, Str s);
void buf_add_escaped(Buf *b, Str s);               /* compact quoting rules */
void fmt_num(Buf *b, double d);                    /* 3 -> "3", 3.5 -> "3.5" */

/* ---------- hashing ---------- */
typedef struct { uint32_t h[8]; uint64_t ln; uint8_t buf[64]; size_t n; } Sha256;
void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *p, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);
void sha256_buf(const void *p, size_t n, uint8_t out[32]);
void base16(const uint8_t *d, size_t n, char *out);
Str  hash12(Arena *a, const void *p, size_t n);
Str  hash4(Arena *a, const void *p, size_t n);
uint64_t fnv1a(const void *p, size_t n);

/* ---------- glob + path ---------- */
bool glob_match(const char *pat, const char *name);
bool glob_path(const char *pat, const char *path);
Str  path_join(Arena *a, const char *dir, const char *name);
Str  path_norm(Arena *a, const char *p);
const char *path_base(const char *p);
void path_ext(const char *p, char *out, size_t n);  /* ".c" / "" */
bool path_is_abs(const char *p);
bool path_within(const char *root_norm, const char *p_norm);

/* ---------- child environment overrides (SPEC 4.3: sh.run(.., {env:{K:V}})) ----------
 * One option crossing three modules, which is why the type is here and not in
 * shell_int.h: lib_sh parses the {env:{K:V}} record, plan.c binds it into the
 * token, shell.c merges it into the block the child actually gets.
 * Keys and values are NUL-terminated and outlive the call (arena-owned). */
typedef struct { const char *k; const char *v; } ShEnvPair;
typedef struct { ShEnvPair *p; int n; } ShEnv;

/* ---------- values ---------- */
typedef struct V V;
typedef struct Node Node;
typedef struct Env Env;
typedef struct Plan Plan;
typedef enum { V_NULL, V_BOOL, V_NUM, V_STR, V_LIST, V_REC, V_FN, V_PLAN, V_ERR, V_NAT } VType;
typedef enum { F_SCRIPT, F_NATIVE } FnKind;

struct V {
  uint8_t t;
  union { bool b; double n; Str s; struct List *l; struct Rec *r; struct Fn *f;
          struct Plan *pl; struct PErr *e; void *nat; } u;
};

typedef struct List { int len, cap; V *v; } List;
typedef struct Pair { Str k; V v; } Pair;
typedef struct Rec { int len, cap; Pair *kv; } Rec;
typedef struct Fn {
  FnKind kind; Node *body; Str *names; Env *env; int nparams;
  const char *name; V (*native)(V *argv, void *ud); void *ud;
} Fn;
typedef struct PErr { ErrCode code; Str msg; Str hint; V data; } PErr;

extern const V VN;
extern const V VT, VF;

/* val.c */
const char *v_typename(V v);
bool  v_truthy(V v);
bool  v_eq(V a, V b);
int   v_cmp(V a, V b, bool *ok);
V     v_bool(bool b); V v_num(double n); V v_str(Str s); V v_nil(void);
V     v_strz(Arena *a, const char *s);
V     v_err(Arena *a, ErrCode c, const char *fmt, ...);
V     v_err_hint(ErrCode c, Str msg, Str hint);
bool  v_is_err(V v);
ErrCode v_errcode(V v);
List *list_new(Arena *a);
void  list_push(Arena *a, List *l, V v);
V    *list_get(List *l, int i);
Rec  *rec_new(Arena *a);
V    *rec_get(Rec *r, Str k);
V    *rec_getz(Rec *r, const char *k);
void  rec_set(Arena *a, Rec *r, Str k, V v);
void  rec_setz(Arena *a, Rec *r, const char *k, V v);
bool  rec_del(Arena *a, Rec *r, Str k);
V     rec_to_v(Rec *r);
V     v_list(Arena *a, int n, ...);                 /* V args */
V     v_rec(Arena *a, int n, ...);                  /* n pairs: Str key, V val */
Str   key(Arena *a, const char *k);                 /* interned-ish static key */
void  v_disp(Buf *b, V v, int depth, bool compact);
Str   v_tostr(Arena *a, V v, bool compact);
Str   v_tojson(Arena *a, V v);
Str   v_repr(Arena *a, V v);
int   v_tok_est(V v);                               /* est tokens of compact form */
int   tok_est(const char *p, size_t n);             /* ceil(bytes/4) heuristic */

/* ast.h */

typedef struct { const char *src; size_t len; Arena *a; } Src;

/* interp.c */
typedef struct Ctx Ctx;
Ctx *ctx_new(Arena *a, const char *root, const char *script_path);
void ctx_free(Ctx *c);
Node *parse_script(Ctx *c, const char *src, size_t len, const char *name, V *err_out);
V  eval_node(Ctx *c, Node *n);
V  call_fn(Ctx *c, V fn, V *args, int nargs);
V  run_builtin(Ctx *c, const char *ns, const char *name, V *args, int nargs);
void set_error(Ctx *c, ErrCode code, const char *fmt, ...);
bool ctx_has_err(Ctx *c);
V  ctx_err(Ctx *c);
const char *ctx_root(Ctx *c);
Arena *ctx_arena(Ctx *c);
Str  ctx_argstr(Ctx *c);            /* --args value for out.ask */
void ctx_set_steps(Ctx *c, long long max);
/* env */
Env *env_new(Arena *a, Env *parent);
V   *env_get(Env *e, const char *name);
void env_set(Arena *a, Env *e, const char *name, V v);   /* declare in this scope */
bool env_assign(Env *e, const char *name, V v);          /* find & write */
Str  env_dump(Arena *a, Env *e);

/* fmt output */
typedef enum { FMT_AUTO, FMT_JSON, FMT_TEXT } Fmt;
void emit(V v, Fmt fmt, FILE *f);

/* plan.c */
typedef enum { CF_NONE, CF_JAIL, CF_ASK, CF_ALL } Confirm;
typedef enum { OP_WRITE, OP_APPEND, OP_DELETE, OP_MOVE, OP_RESTORE, OP_EXEC, OP_CFG } OpKindP;
Plan *plan_new(Ctx *c, const char *opname, Confirm need);
void  plan_add_file(Ctx *c, Plan *p, const char *path, const char *kind,
                    const char *from_hash, const char *to_hash, long bytes);
void  plan_add_arg(Ctx *c, Plan *p, const char *k, const char *v);
Str   plan_token(Ctx *c, Plan *p);          /* stable, side-effect free */
void  plan_to_v(Ctx *c, Plan *p, Rec *out); /* fills files/token/confirm */
V     plan_execute(Ctx *c, Plan *p, const char *given_token);
void  plan_disp(Plan *p, Buf *b, bool compact);   /* used by v_disp for V_PLAN */
V     v_plan(Plan *p);
const char *plan_op(Plan *p);
/* idempotency journal + stall detection (SPEC 3.4, 3.7) */
void  journal_note(Ctx *c, const char *kind, const char *token, const char *detail);
int   journal_repeat(Ctx *c, const char *fingerprint);
void  journal_run(Ctx *c, const char *fingerprint, int code);
Confirm confirm_from_str(const char *s, bool *ok);
V  op_apply(Ctx *c, V pv, V tokv);   /* PLAN.apply(...) builtin entry */
/* builders used by the fs/sh builtins */
Plan *plan_target(Ctx *c, const char *op, Confirm need, int kind, const char *path);
void  plan_set_payload(Ctx *c, Plan *p, const char *data, size_t n);
void  plan_set_move(Ctx *c, Plan *p, const char *to);
Plan *plan_exec_new(Ctx *c, const char *op, Confirm need, V argv_or_str);
void  plan_exec_opts(Ctx *c, Plan *p, const char *cwd, int timeout_ms, int tail, bool redact);
/* The two options plan_exec_opts predates. Both BOUND into the canonical text, so
 * a token minted for one environment or one stdin is not spendable on another:
 * plan_exec_env stores the pairs for the child and adds `env=KEY=sha(value)..`,
 * plan_exec_stdin keeps the bytes and adds `stdin=sha:..,len:N`. Neither the
 * values nor the input bytes ever reach the printed plan or the journal: see
 * plan_journal_detail, which is the redacted form both of those show. */
void  plan_exec_env(Ctx *c, Plan *p, const ShEnv *env);
void  plan_exec_stdin(Ctx *c, Plan *p, const char *data, size_t n);
Str   plan_journal_detail(Plan *p);   /* "sh.run,env=CC=***,stdin=sha:x,len:3" */
enum { PK_WRITE, PK_APPEND, PK_DELETE, PK_MOVE, PK_RESTORE, PK_EXEC, PK_CFG };
Confirm policy_for(Ctx *c, const char *what);   /* manifest/config lookup, default jail */

/* fsx.c */
typedef struct { char *p; size_t len, cap; int lines; } Slurp;
bool   fs_exists(const char *p);
bool   fs_is_dir(const char *p);
long   fs_size(const char *p);
char  *fs_slurp(Arena *a, const char *p, size_t *len_out, ErrCode *err);
V      fs_write_atomic(Ctx *c, const char *path, const char *data, size_t n, bool append, ErrCode *err);
V      fs_move(Ctx *c, const char *from, const char *to, ErrCode *err);
V      fs_to_trash(Ctx *c, const char *path, long *seq_out, ErrCode *err);
V      fs_restore(Ctx *c, long seq, ErrCode *err);
Str    fs_hash_file(Arena *a, const char *p, ErrCode *err);
/* read with line range / grep / anchors / token budget. opts may be NULL. */
V      fs_read_range(Ctx *c, const char *path, int from, int to, const char *grep,
                     bool anchors, int ctx_lines, int max_tok, ErrCode *err);
V      fs_ls(Ctx *c, const char *dir, const char *globpat, bool rec, int max, ErrCode *err);
V      fs_glob(Ctx *c, const char *pattern, const char *root, int max, ErrCode *err);
const char *ws_root(Ctx *c);
Str    ws_resolve(Arena *a, Ctx *c, const char *p, ErrCode *err);  /* jail + norm */
const char *ws_trash(Ctx *c);   /* absolute */
const char *ws_tmp(Ctx *c);

/* xdiff.c - SPEC 4.2. Everything here returns arena-owned values, so every
 * entry point takes the Arena* rather than a Ctx*: a Ctx-first prototype gave
 * callers no way to say which script's memory the result belongs to, and the
 * result outlived the arena that owned it. */
typedef struct { int n; Str at; Str text; } Anch;
Anch *anchors_of(Arena *a, const char *path, Str text, int *count_out, ErrCode *err);
Str   anchor_for(Arena *a, const char *path, int lineno, Str line, Str text);
Str   anchor_at(Arena *a, const char *path, int idx, Str prev, Str cur, Str next);
typedef struct { Str op, at, text; int n; bool used; } PatchOp;
V   tx_find_anchor(Arena *a, Anch *A, int n, Str at, int occ);
V   tx_patch_apply(Arena *a, Str text, const char *path, V ops);
V   tx_diff(Arena *a, Str ta, Str tb, int ctxl, bool unified);
int tx_diff_stats(const char *a, size_t na, const char *b, size_t nb, int *adds, int *dels, int *hunks);

/* outline.c */
typedef struct { const char *name; int name_len; const char *kind; int line; const char *sig; } Sym;
V   outline_file(Ctx *c, const char *path, Str text, const char *lang);
int outline_lang_detect(const char *path, Str text, char *out /*[16]*/);
V   bundle_files(Ctx *c, V paths, const char *query, int max_tok, bool only_symbols);

/* sim.c */
double sim_ratio(const char *a, int na, const char *b, int nb);       /* 0..1 */
double sim_jaro(const char *a, int na, const char *b, int nb);
V    sim_resolve(Ctx *c, V candidates, Str want, double min);

void  ensure_dir_for(const char *p);
void  ensure_parent_of(const char *p);   /* mkdir the containing dir only: leaf is a file */
V     list_of(List *l);

/* shell.c */
typedef struct {
  int code; char *out; size_t out_len; char *err; size_t err_len;
  long ms; bool timed_out, truncated, total_lines_set; int total_lines;
} ShRes;
void sh_exec(Ctx *c, const char *cmd, const char *cwd, const char *stdin_data,
             int timeout_ms, int tail_lines, bool redact, ShRes *r);
/* sh_exec plus an environment and an exact stdin length. Kept as a second entry
 * point rather than a new parameter on sh_exec, whose shape is frozen by the
 * stubs other suites link against. stdin_len counts bytes (a VXA string may hold
 * NULs); pass 0 to mean "use strlen(stdin_data)". env==NULL => inherit only. */
void sh_exec_env(Ctx *c, const char *cmd, const char *cwd, const char *stdin_data,
                 size_t stdin_len, const ShEnv *env, int timeout_ms, int tail_lines,
                 bool redact, ShRes *r);
/* Environment order, shared by the child's block (shell.c) and the plan's env
 * fingerprint (plan.c): Windows requires the block sorted case-insensitively by
 * variable NAME with any leading '=' ignored (real blocks do carry `=C:=C:\dir`
 * drive-map entries), ties break on the rest of the entry so the order is stable.
 * sh_env_cmp_pp is the same comparator in qsort form over `char *const *`. */
int  sh_env_key_cmp(const char *x, const char *y);
int  sh_env_cmp_pp(const void *x, const void *y);
void sh_free(ShRes *r);
V    sh_result_v(Ctx *c, ShRes *r, const char *cmd);
bool sh_is_readonly(const char *cmd);
char *sh_join_argv(char **argv, int n);
Str  sh_redact(Arena *a, Str s);

/* cfg / manifest */
typedef struct {
  Fmt fmt; Confirm pol_fs, pol_sh; bool strict, no_audit, quiet, anchors;
  int timeout_ms, max_tok, jobs; long long steps; size_t max_mem;
} Opts;
extern Opts O;
V   cfg_get_v(Ctx *c, Str k, V defv);
void cfg_set_c(Ctx *c, const char *k, V v);
V   manifest_load(Ctx *c, const char *dir, ErrCode *err);
V   manifest_policies(Ctx *c, V man);

/* main.c helpers */
const char *arg_value(int argc, char **argv, const char *flag); /* "--flag val" or "--flag=val" */
bool arg_has(int argc, char **argv, const char *flag);
char *read_file_z(const char *p, size_t *len_out);

#endif
