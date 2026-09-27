/* main.c - the VXA CLI: the only surface an agent ever touches.
 *
 * Three invariants hold this file together:
 *   1. stdout is machine output. One result per call, compact form (SPEC 2.1).
 *      Diagnostics and progress go to stderr and die under --quiet.
 *   2. $? is the protocol. Every failure leaves through err_exit(code), so the
 *      agent branches on a number and on `code=`, never on a sentence.
 *   3. A side effect is never a surprise: it either already happened inside the
 *      jail (new file => receipt) or it comes back as a PLAN with a token bound
 *      to the old content hashes. Repeating one failure three times is itself a
 *      stop signal (SPEC 3.7) and is reported ahead of the usual output.
 *
 * No helper exits or aborts: only main() returns a code. */
#include "vxa.h"
#include "interp.h"
#include "ast.h"
#include "lib.h"
#ifdef _WIN32
#define PATHSEP ";"
#else
#define PATHSEP ":"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  include <fcntl.h>
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/statvfs.h>
#endif

/* MinGW's io.h (and unistd.h) #define X_OK 1 as a POSIX access mode AFTER vxa.h
 * declared the exit-code enum, which silently turns every successful exit into
 * status 1. Take the enum constant back. */
#ifdef X_OK
#  undef X_OK
#endif

/* plan.c defines this for the stall detector but vxa.h never declared it */
void journal_run(Ctx *c, const char *fp, int code);

/* the single global this CLI owns (declared by vxa.h, readable by the libs) */
Opts O;

/* machine probes, defined with the doctor command and used by config seeding */
static const char *machine_os(void);
static const char *machine_arch(void);

#define LOOP_STREAK 3            /* SPEC 3.7: three identical runs = stop signal */
#define DEF_MAX_BYTES 65536      /* read budget unless config/--max-bytes says otherwise */
#define DEF_TIMEOUT_MS 30000
#define DEF_MAX_MEM ((size_t)512 * 1024u * 1024u)
#define SCAN_MAX 2000            /* entries fs_ls collects before --max slices them */
#define TOK_BYTES 4              /* SPEC 2: est tokens = ceil(bytes/4) */

/* ==================== plumbing ==================== */
static void opts_default(void) {
  memset(&O, 0, sizeof O);
  O.fmt = FMT_AUTO; O.pol_fs = CF_JAIL; O.pol_sh = CF_ASK;
  O.timeout_ms = DEF_TIMEOUT_MS; O.max_tok = DEF_MAX_BYTES / TOK_BYTES; O.jobs = 1;
  O.steps = 20000000LL; O.max_mem = DEF_MAX_MEM;
}

/* the C runtime must not turn '\n' into "\r\n": the protocol is bytes */
static void binary_stdio(void) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#endif
}

static void out_raw(const char *p, size_t n) {
  if (p && n) fwrite(p, 1, n, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}
static void out_str(Str s) { out_raw(s.p, (size_t)s.len); }
static void out_cstr(const char *s) { if (s) out_raw(s, strlen(s)); }
/* already newline-terminated blocks (generated doc pages) go out verbatim */
static void out_block(const char *p, size_t n) {
  if (p && n) fwrite(p, 1, n, stdout);
  fflush(stdout);
}
static void note(const char *fmt, ...) {
  if (O.quiet) return;
  char buf[512];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  fprintf(stderr, "vxa: %s\n", buf);
  fflush(stderr);
}
static void note_confirm(const char *tok) {
  if (O.quiet) return;
  fprintf(stderr, "vxa: nothing was applied; rerun with --confirm %s\n", tok ? tok : "<token>");
  fflush(stderr);
}

static const char *cwd_of(Arena *a) {
  char b[4096];
#ifdef _WIN32
  if (!_getcwd(b, sizeof b)) return arena_strdup(a, ".");
#else
  if (!getcwd(b, sizeof b)) return arena_strdup(a, ".");
#endif
  return arena_strdup(a, b);
}
/* the jail root: --root > nearest ancestor holding .vxa/ > cwd (SPEC 5) */
static char *root_walk_up(Arena *a, const char *from) {
  char *cur = arena_strdup(a, from);
  for (int hop = 0; hop < 12 && cur && *cur; hop++) {
    Str probe = path_join(a, cur, ".vxa");
    if (fs_is_dir(probe.p)) return cur;
    char *slash = strrchr(cur, '/');
    if (!slash || slash == cur) return NULL;
    *slash = 0;
  }
  return NULL;
}

/* ==================== the output funnel ==================== */
/* text form: readable, may span lines; auto and json stay on one line (SPEC 2.1) */
static void text_inline(Buf *b, V v) {
  if (v.t == V_STR) { buf_put(b, v.u.s.p, (size_t)v.u.s.len); return; }
  buf_puts(b, v_tostr(b->a, v, true).p);
}
static void text_disp(Buf *b, V v, int indent) {
  if (v.t == V_REC && v.u.r && v.u.r->len) {
    for (int i = 0; i < v.u.r->len; i++) {
      for (int k = 0; k < indent; k++) buf_putc(b, ' ');
      buf_put(b, v.u.r->kv[i].k.p, (size_t)v.u.r->kv[i].k.len);
      buf_puts(b, ": ");
      V val = v.u.r->kv[i].v;
      if ((val.t == V_STR && s_findz(val.u.s, "\n", 0) >= 0) || val.t == V_REC || val.t == V_LIST) {
        buf_putc(b, '\n');
        text_disp(b, val, indent + 2);
      } else text_inline(b, val);
      buf_putc(b, '\n');
    }
    return;
  }
  if (v.t == V_LIST && v.u.l && v.u.l->len) {
    for (int i = 0; i < v.u.l->len; i++) {
      for (int k = 0; k < indent; k++) buf_putc(b, ' ');
      buf_puts(b, "- ");
      V it = v.u.l->v[i];
      if (it.t == V_REC || it.t == V_LIST) { buf_putc(b, '\n'); text_disp(b, it, indent + 2); }
      else text_inline(b, it);
      buf_putc(b, '\n');
    }
    return;
  }
  for (int k = 0; k < indent; k++) buf_putc(b, ' ');
  text_inline(b, v);
  buf_putc(b, '\n');
}

void emit(V v, Fmt fmt, FILE *f) {
  Arena a;
  arena_init(&a, 0);
  Str s;
  bool newline = true;
  if (fmt == FMT_JSON) s = v_tojson(&a, v);
  else if (fmt == FMT_TEXT) {
    Buf b;
    buf_init(&b, &a);
    text_disp(&b, v, 0);
    s = buf_take(&b);
    newline = !(s.len && s.p[s.len - 1] == '\n');
  } else s = v_tostr(&a, v, true);
  if (s.p) fwrite(s.p, 1, (size_t)s.len, f);
  if (newline) fputc('\n', f);
  fflush(f);
  arena_free(&a);
}

/* ---------- error values and error lines ---------- */
static V err_val(Arena *a, ErrCode code, const char *msg, const char *hint,
                 const char *dk, const char *dv) {
  PErr *e = (PErr*)arena_zalloc(a, sizeof(PErr));
  e->code = code;
  e->msg = s_lit(a, msg ? msg : "");
  e->hint = s_lit(a, hint && *hint ? hint : err_hint(code));
  if (dk && dv) {
    Rec *d = rec_new(a);
    rec_setz(a, d, dk, v_strz(a, dv));
    e->data = rec_to_v(d);
  } else e->data = VN;
  V v; v.t = V_ERR; v.u.e = e; return v;
}
static void err_line(Arena *a, ErrCode code, const char *msg, const char *hint,
                     const char *dk, const char *dv) {
  out_str(v_tostr(a, err_val(a, code, msg, hint, dk, dv), true));
}
/* the usage error shape the protocol pins down (rule 11) */
static int bad_flag(Arena *a, const char *flag) {
  Buf b;
  buf_init(&b, a);
  buf_puts(&b, "!ERR code=BAD_INPUT flag=");
  buf_add_escaped(&b, s_wrap(flag ? flag : ""));
  buf_puts(&b, " hint=\"vxa help\"");
  out_str(buf_take(&b));
  return X_USAGE;
}

/* Pull a top-level `key=value` out of a compact record text. Depth aware, so the
 * commas inside files=[{..},{..}] cannot cut a value short. */
static const char *rec_field(Arena *a, const char *rec, const char *key) {
  if (!rec || !*rec) return NULL;
  size_t kl = strlen(key);
  const char *p = rec;
  int depth = 0;
  while (*p) {
    if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; } if (*p == '"') p++; continue; }
    if (*p == '{' || *p == '[') { depth++; p++; continue; }
    if (*p == '}' || *p == ']') { depth--; p++; continue; }
    bool at_key = depth == 1 && (p == rec || *(p - 1) == ',');
    if (at_key && !strncmp(p, key, kl) && p[kl] == '=') {
      const char *v = p + kl + 1, *q = v;
      int d = 0;
      for (; *q; q++) {
        if (*q == '"') { q++; while (*q && *q != '"') { if (*q == '\\' && q[1]) q++; q++; } if (!*q) break; continue; }
        if (*q == '{' || *q == '[') d++;
        else if (*q == '}' || *q == ']') { if (d == 0) break; d--; }
        else if (*q == ',' && d == 0) break;
      }
      return arena_strndup(a, v, (size_t)(q - v));
    }
    p++;
  }
  return NULL;
}
static bool looks_balanced(const char *s) {
  if (!s || !*s) return false;
  int br = 0, ck = 0;
  for (const char *p = s; *p; p++) {
    if (*p == '[') br++; else if (*p == ']') br--;
    else if (*p == '{') ck++; else if (*p == '}') ck--;
    if (br < 0 || ck < 0) return false;
  }
  return br == 0 && ck == 0;
}

/* NEED_CONFIRM must be addressable by field:
 *   !ERR code=NEED_CONFIRM op=fs.write token=9f2c1a files=[{path,kind,from,to,bytes}]
 *   hint="rerun with --confirm 9f2c1a"
 * When the plan layer hands over its PLAN record, that is exact. When it only
 * put the plan in the error prose, lift the fields back out of the message. */
/* unapplied = the script ended on a PLAN it never called .apply() on. Telling such
 * a caller to "rerun with --confirm" is a lie that costs a round trip: the rerun
 * behaves identically. It is a defect in the script, so it gets its own code. */
static void plan_line_from_rec(Ctx *c, Rec *r, bool unapplied) {
  Arena *a = ctx_arena(c);
  V *op = rec_getz(r, "op"), *tok = rec_getz(r, "token");
  V *files = rec_getz(r, "files"), *cnt = rec_getz(r, "count");
  Buf b;
  buf_init(&b, a);
  buf_puts(&b, unapplied ? "!ERR code=UNAPPLIED" : "!ERR code=NEED_CONFIRM");
  if (op && op->t == V_STR) buf_fmt(&b, " op=%s", op->u.s.p);
  if (tok && tok->t == V_STR) buf_fmt(&b, " token=%s", tok->u.s.p);
  if (files) buf_fmt(&b, " files=%s", v_tostr(a, *files, true).p);
  if (cnt && cnt->t == V_NUM) buf_fmt(&b, " count=%d", (int)cnt->u.n);
  buf_puts(&b, " hint=");
  if (unapplied)
    buf_add_escaped(&b, s_wrap("nothing was written: end the script with p.apply() on this plan"));

  else
    buf_add_escaped(&b, tok && tok->t == V_STR ? s_fmt(a, "rerun with --confirm %s", tok->u.s.p)
                                               : s_wrap(err_hint(E_NEED_CONFIRM)));
  out_str(buf_take(&b));
  if (!unapplied) note_confirm(tok && tok->t == V_STR ? tok->u.s.p : NULL);
}
static void confirm_line_from_rec(Ctx *c, Rec *r) { plan_line_from_rec(c, r, false); }
static void print_err(Arena *a, V ev, Fmt fmt) {
  if (!v_is_err(ev)) { emit(ev, fmt, stdout); return; }
  if (fmt == FMT_JSON) { out_str(v_tojson(a, ev)); return; }
  PErr *e = ev.u.e;
  if (e->code != E_NEED_CONFIRM || (e->data.t == V_REC && rec_getz(e->data.u.r, "token"))) {
    out_str(v_tostr(a, ev, true));
    return;
  }
  const char *msg = e->msg.p ? e->msg.p : "";
  const char *plan = strstr(msg, "plan: {");
  const char *body = plan ? plan + 6 : NULL;
  const char *op = rec_field(a, body, "op");
  const char *tok = rec_field(a, body, "token");
  const char *files = rec_field(a, body, "files");
  if (!tok && !op) { out_str(v_tostr(a, ev, true)); return; }
  Buf b;
  buf_init(&b, a);
  buf_puts(&b, "!ERR code=NEED_CONFIRM");
  if (op) buf_fmt(&b, " op=%s", op);
  if (tok) buf_fmt(&b, " token=%s", tok);
  if (looks_balanced(files)) buf_fmt(&b, " files=%s", files);
  Str m = s_wrap(msg);
  int cut = s_findz(m, " (plan:", 0);
  buf_puts(&b, " msg=");
  buf_add_escaped(&b, cut > 0 ? s_slice(m, 0, cut) : m);
  buf_puts(&b, " hint=");
  if (tok) buf_add_escaped(&b, s_fmt(a, "rerun with --confirm %s", tok));
  else buf_add_escaped(&b, s_wrap(err_hint(e->code)));
  out_str(buf_take(&b));
  note_confirm(tok);
}

/* ==================== flags ==================== */
typedef struct { const char *name; bool takes_value; } Flag;
#define FLAG_END { NULL, false }

typedef struct { const char *k; const char *v; } KV;
typedef struct {
  const char *pos[16]; int npos;
  KV kv[24]; int nkv;
  const char *bad;                    /* the flag that was rejected */
} Fargs;

static bool flag_lookup(const Flag *t1, const Flag *t2, const char *name, bool *takes) {
  for (const Flag *t = t1; t && t->name; t++) if (!strcmp(t->name, name)) { *takes = t->takes_value; return true; }
  if (t2) for (const Flag *t = t2; t && t->name; t++) if (!strcmp(t->name, name)) { *takes = t->takes_value; return true; }
  return false;
}
static const char *fval(const Fargs *f, const char *k) {
  for (int i = 0; i < f->nkv; i++) if (!strcmp(f->kv[i].k, k)) return f->kv[i].v;
  return NULL;
}
static bool fhas(const Fargs *f, const char *k) {
  const char *v = fval(f, k);
  return v && (!*v || !strcmp(v, "true"));
}

/* Strict on purpose (rule 11): an unknown flag, an abbreviation, a missing value
 * or a switch carrying one is a usage error. Nothing is guessed, because a
 * guessed flag costs the agent a whole wrong run. */
static int flags_parse(Arena *a, int argc, char **argv, int start,
                       const Flag *base, const Flag *extra, Fargs *f) {
  memset(f, 0, sizeof *f);
  for (int i = start; i < argc; i++) {
    const char *s = argv[i];
    if (s[0] == '-' && s[1] == '-' && s[2]) {
      const char *eq = strchr(s, '=');
      const char *name = eq ? arena_strndup(a, s, (size_t)(eq - s)) : s;
      const char *val = eq ? eq + 1 : NULL;
      bool takes = false;
      if (!flag_lookup(base, extra, name, &takes)) { f->bad = name; return -1; }
      if (takes) {
        if (!val) {
          if (i + 1 >= argc) { f->bad = name; return -1; }
          val = argv[++i];
        }
      } else if (val) {
        if (strcmp(val, "true") && strcmp(val, "false")) { f->bad = name; return -1; }
        if (!strcmp(val, "false")) continue;             /* explicitly off = unset */
      }
      if (f->nkv >= (int)(sizeof f->kv / sizeof f->kv[0])) { f->bad = name; return -1; }
      f->kv[f->nkv].k = name;
      f->kv[f->nkv].v = val ? val : "";
      f->nkv++;
    } else if (s[0] == '-' && s[1]) {
      f->bad = s; return -1;                             /* no short flags exist */
    } else {
      if (f->npos >= (int)(sizeof f->pos / sizeof f->pos[0])) { f->bad = s; return -1; }
      f->pos[f->npos++] = s;
    }
  }
  return 0;
}

/* declared in vxa.h as main.c helpers: a raw argv peek */
const char *arg_value(int argc, char **argv, const char *flag) {
  size_t fl = strlen(flag);
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], flag, fl)) continue;
    if (argv[i][fl] == '=') return argv[i] + fl + 1;
    if (!argv[i][fl] && i + 1 < argc) return argv[i + 1];
  }
  return NULL;
}
bool arg_has(int argc, char **argv, const char *flag) {
  size_t fl = strlen(flag);
  for (int i = 1; i < argc; i++)
    if (!strncmp(argv[i], flag, fl) && (argv[i][fl] == 0 || argv[i][fl] == '=')) return true;
  return false;
}
/* malloc'd on purpose: usable with no arena around; NUL-terminated */
char *read_file_z(const char *p, size_t *len_out) {
  if (len_out) *len_out = 0;
  if (!p) return NULL;
  if (!strcmp(p, "-")) {                                /* stdin, for `--ops -` */
    size_t cap = 8192, n = 0;
    char *b = (char*)malloc(cap);
    if (!b) return NULL;
    for (;;) {
      if (n + 512 > cap) {
        cap *= 2;
        char *r = (char*)realloc(b, cap);
        if (!r) { free(b); return NULL; }
        b = r;
      }
      size_t got = fread(b + n, 1, cap - n, stdin);
      if (got == 0) break;
      n += got;
    }
    b[n] = 0;
    if (len_out) *len_out = n;
    return b;
  }
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return NULL; }
  rewind(f);
  char *b = (char*)malloc((size_t)sz + 1);
  if (!b) { fclose(f); return NULL; }
  size_t got = sz ? fread(b, 1, (size_t)sz, f) : 0;
  fclose(f);
  b[got] = 0;
  if (len_out) *len_out = got;
  return b;
}

/* ==================== the command surface (one source of truth) ====================
 * These tables drive both flag validation and `vxa help`, so the manual cannot
 * drift from what the parser accepts. */
static const Flag FL_BASE[] = {
  { "--fmt", true }, { "--confirm", true }, { "--set", true }, { "--root", true },
  { "--timeout", true }, { "--steps", true }, { "--max-mem", true },
  { "--quiet", false }, { "--strict", false }, { "--no-audit", false }, { "--help", false },
  FLAG_END
};
static const Flag FL_NONE[] = { FLAG_END };
static const Flag FL_SCRIPT[] = { { "--args", true }, FLAG_END };
static const Flag FL_READ[] = {
  { "--lines", true }, { "--grep", true }, { "--ctx", true }, { "--anchors", false },
  { "--max-bytes", true }, { "--max-tok", true }, FLAG_END
};
static const Flag FL_LS[] = { { "--glob", true }, { "--recursive", false }, { "--max", true }, FLAG_END };
static const Flag FL_BUNDLE[] = {
  { "--query", true }, { "--max-bytes", true }, { "--max-tok", true }, { "--only-symbols", false }, FLAG_END
};
static const Flag FL_SH[] = {
  { "--ro", false }, { "--cwd", true }, { "--tail", true }, { "--no-redact", false }, FLAG_END
};
static const Flag FL_DIFF[] = { { "--ctx", true }, { "--unified", false }, { "--compact", false }, FLAG_END };
static const Flag FL_PATCH[] = { { "--ops", true }, FLAG_END };
static const Flag FL_FIND[] = { { "--glob", true }, { "--max", true }, { "--ctx", true }, FLAG_END };
static const Flag FL_DOC[] = { { "--all", false }, FLAG_END };
static const Flag FL_LANG[] = { { "--full", false }, FLAG_END };
static const Flag FL_CHECK[] = { { "--max", true }, FLAG_END };

typedef struct {
  const char *name;
  int npos_min, npos_max;              /* npos_max < 0 = any */
  const char *args;                    /* positional shape, for help */
  const char *summary;
  const Flag *extra;
  bool plan_mode;                      /* force: never execute */
  bool takes_token;                    /* a bare 12-hex token may follow */
} Cmd;

static const Cmd CMDS[] = {
  { "eval", 1, 2, "'expr'|- [token]", "one expression; prints its value", FL_SCRIPT, false, true },
  { "run", 1, 2, "<file.vxa>|- [token]", "run a script (side effects need --confirm)", FL_SCRIPT, false, true },
  { "plan", 1, 1, "<file.vxa>|-", "plan-only: never executes, prints PLAN(s)", FL_NONE, true, false },
  { "fs.read", 1, 1, "<p>", "bounded read: hash, lines, bytes, truncated flag", FL_READ, false, false },
  { "fs.ls", 0, 1, "[dir]", "list files: path, bytes, lines (sorted, stable)", FL_LS, false, false },
  { "fs.outline", 1, 1, "<p>", "symbols only - the skeleton, not the body", FL_NONE, false, false },
  { "fs.bundle", 1, -1, "<p>...", "several files, ranked, inside one byte budget", FL_BUNDLE, false, false },
  { "sh", 1, 1, "'cmd' [token]", "run a command; --ro only for whitelisted argv", FL_SH, false, true },
  { "diff", 2, 2, "<a> <b>", "diff two files", FL_DIFF, false, false },
  { "patch", 1, 2, "<p> --ops f.json|- [token]", "apply anchor ops (set/del/ins_before/ins_after)", FL_PATCH, false, true },
  { "find", 1, 2, "<needle> [dir]", "case-insensitive search across a tree", FL_FIND, false, false },
  { "doc", 0, 1, "[ns|ns.name]", "builtin docs, generated from the registry", FL_DOC, false, false },
  { "lang", 0, 0, "", "the cheat sheet: correct VXA after one call", FL_LANG, false, false },
  { "check", 0, 1, "[file|dir]", "syntax-check .vxa + run tests/*.vxa cases", FL_CHECK, false, false },
  { "doctor", 0, 0, "", "one environment line an agent can branch on", FL_NONE, false, false },
  { "version", 0, 0, "", "version, os, arch", FL_NONE, false, false },
  { "help", 0, 1, "[topic]", "this list, or one command's flags", FL_NONE, false, false },
  { NULL, 0, 0, NULL, NULL, NULL, false, false }
};
static const Cmd *cmd_spec(const char *name) {
  for (const Cmd *c = CMDS; c->name; c++) if (!strcmp(c->name, name)) return c;
  return NULL;
}
static void flags_help(Buf *b, const Flag *t) {
  for (; t && t->name; t++) { buf_fmt(b, " %s", t->name); if (t->takes_value) buf_puts(b, " V"); }
}
static void help_all(void) {
  Arena a;
  arena_init(&a, 0);
  Buf b;
  buf_init(&b, &a);
  buf_puts(&b, "vxa " VXA_VERSION " - commands (stdout is machine output; $? is the protocol)\n");
  for (const Cmd *c = CMDS; c->name; c++) {
    buf_fmt(&b, "%-12s %s", c->name, c->args ? c->args : "");
    flags_help(&b, FL_BASE);
    flags_help(&b, c->extra);
    buf_fmt(&b, "\n             %s\n", c->summary);
  }
  buf_puts(&b, "exit     0 ok 1 usage 2 script 3 need-confirm 4 policy 5 io 6 internal 7 timeout\n");
  buf_puts(&b, "confirm  rerun the SAME call with --confirm <token> to apply a plan; --confirm all applies whatever the script plans\n");
  buf_puts(&b, "config   --set k=v (scalar) > .vxa/config.json + .vxa/manifest.json > built-in defaults; --timeout is seconds\n");
  out_block(b.p, b.len);
  arena_free(&a);
}
static int help_topic(Arena *a, const char *topic) {
  const Cmd *c = cmd_spec(topic);
  if (c) {
    Buf b;
    buf_init(&b, a);
    buf_fmt(&b, "vxa %s %s", c->name, c->args ? c->args : "");
    flags_help(&b, FL_BASE);
    flags_help(&b, c->extra);
    buf_fmt(&b, "\n%s\n", c->summary);
    out_block(b.p, b.len);
    return X_OK;
  }
  const char *dot = strchr(topic, '.');
  const char *ns = dot ? arena_strndup(a, topic, (size_t)(dot - topic)) : "";
  const char *nm = dot ? dot + 1 : topic;
  const Builtin *bl = lib_find(ns, nm);
  if (bl && !strcmp(bl->name, nm) && !strcmp(bl->ns, ns)) {
    Buf b;
    buf_init(&b, a);
    doc_one(a, &b, topic);
    buf_putc(&b, '\n');
    out_block(b.p, b.len);
    return X_OK;
  }
  err_line(a, E_BAD_INPUT, "unknown help topic", "vxa help lists commands; vxa doc <ns> lists builtins", "topic", topic);
  return X_USAGE;
}

/* ==================== session: arena + ctx + the three config layers ========== */
typedef struct {
  Arena a;
  Ctx *c;
  Fargs f;
  const Cmd *cmd;
  char *root;
  const char *confirm;
  bool is_eval;                 /* eval always prints its value, null included */
  bool done;                    /* fully handled here: caller returns code */
  int code;
} Sess;

static void seed(Ctx *c, const char *k, V v) {
  rec_setz(ctx_arena(c), ctx_cfg(c), k, v);
}
static int cfg_int(Ctx *c, const char *k, int dflt) {
  V *p = rec_getz(ctx_cfg(c), k);
  if (!p) return dflt;
  if (p->t == V_NUM) return (int)p->u.n;
  if (p->t == V_STR) { long long i; if (s_is_int(p->u.s, &i)) return (int)i; }
  return dflt;
}
/* --set values are scalars: VXA refuses to guess, and so does this. Structured
 * config belongs in cfg.set inside the script. */
static V coerce(Arena *a, const char *v) {
  long long i;
  Str s = s_wrap(v);
  if (!strcmp(v, "true")) return VT;
  if (!strcmp(v, "false")) return VF;
  if (!strcmp(v, "null")) return VN;
  char *end = NULL;
  double d = strtod(v, &end);
  if (end && end != v && !*end && isfinite(d)) return v_num(d);
  if (s_is_int(s, &i)) return v_num((double)i);
  return v_strz(a, v);
}
/* flatten a parsed JSON/VXA record into dotted keys, the shape the plan layer
 * looks up: policy_for("policies.fs") (SPEC 6/7) */
static void flatten(Ctx *c, const char *prefix, V v) {
  Arena *a = ctx_arena(c);
  if (v.t == V_REC && v.u.r) {
    for (int i = 0; i < v.u.r->len; i++) {
      Str k = v.u.r->kv[i].k;
      Str full = (prefix && *prefix) ? s_fmt(a, "%s.%.*s", prefix, k.len, k.p) : k;
      flatten(c, full.p, v.u.r->kv[i].v);
    }
    return;
  }
  if (prefix && *prefix) seed(c, prefix, v);
}
static void load_kv_file(Sess *s, const char *path) {
  if (!fs_exists(path)) return;
  Ctx *c = s->c;
  Arena *a = ctx_arena(c);
  size_t n = 0; ErrCode e = E_NONE;
  char *txt = fs_slurp(a, path, &n, &e);
  if (!txt) { note("ignoring %s (%s)", path, err_name(e)); return; }
  V perr = VN;
  Node *p = parse_script(c, txt, n, path, &perr);
  ctx_clear_err(c);
  if (!p) { note("ignoring %s (not a valid record)", path); return; }
  V v = eval_node(c, p);
  ctx_clear_err(c);
  if (v.t != V_REC) { note("ignoring %s", path); return; }
  flatten(c, "", v);
}
/* A JSON document is a VXA record literal, so the manifest needs no second
 * parser: read it with the language and rename the two keys whose spelling the
 * plan layer and the manifest disagree on (SPEC 7 vs policy_for). */
static void manifest_alias(Ctx *c, const char *from, const char *to, bool over) {
  V *v = rec_getz(ctx_cfg(c), from);
  if (!v) return;
  if (!over && rec_getz(ctx_cfg(c), to)) return;
  seed(c, to, *v);
}
static void apply_set_flags(Sess *s, Ctx *c) {
  Arena *a = ctx_arena(c);
  for (int i = 0; i < s->f.nkv; i++)
    if (!strcmp(s->f.kv[i].k, "--set")) {
      const char *kv = s->f.kv[i].v;
      const char *eq = strchr(kv, '=');
      if (!eq) continue;                                  /* validated in sess_open */
      char *k = arena_strndup(a, kv, (size_t)(eq - kv));
      cfg_set_c(c, k, coerce(a, eq + 1));
    }
}

static Ctx *make_ctx(Sess *s, const char *script) {
  Ctx *c = ctx_new(&s->a, s->root, script);
  ctx_set_confirm(c, s->confirm ? s->confirm : "");
  Arena *a = ctx_arena(c);
  /* layer 3: built-in defaults */
  seed(c, "policies.fs", v_strz(a, "jail"));
  seed(c, "policies.sh", v_strz(a, "ask"));
  seed(c, "policies.cfg", v_strz(a, "jail"));
  seed(c, "fmt", v_strz(a, O.fmt == FMT_JSON ? "json" : O.fmt == FMT_TEXT ? "text" : "auto"));
  seed(c, "max_bytes", v_num(DEF_MAX_BYTES));
  seed(c, "max_tok", v_num(O.max_tok));
  seed(c, "timeout", v_num(O.timeout_ms));
  seed(c, "steps", v_num((double)O.steps));
  seed(c, "jobs", v_num(O.jobs));
  seed(c, "strict", v_bool(O.strict));
  seed(c, "audit", v_bool(!O.no_audit));
  seed(c, "root", v_strz(a, s->root));
  seed(c, "version", v_strz(a, VXA_VERSION));
  seed(c, "os", v_strz(a, machine_os()));
  seed(c, "arch", v_strz(a, machine_arch()));
  /* layer 2: .vxa/config.json, then .vxa/manifest.json */
  load_kv_file(s, path_join(a, s->root, ".vxa/config.json").p);
  ctx_clear_err(c);
  load_kv_file(s, path_join(a, s->root, ".vxa/manifest.json").p);
  ctx_clear_err(c);
  manifest_alias(c, "policies.shell", "policies.sh", true);
  manifest_alias(c, "vars.max_tok", "max_tok", true);
  manifest_alias(c, "vars.fmt", "fmt", true);
  /* VXA_* overrides paths.* only - it never reaches policy (SPEC 6) */
  static const char *envk[3][2] = {
    { "VXA_TMP", "paths.tmp" }, { "VXA_CACHE", "paths.cache" }, { "VXA_LOG", "paths.log" }
  };
  for (int i = 0; i < 3; i++) {
    const char *v = getenv(envk[i][0]);
    if (v && *v) seed(c, envk[i][1], v_strz(a, v));
  }
  /* layer 1: CLI wins, so --set and --fmt are re-applied last (SPEC 6) */
  if (fval(&s->f, "--fmt")) seed(c, "fmt", v_strz(a, O.fmt == FMT_JSON ? "json" : O.fmt == FMT_TEXT ? "text" : "auto"));
  seed(c, "timeout", v_num(O.timeout_ms));
  seed(c, "steps", v_num((double)O.steps));
  seed(c, "strict", v_bool(O.strict));
  apply_set_flags(s, c);
  if (s->cmd && s->cmd->plan_mode) {
    /* `vxa plan` must not touch the disk: every policy becomes 'all', so the plan
     * layer demands a token even for a new file inside the jail. Forced last. */
    seed(c, "policies.fs", v_strz(a, "all"));
    seed(c, "policies.sh", v_strz(a, "all"));
    seed(c, "policies.cfg", v_strz(a, "all"));
    seed(c, "policies.tx", v_strz(a, "all"));
  }
  ctx_set_fmt(c, O.fmt);
  ctx_set_strict(c, O.strict);
  ctx_set_steps(c, O.steps);
  ctx_set_anchors(c, fhas(&s->f, "--anchors"));
  if (s->confirm && !(s->cmd && s->cmd->plan_mode)) ctx_set_confirm(c, s->confirm);
  const char *args = fval(&s->f, "--args");
  if (args) { ctx_set_argstr(c, args); seed(c, "args", v_strz(a, args)); }
  return c;
}

/* ---------- numeric option parsing (suffixes only where the SPEC has them) -- */
static long long flong(const Fargs *f, const char *k, long long dflt, bool *bad) {
  const char *v = fval(f, k);
  if (!v) return dflt;
  char *end = NULL;
  double d = strtod(v, &end);
  if (end == v || (end && *end) || !isfinite(d) || d < 0 || d > 1e18) {
    if (bad) *bad = true;
    return dflt;
  }
  return (long long)d;
}
static int fint(const Fargs *f, const char *k, int dflt, bool *bad) {
  long long v = flong(f, k, dflt, bad);
  if (v > 1000000000LL) { if (bad) *bad = true; return dflt; }
  return (int)v;
}
/* --timeout is seconds; an explicit s/ms/m/h suffix is accepted */
static int parse_ms(const char *v, bool *bad) {
  if (!v || !*v) return DEF_TIMEOUT_MS;
  char *end = NULL;
  double d = strtod(v, &end);
  if (end == v || !isfinite(d) || d < 0) { if (bad) *bad = true; return DEF_TIMEOUT_MS; }
  while (*end == ' ') end++;
  double mult;
  if (!strcmp(end, "ms")) mult = 1;
  else if (!strcmp(end, "s") || !*end) mult = 1000;
  else if (!strcmp(end, "m")) mult = 60000;
  else if (!strcmp(end, "h")) mult = 3600000;
  else { if (bad) *bad = true; return DEF_TIMEOUT_MS; }
  double ms = d * mult;
  if (ms > 86400000.0) { if (bad) *bad = true; return DEF_TIMEOUT_MS; }
  return (int)(ms + 0.5);
}
static bool is_token(const char *s) {
  if (!s) return false;
  if (!strcmp(s, "all")) return true;
  size_t n = strlen(s);
  if (n != 12) return false;
  for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

static int sess_fail(Sess *s, const char *flag) {
  s->code = bad_flag(&s->a, flag);
  s->done = true;
  return s->code;
}
/* Open a session: parse flags, resolve the jail, build the ctx.
 * Returns X_OK with s->done false when the session is usable; otherwise the
 * call is finished (help, usage error) and s->code is the exit code. */
static int sess_open(Sess *s, const char *cmdname, int argc, char **argv, int start,
                     const char *script) {
  memset(s, 0, sizeof *s);
  /* One arena for the whole call, sized before anything is allocated: the limit
   * itself is a flag, so --max-mem is peeked with the raw argv helper. */
  {
    const char *m = arg_value(argc, argv, "--max-mem");
    if (m && *m) {
      char *e = NULL;
      double d = strtod(m, &e);
      if (e != m && isfinite(d) && d > 0 && d < 1e7) O.max_mem = (size_t)d * 1024u * 1024u;
    }
  }
  arena_init(&s->a, O.max_mem);
  s->cmd = cmd_spec(cmdname);
  if (flags_parse(&s->a, argc, argv, start, FL_BASE, s->cmd ? s->cmd->extra : FL_NONE, &s->f) != 0)
    return sess_fail(s, s->f.bad ? s->f.bad : (start < argc ? argv[start] : cmdname));
  if (fhas(&s->f, "--help")) {
    help_topic(&s->a, s->cmd ? s->cmd->name : cmdname);
    s->done = true; s->code = X_OK;
    return X_OK;
  }
  const char *fmt = fval(&s->f, "--fmt");
  if (fmt) {
    if (!strcmp(fmt, "auto")) O.fmt = FMT_AUTO;
    else if (!strcmp(fmt, "json")) O.fmt = FMT_JSON;
    else if (!strcmp(fmt, "text")) O.fmt = FMT_TEXT;
    else return sess_fail(s, "--fmt");
  }
  O.quiet = fhas(&s->f, "--quiet");
  O.no_audit = fhas(&s->f, "--no-audit");
  O.strict = fhas(&s->f, "--strict");
  bool bad = false;
  O.timeout_ms = parse_ms(fval(&s->f, "--timeout"), &bad);
  if (bad) return sess_fail(s, "--timeout");
  long long steps = flong(&s->f, "--steps", (long long)O.steps, &bad);
  if (bad) return sess_fail(s, "--steps");
  if (steps > 0) O.steps = steps;
  long long mb = flong(&s->f, "--max-mem", 512, &bad);
  if (bad) return sess_fail(s, "--max-mem");
  if (mb > 0) O.max_mem = (size_t)mb * 1024u * 1024u;
  for (int i = 0; i < s->f.nkv; i++)
    if (!strcmp(s->f.kv[i].k, "--set") && !strchr(s->f.kv[i].v, '=')) return sess_fail(s, "--set");
  const char *rflag = fval(&s->f, "--root");
  if (rflag && !*rflag) return sess_fail(s, "--root");
  char *root;
  if (rflag)
    root = path_norm(&s->a, path_is_abs(rflag) ? rflag : path_join(&s->a, cwd_of(&s->a), rflag).p).p;
  else {
    char *cw = arena_strdup(&s->a, path_norm(&s->a, cwd_of(&s->a)).p);
    char *walk = root_walk_up(&s->a, cw);
    root = walk ? walk : cw;
  }
  s->root = root;
  if (s->cmd && (s->f.npos < s->cmd->npos_min ||
                 (s->cmd->npos_max >= 0 && s->f.npos > s->cmd->npos_max))) {
    err_line(&s->a, E_BAD_INPUT, "wrong number of arguments", "vxa help", "need", s->cmd->args);
    s->done = true; s->code = X_USAGE;
    return X_USAGE;
  }
  s->confirm = fval(&s->f, "--confirm");
  if (s->confirm && s->cmd && s->cmd->plan_mode) return sess_fail(s, "--confirm");
  if (!s->confirm && s->cmd && s->cmd->takes_token && s->f.npos > s->cmd->npos_min &&
      is_token(s->f.pos[s->f.npos - 1])) {
    s->confirm = s->f.pos[s->f.npos - 1];       /* a bare token is accepted */
    s->f.npos--;
  }
  s->c = make_ctx(s, script ? script : (s->f.npos > 0 ? s->f.pos[0] : cmdname));
  return X_OK;
}
static void sess_close(Sess *s) {
  if (s->c) ctx_free(s->c);
  s->c = NULL;
  arena_free(&s->a);
}
/* every command opens the same way, and none of them may keep going when the
 * session decided the call is finished */
#define SESS(sname, cmdname, script)                                            \
  Sess sname;                                                                   \
  if (sess_open(&sname, (cmdname), argc, argv, (start), (script)) != X_OK) {    \
    int _c = sname.code; sess_close(&sname); return _c;                         \
  } else if (sname.done) {                                                      \
    int _c = sname.code; sess_close(&sname); return _c;                         \
  } else (void)0

/* ==================== run / eval / plan ==================== */
static const char *resolve_confirm(Sess *s, Plan *p);   /* defined with the CLI plans */
static V plan_rec(Ctx *c, Plan *p) {
  Rec *r = rec_new(ctx_arena(c));
  plan_to_v(c, p, r);
  return rec_to_v(r);
}
/* fingerprint = script | sorted args | exit | first error code (SPEC 3.7).
 * No commas allowed: the journal line is `fp=...,code=N`. */
static Str fingerprint(Arena *a, const Fargs *f, const char *base, int code, ErrCode ec) {
  Buf b;
  buf_init(&b, a);
  buf_puts(&b, base ? base : "-");
  buf_putc(&b, '|');
  const char *items[16];
  int n = 0;
  for (int i = 0; i < f->nkv && n < 16; i++) {
    const char *k = f->kv[i].k;
    if (strcmp(k, "--set") && strcmp(k, "--confirm") && strcmp(k, "--args") && strcmp(k, "--ops")) continue;
    items[n++] = s_fmt(a, "%s=%s", k + 2, f->kv[i].v).p;
  }
  for (int i = 1; i < n; i++)                  /* sorted: flag order is not state */
    for (int j = i; j > 0 && strcmp(items[j - 1], items[j]) > 0; j--) {
      const char *t = items[j - 1]; items[j - 1] = items[j]; items[j] = t;
    }
  for (int i = 0; i < n; i++) buf_fmt(&b, "%s%s", i ? ";" : "", items[i]);
  buf_fmt(&b, "|%d|%s", code, err_name(ec));
  Str out = buf_take(&b);
  for (int i = 0; i < out.len; i++)
    if (out.p[i] == ',' || out.p[i] == '\n' || out.p[i] == '\r') out.p[i] = ';';
  return out;
}
/* LOOP_DETECTED leads the output: an agent that repeats an identical failure
 * three times is stuck, and the cheapest fix is to stop repeating */
static void stall_check(Ctx *c, Arena *a, const Fargs *f, const char *base, int code, ErrCode ec) {
  Str fp = fingerprint(a, f, base, code, ec);
  journal_run(c, fp.p, code);
  int streak = journal_repeat(c, fp.p);
  if (streak < LOOP_STREAK) return;
  Buf b;
  buf_init(&b, a);
  buf_fmt(&b, "!ERR code=LOOP_DETECTED times=%d same_as=prev hint=\"change something instead of repeating this\" fp=%s",
          streak, fp.p);
  out_str(buf_take(&b));
}

/* lib_core's out.* builtins answer {emitted=t}/{logged=t}. That ack is not a
 * result, and echoing it would break "one result per invocation", so the CLI
 * swallows it. (The cleaner fix is out.emit returning null; until then this is
 * the one place that knows the ack shape.) */
static bool is_ack(V v) {
  if (v.t != V_REC || !v.u.r || v.u.r->len != 1) return false;
  Str k = v.u.r->kv[0].k;
  if (!s_eqz(k, "emitted") && !s_eqz(k, "logged")) return false;
  return v_truthy(v.u.r->kv[0].v);
}

static int run_src(Sess *s, const char *src, size_t len, const char *name, const char *fpbase) {
  Arena *a = ctx_arena(s->c);
  bool planning = s->cmd && s->cmd->plan_mode;
  V perr = VN;
  Node *prog = parse_script(s->c, src, len, name, &perr);
  V res = VN, ev = VN;
  ErrCode ec = E_NONE;
  if (!prog) ev = v_is_err(perr) ? perr : err_val(a, E_SYNTAX, "cannot parse", NULL, NULL, NULL);
  else {
    res = eval_node(s->c, prog);
    if (ctx_has_err(s->c)) ev = ctx_err(s->c);
    else if (v_is_err(res)) ev = res;
    /* A PLAN that reaches the CLI was never applied by the script itself.
     * Do NOT execute it here: only the trailing value would run, so a script with
     * several writes would silently drop all but the last while still exiting 0 -
     * an agent would then act on files that were never written. Report it and let
     * the exit code make the missing .apply() visible. */
  }
  bool plan_shown = !v_is_err(ev) && res.t == V_PLAN;
  int code;
  if (v_is_err(ev)) { ec = v_errcode(ev); code = err_exit(ec); }
  else code = plan_shown ? (planning ? X_CONFIRM : X_SCRIPT) : X_OK;
  if (plan_shown)
    fprintf(stderr, "vxa: script ended on an unapplied PLAN; nothing was written."
                    " call p.apply() on it in the script (inspect without running: vxa plan)\n");

  stall_check(s->c, a, &s->f, fpbase, code, ec);

  if (v_is_err(ev)) {
    /* NEED_CONFIRM for a plan I hold is rendered from the plan itself, so token
     * and file list are exact instead of scraped back out of the message */
    print_err(a, ev, O.fmt);
  } else if (plan_shown) plan_line_from_rec(s->c, plan_rec(s->c, res.u.pl).u.r, !planning);
  else if (planning) {
    if (res.t != V_NULL) emit(res, O.fmt, stdout);
    else out_cstr("plan=none");
  } else if (!is_ack(res) && (res.t != V_NULL || s->is_eval)) emit(res, O.fmt, stdout);
  return code;
}

/* read '-' (stdin) or a path relative to CWD, never to --root (rule 7) */
static char *load_src(Sess *s, const char *p, size_t *len_out, int *code) {
  Arena *a = ctx_arena(s->c);
  *code = X_OK;
  if (!strcmp(p, "-")) {
    size_t n = 0;
    char *m = read_file_z("-", &n);
    if (!m) { err_line(a, E_IO, "cannot read stdin", NULL, NULL, NULL); *code = X_IO; return NULL; }
    char *d = (char*)arena_alloc(a, n + 1);
    memcpy(d, m, n + 1);
    free(m);
    *len_out = n;
    return d;
  }
  if (fs_is_dir(p)) {
    err_line(a, E_ISDIR, p, NULL, NULL, NULL);
    *code = err_exit(E_ISDIR);
    return NULL;
  }
  ErrCode e = E_NONE;
  size_t n = 0;
  char *d = fs_slurp(a, p, &n, &e);
  if (!d) {
    if (e == E_NONE) e = E_NOENT;
    err_line(a, e, p, NULL, NULL, NULL);
    *code = err_exit(e);
    return NULL;
  }
  *len_out = n;
  return d;
}

static int cmd_script(int argc, char **argv, int start, const char *cmdname) {
  bool is_eval = !strcmp(cmdname, "eval");
  SESS(s, cmdname, is_eval ? "<eval>" : NULL);
  s.is_eval = is_eval;
  const char *what = s.f.pos[0];
  Arena *a = ctx_arena(s.c);
  size_t len = 0;
  char *src;
  if (is_eval && strcmp(what, "-")) {
    len = strlen(what);
    src = (char*)arena_alloc(a, len + 1);
    memcpy(src, what, len + 1);
  } else {                          /* a script file, or '-' for stdin */
    int code = X_OK;
    src = load_src(&s, what, &len, &code);
    if (!src) { sess_close(&s); return code; }
  }
  Str h = hash12(a, src, len);
  const char *fpbase = is_eval ? s_fmt(a, "eval#%.*s", h.len, h.p).p
                               : s_fmt(a, "%s#%.*s", what, h.len, h.p).p;
  int code = run_src(&s, src, len, is_eval ? "<eval>" : what, fpbase);
  sess_close(&s);
  return code;
}

/* ==================== plans the CLI owns (sh / patch) ==================== */
static const char *resolve_confirm(Sess *s, Plan *p) {
  if (!s->confirm) return "";
  if (!strcmp(s->confirm, "all")) return plan_token(s->c, p).p;  /* whatever this plan is */
  return s->confirm;
}
static int apply_plan(Sess *s, Plan *p, const char *fpbase) {
  Arena *a = ctx_arena(s->c);
  /* an error raised while the plan was being built (an out-of-jail --cwd, for
   * instance) must be reported, not cleared and then executed anyway */
  if (ctx_has_err(s->c)) {
    V e = ctx_err(s->c);
    ErrCode ec = v_errcode(e);
    int code = err_exit(ec);
    stall_check(s->c, a, &s->f, fpbase, code, ec);
    print_err(a, e, O.fmt);
    return code;
  }
  if (!p) {
    V e = err_val(a, E_INTERNAL, "no plan was built", NULL, NULL, NULL);
    ErrCode ec = v_errcode(e);
    int code = err_exit(ec);
    stall_check(s->c, a, &s->f, fpbase, code, ec);
    print_err(a, e, O.fmt);
    return code;
  }
  ctx_clear_err(s->c);
  V r = plan_execute(s->c, p, resolve_confirm(s, p));
  V ev = ctx_has_err(s->c) ? ctx_err(s->c) : (v_is_err(r) ? r : VN);
  ErrCode ec = v_is_err(ev) ? v_errcode(ev) : E_NONE;
  int code = ec ? err_exit(ec) : X_OK;
  stall_check(s->c, a, &s->f, fpbase, code, ec);
  if (v_is_err(ev)) {
    if (ec == E_NEED_CONFIRM) confirm_line_from_rec(s->c, plan_rec(s->c, p).u.r);
    else print_err(a, ev, O.fmt);
    return code;
  }
  if (r.t != V_NULL) emit(r, O.fmt, stdout);
  return code;
}

/* ==================== fs.* read commands ==================== */
static bool parse_line_spec(const char *spec, int *from, int *to) {
  *from = 0; *to = 0;
  if (!spec || !*spec) return true;
  const char *d = strchr(spec, '-');
  if (!d) {
    if (!isdigit((unsigned char)spec[0])) return false;
    *from = atoi(spec);
    *to = *from;
    return *from > 0;
  }
  for (const char *p = spec; p < d; p++) if (!isdigit((unsigned char)*p)) return false;
  for (const char *p = d + 1; *p; p++) if (!isdigit((unsigned char)*p)) return false;
  if (d != spec) *from = atoi(spec);
  *to = atoi(d + 1);
  return *from >= 0 && *to >= 0;
}
static int cmd_fs_read(int argc, char **argv, int start) {
  SESS(s, "fs.read", "<fs.read>");
  Arena *a = ctx_arena(s.c);
  const char *path = s.f.pos[0];
  int from = 0, to = 0;
  bool bad = false;
  if (!parse_line_spec(fval(&s.f, "--lines"), &from, &to)) { int r = bad_flag(a, "--lines"); sess_close(&s); return r; }
  int ctxl = fint(&s.f, "--ctx", 0, &bad);
  int mb = fint(&s.f, "--max-bytes", cfg_int(s.c, "max_bytes", DEF_MAX_BYTES), &bad);
  if (fval(&s.f, "--max-tok") && !fval(&s.f, "--max-bytes")) mb = fint(&s.f, "--max-tok", 0, &bad) * TOK_BYTES;
  if (bad || mb < 0) { int r = bad_flag(a, "--max-bytes"); sess_close(&s); return r; }
  ErrCode e = E_NONE;
  V v = fs_read_range(s.c, path, from, to, fval(&s.f, "--grep"), fhas(&s.f, "--anchors"), ctxl, mb, &e);
  if (v.t == V_NULL || v_is_err(v) || e != E_NONE) {
    if (!v_is_err(v)) v = err_val(a, e == E_NONE ? E_IO : e, path, NULL, NULL, NULL);
    print_err(a, v, O.fmt);
    int code = err_exit(v_errcode(v));
    sess_close(&s);
    return code;
  }
  emit(v, O.fmt, stdout);
  sess_close(&s);
  return X_OK;
}
static int cmd_fs_ls(int argc, char **argv, int start) {
  SESS(s, "fs.ls", "<fs.ls>");
  Arena *a = ctx_arena(s.c);
  const char *dir = s.f.npos ? s.f.pos[0] : ".";
  bool bad = false;
  int max = fint(&s.f, "--max", 200, &bad);
  if (bad || max < 0) { int r = bad_flag(a, "--max"); sess_close(&s); return r; }
  ErrCode e = E_NONE;
  V all = fs_ls(s.c, dir, fval(&s.f, "--glob"), fhas(&s.f, "--recursive"), SCAN_MAX, &e);
  if (e != E_NONE || all.t != V_LIST) {
    if (!v_is_err(all)) all = err_val(a, e == E_NONE ? E_IO : e, dir, NULL, NULL, NULL);
    print_err(a, all, O.fmt);
    int code = err_exit(v_errcode(all));
    sess_close(&s);
    return code;
  }
  int n = all.u.l->len;
  int shown = (max > 0 && n > max) ? max : n;
  List *cut = list_new(a);
  long long bytes = 0;
  for (int i = 0; i < shown; i++) {
    list_push(a, cut, all.u.l->v[i]);
    V *bz = rec_getz(all.u.l->v[i].u.r, "bytes");
    if (bz && bz->t == V_NUM) bytes += (long long)bz->u.n;
  }
  V out = v_ok(a, 7,
    "dir", v_strz(a, dir),
    "total", v_num((double)n),
    "shown", v_num((double)shown),
    "bytes", v_num((double)bytes),
    "truncated", v_bool(n > shown),
    "recursive", v_bool(fhas(&s.f, "--recursive")),
    "files", list_of(cut));
  emit(out, O.fmt, stdout);
  sess_close(&s);
  return X_OK;
}
static int cmd_fs_outline(int argc, char **argv, int start) {
  SESS(s, "fs.outline", "<fs.outline>");
  Arena *a = ctx_arena(s.c);
  const char *path = s.f.pos[0];
  int code = X_OK;
  size_t n = 0;
  char *txt = load_src(&s, path, &n, &code);
  if (!txt) { sess_close(&s); return code; }
  Str text = s_from(a, txt, n);
  char lang[16];
  lang[0] = 0;
  outline_lang_detect(path, text, lang);
  V v = outline_file(s.c, path, text, lang[0] ? lang : NULL);
  if (v.t == V_NULL || v_is_err(v)) {
    if (!v_is_err(v)) v = err_val(a, E_IO, path, NULL, NULL, NULL);
    print_err(a, v, O.fmt);
    code = err_exit(v_errcode(v));
  } else emit(v, O.fmt, stdout);
  sess_close(&s);
  return code;
}
static int cmd_fs_bundle(int argc, char **argv, int start) {
  SESS(s, "fs.bundle", "<fs.bundle>");
  Arena *a = ctx_arena(s.c);
  List *paths = list_new(a);
  for (int i = 0; i < s.f.npos; i++) list_push(a, paths, v_strz(a, s.f.pos[i]));
  bool bad = false;
  int mb = fint(&s.f, "--max-bytes", cfg_int(s.c, "max_bytes", DEF_MAX_BYTES), &bad);
  if (fval(&s.f, "--max-tok") && !fval(&s.f, "--max-bytes")) mb = fint(&s.f, "--max-tok", 0, &bad) * TOK_BYTES;
  if (bad || mb <= 0) { int r = bad_flag(a, "--max-bytes"); sess_close(&s); return r; }
  V v = bundle_files(s.c, list_of(paths), fval(&s.f, "--query"), mb, fhas(&s.f, "--only-symbols"));
  if (v.t == V_NULL || v_is_err(v)) {
    if (!v_is_err(v)) v = err_val(a, E_IO, "bundle", NULL, NULL, NULL);
    print_err(a, v, O.fmt);
    int code = err_exit(v_errcode(v));
    sess_close(&s);
    return code;
  }
  emit(v, O.fmt, stdout);
  sess_close(&s);
  return X_OK;
}

/* ==================== sh ==================== */
/* Split into argv, quote aware. Anything that still smells like shell syntax is
 * refused before the whitelist sees it, so `--ro` cannot be talked into it. */
static int split_argv(Arena *a, const char *cmd, char ***out) {
  if (strlen(cmd) + 1 > 2048) return -1;
  if (strpbrk(cmd, ";|&<>$`\n")) return -1;
  char *buf = arena_strdup(a, cmd);
  char **av = (char**)arena_zalloc(a, sizeof(char*) * 32);
  int n = 0;
  size_t cap = strlen(buf) + 1, i = 0;
  while (i < cap) {
    while (buf[i] == ' ' || buf[i] == '\t') buf[i++] = 0;
    if (!buf[i]) break;
    char tok[512];
    size_t tl = 0;
    char q = 0;
    while (buf[i]) {
      char ch = buf[i];
      if (q) {
        if (ch == q) q = 0;
        else { if (tl + 1 >= sizeof tok) return -1; tok[tl++] = ch; }
      } else if (ch == '"' || ch == '\'') q = ch;
      else if (ch == ' ' || ch == '\t') break;
      else { if (tl + 1 >= sizeof tok) return -1; tok[tl++] = ch; }
      i++;
    }
    if (q || tl == 0) return -1;
    tok[tl] = 0;
    if (n >= 32) return -1;
    av[n++] = arena_strdup(a, tok);
  }
  if (n <= 0) return -1;
  *out = av;
  return n;
}
/* Re-join argv into the single string the whitelist checker parses. Quoting is
 * only what split_argv understands again, so the array that was approved is the
 * array that runs. (sh_join_argv is declared in vxa.h but not defined anywhere
 * yet, so the CLI does its own join rather than link against a hole.) */
static const char *argv_join(Arena *a, char **av, int n) {
  Buf b;
  buf_init(&b, a);
  for (int i = 0; i < n; i++) {
    if (i) buf_putc(&b, ' ');
    const char *s = av[i];
    if (*s && strpbrk(s, " \t\"'")) { buf_putc(&b, '"'); buf_puts(&b, s); buf_putc(&b, '"'); }
    else buf_puts(&b, s);
  }
  return buf_take(&b).p;
}
static int cmd_sh(int argc, char **argv, int start) {
  SESS(s, "sh", "<sh>");
  Arena *a = ctx_arena(s.c);
  const char *cmd = s.f.pos[0];
  bool bad = false;
  int tail = fint(&s.f, "--tail", 40, &bad);
  if (bad || tail < 0) { int r = bad_flag(a, "--tail"); sess_close(&s); return r; }
  bool redact = !fhas(&s.f, "--no-redact");
  const char *cwd = fval(&s.f, "--cwd");
  Plan *p;
  if (fhas(&s.f, "--ro")) {
    char **av = NULL;
    int n = split_argv(a, cmd, &av);
    const char *argvv = n > 0 ? argv_join(a, av, n) : NULL;
    bool approved = argvv && *argvv && sh_is_readonly(argvv);
    if (approved) {
      /* argv form, never a shell: read-only, so the fast path applies at once */
      List *l = list_new(a);
      for (int i = 0; i < n; i++) list_push(a, l, v_strz(a, av[i]));
      p = plan_exec_new(s.c, "sh.ro", CF_NONE, list_of(l));
    } else {
      note("--ro not granted (not a whitelisted argv command): this needs a plan");
      p = plan_exec_new(s.c, "sh.run", CF_ASK, v_strz(a, cmd));
    }
  } else {
    p = plan_exec_new(s.c, "sh.run", CF_ASK, v_strz(a, cmd));
  }
  Str fp = s_fmt(a, "sh#%.*s|%s", 8, hash12(a, cmd, strlen(cmd)).p, cwd ? cwd : ".");
  plan_exec_opts(s.c, p, cwd, O.timeout_ms, tail, redact);
  int code = apply_plan(&s, p, fp.p);
  /* a command that ran and failed is a RESULT (code= in the payload), not a VXA
   * failure: only our own inability to start it, or a timeout, moves $? */
  sess_close(&s);
  return code;
}

/* ==================== diff / patch ==================== */
static int cmd_diff(int argc, char **argv, int start) {
  SESS(s, "diff", "<diff>");
  Arena *a = ctx_arena(s.c);
  int code = X_OK;
  size_t na = 0, nb = 0;
  char *A = load_src(&s, s.f.pos[0], &na, &code);
  if (!A) { sess_close(&s); return code; }
  char *B = load_src(&s, s.f.pos[1], &nb, &code);
  if (!B) { sess_close(&s); return code; }
  bool bad = false;
  int ctxl = fint(&s.f, "--ctx", 3, &bad);
  if (bad || ctxl < 0) { int r = bad_flag(a, "--ctx"); sess_close(&s); return r; }
  V v = tx_diff(a, s_from(a, A, na), s_from(a, B, nb), ctxl, fhas(&s.f, "--unified"));
  if (v.t == V_NULL || v_is_err(v)) {
    if (!v_is_err(v)) v = err_val(a, E_INTERNAL, "diff produced nothing", NULL, NULL, NULL);
    print_err(a, v, O.fmt);
    code = err_exit(v_errcode(v));
  } else emit(v, O.fmt, stdout);
  sess_close(&s);
  return code;
}
static int cmd_patch(int argc, char **argv, int start) {
  SESS(s, "patch", "<patch>");
  Arena *a = ctx_arena(s.c);
  const char *path = s.f.pos[0];
  const char *opsf = fval(&s.f, "--ops");
  if (!opsf) {
    err_line(a, E_BAD_INPUT, "patch needs --ops <file.json|->", "vxa help patch", "flag", "--ops");
    sess_close(&s);
    return X_USAGE;
  }
  int code = X_OK;
  size_t tn = 0, on = 0;
  char *text = load_src(&s, path, &tn, &code);
  if (!text) { sess_close(&s); return code; }
  Str fp = s_fmt(a, "patch#%.*s", 12, hash12(a, text, tn).p);
  char *osrc = read_file_z(opsf, &on);
  if (!osrc) {
    err_line(a, E_NOENT, opsf, "write the ops file first, or use --ops -", "path", opsf);
    sess_close(&s);
    return X_IO;
  }
  V perr = VN;
  Node *op = parse_script(s.c, osrc, on, opsf, &perr);
  if (!op) {
    print_err(a, perr, O.fmt);
    free(osrc);
    sess_close(&s);
    return err_exit(v_errcode(perr));
  }
  ctx_clear_err(s.c);
  V ops = eval_node(s.c, op);
  free(osrc);
  if (ctx_has_err(s.c)) ops = ctx_err(s.c);
  if (v_is_err(ops)) {
    print_err(a, ops, O.fmt);
    sess_close(&s);
    return err_exit(v_errcode(ops));
  }
  if (ops.t != V_LIST) {
    err_line(a, E_TYPE, "--ops must be a list of {at,op,text,n}", "vxa help patch", NULL, NULL);
    sess_close(&s);
    return X_SCRIPT;
  }
  V pr = tx_patch_apply(a, s_from(a, text, tn), path, ops);
  if (v_is_err(pr) || pr.t != V_REC) {
    if (!v_is_err(pr)) pr = err_val(a, E_INTERNAL, "patch produced nothing", NULL, NULL, NULL);
    print_err(a, pr, O.fmt);
    sess_close(&s);
    return err_exit(v_errcode(pr));
  }
  V *tv = rec_getz(pr.u.r, "text");
  if (!tv || tv->t != V_STR) {
    err_line(a, E_INTERNAL, "patch returned no text", NULL, NULL, NULL);
    sess_close(&s);
    return X_INTERNAL;
  }
  Plan *p = plan_target(s.c, "fs.write", policy_for(s.c, "policies.fs"), PK_WRITE, path);
  if (!p) {
    V e = ctx_has_err(s.c) ? ctx_err(s.c) : err_val(a, E_IO, path, NULL, NULL, NULL);
    print_err(a, e, O.fmt);
    int c2 = err_exit(v_errcode(e));
    sess_close(&s);
    return c2;
  }
  ctx_clear_err(s.c);
  plan_set_payload(s.c, p, tv->u.s.p, (size_t)tv->u.s.len);
  int rc = apply_plan(&s, p, fp.p);
  sess_close(&s);
  return rc;
}

/* ==================== find ==================== */
/* returns false when the file could not be searched at all */
static bool scan_file(Arena *a, const char *path, Str needle, int ctxl, int max,
                      List *hits, int *total, long long *bytes) {
  size_t n = 0; ErrCode e = E_NONE;
  char *txt = fs_slurp(a, path, &n, &e);
  if (!txt) return false;
  if (memchr(txt, 0, n)) return false;                  /* binary */
  if (bytes) *bytes += (long long)n;
  Str *prev = ctxl > 0 ? (Str*)arena_zalloc(a, sizeof(Str) * (size_t)ctxl) : NULL;
  int nprev = 0, wi = 0;
  size_t off = 0;
  int lineno = 0;
  for (;;) {
    size_t end = off;
    while (end < n && txt[end] != '\n') end++;
    Str line = s_from(a, txt + off, end - off);
    lineno++;
    if (s_find(s_lower(a, line), needle, 0) >= 0) {
      (*total)++;
      if (max <= 0 || hits->len < max) {
        Rec *h = rec_new(a);
        rec_setz(a, h, "path", v_strz(a, path));
        rec_setz(a, h, "line", v_num(lineno));
        rec_setz(a, h, "text", v_str(line));
        if (ctxl > 0 && nprev > 0) {
          List *c = list_new(a);
          for (int k = 0; k < nprev; k++) list_push(a, c, v_str(prev[(wi + k) % nprev]));
          rec_setz(a, h, "before", list_of(c));
        }
        list_push(a, hits, rec_to_v(h));
      }
    }
    if (ctxl > 0) {
      if (nprev < ctxl) { prev[nprev++] = line; }
      else { prev[wi] = line; wi = (wi + 1) % ctxl; }
    }
    if (end >= n) break;
    off = end + 1;
  }
  return true;
}
static int cmd_find(int argc, char **argv, int start) {
  SESS(s, "find", "<find>");
  Arena *a = ctx_arena(s.c);
  const char *dirn = s.f.npos > 1 ? s.f.pos[1] : ".";
  bool bad = false;
  int max = fint(&s.f, "--max", 200, &bad);
  int ctxl = fint(&s.f, "--ctx", 0, &bad);
  if (bad || max < 0 || ctxl < 0) { int r = bad_flag(a, "--max"); sess_close(&s); return r; }
  Str nd = s_lower(a, s_wrap(s.f.pos[0]));
  ErrCode e = E_NONE;
  V all = fs_ls(s.c, dirn, NULL, true, SCAN_MAX, &e);
  if (e != E_NONE || all.t != V_LIST) {
    if (!v_is_err(all)) all = err_val(a, e == E_NONE ? E_IO : e, dirn, NULL, NULL, NULL);
    print_err(a, all, O.fmt);
    int code = err_exit(v_errcode(all));
    sess_close(&s);
    return code;
  }
  const char *gp = fval(&s.f, "--glob");
  List *hits = list_new(a);
  int total = 0, scanned = 0, skipped = 0;
  long long bytes = 0;
  for (int i = 0; i < all.u.l->len; i++) {
    V *pv = rec_getz(all.u.l->v[i].u.r, "path");
    if (!pv || pv->t != V_STR) continue;
    if (gp && *gp && !glob_match(gp, path_base(pv->u.s.p))) continue;
    if (scan_file(a, pv->u.s.p, nd, ctxl, max, hits, &total, &bytes)) scanned++;
    else skipped++;
  }
  V out = v_ok(a, 9,
    "needle", v_strz(a, s.f.pos[0]),
    "dir", v_strz(a, dirn),
    "total", v_num(total),
    "shown", v_num((double)hits->len),
    "bytes", v_num((double)bytes),
    "files", v_num(scanned),
    "skipped", v_num(skipped),
    "truncated", v_bool(total > hits->len),
    "matches", list_of(hits));
  emit(out, O.fmt, stdout);
  sess_close(&s);
  return X_OK;
}

/* ==================== doc / lang / check / doctor / version ==================== */
static int cmd_doc(int argc, char **argv, int start) {
  SESS(s, "doc", "<doc>");
  Arena *a = ctx_arena(s.c);
  if (!s.f.npos) {
    int n = 0;
    const Builtin *t = lib_all(&n);
    const char *seen[16];
    int ns = 0;
    for (int i = 0; i < n; i++) {
      bool dup = false;
      for (int k = 0; k < ns; k++) if (!strcmp(seen[k], t[i].ns)) { dup = true; break; }
      if (!dup && ns < 16) seen[ns++] = t[i].ns;
    }
    Buf b;
    buf_init(&b, a);
    buf_puts(&b, "namespaces=");
    for (int i = 0; i < ns; i++) buf_fmt(&b, "%s%s", i ? "," : "", seen[i]);
    buf_fmt(&b, " builtins=%d hint=\"vxa doc <ns> | vxa doc <ns>.<name> | vxa lang\"", n);
    out_str(buf_take(&b));
    sess_close(&s);
    return X_OK;
  }
  const char *topic = s.f.pos[0];
  Buf b;
  buf_init(&b, a);
  const char *dot = strchr(topic, '.');
  if (dot) {
    const char *ns = arena_strndup(a, topic, (size_t)(dot - topic));
    const Builtin *bl = lib_find(ns, dot + 1);
    if (!bl || strcmp(bl->ns, ns) || strcmp(bl->name, dot + 1)) {
      err_line(a, E_NOENT, "no such builtin", "vxa doc <ns> lists the namespace", "name", topic);
      sess_close(&s);
      return X_IO;
    }
    doc_one(a, &b, topic);
    buf_putc(&b, '\n');
  } else {
    doc_namespace(a, &b, topic, fhas(&s.f, "--all"));
  }
  out_block(b.p, b.len);
  sess_close(&s);
  return X_OK;
}
static int cmd_lang(int argc, char **argv, int start) {
  SESS(s, "lang", "<lang>");
  Arena *a = ctx_arena(s.c);
  Buf b;
  buf_init(&b, a);
  doc_lang(a, &b, fhas(&s.f, "--full") ? 3 : 1);
  out_block(b.p, b.len);
  sess_close(&s);
  return X_OK;
}

/* Syntax check with a position: parse_script() drops the line number, so this
 * command drives the lexer and the parser directly. */
static int check_syntax(Arena *a, const char *path, char *msg, size_t msn, int *eline) {
  size_t n = 0; ErrCode e = E_NONE;
  char *src = fs_slurp(a, path, &n, &e);
  *eline = 0;
  msg[0] = 0;
  if (!src) { snprintf(msg, msn, "%s: %s", err_name(e), err_hint(e)); return -1; }
  Lexer lx;
  memset(&lx, 0, sizeof lx);
  lx.a = a;
  lex(a, src, n, &lx);
  if (lx.err) {
    *eline = lx.err_line;
    snprintf(msg, msn, "%s", lx.err);
    return 1;
  }
  char *pe = NULL;
  Node *p = parse_program(a, lx.toks, lx.n, path, &pe, eline);
  if (!p) { snprintf(msg, msn, "%s", pe ? pe : "syntax error"); return 1; }
  return 0;
}
static bool path_has_tests(const char *p) {
  const char *s = p;
  while (*s) {
    if (*s == '/' || *s == '\\') { s++; continue; }
    const char *st = s;
    while (*s && *s != '/' && *s != '\\') s++;
    if (s - st == 5 && !strncmp(st, "tests", 5)) return true;
  }
  return false;
}
static int cmd_check(int argc, char **argv, int start) {
  SESS(s, "check", "<check>");
  Arena *a = &s.a;
  const char *target = s.f.npos ? s.f.pos[0] : ".";
  bool bad = false;
  int maxf = fint(&s.f, "--max", 500, &bad);
  if (bad || maxf <= 0) { int r = bad_flag(a, "--max"); sess_close(&s); return r; }
  List *files = list_new(a);
  if (fs_is_dir(target)) {
    ErrCode e = E_NONE;
    V all = fs_ls(s.c, target, "*.vxa", true, maxf, &e);
    if (e != E_NONE || all.t != V_LIST) {
      err_line(a, e == E_NONE ? E_IO : e, target, NULL, NULL, NULL);
      sess_close(&s);
      return err_exit(e == E_NONE ? E_IO : e);
    }
    for (int i = 0; i < all.u.l->len; i++) {
      V *pv = rec_getz(all.u.l->v[i].u.r, "path");
      if (pv && pv->t == V_STR) list_push(a, files, v_str(pv->u.s));
    }
  } else {
    size_t n = 0; ErrCode e = E_NONE;
    char *t = fs_slurp(a, target, &n, &e);
    if (!t) {
      err_line(a, e == E_NONE ? E_NOENT : e, target, NULL, NULL, NULL);
      sess_close(&s);
      return err_exit(e == E_NONE ? E_NOENT : e);
    }
    list_push(a, files, v_strz(a, target));
  }
  char msg[256];
  int nfail = 0, ncase = 0, npass = 0, ncasefail = 0;
  for (int i = 0; i < files->len; i++) {
    const char *path = files->v[i].u.s.p;
    int line = 0;
    if (check_syntax(a, path, msg, sizeof msg, &line) != 0) {
      nfail++;
      Buf b;
      buf_init(&b, a);
      buf_fmt(&b, "%s:%d: %s", path, line, msg);
      out_str(buf_take(&b));
      continue;
    }
    if (!path_has_tests(path)) continue;
    ncase++;
    size_t n = 0; ErrCode se = E_NONE;
    char *src = fs_slurp(a, path, &n, &se);
    if (!src) {
      ncasefail++;
      err_line(a, se == E_NONE ? E_IO : se, path, "case file unreadable", NULL, NULL);
      continue;
    }
    Ctx *cc = make_ctx(&s, path);              /* a ctx per case: no leaked bindings */
    V perr = VN;
    Node *p = parse_script(cc, src, n, path, &perr);
    if (!p) {
      ncasefail++;
      print_err(a, perr, FMT_AUTO);
      ctx_free(cc);
      continue;
    }
    V res = eval_node(cc, p);
    V ev = ctx_has_err(cc) ? ctx_err(cc) : (v_is_err(res) ? res : VN);
    if (v_is_err(ev)) {
      ncasefail++;
      print_err(a, ev, FMT_AUTO);
    } else if (res.t != V_NULL && !v_truthy(res)) {
      ncasefail++;
      err_line(a, E_LIMIT, "case returned a false value", "a case file must end true", "file", path);
    } else npass++;
    ctx_free(cc);
  }
  Buf b;
  buf_init(&b, a);
  buf_fmt(&b, "files=%d ok=%d fail=%d", files->len, files->len - nfail, nfail);
  if (ncase) buf_fmt(&b, " cases=%d pass=%d fail=%d", ncase, npass, ncasefail);
  out_str(buf_take(&b));
  sess_close(&s);
  return (nfail || ncasefail) ? X_SCRIPT : X_OK;
}

/* ---------- machine probes: fields, never prose ---------- */
static const char *machine_os(void) {
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "mac";
#elif defined(__linux__)
  return "linux";
#else
  return "posix";
#endif
}
static const char *machine_arch(void) {
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
  return "x64";
#elif defined(_M_IX86) || defined(__i386__)
  return "x86";
#else
  return "unknown";
#endif
}
static double disk_free_gb(const char *p) {
#ifdef _WIN32
  char q[4096];
  size_t i = 0;
  for (; p && p[i] && i + 1 < sizeof q; i++) q[i] = (p[i] == '/' ? '\\' : p[i]);
  q[i] = 0;
  ULARGE_INTEGER avail, total;
  /* the second out param is TOTAL bytes - reading it as free overstates by the
   * whole used amount, which is the one number an agent trusts to decide to proceed */
  if (!GetDiskFreeSpaceExA(i ? q : ".", &avail, &total, 0)) return -1.0;
  return (double)avail.QuadPart / 1073741824.0;
#else
  struct statvfs st;
  if (statvfs(p && *p ? p : ".", &st) != 0) return -1.0;
  return (double)st.f_bavail * (double)st.f_frsize / 1073741824.0;
#endif
}
static bool utf8_env(void) {
#ifdef _WIN32
  return GetACP() == CP_UTF8 || GetConsoleOutputCP() == CP_UTF8;
#else
  return true;
#endif
}
/* Is a C compiler reachable? Answered by looking for the executable on PATH,
 * never by running something and reading its output. */
static bool g_cc_on_path = false;
const char *cc_probe(void) {
  static char hit[1024];
  hit[0] = 0;
  g_cc_on_path = false;
  static const char *names[] = { "cc", "gcc", "clang", "cl", NULL };
  static const char *roots[] = { "C:/msys64/mingw64/bin", "C:/msys64/ucrt64/bin",
                                 "C:/msys64/usr/bin", "C:/MinGW/bin", NULL };
  const char *path = getenv("CC");
  if (path && *path) {
    char with[600];
    snprintf(with, sizeof with, "%s.exe", path);
    if (fs_exists(path)) { snprintf(hit, sizeof hit, "%s", path); g_cc_on_path = true; }
    else if (fs_exists(with)) { snprintf(hit, sizeof hit, "%s", with); g_cc_on_path = true; }
  }
  if (!hit[0] && path && *path) return NULL;      /* CC set but wrong: say so, do not guess */
  path = getenv("PATH");
  if (!hit[0] && path && *path) {
    char *dup = (char*)malloc(strlen(path) + 1);
    if (dup) {
      char *save = NULL;
      strcpy(dup, path);
      for (char *tok = strtok_r(dup, PATHSEP, &save); tok && !hit[0]; tok = strtok_r(NULL, PATHSEP, &save)) {
        if (strlen(tok) > 512) continue;
        for (int k = 0; names[k] && !hit[0]; k++)
          for (int ext = 0; ext < 2 && !hit[0]; ext++) {
            char cand[600];
            snprintf(cand, sizeof cand, "%s/%s%s", tok, names[k], ext ? ".exe" : "");
            if (fs_exists(cand)) { snprintf(hit, sizeof hit, "%s", cand); g_cc_on_path = true; }
          }
      }
      free(dup);
    }
  }
  if (hit[0]) return hit;
  for (int r = 0; roots[r] && !hit[0]; r++)
    for (int k = 0; names[k] && !hit[0]; k++) {
      char cand[600];
      snprintf(cand, sizeof cand, "%s/%s.exe", roots[r], names[k]);
      if (fs_exists(cand)) snprintf(hit, sizeof hit, "%s", cand);
    }
  return hit[0] ? hit : NULL;
}

static bool tmp_writable(const char *tmp) {
  if (!tmp || !*tmp) return false;
  ensure_dir_for(tmp);
  if (!fs_is_dir(tmp)) return false;
  char cand[4300];
  snprintf(cand, sizeof cand, "%s/.vxa-probe", tmp);
  FILE *f = fopen(cand, "wb");
  if (!f) return false;
  fputs("x", f);
  fclose(f);
  remove(cand);
  return true;
}
static int cmd_doctor(int argc, char **argv, int start) {
  SESS(s, "doctor", "<doctor>");
  Arena *a = ctx_arena(s.c);
  char tmpbuf[4096];
  const char *tmp = ws_tmp(s.c);
  V *pt = rec_getz(ctx_cfg(s.c), "paths.tmp");
  if (pt && pt->t == V_STR) {
    Str j = path_join(a, s.root, pt->u.s.p);
    snprintf(tmpbuf, sizeof tmpbuf, "%s", j.p);
    tmp = tmpbuf;
  }
  double gb = disk_free_gb(s.root);
  const char *cc = cc_probe();
  bool have_cc = cc != NULL;
  bool cc_ready = have_cc && g_cc_on_path;
  bool tw = tmp_writable(tmp);
  Buf b;
  buf_init(&b, a);
  buf_fmt(&b, "vxa %s os=%s arch=%s cc=%s root=%s tmp=%s disk_free_gb=%.1f utf8=%s args=%d",
          VXA_VERSION, machine_os(), machine_arch(), cc ? cc : "no",
          s.root, tmp ? tmp : "-", gb < 0 ? 0.0 : gb, utf8_env() ? "t" : "f", argc);
  Buf badb;
  buf_init(&badb, a);
  /* found-but-not-on-PATH is actionable, so it is reported as its own condition
     rather than folded into cc=no (which sent two agents hunting a phantom) */
  if (!have_cc) buf_puts(&badb, "cc");
  else if (!cc_ready) buf_puts(&badb, "cc-not-on-path");
  if (gb < 0.5) buf_fmt(&badb, "%sdisk", badb.len ? "," : "");
  if (!tw) buf_fmt(&badb, "%stmp", badb.len ? "," : "");
  if (badb.len) buf_fmt(&b, " bad=%s", badb.p);
  out_str(buf_take(&b));
  sess_close(&s);
  if (!tw || gb < 0.0) return X_IO;
  if (!have_cc) return X_POLICY;                /* a required tool is missing (SPEC 7) */
  return X_OK;
}
static int cmd_version(int argc, char **argv, int start) {
  (void)argc; (void)argv; (void)start;
  Arena a;
  arena_init(&a, 0);
  out_str(s_fmt(&a, "vxa %s os=%s arch=%s", VXA_VERSION, machine_os(), machine_arch()));
  arena_free(&a);
  return X_OK;
}

/* ==================== dispatch ==================== */
int main(int argc, char **argv) {
  binary_stdio();
  opts_default();
  if (argc < 2) {
    Arena a;
    arena_init(&a, 0);
    err_line(&a, E_BAD_INPUT, "no command given", "vxa help", NULL, NULL);
    arena_free(&a);
    return X_USAGE;
  }
  const char *cmd = argv[1];
  if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) cmd = "help";
  else if (!strcmp(cmd, "-v") || !strcmp(cmd, "--version")) cmd = "version";
  if (!strcmp(cmd, "help")) {
    if (argc > 2) {
      Arena a;
      arena_init(&a, 0);
      int r = help_topic(&a, argv[2]);
      arena_free(&a);
      return r;
    }
    help_all();
    return X_OK;
  }
  if (!cmd_spec(cmd)) {
    Arena a;
    arena_init(&a, 0);
    err_line(&a, E_BAD_INPUT, "unknown command", "vxa help", "command", cmd);
    arena_free(&a);
    return X_USAGE;
  }
  if (!strcmp(cmd, "eval") || !strcmp(cmd, "run") || !strcmp(cmd, "plan"))
    return cmd_script(argc, argv, 2, cmd);
  if (!strcmp(cmd, "fs.read"))    return cmd_fs_read(argc, argv, 2);
  if (!strcmp(cmd, "fs.ls"))      return cmd_fs_ls(argc, argv, 2);
  if (!strcmp(cmd, "fs.outline")) return cmd_fs_outline(argc, argv, 2);
  if (!strcmp(cmd, "fs.bundle"))  return cmd_fs_bundle(argc, argv, 2);
  if (!strcmp(cmd, "sh"))         return cmd_sh(argc, argv, 2);
  if (!strcmp(cmd, "diff"))       return cmd_diff(argc, argv, 2);
  if (!strcmp(cmd, "patch"))      return cmd_patch(argc, argv, 2);
  if (!strcmp(cmd, "find"))       return cmd_find(argc, argv, 2);
  if (!strcmp(cmd, "doc"))        return cmd_doc(argc, argv, 2);
  if (!strcmp(cmd, "lang"))       return cmd_lang(argc, argv, 2);
  if (!strcmp(cmd, "check"))      return cmd_check(argc, argv, 2);
  if (!strcmp(cmd, "doctor"))     return cmd_doctor(argc, argv, 2);
  if (!strcmp(cmd, "version"))    return cmd_version(argc, argv, 2);
  Arena a;
  arena_init(&a, 0);
  err_line(&a, E_UNSUPPORTED, cmd, "vxa help lists what exists", NULL, NULL);
  arena_free(&a);
  return X_USAGE;
}
