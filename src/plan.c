/* plan.c - the two-phase confirm protocol (SPEC 3): the reason an agent can be
 * allowed to touch a disk at all.
 *
 *   build plan -> canonical text -> token bound to target content hashes
 *   execute     -> only with a matching token, or a policy that permits it
 *   journal     -> a consumed token never applies twice; repeats are detected
 *
 * The token covers the OLD content hash of every target, so if anything changed
 * between planning and confirming, the token is stale instead of destructive. */
#include "vxa.h"
#include "interp.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

/* PK_* kinds come from vxa.h */

typedef struct {
  char *path;
  char *kind;          /* create | modify | delete | move | touch */
  char from[16];       /* old content hash, "" when the file is absent */
  char to[16];         /* planned new content hash */
  long bytes;
} Target;

struct Plan {
  Ctx *cx;
  const char *op;
  Confirm need;
  int kind;
  Target *t; int nt, cap;
  char *args; int nargs;              /* canonical arg text */
  char *data; size_t dlen;           /* payload for write/append */
  char *to;                          /* move destination */
  char *cmd; char **argv; int argc;   /* exec payload */
  char *cwd; int timeout; int tail; bool redact;
  ShEnv env;                          /* exec: overrides the child gets, values stay here */
  char *env_disp;                     /* exec: "CC=***,GOFLAGS=***" for plan/journal text */
  char *in_data; size_t in_len; bool in_set;   /* exec: stdin payload */
  char *in_disp;                      /* exec: "sha:<12>,len:<n>" */
  long seq;                          /* restore */
  char token[16];
  bool sealed;
};

static Arena *g_plan_arena = NULL;

static V plan_value(Ctx *c, Plan *p);

V v_plan(Plan *p) { V v; v.t = V_PLAN; v.u.pl = p; return v; }
const char *plan_op(Plan *p) { return p ? p->op : ""; }

Plan *plan_new(Ctx *c, const char *opname, Confirm need) {
  g_plan_arena = ctx_arena(c);
  Arena *a = ctx_arena(c);
  Plan *p = (Plan*)arena_zalloc(a, sizeof(Plan));
  p->cx = c;
  p->op = arena_strdup(a, opname);
  p->need = need;
  p->args = arena_zalloc(a, 1);
  p->timeout = 30000;
  p->tail = 40;
  p->redact = true;
  return p;
}

void plan_add_file(Ctx *c, Plan *p, const char *path, const char *kind,
                   const char *from_hash, const char *to_hash, long bytes) {
  Arena *a = ctx_arena(c);
  if (p->nt == p->cap) {
    int nc = p->cap ? p->cap * 2 : 4;
    Target *nt2 = (Target*)arena_zalloc(a, sizeof(Target) * (size_t)nc);
    if (p->cap) memcpy(nt2, p->t, sizeof(Target) * (size_t)p->cap);
    p->t = nt2; p->cap = nc;
  }
  Target *t = &p->t[p->nt++];
  t->path = arena_strdup(a, path ? path : "");
  t->kind = arena_strdup(a, kind ? kind : "touch");
  snprintf(t->from, sizeof t->from, "%s", from_hash ? from_hash : "");
  snprintf(t->to, sizeof t->to, "%s", to_hash ? to_hash : "");
  t->bytes = bytes;
  p->sealed = false;
}

void plan_add_arg(Ctx *c, Plan *p, const char *k, const char *v) {
  Arena *a = ctx_arena(c);
  Buf b; buf_init(&b, a);
  buf_puts(&b, p->args ? p->args : "");
  if (p->args && *p->args) buf_putc(&b, ',');
  buf_fmt(&b, "%s=%s", k, v ? v : "");
  p->args = buf_take(&b).p;
  p->sealed = false;
}

static int cmp_target(const void *x, const void *y) {
  const Target *a = (const Target*)x, *b = (const Target*)y;
  return strcmp(a->path, b->path);
}

static const char *confirm_name(Confirm n) {
  return n == CF_NONE ? "none" : n == CF_JAIL ? "jail" : n == CF_ALL ? "all" : "ask";
}

static char *canonical(Ctx *c, Plan *p) {
  Arena *a = ctx_arena(c);
  Buf b; buf_init(&b, a);
  /* the confirm level is part of the identity of a plan: a token minted for an
   * "ask" plan must not be spendable on the "none" version of the same write */
  buf_fmt(&b, "vxa2|%s|confirm=%s|root=%s", p->op, confirm_name(p->need), ctx_root(c));
  /* targets sorted by path so the token does not depend on construction order */
  int n = p->nt;
  Target *sorted = (Target*)arena_zalloc(a, sizeof(Target) * (size_t)(n ? n : 1));
  if (n) memcpy(sorted, p->t, sizeof(Target) * (size_t)n);
  qsort(sorted, (size_t)n, sizeof(Target), cmp_target);
  buf_puts(&b, "|files=");
  for (int i = 0; i < n; i++) {
    if (i) buf_putc(&b, ';');
    buf_fmt(&b, "%s:%s:%s:%s:%ld", sorted[i].path, sorted[i].kind,
            sorted[i].from, sorted[i].to, sorted[i].bytes);
  }
  buf_fmt(&b, "|args=%s", p->args && *p->args ? p->args : "-");
  if (p->kind == PK_EXEC) {
    buf_puts(&b, "|cmd=");
    if (p->argv) {
      for (int i = 0; i < p->argc; i++) { buf_puts(&b, p->argv[i]); buf_putc(&b, 0); }
    } else buf_puts(&b, p->cmd ? p->cmd : "");
  } else if (p->data) {
    buf_fmt(&b, "|payload=%zu", p->dlen);
    Sha256 s; sha256_init(&s); sha256_update(&s, p->data, p->dlen);
    uint8_t d[32]; sha256_final(&s, d);
    char hx[65]; base16(d, 32, hx);
    buf_fmt(&b, "|sha=%s", hx);
  }
  return buf_take(&b).p;
}

Str plan_token(Ctx *c, Plan *p) {
  if (!p->sealed || !p->token[0]) {
    char *can = canonical(c, p);
    Str h = hash12(ctx_arena(c), can, strlen(can));
    snprintf(p->token, sizeof p->token, "%.*s", h.len, h.p);
    p->sealed = true;
  }
  return s_wrap(p->token);
}

Confirm policy_for(Ctx *c, const char *what) {
  V *p = rec_getz(ctx_cfg(c), what);           /* "policies.fs" / "policies.sh" */
  if (p && p->t == V_STR) {
    bool ok = false;
    Confirm cf = confirm_from_str(p->u.s.p, &ok);
    if (ok) return cf;
  }
  return what && !strcmp(what, "policies.sh") ? CF_ASK : CF_JAIL;
}

/* ---------- journal: consumed tokens and stall detection ---------- */
void ensure_parent_of(const char *p);

static const char *journal_path(Ctx *c) {
  Arena *a = ctx_arena(c);
  return path_join(a, ctx_root(c), ".vxa/journal.ndjson").p;
}

/* The journal is append-only and uses the SAME kv form as the output protocol
 * (SPEC 2.1) - one line per event, greppable, no JSON parser needed:
 *   {t=1690000000,kind=apply,token=9f2c1a,at=fs.write}
 *   {t=1690000000,kind=run,fp=<script|args|exit|errcode>,code=3} */
static void journal_line(Ctx *c, const char *kind, const char *a, const char *b) {
  const char *jp = journal_path(c);
  ensure_parent_of(jp);
  FILE *f = fopen(jp, "ab");
  if (!f) return;
  fprintf(f, "{t=%ld,kind=%s,%s}\n", (long)time(NULL), kind, a ? a : "");
  (void)b;
  fclose(f);
}

void journal_note(Ctx *c, const char *kind, const char *token, const char *detail) {
  Arena *a = ctx_arena(c);
  Str line = s_fmt(a, "token=%s,at=%s", token ? token : "-", detail ? detail : "-");
  journal_line(c, kind ? kind : "-", line.p, NULL);
}

/* fingerprint = script | args | exit code | first error code */
void journal_run(Ctx *c, const char *fp, int code) {
  Arena *a = ctx_arena(c);
  Str line = s_fmt(a, "fp=%s,code=%d", fp ? fp : "-", code);
  journal_line(c, "run", line.p, NULL);
}

/* Trailing streak of identical failing runs. Three is the stop signal: repeated
 * identical failures are the measured stall pattern in agent traces, and the
 * cheapest thing an agent can do is not repeat the step. */
int journal_repeat(Ctx *c, const char *fp) {
  FILE *f = fopen(journal_path(c), "rb");
  if (!f) return 0;
  char line[2048];
  int streak = 0;
  char *all = NULL; size_t len = 0, cap = 0;
  while (fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    if (len + n + 1 > cap) { cap = (len + n + 1) * 2; all = (char*)realloc(all, cap); }
    memcpy(all + len, line, n); len += n; all[len] = 0;
  }
  fclose(f);
  if (!all) return 0;
  /* walk lines from the end */
  char *end = all + len;
  while (end > all) {
    char *st = end - 1;
    while (st > all && *(st - 1) != '\n') st--;
    char save = *end;
    *end = 0;
    bool is_run = !!strstr(st, "kind=run");
    bool same = false;
    if (is_run) {
      char *q = strstr(st, "fp=");
      if (q) {
        q += 3;
        char *e = strchr(q, ',');
        size_t n = e ? (size_t)(e - q) : strlen(q);
        same = fp && n == strlen(fp) && !strncmp(q, fp, n);
      }
    }
    *end = save;
    if (!is_run) { break; }
    if (same) streak++; else break;
    end = st;
  }
  free(all);
  return streak;
}

static int journal_has_token(Ctx *c, const char *token) {
  FILE *f = fopen(journal_path(c), "rb");
  if (!f) return 0;
  char line[2048], need[160];
  int found = 0;
  snprintf(need, sizeof need, "kind=apply,token=%s,", token);
  while (fgets(line, sizeof line, f)) if (strstr(line, need)) { found = 1; break; }
  fclose(f);
  return found;
}

/* ---------- policy ---------- */
Confirm confirm_from_str(const char *s, bool *ok) {
  if (ok) *ok = true;
  if (!s) { if (ok) *ok = false; return CF_ASK; }
  if (!strcmp(s, "none")) return CF_NONE;
  if (!strcmp(s, "jail")) return CF_JAIL;
  if (!strcmp(s, "ask"))  return CF_ASK;
  if (!strcmp(s, "all"))  return CF_ALL;
  if (ok) *ok = false;
  return CF_ASK;
}

static bool all_in_jail(Ctx *c, Plan *p) {
  for (int i = 0; i < p->nt; i++) {
    ErrCode e = E_NONE;
    Str r = ws_resolve(ctx_arena(c), c, p->t[i].path, &e);
    if (e != E_NONE || !r.p) return false;
  }
  return true;
}

void plan_to_v(Ctx *c, Plan *p, Rec *out) {
  V v = plan_value(c, p);
  for (int i = 0; i < v.u.r->len; i++) rec_set(ctx_arena(c), out, v.u.r->kv[i].k, v.u.r->kv[i].v);
}

static V plan_value(Ctx *c, Plan *p) {
  Arena *a = ctx_arena(c);
  Rec *r = rec_new(a);
  rec_setz(a, r, "op", v_str(s_lit(a, p->op)));
  rec_setz(a, r, "confirm", v_str(s_lit(a, confirm_name(p->need))));
  rec_setz(a, r, "token", v_str(s_lit(a, plan_token(c, p).p)));
  List *files = list_new(a);
  for (int i = 0; i < p->nt; i++) {
    Rec *f = rec_new(a);
    rec_setz(a, f, "path", v_str(s_lit(a, p->t[i].path)));
    rec_setz(a, f, "kind", v_str(s_lit(a, p->t[i].kind)));
    if (p->t[i].from[0]) rec_setz(a, f, "from", v_str(s_lit(a, p->t[i].from)));
    if (p->t[i].to[0])   rec_setz(a, f, "to", v_str(s_lit(a, p->t[i].to)));
    rec_setz(a, f, "bytes", v_num((double)p->t[i].bytes));
    list_push(a, files, rec_to_v(f));
  }
  rec_setz(a, r, "files", list_of(files));
  rec_setz(a, r, "count", v_num((double)p->nt));
  /* Keys appear only when the plan actually sets them, so a plain command still
   * prints exactly what it printed before. The values are masked here for the same
   * reason the canonical text carries hashes: this record is what an agent pastes
   * into a transcript, and what `vxa plan` writes out. */
  if (p->env_disp) rec_setz(a, r, "env", v_str(s_lit(a, p->env_disp)));
  if (p->in_disp) rec_setz(a, r, "stdin", v_str(s_lit(a, p->in_disp)));
  return rec_to_v(r);
}

void plan_disp(Plan *p, Buf *b, bool compact) {
  if (!p) { buf_puts(b, "-"); return; }
  V v = plan_value(p->cx, p);
  if (!compact) { v_disp(b, v, 0, false); return; }
  /* compact: a plan is not a plain record on the wire. The tag is what tells an
   * agent reading one output line that nothing has happened yet and a token is
   * owed, and it keeps "PLAN(op=" greppable in a journal or a transcript. */
  buf_puts(b, "PLAN(");
  v_disp(b, v, 0, true);
  buf_putc(b, ')');
}

/* ---------- execution ---------- */
static ErrCode verify_targets(Ctx *c, Plan *p) {
  Arena *a = ctx_arena(c);
  for (int i = 0; i < p->nt; i++) {
    if (!p->t[i].from[0]) {
      /* planned as a new file: it must still be absent */
      if (!strcmp(p->t[i].kind, "create") && fs_exists(p->t[i].path)) return E_STALE_PLAN;
      continue;
    }
    if (!fs_exists(p->t[i].path)) return E_STALE_PLAN;
    ErrCode e = E_NONE;
    Str h = fs_hash_file(a, p->t[i].path, &e);
    if (e != E_NONE) return e;
    if (!s_eqz(h, p->t[i].from)) return E_STALE_PLAN;
  }
  return E_NONE;
}

static V do_exec(Ctx *c, Plan *p) {
  Arena *a = ctx_arena(c);
  ShRes r; memset(&r, 0, sizeof r);
  /* The env and the input the token was minted for, handed to the child here and
   * nowhere else: this is the confirmed path, and it is the reason a plan carries
   * the payload at all rather than only its fingerprint. */
  const char *in = p->in_set ? p->in_data : NULL;
  size_t in_len = p->in_set ? p->in_len : 0;
  const ShEnv *env = p->env.n ? &p->env : NULL;
  if (p->argv) {
    char *joined = sh_join_argv(p->argv, p->argc);
    sh_exec_env(c, joined, p->cwd, in, in_len, env, p->timeout, p->tail, p->redact, &r);
  } else {
    sh_exec_env(c, p->cmd ? p->cmd : "", p->cwd, in, in_len, env,
                p->timeout, p->tail, p->redact, &r);
  }
  V out = sh_result_v(c, &r, p->argv ? p->argv[0] : p->cmd);
  sh_free(&r);
  (void)a;
  return out;
}

V plan_execute(Ctx *c, Plan *p, const char *given) {
  Arena *a = ctx_arena(c);
  Str tok = plan_token(c, p);
  char tokz[16]; snprintf(tokz, sizeof tokz, "%.*s", tok.len, tok.p);

  /* 1. replay protection: a consumed token is never executed twice */
  if (given && *given && journal_has_token(c, tokz))
    return v_ok(a, 5, "op", v_str(s_lit(a, p->op)), "token", v_str(s_lit(a, tokz)),
                "applied", VF, "replayed", VT,
                "note", v_str(s_lit(a, "token already consumed; the journal holds the record")));

  /* 2. decide whether a token is needed at all */
  bool wants = true;
  if (p->need == CF_NONE) wants = false;
  else if (p->need == CF_JAIL) {
    bool replace = false;
    for (int i = 0; i < p->nt; i++)
      if (strcmp(p->t[i].kind, "create") != 0) replace = true;
    wants = !(all_in_jail(c, p) && !replace && p->kind != PK_EXEC);
  } else if (p->need == CF_ALL) wants = true;

  if (wants) {
    if (!given || !*given) {
      V pv = plan_value(c, p);
      set_error(c, E_NEED_CONFIRM, "rerun with --confirm %s (plan: %s)",
                tokz, v_tostr(a, pv, true).p);
      return VN;
    }
    if (strcmp(given, tokz) != 0) {
      set_error(c, E_BAD_TOKEN, "token does not match this plan: expected %s", tokz);
      return VN;
    }
  }

  /* 3. TOCTOU: the world must still look like the plan assumed */
  ErrCode v = verify_targets(c, p);
  if (v == E_STALE_PLAN) {
    set_error(c, E_STALE_PLAN, "a target changed after the plan was made; rerun to get a fresh token (%s)", tokz);
    return VN;
  }
  if (v != E_NONE) {
    set_error(c, v, "cannot apply plan: %s", err_name(v));
    return VN;
  }

  /* 4. apply */
  V res = VN;
  ErrCode e = E_NONE;
  switch (p->kind) {
    case PK_WRITE:   res = fs_write_atomic(c, p->t[0].path, p->data, p->dlen, false, &e); break;
    case PK_APPEND:  res = fs_write_atomic(c, p->t[0].path, p->data, p->dlen, true, &e); break;
    case PK_DELETE: {
      long seq = 0;
      res = fs_to_trash(c, p->t[0].path, &seq, &e);
      if (e == E_NONE) res = v_ok(a, 4, "path", v_str(s_lit(a, p->t[0].path)), "trash", VT,
                                  "seq", v_num((double)seq), "restorable", VT);
      break;
    }
    case PK_MOVE:
      res = fs_move(c, p->t[0].path, p->to, &e);
      break;
    case PK_RESTORE:
      res = fs_restore(c, p->seq, &e);
      break;
    case PK_EXEC:
      res = do_exec(c, p);
      break;
    case PK_CFG:
      res = fs_write_atomic(c, p->t[0].path, p->data, p->dlen, false, &e);
      break;
    default: e = E_UNSUPPORTED; break;
  }
  if (e != E_NONE) {
    set_error(c, e, "apply failed for %s: %s", p->op, err_hint(e));
    return VN;
  }
  journal_note(c, "apply", tokz, plan_journal_detail(p).p);
  if (res.t == V_REC) rec_set(a, res.u.r, s_wrap("token"), v_str(s_lit(a, tokz)));
  return res;
}

/* ---------- builders used by the fs/sh builtins ---------- */
Plan *plan_target(Ctx *c, const char *op, Confirm need, int kind, const char *path) {
  Arena *a = ctx_arena(c);
  ErrCode e = E_NONE;
  Str abs = ws_resolve(a, c, path, &e);
  if (e != E_NONE) { set_error(c, e, "%s: '%s' - %s", op, path, err_hint(e)); return NULL; }
  Plan *p = plan_new(c, op, need);
  p->kind = kind;
  char *ps = arena_strdup(a, abs.p);
  bool exists = fs_exists(ps);
  char from[16] = "";
  if (exists) {
    ErrCode he = E_NONE;
    Str h = fs_hash_file(a, ps, &he);
    if (he == E_NONE) snprintf(from, sizeof from, "%.*s", h.len, h.p);
  }
  /* the label is what the agent reads to judge blast radius: "modify" and "delete"
   * are different decisions even when the path is the same */
  const char *lbl;
  if (kind == PK_DELETE) lbl = "delete";
  else if (kind == PK_MOVE) lbl = "move";
  else lbl = exists ? "modify" : "create";
  plan_add_file(c, p, ps, lbl, from, "", exists ? fs_size(ps) : 0);
  return p;
}

void plan_set_payload(Ctx *c, Plan *p, const char *data, size_t n) {
  p->data = (char*)data;
  p->dlen = n;
  p->sealed = false;
  Str h = hash12(ctx_arena(c), data, n);
  snprintf(p->t[0].to, sizeof p->t[0].to, "%.*s", h.len, h.p);
  p->t[0].bytes = (long)n;
}

void plan_set_move(Ctx *c, Plan *p, const char *to) {
  ErrCode e = E_NONE;
  Str abs = ws_resolve(ctx_arena(c), c, to, &e);
  if (e != E_NONE) { set_error(c, e, "move target '%s' - %s", to, err_hint(e)); return; }
  p->to = arena_strdup(ctx_arena(c), abs.p);
  bool exists = fs_exists(p->to);
  char from[16] = "";
  if (exists) {
    ErrCode he = E_NONE;
    Str h = fs_hash_file(ctx_arena(c), p->to, &he);
    if (he == E_NONE) snprintf(from, sizeof from, "%.*s", h.len, h.p);
  }
  plan_add_file(c, p, p->to, exists ? "modify" : "create", from, "", exists ? fs_size(p->to) : 0);
  p->sealed = false;
}

Plan *plan_exec_new(Ctx *c, const char *op, Confirm need, V argv_or_str) {
  Arena *a = ctx_arena(c);
  Plan *p = plan_new(c, op, need);
  p->kind = PK_EXEC;
  if (argv_or_str.t == V_LIST) {
    int n = argv_or_str.u.l->len;
    p->argv = (char**)arena_zalloc(a, sizeof(char*) * (size_t)(n ? n : 1));
    p->argc = n;
    Buf b; buf_init(&b, a);
    for (int i = 0; i < n; i++) {
      V *v = &argv_or_str.u.l->v[i];
      p->argv[i] = arena_strdup(a, v->t == V_STR ? v->u.s.p : "");
      if (i) buf_putc(&b, ' ');
      buf_add_escaped(&b, v->t == V_STR ? v->u.s : s_null());
    }
    plan_add_arg(c, p, "argv", b.p ? b.p : "");
  } else if (argv_or_str.t == V_STR) {
    p->cmd = arena_strdup(a, argv_or_str.u.s.p);
    plan_add_arg(c, p, "cmd", p->cmd);
  } else {
    set_error(c, E_TYPE, "a command must be a list of arguments or a string");
    return p;
  }
  return p;
}

void plan_exec_opts(Ctx *c, Plan *p, const char *cwd, int timeout_ms, int tail, bool redact) {
  Arena *a = ctx_arena(c);
  if (cwd) {
    ErrCode e = E_NONE;
    Str abs = ws_resolve(a, c, cwd, &e);
    if (e != E_NONE) { set_error(c, e, "cwd '%s' is outside the workspace", cwd); return; }
    p->cwd = arena_strdup(a, abs.p);
    plan_add_arg(c, p, "cwd", p->cwd);
  }
  if (timeout_ms > 0) p->timeout = timeout_ms;
  if (tail >= 0) p->tail = tail;
  p->redact = redact;
  plan_add_arg(c, p, "timeout", s_fmt(a, "%d", p->timeout).p);
}

/* ---------- env + stdin: bound into the token, never written in the clear -----
 * WHY the token covers them at all: a confirmation is a statement about ONE
 * execution. A token minted for `{env:{CC:"gcc"}}` must not be spendable on the
 * same command line with a different PATH/CC, and a token minted for a plan that
 * pipes a patch must not run with empty input -- so both go into the canonical
 * text, exactly like a file's old hash does.
 * WHY only hashes get into the canonical text: canonical() is the source of the
 * printed plan and of every journal/audit line, and those are plain text on disk
 * that outlives the run. Environment values are where agents put tokens and
 * database passwords, so the plan carries `KEY=sha256(value)[:12]`: enough to make
 * the token change when the value changes, useless as a leak. */
static void join_sorted(char **v, int n, Buf *b) {
  qsort(v, (size_t)n, sizeof(char*), sh_env_cmp_pp);
  for (int i = 0; i < n; i++) { if (i) buf_putc(b, ','); buf_puts(b, v[i]); }
}

void plan_exec_env(Ctx *c, Plan *p, const ShEnv *env) {
  Arena *a = ctx_arena(c);
  if (!p || !env || env->n <= 0) return;
  int n = env->n;
  ShEnvPair *cp = (ShEnvPair*)arena_zalloc(a, sizeof(ShEnvPair) * (size_t)n);
  char **fp = (char**)arena_zalloc(a, sizeof(char*) * (size_t)n);    /* KEY=hash(value) */
  char **dp = (char**)arena_zalloc(a, sizeof(char*) * (size_t)n);    /* KEY=***         */
  for (int i = 0; i < n; i++) {
    cp[i].k = arena_strdup(a, env->p[i].k ? env->p[i].k : "");
    cp[i].v = arena_strdup(a, env->p[i].v ? env->p[i].v : "");
    Str k = s_wrap(cp[i].k);
    fp[i] = s_fmt(a, "%.*s=%s", k.len, k.p, hash12(a, cp[i].v, strlen(cp[i].v)).p).p;
    dp[i] = s_fmt(a, "%.*s=***", k.len, k.p).p;
  }
  Buf f, d;
  buf_init(&f, a); join_sorted(fp, n, &f);
  buf_init(&d, a); join_sorted(dp, n, &d);
  p->env.p = cp; p->env.n = n;
  p->env_disp = buf_take(&d).p;
  plan_add_arg(c, p, "env", buf_str(&f).p);
  p->sealed = false;
}

void plan_exec_stdin(Ctx *c, Plan *p, const char *data, size_t n) {
  Arena *a = ctx_arena(c);
  if (!p) return;
  p->in_data = (char*)arena_zalloc(a, n + 1);
  if (n && data) memcpy(p->in_data, data, n);
  p->in_data[n] = 0;
  p->in_len = n;
  p->in_set = true;
  Str h = hash12(a, data ? data : "", n);
  p->in_disp = s_fmt(a, "sha:%s,len:%zu", h.p, n).p;
  /* The input itself never enters the canonical text either, for the same reason a
   * secret does not: a patch or a here-doc is the payload, and the journal would
   * copy it. len+hash binds it just as tightly. */
  plan_add_arg(c, p, "stdin", p->in_disp);
  p->sealed = false;
}

/* What an apply receipt says about an exec plan: the op, then the two options in
 * the only form that is safe to put on disk -- env names with masked values, the
 * input by hash and length. Non-exec plans, and exec plans that set neither, get
 * just the op name, exactly as they always did. */
Str plan_journal_detail(Plan *p) {
  if (!p) return s_wrap("");
  Arena *a = ctx_arena(p->cx);
  Buf b; buf_init(&b, a);
  buf_puts(&b, p->op ? p->op : "-");
  if (p->env_disp) buf_fmt(&b, ",env=%s", p->env_disp);
  if (p->in_disp) buf_fmt(&b, ",stdin=%s", p->in_disp);
  return buf_take(&b);
}

V op_apply(Ctx *c, V pv, V tokv) {
  if (pv.t != V_PLAN) { set_error(c, E_TYPE, "apply needs a PLAN value"); return VN; }
  /* an explicit token argument wins; otherwise --confirm on the command line
   * is the token (the agent cannot answer a prompt, so confirmation is a replay) */
  const char *tok = tokv.t == V_STR ? tokv.u.s.p : (ctx_confirm(c) ? ctx_confirm(c) : "");
  return plan_execute(c, pv.u.pl, tok);
}

/* argv -> one command line for sh_exec. Windows quoting rules: backslashes before
 * a double quote are doubled, so an argument can never break out of its quotes. */
char *sh_join_argv(char **argv, int n) {
  Buf b;
  buf_init(&b, g_plan_arena);
  for (int i = 0; i < n; i++) {
    if (i) buf_putc(&b, ' ');
    const char *w = argv[i] ? argv[i] : "";
    bool need = !*w;
    for (const char *q = w; *q; q++)
      if (*q == ' ' || *q == '\t' || *q == '"' || *q == '\\') need = true;
    if (!need) { buf_puts(&b, w); continue; }
    buf_putc(&b, '"');
    int bs = 0;
    for (const char *q = w; ; q++) {
      if (*q == '\\') { bs++; continue; }
      if (*q == '"') { for (int k = 0; k < bs * 2 + 1; k++) buf_putc(&b, '\\'); buf_putc(&b, '"'); bs = 0; continue; }
      if (!*q) { for (int k = 0; k < bs * 2; k++) buf_putc(&b, '\\'); break; }
      for (int k = 0; k < bs; k++) buf_putc(&b, '\\');
      bs = 0;
      buf_putc(&b, *q);
    }
    buf_putc(&b, '"');
  }
  return buf_take(&b).p;
}
