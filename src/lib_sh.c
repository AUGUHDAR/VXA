/* lib_sh.c - sh.* : how an agent runs an external command (SPEC §3, §4.3).
 *
 * One rule shapes this whole file: 二次确认 before executing commands. Every
 * command becomes a PLAN that needs a token. There is exactly one exception and
 * it is narrow by construction: an *argv* whose head the shell.c allowlist proves
 * read-only is downgraded to CF_NONE and runs at once, so `git status` does not
 * cost two round trips. "Provable" means all of:
 *   - the argv form only, never a string: `ls; rm -rf x` is ONE string and would
 *     look read-only to any prefix test, so a string command is always CF_ASK;
 *   - argv[0]/argv[1] matched against the table in shell.c via
 *     sh_argv_readonly(), which also carries the danger guards (git push,
 *     find -delete, sed -i) that no textual check can see;
 *   - no argv element holding a shell metacharacter, because plan.c re-joins
 *     argv into one command line and shell.c routes a metacharacter back to the
 *     shell: an allowlisted head plus a `;` argument would run something that
 *     was never planned. Such a command is never auto-approved.
 *
 * Execution always ends in shell.c (sh_exec / sh_result_v / sh_free): nothing
 * here spawns a process, quotes an argument, tails a buffer or redacts a secret.
 * Confirmation always ends in plan.c (plan_exec_new / plan_token / plan_execute
 * / journal), and an auto-approved run writes the same receipt line by hand. */
#include "vxa.h"
#include "interp.h"
#include "lib.h"
#include "shell_int.h"              /* sh_argv_readonly, sh_has_meta, sh_command_line */
#include <stdlib.h>
#include <string.h>

/* agent-facing limits, restated inside every error that hits one */
#define SH_DEF_TIMEOUT_S  30
#define SH_MAX_TIMEOUT_S  3600
#define SH_DEF_TAIL       40        /* sh.run: the reason a build broke is at the end */
#define SH_OUT_TAIL       200       /* sh.out: you want the text, get more of it */
#define SH_MAX_JOBS       64
#define SH_MAX_ARGV       SH_MAXARGS /* over that, shell.c silently takes the shell path */

/* ---------------- a command: argv list, or one shell string ---------------- */
typedef struct {
  V    v;            /* the value handed to plan_exec_new: V_LIST or V_STR */
  bool is_string;    /* true => goes through the shell, never auto-approved */
  char **argv;       /* argv form only, arena-owned */
  int  argc;
  const char *line;  /* what sh_exec receives: quoted argv line, or the string */
  const char *shown; /* compact display text used in error messages */
  bool ro;           /* allowlist verdict for this command as it stands */
  bool ro_fast;      /* ro AND argv form AND shell-safe => CF_NONE is allowed */
} Cmd;

static const char *join_show(Arena *a, char **argv, int argc) {
  Buf b; buf_init(&b, a);
  for (int i = 0; i < argc; i++) {
    if (i) buf_putc(&b, ' ');
    const char *s = argv[i] ? argv[i] : "";
    if (!*s || strchr(s, ' ') || strchr(s, '\t')) buf_fmt(&b, "\"%s\"", s);
    else buf_puts(&b, s);
  }
  return buf_take(&b).p;
}

static bool argv_shell_safe(char **argv, int argc) {
  for (int i = 0; i < argc; i++) if (sh_has_meta(argv[i])) return false;
  return true;
}

/* A wrong element type must be an error, not a silent "": plan_exec_new copies
 * argv with `v->t == V_STR ? v->u.s.p : ""`, so a number in the list would turn
 * into an empty argument and run the program with arguments you never passed. */
static Cmd cmd_from(Ctx *c, V v, const char *who) {
  Arena *a = ctx_arena(c);
  Cmd m;
  memset(&m, 0, sizeof m);
  m.v = v;
  if (v.t == V_STR) {
    if (!v.u.s.p || !*v.u.s.p) {
      set_error(c, E_BAD_INPUT, "%s: the command string is empty", who);
      return m;
    }
    /* sh_is_readonly() splits the text and consults the same table, and returns
     * false for anything containing a metacharacter. That is the whole reason a
     * string may be `ro` for sh.ro but can never be `ro_fast`. */
    m.is_string = true;
    m.line = m.shown = v.u.s.p;
    m.ro = sh_is_readonly(v.u.s.p);
    return m;
  }
  if (v.t != V_LIST) {
    set_error(c, E_TYPE, "%s: a command must be a list of arguments or a string, got %s",
              who, v_typename(v));
    return m;
  }
  List *l = v.u.l;
  if (!l || !l->len) {
    set_error(c, E_BAD_INPUT, "%s: the argv list is empty - pass at least the program name", who);
    return m;
  }
  if (l->len > SH_MAX_ARGV) {
    set_error(c, E_LIMIT, "%s: %d arguments, the limit is %d - split the work",
              who, l->len, SH_MAX_ARGV);
    return m;
  }
  char **av = (char**)arena_zalloc(a, sizeof(char*) * (size_t)l->len);
  for (int i = 0; i < l->len; i++) {
    V e = l->v[i];
    if (e.t != V_STR) {
      set_error(c, E_TYPE, "%s: argv[%d] must be a string, got %s", who, i, v_typename(e));
      return m;
    }
    av[i] = arena_strdup(a, e.u.s.p ? e.u.s.p : "");
  }
  m.argv = av;
  m.argc = l->len;
  m.shown = join_show(a, av, l->len);
  m.line = sh_command_line(a, av, l->len).p;   /* sh_split inverts this exactly */
  m.ro = argv_shell_safe(av, l->len) && sh_argv_readonly(av, l->len);
  m.ro_fast = m.ro;
  return m;
}

/* ---------------- options ---------------- */
static const char *const RUN_OPTS[] =
  { "cwd", "timeout", "tail", "redact", "stdin", "env", "confirm", NULL };
static const char *const RO_OPTS[] =
  { "cwd", "timeout", "tail", "redact", "stdin", "env", NULL };
static const char *const JOB_OPTS[] =
  { "cwd", "timeout", "tail", "redact", "stdin", "env", "confirm",
    "jobs", "all", "token", NULL };

typedef struct {
  const char *cwd;        /* jail-resolved; NULL => inherit the script's cwd */
  int  timeout_ms;
  int  tail;              /* 0 = unlimited lines (shell.c still caps bytes) */
  bool redact;
  Str  stdin_data;
  bool stdin_set;
  ShEnv env;              /* overrides merged into the child's block; empty => none */
  bool env_set;
  bool confirm_set;
  Confirm confirm;
  int  jobs;              /* sh.jobs only */
  bool all;               /* sh.jobs only */
  Str  token;             /* sh.jobs second phase only */
  bool token_set;
} ShOpt;

static V opt_field(Args *x, int oi, const char *name) {
  if (!arg_present(x, oi) || x->a[oi].t != V_REC) return VN;
  V *p = rec_getz(x->a[oi].u.r, name);
  return p ? *p : VN;
}

/* An unknown option key is a typo (tiemout:, redacton:) and the caller would
 * otherwise keep wondering why the setting had no effect. VXA does not guess. */
static bool keys_ok(Ctx *c, Args *x, int oi, const char *const *allowed, const char *who) {
  if (!arg_present(x, oi)) return true;
  V o = x->a[oi];
  if (o.t != V_REC) {
    set_error(c, E_TYPE, "%s: options must be a record {k:v}, got %s", who, v_typename(o));
    return false;
  }
  for (int i = 0; i < o.u.r->len; i++) {
    Str k = o.u.r->kv[i].k;
    bool found = false;
    for (int j = 0; allowed[j] && !found; j++)
      found = k.p && strlen(allowed[j]) == (size_t)k.len && !memcmp(k.p, allowed[j], (size_t)k.len);
    if (!found) {
      Buf have; buf_init(&have, ctx_arena(c));
      for (int j = 0; allowed[j]; j++) buf_fmt(&have, "%s%s", j ? "|" : "", allowed[j]);
      set_error(c, E_BAD_INPUT, "%s: unknown option '%.*s' - allowed: %s",
                who, k.len, k.p ? k.p : "", have.p ? have.p : "");
      return false;
    }
  }
  return true;
}

/* ---------------- env:{NAME:"value"} -> the child's overrides ----------------
 * Format-strict on purpose. shell.c turns each pair into one `NAME=value` line in
 * the block handed to CreateProcess/exec, so:
 *   - a name holding '=' or a control byte would describe a variable the child
 *     cannot have (and a leading '=' is the block's own separator syntax),
 *   - two names differing only in case are ONE variable on Windows, and which
 *     value won would depend on record order -- so that is an error, not a merge,
 *   - a number is not a string: `env:{JOBS:4}` is a typo for `env:{JOBS:"4"}`, and
 *     VXA fixes typos by refusing, not by guessing.
 * Values are never inspected or masked here: they reach the child as written, and
 * the plan/journal only ever see KEY=*** (see plan_exec_env). */
#define SH_MAX_ENV 256

static bool name_eq(const char *x, const char *y) {
  for (; *x && *y; x++, y++) {
    int a = (*x >= 'A' && *x <= 'Z') ? *x - 'A' + 'a' : (unsigned char)*x;
    int b = (*y >= 'A' && *y <= 'Z') ? *y - 'A' + 'a' : (unsigned char)*y;
    if (a != b) return false;
  }
  return !*x && !*y;
}

static bool env_from(Ctx *c, Rec *r, ShOpt *o, const char *who) {
  Arena *a = ctx_arena(c);
  int n = r ? r->len : 0;
  o->env_set = true;
  if (n == 0) return true;                       /* {env:{}} overrides nothing, and costs no token */
  if (n > SH_MAX_ENV) {
    set_error(c, E_LIMIT, "%s: env sets %d variables, the limit is %d - pass only what the child reads",
              who, n, SH_MAX_ENV);
    return false;
  }
  ShEnvPair *p = (ShEnvPair*)arena_zalloc(a, sizeof(ShEnvPair) * (size_t)n);
  int k = 0;
  for (int i = 0; i < n; i++) {
    Str key = r->kv[i].k;
    V val = r->kv[i].v;
    const char *ks = key.p ? key.p : "";
    if (key.len <= 0) {
      set_error(c, E_BAD_INPUT, "%s: an env name is empty - write env:{NAME:\"value\"}", who);
      return false;
    }
    for (int j = 0; j < key.len; j++) {
      unsigned char ch = (unsigned char)ks[j];
      if (ch == '=' || ch < 0x20 || ch == 0x7f) {
        set_error(c, E_BAD_INPUT,
                  "%s: env name '%.*s' holds byte 0x%02x, which cannot appear in a variable name "
                  "(a name is the part before the first '=')", who, key.len, ks, (unsigned)ch);
        return false;
      }
    }
    if (val.t != V_STR) {
      set_error(c, E_TYPE, "%s: env['%.*s'] must be a string, got %s - quote the value: \"%.*s\"",
                who, key.len, ks, v_typename(val), key.len, ks);
      return false;
    }
    for (int j = 0; j < k; j++)
      if (name_eq(p[j].k, ks)) {
        set_error(c, E_BAD_INPUT,
                  "%s: env sets '%.*s' twice (names differ only in case, which is one variable "
                  "on Windows) - keep the value you mean", who, key.len, ks);
        return false;
      }
    p[k].k = arena_strndup(a, ks, (size_t)key.len);
    p[k].v = arena_strdup(a, val.u.s.p ? val.u.s.p : "");
    k++;
  }
  o->env.p = p; o->env.n = k;
  return true;
}

/* `timeout` is SECONDS at this boundary because that is what an agent reasons
 * in; ms from here down. 0 or negative would mean "no limit" to shell.c, which
 * is the one thing a scripted agent must never get by accident. */
static bool shopt_get(Ctx *c, Args *x, int oi, int def_tail, const char *const *allowed,
                      const char *who, ShOpt *o) {
  Arena *a = ctx_arena(c);
  memset(o, 0, sizeof *o);
  o->timeout_ms = SH_DEF_TIMEOUT_S * 1000;
  o->tail = def_tail;
  o->redact = true;
  o->jobs = 1;
  if (!keys_ok(c, x, oi, allowed, who)) return false;

  V tv = opt_field(x, oi, "timeout");
  if (tv.t == V_NUM) {
    if (!(tv.u.n > 0) || tv.u.n > SH_MAX_TIMEOUT_S) {
      set_error(c, E_RANGE,
                "%s: timeout=%g seconds is out of band - allowed 0.001..%d (default %d); "
                "there is no unlimited timeout",
                who, tv.u.n, SH_MAX_TIMEOUT_S, SH_DEF_TIMEOUT_S);
      return false;
    }
    o->timeout_ms = (int)(tv.u.n * 1000.0);
    if (o->timeout_ms < 1) o->timeout_ms = 1;
  } else if (tv.t != V_NULL) {
    set_error(c, E_TYPE, "%s: timeout must be a number of seconds, got %s", who, v_typename(tv));
    return false;
  }

  V lv = opt_field(x, oi, "tail");
  if (lv.t == V_NUM) {
    if (lv.u.n < 0 || lv.u.n > 1000000) {
      set_error(c, E_RANGE, "%s: tail=%d is out of band - 0 (unlimited) .. 1000000 lines",
                who, (int)lv.u.n);
      return false;
    }
    o->tail = (int)lv.u.n;
  } else if (lv.t != V_NULL) {
    set_error(c, E_TYPE, "%s: tail must be a number of lines, got %s", who, v_typename(lv));
    return false;
  }

  V rv = opt_field(x, oi, "redact");
  if (rv.t != V_NULL) o->redact = v_truthy(rv);

  V sv = opt_field(x, oi, "stdin");
  if (sv.t == V_STR) {
    o->stdin_data = sv.u.s;
    /* Any string counts as "stdin was given", the empty one included: it is what a
     * caller writes to feed zero bytes. To the CHILD there is nothing to tell the
     * two apart -- both are a file at immediate EOF, because stdin is redirected
     * from a file we control rather than left on the console -- so the difference
     * is recorded in the plan (stdin=sha:...,len:0 vs no stdin key at all) and the
     * doc says plainly that an empty input is not a way to test for one. */
    o->stdin_set = true;
  } else if (sv.t != V_NULL) {
    set_error(c, E_TYPE, "%s: stdin must be a string, got %s", who, v_typename(sv));
    return false;
  }

  V cv = opt_field(x, oi, "confirm");
  if (cv.t == V_STR) {
    bool ok = false;
    o->confirm = confirm_from_str(cv.u.s.p, &ok);
    if (!ok) {
      set_error(c, E_BAD_INPUT, "%s: confirm=\"%.*s\" is not a policy - use none|jail|ask|all",
                who, cv.u.s.len, cv.u.s.p ? cv.u.s.p : "");
      return false;
    }
    o->confirm_set = true;
  } else if (cv.t != V_NULL) {
    set_error(c, E_TYPE, "%s: confirm must be one of none|jail|ask|all, got %s",
              who, v_typename(cv));
    return false;
  }

  V ev = opt_field(x, oi, "env");
  if (ev.t != V_NULL) {
    if (ev.t != V_REC) {
      set_error(c, E_TYPE, "%s: env must be a record {NAME:\"value\"}, got %s",
                who, v_typename(ev));
      return false;
    }
    if (!env_from(c, ev.u.r, o, who)) return false;
  }

  V wv = opt_field(x, oi, "cwd");
  if (wv.t == V_STR && wv.u.s.p && *wv.u.s.p) {
    ErrCode e = E_NONE;
    Str abs = ws_resolve(a, c, wv.u.s.p, &e);
    if (e != E_NONE || !abs.p) {
      set_error(c, e == E_NONE ? E_BAD_INPUT : e, "%s: cwd '%.*s' - %s",
                who, wv.u.s.len, wv.u.s.p ? wv.u.s.p : "", err_hint(e));
      return false;
    }
    o->cwd = abs.p;                /* plan_exec_opts resolves an abs in-jail path to itself */
  } else if (wv.t != V_NULL) {
    set_error(c, E_TYPE, "%s: cwd must be a string path, got %s", who, v_typename(wv));
    return false;
  }
  return true;
}

/* ---------------- policy: where 二次确认 is decided ---------------- */
/* manifest/config default, then the caller's explicit override, then - only when
 * nothing was asked for explicitly and this argv is provably read-only - the
 * downgrade that keeps the fast path fast. CF_ALL (audit mode) never downgrades. */
static Confirm policy_of(Ctx *c, const ShOpt *o, bool ro_fast, bool *auto_run) {
  Confirm need = policy_for(c, "policies.sh");
  if (o->confirm_set) need = o->confirm;
  *auto_run = (need == CF_NONE);
  if (ro_fast && !o->confirm_set && (need == CF_ASK || need == CF_JAIL)) {
    need = CF_NONE;
    *auto_run = true;
  }
  return need;
}

/* ---------------- execution: shell.c owns every byte of this ---------------- */
static V run_now(Ctx *c, const Cmd *m, const ShOpt *o, const char *op,
                 const char *token, bool auto_run, Plan *p) {
  Arena *a = ctx_arena(c);
  ShRes r;
  memset(&r, 0, sizeof r);
  const char *in = o->stdin_set ? (o->stdin_data.p ? o->stdin_data.p : "") : NULL;
  size_t in_len = o->stdin_set ? (size_t)(o->stdin_data.len > 0 ? o->stdin_data.len : 0) : 0;
  sh_exec_env(c, m->line, o->cwd, in, in_len, o->env.n ? &o->env : NULL,
              o->timeout_ms, o->tail, o->redact, &r);
  V res = sh_result_v(c, &r, m->line);
  sh_free(&r);
  if (res.t != V_REC) return res;
  rec_setz(a, res.u.r, "op", v_str(s_lit(a, op)));
  rec_setz(a, res.u.r, "confirm", v_str(s_lit(a, "none")));
  if (auto_run) rec_setz(a, res.u.r, "auto", v_bool(true));
  if (token && *token) rec_setz(a, res.u.r, "token", v_str(s_lit(a, token)));
  /* the receipt an auto run owes the journal: the same line plan_execute writes
   * for a confirmed apply (SPEC 3.4), env names masked and input by hash */
  journal_note(c, "apply", (token && *token) ? token : "-",
               p ? plan_journal_detail(p).p : op);
  return res;
}

/* Plan for one command + its options. `mark_auto` puts the auto=t record in the
 * canonical args, which is what distinguishes a fast-path plan from the same
 * command asked about. Every plan_add_arg happens before the token is taken, or
 * the token would not cover what it claims to -- which is why env and stdin are
 * bound here and not at run time: the same setters feed the plan's payload, so a
 * confirmed apply runs with the environment and the input that were hashed into
 * the token the caller is holding. */
static Plan *plan_for(Ctx *c, const Cmd *m, const ShOpt *o, Confirm need, bool mark_auto,
                      const char *op, const char *name) {
  Arena *a = ctx_arena(c);
  Plan *p = plan_exec_new(c, op, need, m->v);
  if (!p || ctx_has_err(c)) return NULL;
  plan_exec_opts(c, p, o->cwd, o->timeout_ms, o->tail, o->redact);
  if (ctx_has_err(c)) return NULL;
  plan_add_arg(c, p, "tail", s_fmt(a, "%d", o->tail).p);
  plan_add_arg(c, p, "redact", o->redact ? "t" : "f");
  if (name && *name) plan_add_arg(c, p, "name", name);
  if (o->env_set && o->env.n) plan_exec_env(c, p, &o->env);
  if (o->stdin_set)
    plan_exec_stdin(c, p, o->stdin_data.p ? o->stdin_data.p : "",
                    (size_t)(o->stdin_data.len > 0 ? o->stdin_data.len : 0));
  if (ctx_has_err(c)) return NULL;
  if (mark_auto) plan_add_arg(c, p, "auto", "t");
  return p;
}

/* ============================ sh.run ============================ */
static V f_run(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  V first = arg_at(&x, 0);
  if (ctx_has_err(c)) return VN;
  Cmd m = cmd_from(c, first, "sh.run");
  if (ctx_has_err(c)) return VN;
  ShOpt o;
  if (!shopt_get(c, &x, 1, SH_DEF_TAIL, RUN_OPTS, "sh.run", &o)) return VN;

  bool auto_run = false;
  Confirm need = policy_of(c, &o, m.ro_fast, &auto_run);
  Plan *p = plan_for(c, &m, &o, need, auto_run, "sh.run", NULL);
  if (!p) return VN;
  Str tok = plan_token(c, p);
  if (auto_run) return run_now(c, &m, &o, "sh.run", tok.p, true, p);
  /* gate closed: `--confirm <token>` on the command line replays this exact plan
   * (SPEC 3.4); otherwise the agent gets the PLAN and reads the token out of it */
  const char *cli = ctx_confirm(c);
  if (cli && *cli) return plan_execute(c, p, cli);
  return v_plan(p);
}

/* ============================ sh.ro ============================ */
/* No plan phase, no policy consultation, no downgrade: either the argv is on
 * the read-only allowlist, or this refuses. That is the entire value of `ro`. */
static V f_ro(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  V first = arg_at(&x, 0);
  if (ctx_has_err(c)) return VN;
  Cmd m = cmd_from(c, first, "sh.ro");
  if (ctx_has_err(c)) return VN;
  ShOpt o;
  if (!shopt_get(c, &x, 1, SH_DEF_TAIL, RO_OPTS, "sh.ro", &o)) return VN;
  if (!m.ro) {
    set_error(c, E_POLICY,
              "sh.ro: '%s' is not provably read-only - use sh.run(%s) and confirm the token "
              "it returns; the allowlist lives in shell.c and matches argv, never a prefix",
              m.shown, m.is_string ? "the same string" : "the same argv list");
    return VN;
  }
  /* sh.ro always executes at once, so stdin is honoured; it is not the sh.run
   * fast path, so no auto=t in its canonical args */
  Plan *p = plan_for(c, &m, &o, CF_NONE, false, "sh.ro", NULL);
  if (!p) return VN;
  Str tok = plan_token(c, p);
  V res = run_now(c, &m, &o, "sh.ro", tok.p, false, p);
  if (res.t == V_REC) rec_setz(ctx_arena(c), res.u.r, "ro", v_bool(true));
  return res;
}

/* ========================== sh.allowed ========================== */
static V f_allowed(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  V first = arg_at(&x, 0);
  if (ctx_has_err(c)) return VN;
  Cmd m = cmd_from(c, first, "sh.allowed");
  if (ctx_has_err(c)) return VN;
  /* the verdict sh.run and sh.ro will act on, so an agent can pick its path
   * before spending a round trip: true => runs at once, false => confirm first. */
  return v_bool(m.ro);
}

/* ============================ sh.out ============================ */
static V f_out(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  V first = arg_at(&x, 0);
  if (ctx_has_err(c)) return VN;
  Cmd m = cmd_from(c, first, "sh.out");
  if (ctx_has_err(c)) return VN;
  ShOpt o;
  if (!shopt_get(c, &x, 1, SH_OUT_TAIL, RUN_OPTS, "sh.out", &o)) return VN;
  bool auto_run = false;
  Confirm need = policy_of(c, &o, m.ro_fast, &auto_run);
  Plan *p = plan_for(c, &m, &o, need, auto_run, "sh.out", NULL);
  if (!p) return VN;
  Str tok = plan_token(c, p);
  if (!auto_run) {
    const char *cli = ctx_confirm(c);
    if (!(cli && *cli)) return v_plan(p);       /* still gated: a PLAN, not a string */
    V res = plan_execute(c, p, cli);
    if (res.t != V_REC) return res;
    V *f = rec_getz(res.u.r, "out");
    return (f && f->t == V_STR) ? *f : v_str(s_lit(ctx_arena(c), ""));
  }
  V res = run_now(c, &m, &o, "sh.out", tok.p, true, p);
  if (res.t != V_REC) return res;
  V *f = rec_getz(res.u.r, "out");
  return (f && f->t == V_STR) ? *f : v_str(s_lit(ctx_arena(c), ""));
}

/* ============================ sh.jobs ============================ */
typedef struct {
  Str  name;
  Cmd  cmd;
  int  timeout_ms;                 /* 0 => the run-wide default */
  int *needs;
  int  nneeds;
  int  indeg;
} Job;

static V f_jobs(Ctx *c, V *args, int nargs) {
  Args x = ARGS();
  Arena *a = ctx_arena(c);
  V jv = arg_at(&x, 0);
  if (ctx_has_err(c)) return VN;
  if (jv.t != V_LIST) {
    set_error(c, E_TYPE, "sh.jobs: the first argument must be a list of job records, got %s",
              v_typename(jv));
    return VN;
  }
  List *jl = jv.u.l;
  if (!jl->len) {
    set_error(c, E_BAD_INPUT, "sh.jobs: the job list is empty - pass [{name,cmd}, ..]");
    return VN;
  }
  if (jl->len > SH_MAX_JOBS) {
    set_error(c, E_LIMIT, "sh.jobs: %d jobs, the limit is %d", jl->len, SH_MAX_JOBS);
    return VN;
  }
  ShOpt o;
  if (!shopt_get(c, &x, 1, SH_DEF_TAIL, JOB_OPTS, "sh.jobs", &o)) return VN;
  V jobs_v = opt_field(&x, 1, "jobs");
  if (jobs_v.t == V_NUM) {
    if (jobs_v.u.n < 1 || jobs_v.u.n > SH_MAX_JOBS) {
      set_error(c, E_RANGE, "sh.jobs: jobs=%d is out of band - 1..%d",
                (int)jobs_v.u.n, SH_MAX_JOBS);
      return VN;
    }
    o.jobs = (int)jobs_v.u.n;
  } else if (jobs_v.t != V_NULL) {
    set_error(c, E_TYPE, "sh.jobs: jobs must be a number, got %s", v_typename(jobs_v));
    return VN;
  }
  o.all = v_truthy(opt_field(&x, 1, "all"));
  V tok_v = opt_field(&x, 1, "token");
  if (tok_v.t == V_STR && tok_v.u.s.len) { o.token = tok_v.u.s; o.token_set = true; }
  else if (tok_v.t != V_NULL && tok_v.t != V_STR) {
    set_error(c, E_TYPE, "sh.jobs: token must be a string, got %s", v_typename(tok_v));
    return VN;
  }

  int n = jl->len;
  Job *J = (Job*)arena_zalloc(a, sizeof(Job) * (size_t)n);
  /* 1. shape: name + cmd required, names unique */
  for (int i = 0; i < n; i++) {
    V j = jl->v[i];
    if (j.t != V_REC) {
      set_error(c, E_TYPE, "sh.jobs: job %d must be a record {name,cmd,needs}, got %s",
                i + 1, v_typename(j));
      return VN;
    }
    V *nv = rec_getz(j.u.r, "name");
    if (!nv || nv->t != V_STR || !nv->u.s.len) {
      set_error(c, E_BAD_INPUT, "sh.jobs: job %d has no name (needs refers to it)", i + 1);
      return VN;
    }
    for (int k = 0; k < i; k++)
      if (s_eq(J[k].name, nv->u.s)) {
        set_error(c, E_BAD_INPUT, "sh.jobs: job name '%.*s' is used twice",
                  nv->u.s.len, nv->u.s.p);
        return VN;
      }
    J[i].name = nv->u.s;
    V *cv = rec_getz(j.u.r, "cmd");
    if (!cv) {
      set_error(c, E_BAD_INPUT, "sh.jobs: job '%.*s' has no cmd", J[i].name.len, J[i].name.p);
      return VN;
    }
    char who[128];
    snprintf(who, sizeof who, "sh.jobs job '%.*s'", J[i].name.len, J[i].name.p);
    J[i].cmd = cmd_from(c, *cv, who);
    if (ctx_has_err(c)) return VN;
    V *tv = rec_getz(j.u.r, "timeout");
    if (tv && tv->t == V_NUM) {
      if (!(tv->u.n > 0) || tv->u.n > SH_MAX_TIMEOUT_S) {
        set_error(c, E_RANGE,
                  "sh.jobs: job '%.*s' timeout=%g seconds is out of band - allowed 0.001..%d",
                  J[i].name.len, J[i].name.p, tv->u.n, SH_MAX_TIMEOUT_S);
        return VN;
      }
      J[i].timeout_ms = (int)(tv->u.n * 1000.0);
    } else if (tv && tv->t != V_NULL) {
      set_error(c, E_TYPE, "sh.jobs: job '%.*s' timeout must be a number of seconds, got %s",
                J[i].name.len, J[i].name.p, v_typename(*tv));
      return VN;
    }
  }
  /* 2. needs -> indices: an unknown name is a typo, not a dependency to ignore */
  for (int i = 0; i < n; i++) {
    V *dep = rec_getz(jl->v[i].u.r, "needs");
    if (!dep || dep->t == V_NULL) continue;
    if (dep->t != V_LIST) {
      set_error(c, E_TYPE, "sh.jobs: job '%.*s' needs must be a list of job names",
                J[i].name.len, J[i].name.p);
      return VN;
    }
    List *dl = dep->u.l;
    J[i].needs = (int*)arena_zalloc(a, sizeof(int) * (size_t)(dl->len ? dl->len : 1));
    J[i].nneeds = dl->len;
    for (int k = 0; k < dl->len; k++) {
      if (dl->v[k].t != V_STR) {
        set_error(c, E_TYPE, "sh.jobs: job '%.*s' needs[%d] must be a job name",
                  J[i].name.len, J[i].name.p, k);
        return VN;
      }
      int found = -1;
      for (int q = 0; q < n; q++) if (s_eq(J[q].name, dl->v[k].u.s)) { found = q; break; }
      if (found < 0) {
        Buf names; buf_init(&names, a);
        for (int q = 0; q < n; q++)
          buf_fmt(&names, "%s%.*s", q ? "|" : "", J[q].name.len, J[q].name.p);
        set_error(c, E_BAD_INPUT,
                  "sh.jobs: job '%.*s' needs '%.*s' which is not a job name here: %s",
                  J[i].name.len, J[i].name.p, dl->v[k].u.s.len, dl->v[k].u.s.p,
                  names.p ? names.p : "");
        return VN;
      }
      if (found == i) {
        set_error(c, E_BAD_INPUT, "sh.jobs: job '%.*s' needs itself",
                  J[i].name.len, J[i].name.p);
        return VN;
      }
      J[i].needs[k] = found;
      J[i].indeg++;                  /* an unmet prerequisite of THIS job, not of its dep */
    }
  }
  /* 3. deterministic topological order: always the earliest ready job, so the
   * same job list always produces the same token and the same run order */
  int *order = (int*)arena_zalloc(a, sizeof(int) * (size_t)n);
  int *deg = (int*)arena_zalloc(a, sizeof(int) * (size_t)n);
  bool *put = (bool*)arena_zalloc(a, sizeof(bool) * (size_t)n);
  for (int i = 0; i < n; i++) deg[i] = J[i].indeg;
  int placed = 0;
  for (;;) {
    int pick = -1;
    for (int i = 0; i < n; i++) if (!put[i] && deg[i] == 0) { pick = i; break; }
    if (pick < 0) break;
    put[pick] = true;
    order[placed++] = pick;
    for (int i = 0; i < n; i++)
      for (int k = 0; k < J[i].nneeds; k++)
        if (J[i].needs[k] == pick && deg[i] > 0) deg[i]--;
  }
  if (placed != n) {
    Buf cyc; buf_init(&cyc, a);
    for (int i = 0; i < n; i++)
      if (!put[i]) buf_fmt(&cyc, "%s%.*s", cyc.len ? "," : "", J[i].name.len, J[i].name.p);
    set_error(c, E_BAD_INPUT, "sh.jobs: needs cycle between [%s] - drop one edge to break it",
              cyc.p ? cyc.p : "");
    return VN;
  }

  /* 4. the plan: every job is an arg, so the token covers the whole graph */
  bool all_ro = true;
  for (int i = 0; i < n; i++) if (!J[i].cmd.ro_fast) all_ro = false;
  bool auto_run = false;
  Confirm need = policy_of(c, &o, all_ro, &auto_run);
  if (o.token_set) auto_run = true;                  /* the replay carries its own proof */

  /* kind stays PK_WRITE (plan_new default) on purpose: this plan is never handed
   * to plan_execute as a V_PLAN - sh.jobs runs its own graph (see the report). */
  Plan *p = plan_new(c, "sh.jobs", need);
  for (int i = 0; i < n; i++) {
    Job *j = &J[order[i]];
    Buf spec; buf_init(&spec, a);
    buf_fmt(&spec, "%.*s:%ds", j->name.len, j->name.p, j->timeout_ms / 1000);
    for (int k = 0; k < j->nneeds; k++)
      buf_fmt(&spec, "%s%.*s", k ? "+" : ">", J[j->needs[k]].name.len, J[j->needs[k]].name.p);
    plan_add_arg(c, p, "job", spec.p ? spec.p : "");
    plan_add_arg(c, p, "cmd", j->cmd.line ? j->cmd.line : "");
  }
  plan_add_arg(c, p, "planned", s_fmt(a, "%d", n).p);
  plan_add_arg(c, p, "parallel", s_fmt(a, "%d", o.jobs).p);
  plan_add_arg(c, p, "all", o.all ? "t" : "f");
  plan_add_arg(c, p, "timeout", s_fmt(a, "%d", o.timeout_ms).p);
  plan_add_arg(c, p, "tail", s_fmt(a, "%d", o.tail).p);
  plan_add_arg(c, p, "redact", o.redact ? "t" : "f");
  if (o.cwd) plan_add_arg(c, p, "cwd", o.cwd);
  /* env and stdin bind the WHOLE graph too: they are run-wide options here (every
   * job gets them), so a token minted for one environment must not be spendable on
   * a replay that changed it -- otherwise {token:...} would be a way to confirm a
   * command set while quietly swapping the CC/PATH it runs under. */
  if (o.env_set && o.env.n) plan_exec_env(c, p, &o.env);
  if (o.stdin_set)
    plan_exec_stdin(c, p, o.stdin_data.p ? o.stdin_data.p : "",
                    (size_t)(o.stdin_data.len > 0 ? o.stdin_data.len : 0));
  /* no "auto" marker in the plan args here: the same job list has to produce the
   * same token whether it is being inspected or replayed with {token:...} */
  Str tok = plan_token(c, p);

  if (o.token_set) {
    /* the second phase of the protocol: same job list in, same token, so a token
     * from an older or different graph cannot run anything */
    if (!s_eqz(o.token, tok.p)) {
      set_error(c, E_BAD_TOKEN,
                "sh.jobs: token '%.*s' does not match this job list (expected %s) - the jobs, "
                "their order, cwd or timeouts changed",
                o.token.len, o.token.p ? o.token.p : "", tok.p);
      return VN;
    }
  } else if (!auto_run) {
    /* Gate closed. A record with the real plan fields rather than a V_PLAN:
     * plan.c's executor runs exactly one command per plan, so the graph cannot
     * live behind PLAN.apply() yet (reported). */
    Rec *r = rec_new(a);
    plan_to_v(c, p, r);
    List *planned = list_new(a);
    for (int i = 0; i < n; i++) {
      Job *j = &J[order[i]];
      List *nd = list_new(a);
      for (int k = 0; k < j->nneeds; k++)
        list_push(a, nd, v_str(J[j->needs[k]].name));
      Rec *pr = rec_new(a);
      rec_setz(a, pr, "name", v_str(j->name));
      rec_setz(a, pr, "needs", list_of(nd));
      list_push(a, planned, rec_to_v(pr));
    }
    rec_setz(a, r, "order", list_of(planned));
    rec_setz(a, r, "hint", v_str(s_fmt(a, "sh.jobs(<the same job list>, {token:\"%s\"}) to run it",
                                        tok.p)));
    rec_setz(a, r, "jobs_sequential", v_bool(true));
    return rec_to_v(r);
  }

  /* 5. apply: sequential, in dependency order; a failure stops the run unless
   * {all:true}. o.jobs>1 is accepted and ignored on purpose - the semantics the
   * agent gets are exactly the documented sequential ones. */
  List *good = list_new(a), *bad = list_new(a);
  Str first_fail = s_null();
  long total_ms = 0;
  for (int i = 0; i < n; i++) {
    Job *j = &J[order[i]];
    ShOpt jo = o;
    jo.confirm_set = true;                    /* the gate above already passed */
    jo.confirm = CF_NONE;
    if (j->timeout_ms) jo.timeout_ms = j->timeout_ms;
    Plan *cp = plan_for(c, &j->cmd, &jo, CF_NONE, true, "sh.jobs", j->name.p);
    if (!cp) return VN;
    Str ctok = plan_token(c, cp);
    V res = run_now(c, &j->cmd, &jo, "sh.jobs", ctok.p, true, cp);
    if (res.t != V_REC) {
      set_error(c, E_INTERNAL, "sh.jobs: job '%.*s' returned no result record",
                j->name.len, j->name.p);
      return VN;
    }
    V code = VN, ms = VN;
    V *pc = rec_getz(res.u.r, "code"), *pm = rec_getz(res.u.r, "ms");
    if (pc) code = *pc;
    if (pm) ms = *pm;
    total_ms += (ms.t == V_NUM) ? (long)ms.u.n : 0;
    bool ok = (code.t == V_NUM && code.u.n == 0);
    Rec *e = rec_new(a);
    rec_setz(a, e, "name", v_str(j->name));
    rec_setz(a, e, "code", code);
    rec_setz(a, e, "ms", ms);
    if (!ok) {
      static const char *const COPY[] = { "out", "err", "truncated", "timed_out", NULL };
      for (int k = 0; COPY[k]; k++) {
        V *f = rec_getz(res.u.r, COPY[k]);
        if (f) rec_setz(a, e, COPY[k], *f);
      }
      if (!first_fail.p) first_fail = j->name;
      list_push(a, bad, rec_to_v(e));
      if (!o.all) break;
    } else {
      list_push(a, good, rec_to_v(e));
    }
  }
  Rec *r = rec_new(a);
  rec_setz(a, r, "op", v_str(s_lit(a, "sh.jobs")));
  rec_setz(a, r, "confirm", v_str(s_lit(a, "none")));
  if (all_ro) rec_setz(a, r, "auto", v_bool(true));
  rec_setz(a, r, "planned", v_num((double)n));
  rec_setz(a, r, "ran", v_num((double)(good->len + bad->len)));
  rec_setz(a, r, "done", list_of(good));
  rec_setz(a, r, "failed", list_of(bad));
  if (first_fail.p) rec_setz(a, r, "first_fail", v_str(first_fail));
  rec_setz(a, r, "ms", v_num((double)total_ms));
  rec_setz(a, r, "token", v_str(s_lit(a, tok.p)));
  rec_setz(a, r, "jobs", v_num((double)o.jobs));
  rec_setz(a, r, "jobs_sequential", v_bool(true));
  return rec_to_v(r);
}

/* ---------------- registry: one row per builtin, doc + example with it ------
 * The sig/doc pair here IS the documentation an agent reads (`vxa doc sh.run`), so
 * env and stdin are spelled out the way they now behave: merged, not replaced; fed
 * through a file, not a shell; bound into the token, shown masked. */
static const Builtin sh_tab[] = {
  { "sh", "run", f_run, 1, 2,
    "sh.run(argv|[str], {cwd,timeout:30,tail:40,env:{K:\"V\"},stdin:\"\",redact:true,confirm:\"ask\"}) -> PLAN|{code,out,err,ms}",
    "gated unless the argv is provably read-only; env:{K:V} is MERGED over this process's environment (PATH stays, nothing is ever setenv'd here) and stdin is fed to the child from a file, no shell - to the child stdin:\"\" and no stdin are the same immediate EOF, though the plan still tells them apart; both are hashed into the token and printed masked as env=K=***,stdin=sha:..,len:..",
    "sh.run([\"go\",\"test\",\"./...\"], {env:{GOFLAGS:\"-mod=mod\"}, timeout:120})", 0 },
  { "sh", "ro", f_ro, 1, 2,
    "sh.ro(argv|[str], {cwd,timeout:30,tail:40,env:{K:\"V\"},stdin:\"\",redact:true}) -> {code,out,err,ms}",
    "read-only allowlist execution with no plan phase; refuses with POLICY rather than run anything unproven, so never use it for a write; env/stdin behave as in sh.run",
    "sh.ro([\"git\",\"status\"])", 0 },
  { "sh", "allowed", f_allowed, 1, 1,
    "sh.allowed(argv|[str]) -> bool",
    "check before you pick a path: true means sh.ro accepts it and sh.run executes at once - a string is gated whatever this answers",
    "ok = sh.allowed([\"git\",\"diff\"])", BF_PURE },
  { "sh", "out", f_out, 1, 2,
    "sh.out(argv|[str], {cwd,timeout:30,tail:200,env:{K:\"V\"},stdin:\"\",redact:true}) -> str|PLAN",
    "only stdout, more lines; same policy as sh.run so it cannot bypass confirmation, and it drops the exit code - use sh.run when the code matters",
    "sh.out([\"cat\",\"notes.txt\"])", 0 },
  { "sh", "jobs", f_jobs, 1, 2,
    "sh.jobs([{name,cmd,needs,timeout}], {jobs:1,all:false,env:{K:\"V\"},stdin:\"\",confirm:\"ask\",token}) -> PLAN|{done,failed,first_fail}",
    "run a dependency-ordered command set in one round trip; env and stdin are run-wide and bound into the graph's token; a needs cycle is an error, a failure stops the run unless all:true, jobs>1 is accepted but runs 1 at a time",
    "sh.jobs([{name:\"a\",cmd:[\"cat\",\"a.txt\"]}], {all:true})", 0 },
};
const Builtin *t_sh(int *n) { *n = (int)(sizeof(sh_tab) / sizeof(sh_tab[0])); return sh_tab; }
