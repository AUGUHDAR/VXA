/* shell.c - sh.* : run external commands for the agent (SPEC §4.3).
 *
 * Threat model, and what that means in this file:
 *   - argv-array execution: a plain "prog arg arg" string is tokenized into
 *     real argv and executed WITHOUT a shell, so `; && | $() ` > <` carry no
 *     meaning at all. Quoting follows the MSVC argv rules and is built
 *     byte-by-byte (never string concatenation), so an argument cannot close
 *     itself out of the quote it lives in.
 *   - a string containing shell metacharacters is an explicit request for
 *     shell semantics: it goes through cmd.exe /c and can never be read-only.
 *   - three temp files instead of pipes: no pipe-buffer deadlock, and the real
 *     byte size is known before we touch memory (we seek, never slurp). Stdin is
 *     handed over as a file we wrote, so a command that reads input never finds
 *     the console and waits for a keystroke nobody is going to press.
 *   - the child's environment is a block we build per run (parent's entries +
 *     the caller's overrides, merged and sorted). The process environment is
 *     never touched: it would leak one command's env into every later one.
 *   - timeout is a wall-clock budget; on expiry the child is killed together
 *     with its whole tree (job object with KILL_ON_JOB_CLOSE / POSIX process
 *     group) and whatever it had already written is still returned.
 *   - redaction runs last, on exactly the bytes that reach the transcript.
 *   - nothing here aborts and nothing writes to stdout; the CLI owns stdout.
 */
#include "vxa.h"
#include "shell_int.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#endif

/* =====================================================================
 * 1. word splitting and the shell decision
 * ===================================================================== */
static bool sh_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int sh_split(const char *cmd, char *buf, size_t bufsz, char *argv[], int maxargs) {
  if (!cmd || !buf || bufsz < 1) return 0;
  size_t i = 0, b = 0;
  int argc = 0;
  for (;;) {
    while (cmd[i] && sh_space(cmd[i])) i++;
    if (!cmd[i]) break;
    if (argc >= maxargs) return -2;
    size_t start = b;
    for (;;) {
      char ch = cmd[i];
      if (!ch || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') break;
      if (ch == '"' || ch == '\'') {
        char q = ch;
        i++;
        for (;;) {
          if (!cmd[i]) return -4;                     /* unbalanced quote: fail closed */
          if (cmd[i] == q) { i++; break; }
          if (q == '"' && cmd[i] == '\\' && (cmd[i + 1] == '"' || cmd[i + 1] == '\\')) i++;
          if (b + 1 >= bufsz) return -3;
          buf[b++] = cmd[i++];
        }
        continue;
      }
      if (b + 1 >= bufsz) return -3;
      buf[b++] = ch;
      i++;
    }
    buf[b++] = 0;
    argv[argc++] = buf + (long)start;
  }
  if (b < bufsz) buf[b] = 0;
  return argc;
}

bool sh_has_meta(const char *cmd) {
  if (!cmd) return true;
  for (const char *p = cmd; *p; p++) {
    switch (*p) {
      case ';': case '&': case '|': case '$': case '`':
      case '>': case '<': case '\n':
        return true;
      default: break;
    }
  }
  return false;
}

/* =====================================================================
 * 2. Windows argv quoting (MSVC/CRT rules), always wrapped in '"'
 *    N backslashes before a '"' or before the closing '"' become 2N (2N+1
 *    when they must escape that quote); embedded '"' becomes '\"'.
 *    Always wrapping also defuses a trailing space.
 * ===================================================================== */
static void put_bs(Buf *b, size_t n) { while (n--) buf_putc(b, '\\'); }

void sh_quote(Buf *b, const char *arg) {
  buf_putc(b, '"');
  if (arg) {
    size_t bs = 0;
    for (const char *p = arg; *p; p++) {
      if (*p == '\\') { bs++; continue; }
      if (*p == '"') { put_bs(b, bs * 2 + 1); buf_putc(b, '"'); bs = 0; continue; }
      put_bs(b, bs); bs = 0;
      buf_putc(b, *p);
    }
    put_bs(b, bs * 2);
  }
  buf_putc(b, '"');
}

/* cmd.exe is the one program whose "arguments" are really a command line: it
 * re-parses them itself, and its documented quote handling means an always-quoted
 * tail does NOT run -- given `"cmd" "/c" "echo %X%"` cmd keeps the leading quote
 * and goes looking for a program called `"echo`. A [cmd, /c|/k, one argument]
 * argv therefore hands that one argument over verbatim; see cmd_tail_slot below
 * for the exact (metacharacter-free only) condition and why it adds no shell
 * semantics that the argv path did not already refuse. */
static int cmd_tail_slot(char *const *argv, int argc);

Str sh_command_line(Arena *a, char *const *argv, int argc) {
  Buf b; buf_init(&b, a);
  int tail = cmd_tail_slot(argv, argc);
  for (int i = 0; i < argc; i++) {
    if (i) buf_putc(&b, ' ');
    if (i == tail) buf_puts(&b, argv[i]);        /* cmd re-parses this one itself */
    else sh_quote(&b, argv[i] ? argv[i] : "");
  }
  return buf_take(&b);
}

/* /d  : ignore any AutoRun registry command, /v:off : no delayed expansion.
 * The user string is appended verbatim -- shell semantics are the whole point. */
Str sh_shell_line(Arena *a, const char *cmd) {
  Buf b; buf_init(&b, a);
  buf_puts(&b, "\"cmd.exe\" \"/d\" \"/v:off\" \"/c\" ");
  if (cmd) buf_puts(&b, cmd);
  return buf_take(&b);
}

Str sh_bat_line(Arena *a, const char *bat) {
  Buf b; buf_init(&b, a);
  buf_puts(&b, "\"cmd.exe\" \"/d\" \"/v:off\" \"/c\" ");
  sh_quote(&b, bat);
  return buf_take(&b);
}

/* =====================================================================
 * 3. read-only allowlist: exact argv[0] (+ argv[1] where the tool groups
 *    subcommands) matches. Raw-string prefix matching is what lets
 *    `git status; rm -rf x` through, so we only ever look at argv.
 * ===================================================================== */
static const struct { const char *a0, *a1; } RO_TABLE[] = {
  { "ls", NULL }, { "cat", NULL }, { "head", NULL }, { "tail", NULL },
  { "wc", NULL }, { "grep", NULL }, { "rg", NULL }, { "find", NULL },
  { "file", NULL }, { "stat", NULL }, { "sort", NULL }, { "uniq", NULL },
  { "sed", "-n" },
  { "git", "status" }, { "git", "log" }, { "git", "diff" }, { "git", "show" },
  { "git", "blame" }, { "git", "ls-files" },
  { "go", "build" }, { "go", "list" }, { "go", "vet" }, { "go", "env" },
  { "cargo", "metadata" }, { "cargo", "check" }, { "cargo", "tree" },
  /* toolchain probes: an agent's first move is usually "what is installed", and
   * forcing a confirm round trip for --version would tax every session.
   * Explicit program+flag pairs only; no prefix rule, nothing that can write. */
  { "node", "--version" }, { "python", "--version" }, { "python3", "--version" },
  { "cc", "--version" }, { "gcc", "--version" }, { "clang", "--version" },
  { "git", "--version" }, { "go", "version" }, { "rustc", "--version" },
  { "cargo", "--version" }, { "cmake", "--version" }, { "java", "-version" },
  /* deliberately NOT listed: `echo`. It is not a probe, it is an unbounded row
   * (any arguments at all), it is absent from SPEC 4.3's default allowlist, and on
   * a stock Windows there is no echo.exe to prove read-only -- echo is a cmd.exe
   * builtin, so an argv run of it either fails to start or executes whatever
   * echo.exe happens to sit on PATH. It also buys no round trip: a *string*
   * `sh.run("echo hi > f")` is gated whatever this table says. */
};

static int ni_strn(const char *s, const char *low, size_t n) {
  for (size_t i = 0; i < n; i++) {
    int a = tolower((unsigned char)s[i]), b = tolower((unsigned char)low[i]);
    if (a != b) return a - b;
  }
  return 0;
}
static bool ni_eq(const char *a, const char *b) {
  if (!a || !b) return false;
  size_t la = strlen(a), lb = strlen(b);
  if (la != lb) return false;
  return la == 0 || !ni_strn(a, b, la);
}
static bool ni_has(const char *s, const char *low) {
  size_t ls = strlen(s), ln = strlen(low);
  if (!ln || ls < ln) return false;
  for (size_t i = 0; i + ln <= ls; i++) if (!ni_strn(s + i, low, ln)) return true;
  return false;
}

/* the tail rule of sh_command_line, in one place:
 * is_cmd_prog: program is cmd, however named or pathed (a `C:\Windows\System32\`
 * prefix changes nothing about how it parses /c);
 * cmd_tail_slot: the index of the ONE argument after a /c or /k switch, and only
 * when that argument contains no shell metacharacter and no quote. A tail carrying
 * `; & | $ ` > <` or a newline stays quoted, so it remains one literal word that
 * cmd cannot start -- relaxing the quoting here never turns argv into a shell. */
static bool is_cmd_prog(const char *prog) {
  if (!prog || !*prog) return false;
  const char *base = prog + strlen(prog);
  while (base > prog && base[-1] != '/' && base[-1] != '\\') base--;
  return ni_eq(base, "cmd") || ni_eq(base, "cmd.exe") || ni_eq(base, "cmd.com");
}

static int cmd_tail_slot(char *const *argv, int argc) {
  if (argc < 3 || !is_cmd_prog(argv[0])) return -1;
  for (int i = 1; i < argc; i++) {
    if (!argv[i]) return -1;
    if (!ni_eq(argv[i], "/c") && !ni_eq(argv[i], "/k")) continue;
    if (i + 1 != argc - 1) return -1;          /* more than one tail argument: quote all */
    const char *t = argv[i + 1];
    if (!t || !*t) return -1;
    if (sh_has_meta(t) || strchr(t, '"')) return -1;
    return i + 1;
  }
  return -1;
}

/* canonical program name: bare word, at most one .exe/.com suffix */
static bool prog_canon(const char *prog, char *out, size_t n) {
  if (!prog || !*prog) return false;
  size_t len = strlen(prog);
  if (len + 1 > n) return false;
  memcpy(out, prog, len + 1);
  if (len > 4) {
    char *dot = strrchr(out, '.');
    if (dot && (ni_eq(dot, ".exe") || ni_eq(dot, ".com"))) *dot = 0;
  }
  return *out != 0;
}

bool sh_prog_is_path(const char *prog) {
  if (!prog || !*prog) return true;
  for (const char *p = prog; *p; p++)
    if (*p == '/' || *p == '\\' || *p == ':' || *p == '.') return true;
  return false;
}

/* defense in depth: an allowlisted program that can still write or execute */
static bool argv_extra_guard(char *const *argv, int argc, const char *canon) {
  static const char *const git_danger[] = {
    "checkout", "switch", "reset", "clean", "push", "pull", "fetch", "rebase",
    "merge", "commit", "apply", "am", "stash", "clone", "init", "rm", "mv",
    "gc", "tag", "config", NULL
  };
  for (int i = 1; i < argc; i++) {
    const char *s = argv[i];
    if (!s) continue;
    if (!strcmp(canon, "sed") && (ni_has(s, "-i") || ni_has(s, "--in-place"))) return true;
    if (!strcmp(canon, "find") &&
        (!strcmp(s, "-delete") || !strcmp(s, "-exec") || !strcmp(s, "-execdir") ||
         !strcmp(s, "-ok") || !strcmp(s, "-okdir"))) return true;
    if (!strcmp(canon, "git") && i == 1)
      for (int k = 0; git_danger[k]; k++) if (ni_eq(s, git_danger[k])) return true;
  }
  return false;
}

bool sh_argv_readonly(char *const *argv, int argc) {
  if (argc < 1 || !argv || !argv[0]) return false;
  char canon[128];
  if (!prog_canon(argv[0], canon, sizeof canon)) return false;
  /* after the .exe/.com suffix is gone, any remaining separator or dot means
   * the caller named a path, not an allowlisted program */
  if (strpbrk(canon, "/\\:.")) return false;
  bool hit = false;
  size_t nt = sizeof(RO_TABLE) / sizeof(RO_TABLE[0]);
  for (size_t i = 0; i < nt; i++) {
    if (!ni_eq(canon, RO_TABLE[i].a0)) continue;
    if (RO_TABLE[i].a1) {
      if (argc >= 2 && ni_eq(argv[1], RO_TABLE[i].a1)) { hit = true; break; }
    } else {
      hit = true;
    }
  }
  if (!hit) return false;
  return !argv_extra_guard(argv, argc, canon);
}

bool sh_is_readonly(const char *cmd) {
  if (!cmd || !*cmd) return false;
  if (sh_has_meta(cmd)) return false;                   /* shell string: never ro */
  char buf[2048], *argv[SH_MAXARGS];
  if (strlen(cmd) + 1 > sizeof buf) return false;       /* too big to inspect: refuse */
  int argc = sh_split(cmd, buf, sizeof buf, argv, SH_MAXARGS);
  if (argc <= 0) return false;
  return sh_argv_readonly(argv, argc);
}

/* =====================================================================
 * 3b. the child's environment
 *     Built per run, never installed into this process.
 * ===================================================================== */

/* ASCII-only case folding on purpose: a locale-dependent tolower would order the
 * block differently on two machines, and then the env fingerprint in a plan would
 * not be reproducible across them. */
static int env_fold(int ch) {
  return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch;
}

/* Offset and length of an entry's NAME: past any leading '=' (a real Windows
 * block carries drive-map entries like `=C:=C:\dir`, and they must sort as the
 * name "C:", not ahead of every letter), then up to the next '='. */
static size_t env_name(const char *e, size_t *len) {
  const char *p = e;
  while (*p == '=') p++;
  *len = strcspn(p, "=");
  return (size_t)(p - e);
}

static bool env_same_name(const char *entry, const char *key) {
  size_t n = 0;
  const char *nm = entry + env_name(entry, &n);
  size_t k = strlen(key);
  if (k != n) return false;
  for (size_t i = 0; i < n; i++) if (env_fold(nm[i]) != env_fold(key[i])) return false;
  return true;
}

int sh_env_key_cmp(const char *x, const char *y) {
  if (x == y) return 0;
  if (!x) return -1;
  if (!y) return 1;
  size_t na = 0, nb = 0;
  const char *a = x + env_name(x, &na), *b = y + env_name(y, &nb);
  size_t n = na < nb ? na : nb;
  for (size_t i = 0; i < n; i++) {
    int d = env_fold(a[i]) - env_fold(b[i]);
    if (d) return d;
  }
  if (na != nb) return na < nb ? -1 : 1;
  /* names equal (possibly two spellings of one name): order by the whole entry so
   * the result never depends on the order the block was assembled in */
  for (const char *p = x, *q = y; ; p++, q++) {
    int ca = env_fold(*p), cb = env_fold(*q);
    if (ca != cb) return ca - cb;
    if (!*p) return 0;
  }
}

int sh_env_cmp_pp(const void *x, const void *y) {
  return sh_env_key_cmp(*(char *const *)x, *(char *const *)y);
}

char *sh_env_block(Arena *a, char *const *base, const ShEnv *ovr, size_t *nbytes) {
  if (nbytes) *nbytes = 0;
  if (!a) return NULL;
  int nb = 0;
  if (base) while (base[nb]) nb++;
  int no = (ovr && ovr->n > 0) ? ovr->n : 0;
  if (!nb && !no) return NULL;               /* nothing to say: let the child inherit */
  char **en = (char**)arena_zalloc(a, sizeof(char*) * (size_t)(nb + no + 1));
  int n = 0;
  for (int i = 0; i < nb; i++) en[n++] = arena_strdup(a, base[i]);
  /* An override replaces the parent entry with the same (case-insensitive) name and
   * keeps its OWN spelling -- Windows lookups ignore case anyway, and the caller's
   * spelling is the one an agent typed. Order is fixed up by the sort below. */
  for (int i = 0; i < no; i++) {
    const char *k = ovr->p[i].k, *v = ovr->p[i].v;
    if (!k || !*k) continue;                 /* an empty name is not a variable */
    int hit = -1;
    for (int j = 0; j < n; j++) if (env_same_name(en[j], k)) { hit = j; break; }
    Str e = s_fmt(a, "%s=%s", k, v ? v : "");
    if (hit >= 0) en[hit] = arena_strndup(a, e.p, (size_t)e.len);
    else en[n++] = arena_strndup(a, e.p, (size_t)e.len);
  }
  qsort(en, (size_t)n, sizeof(char*), sh_env_cmp_pp);
  size_t total = 1;                          /* the extra NUL that ends the block */
  for (int i = 0; i < n; i++) total += strlen(en[i]) + 1;
  char *blk = (char*)arena_zalloc(a, total + 1);
  size_t o = 0;
  for (int i = 0; i < n; i++) {
    size_t l = strlen(en[i]);
    memcpy(blk + o, en[i], l + 1);           /* each entry NUL-terminated */
    o += l + 1;
  }
  blk[o++] = 0;                              /* ... plus one more: block terminator */
  if (nbytes) *nbytes = o;
  return blk;
}

char *const *sh_env_parent(void) {
#ifdef _WIN32
  return _environ ? (char *const *)_environ : NULL;
#else
  extern char **environ;
  return environ ? (char *const *)environ : NULL;
#endif
}

/* =====================================================================
 * 4. output shaping
 *    A "line" is bytes up to and including its '\n'; a final unterminated
 *    line still counts as a line. Empty buffer = 0 lines.
 * ===================================================================== */
int sh_count_lines(const char *p, size_t n) {
  if (!p || n == 0) return 0;
  int c = 0;
  for (size_t i = 0; i < n; i++) if (p[i] == '\n') c++;
  if (p[n - 1] != '\n') c++;
  return c;
}

size_t sh_tail_offset(const char *p, size_t n, int keep) {
  if (!p || n == 0 || keep <= 0) return 0;
  int total = sh_count_lines(p, n);
  if (total <= keep) return 0;
  int skip = total - keep;
  size_t i = 0;
  for (int c = 0; c < skip && i < n; c++) {
    while (i < n && p[i] != '\n') i++;
    if (i < n) i++;
  }
  return i;
}

size_t sh_shape(char *buf, size_t len, int tail_lines, int *total_lines, bool *truncated) {
  int total = sh_count_lines(buf, len);
  if (total_lines) *total_lines = total;
  if (truncated) *truncated = false;
  if (tail_lines <= 0 || total <= tail_lines) return len;
  size_t off = sh_tail_offset(buf, len, tail_lines);
  if (off == 0) return len;
  memmove(buf, buf + off, len - off);
  buf[len - off] = 0;
  if (truncated) *truncated = true;
  return len - off;
}

void sh_cap_spans(size_t total, size_t cap, size_t *head_len, size_t *tail_off, size_t *tail_len) {
  if (cap < 128) cap = 128;                             /* room for the marker itself */
  if (total <= cap) {
    if (head_len) *head_len = total;
    if (tail_off) *tail_off = total;
    if (tail_len) *tail_len = 0;
    return;
  }
  size_t hl = cap / 2;
  size_t tl = cap - hl;
  if (head_len) *head_len = hl;
  if (tail_len) *tail_len = tl;
  if (tail_off) *tail_off = total - tl;
}

size_t sh_cap_marker(char *dst, size_t dstsz, size_t kept, size_t total) {
  if (!dst || dstsz == 0) return 0;
  int n = snprintf(dst, dstsz,
                   "\n...[vxa: %zu of %zu bytes kept (head+tail), %zu elided]...\n",
                   kept, total, total > kept ? total - kept : 0);
  if (n < 0) { dst[0] = 0; return 0; }
  return (size_t)n < dstsz ? (size_t)n : dstsz - 1;
}

char *sh_cap_apply(const char *p, size_t n, size_t cap, size_t *out_len) {
  size_t hl, tl, toff;
  sh_cap_spans(n, cap, &hl, &toff, &tl);
  char mark[192];
  size_t ml = tl == 0 ? 0 : sh_cap_marker(mark, sizeof mark, hl + tl, n);
  size_t len = hl + ml + tl;
  char *buf = (char*)malloc(len + 1);
  if (!buf) { if (out_len) *out_len = 0; return NULL; }
  if (p) {
    if (hl) memcpy(buf, p, hl);
    if (ml) memcpy(buf + hl, mark, ml);
    if (tl) memcpy(buf + hl + ml, p + toff, tl);
  }
  buf[len] = 0;
  if (out_len) *out_len = len;
  return buf;
}

/* =====================================================================
 * 5. redaction -- keys and values matched case-insensitively
 * ===================================================================== */
static const char *const SECRET_KEYS[] = {
  "token", "secret", "password", "passwd", "apikey", "api_key",
  "access_key", "authorization", "private_key", "cookie"
};
static const size_t N_KEYS = sizeof(SECRET_KEYS) / sizeof(SECRET_KEYS[0]);

/* blob charset (also used for run boundaries) */
static bool run_char(char c) {
  return isalnum((unsigned char)c) || c == '_' || c == '+' || c == '/' ||
         c == '.' || c == '-';
}

size_t sh_ident_run(const char *p, size_t n, size_t i) {
  if (!p || i >= n || !run_char(p[i])) return i;
  size_t j = i;
  while (j < n && run_char(p[j])) j++;
  return j;
}

/* one line of documentation for the heuristic: a word of >= 24 chars from
 * [A-Za-z0-9+/_.-] holding lowercase AND uppercase AND a digit is a secret,
 * unless the word looks like a path (contains ":/" or "//" or two '/'). */
bool sh_blob_secret(const char *p, size_t n) {
  if (!p || n < 24) return false;
  size_t cnt = 0;
  int slashes = 0;
  bool lo = false, up = false, dig = false;
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    if (c == ':') { if (i + 1 < n && p[i + 1] == '/') return false; continue; }
    if (c == '/') {
      slashes++;
      if (i + 1 < n && p[i + 1] == '/') return false;
      continue;
    }
    if (!run_char(c)) return false;
    cnt++;
    if (islower((unsigned char)c)) lo = true;
    else if (isupper((unsigned char)c)) up = true;
    else if (isdigit((unsigned char)c)) dig = true;
  }
  if (slashes >= 2) return false;
  return cnt >= 24 && lo && up && dig;
}

bool sh_key_run(const char *p, size_t n) {
  if (!p || n == 0 || n > 128) return false;
  /* lower-case, and treat '-' and '.' as the same separator as '_' so
   * api-key and Access.Key hit the api_key rule too */
  char low[129];
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    low[i] = (c == '-' || c == '.') ? '_' : (char)tolower((unsigned char)c);
  }
  low[n] = 0;
  for (size_t k = 0; k < N_KEYS; k++) {
    const char *key = SECRET_KEYS[k];
    size_t kl = strlen(key);
    if (kl == n && !memcmp(low, key, n)) return true;
    for (size_t i = 0; i + kl <= n; i++) {
      if (memcmp(low + i, key, kl)) continue;
      bool left_ok = i == 0 || low[i - 1] == '_' || low[i - 1] == '/';
      bool right_ok = i + kl == n || low[i + kl] == '_' || low[i + kl] == '/';
      if (left_ok && right_ok) return true;
    }
  }
  return false;
}

static size_t skip_hspace(const char *p, size_t n, size_t i) {
  while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
  return i;
}
static bool value_char(char c) {
  switch (c) {
    case ' ': case '\t': case '\n': case '\r': case '"': case '\'':
    case ',': case ';': case ')': case ']': case '}': case '<': case '>':
    case '|': case '&': case '`': case '\\':
      return false;
    default: return (unsigned char)c >= 32;
  }
}
/* keep at most a 4-char prefix, then *** */
static void emit_redacted(Buf *b, const char *v, size_t vlen) {
  if (vlen > 4) buf_put(b, v, 4);
  buf_puts(b, "***");
}

/* `key = value` / `key: value` / `key":"value"` / `Authorization: Bearer x`.
 * Returns the number of bytes consumed from i, 0 => leave the text alone. */
static size_t redact_kv(Buf *b, const char *p, size_t n, size_t i, size_t run) {
  static const char *const schemes[] = { "bearer", "basic", "digest", "token", NULL };
  size_t j = i + run;
  size_t k = skip_hspace(p, n, j);
  if (k < n && (p[k] == '"' || p[k] == '\'')) k = skip_hspace(p, n, k + 1);
  if (k >= n || (p[k] != '=' && p[k] != ':')) return 0;
  k = skip_hspace(p, n, k + 1);
  char q2 = 0;
  if (k < n && (p[k] == '"' || p[k] == '\'')) { q2 = p[k]; k++; }
  for (size_t s = 0; schemes[s]; s++) {          /* "Bearer <tok>", "Basic <b64>" */
    size_t sl = strlen(schemes[s]);
    if (k + sl > n || ni_strn(p + k, schemes[s], sl)) continue;
    char nx = k + sl == n ? ' ' : p[k + sl];
    if (!(nx == ' ' || nx == '\t' || nx == '"' || nx == '\'')) continue;
    k = skip_hspace(p, n, k + sl);
    if (k < n && (p[k] == '"' || p[k] == '\'')) { q2 = p[k]; k++; }
    break;
  }
  size_t vs = k, ve = k;
  if (q2) {
    while (ve < n && p[ve] != q2 && p[ve] != '\n') ve++;
  } else {
    while (ve < n && value_char(p[ve])) ve++;
  }
  if (ve == vs) return 0;                                /* empty value: nothing to hide */
  buf_put(b, p + i, vs - i);                             /* key, separators, Bearer -- as-is */
  emit_redacted(b, p + vs, ve - vs);
  if (q2 && ve < n && p[ve] == q2) { buf_putc(b, q2); ve++; }
  return ve - i;
}

Str sh_redact(Arena *a, Str s) {
  if (!a) return s_null();
  if (!s.p || s.len <= 0) return s_from(a, "", 0);
  const char *p = s.p;
  size_t n = (size_t)s.len;
  Buf b; buf_init(&b, a);
  size_t i = 0;
  while (i < n) {
    size_t end = sh_ident_run(p, n, i);
    if (end > i) {
      size_t rl = end - i;
      if (sh_key_run(p + i, rl)) {
        size_t used = redact_kv(&b, p, n, i, rl);
        if (used) { i += used; continue; }
      }
      /* the blob test looks at the whole word (colon allowed) so a path such
       * as C:/Program/Java/jdk-25 stays readable instead of being masked */
      size_t wend = end;
      while (wend < n && (p[wend] == ':' || run_char(p[wend]))) wend++;
      if (sh_blob_secret(p + i, wend - i)) { emit_redacted(&b, p + i, wend - i); i = wend; continue; }
      buf_put(&b, p + i, rl);
      i = end;
      continue;
    }
    buf_putc(&b, p[i++]);
  }
  return buf_take(&b);
}

/* =====================================================================
 * 6. ShRes plumbing
 * ===================================================================== */
static char *dupz(const char *s) {
  size_t n = s ? strlen(s) : 0;
  char *p = (char*)malloc(n + 1);
  if (!p) return NULL;
  if (n) memcpy(p, s, n);
  p[n] = 0;
  return p;
}

static void res_init(ShRes *r) {
  memset(r, 0, sizeof *r);
  r->code = SH_SPAWN_FAIL;
  r->out = dupz("");
  r->err = dupz("");
}

/* nothing ran (or ran badly): err says why, in one actionable line */
static void res_fail(ShRes *r, int code, const char *msg) {
  char *d = dupz(msg);
  free(r->err);
  r->err = d;
  r->err_len = d ? strlen(msg) : 0;
  r->code = code;
  r->timed_out = code == SH_TIMED_OUT;
  r->truncated = false;
  r->total_lines_set = false;
  r->total_lines = 0;
}

void sh_free(ShRes *r) {
  if (!r) return;
  free(r->out);
  free(r->err);
  memset(r, 0, sizeof *r);
}

/* redaction on the bytes that will reach the transcript; the buffer may grow
 * by a few "***" so it is reallocated (plain malloc, not the arena: these can
 * be megabytes and sh_free owns them). */
static void redact_malloc(char **buf, size_t *len) {
  if (!buf || !*buf || *len == 0) return;
  Arena t;
  arena_init(&t, SH_BYTE_CAP + 64u * 1024u);
  Str s;
  s.len = (int)*len;
  s.p = *buf;
  Str out = sh_redact(&t, s);
  char *nb = (char*)realloc(*buf, (size_t)out.len + 1);
  if (nb) {
    if (out.len) memcpy(nb, out.p, (size_t)out.len);
    nb[out.len] = 0;
    *buf = nb;
    *len = (size_t)out.len;
  } else {
    free(*buf);
    const char *msg = "[vxa: output dropped, redaction allocation failed]";
    *buf = dupz(msg);
    *len = *buf ? strlen(msg) : 0;
  }
  arena_free(&t);
}

static void finish(ShRes *r, int tail_lines, bool redact) {
  int t_out = 0, t_err = 0;
  bool tr1 = false, tr2 = false;
  r->out_len = sh_shape(r->out, r->out_len, tail_lines, &t_out, &tr1);
  r->err_len = sh_shape(r->err, r->err_len, tail_lines, &t_err, &tr2);
  r->total_lines = t_out + t_err;          /* combined pre-truncation line count */
  r->total_lines_set = true;
  r->truncated = tr1 || tr2;
  if (redact) {
    redact_malloc(&r->out, &r->out_len);
    redact_malloc(&r->err, &r->err_len);
  }
}

V sh_result_v(Ctx *c, ShRes *r, const char *cmd) {
  if (!c || !r) return VN;
  Arena *a = ctx_arena(c);
  if (!a) return VN;
  Rec *rec = rec_new(a);
  rec_setz(a, rec, "code", v_num((double)r->code));
  rec_setz(a, rec, "ms", v_num((double)r->ms));
  /* absence carries information: an empty stream gets no key at all */
  if (r->out && r->out_len) rec_setz(a, rec, "out", v_str(s_from(a, r->out, r->out_len)));
  if (r->err && r->err_len) rec_setz(a, rec, "err", v_str(s_from(a, r->err, r->err_len)));
  rec_setz(a, rec, "truncated", v_bool(r->truncated));
  if (r->total_lines_set) rec_setz(a, rec, "total_lines", v_num((double)r->total_lines));
  /* code=-1 alone cannot say why: an agent branches on timed_out= */
  if (r->timed_out) rec_setz(a, rec, "timed_out", v_bool(true));
  rec_setz(a, rec, "signalless", v_num(0));
  if (r->code < 0 && cmd) {
    /* stderr diagnostics only, and never the whole command line (it may carry
     * a secret): just the first token, so the agent knows what to fix */
    char buf[512], *argv[4];
    int argc = 0;
    size_t need = strlen(cmd) + 1;
    if (need <= sizeof buf) argc = sh_split(cmd, buf, sizeof buf, argv, 4);
    fprintf(stderr, "vxa sh: %s prog=%s\n",
            r->code == SH_TIMED_OUT ? "timeout" :
            r->code == SH_NO_CWD ? "cwd-not-found" : "not-started",
            argc > 0 ? argv[0] : "?");
  }
  return rec_to_v(rec);
}

/* =====================================================================
 * 7. workspace .vxa/tmp and directory checks
 * ===================================================================== */
static bool dir_exists(const char *p) {
  if (!p || !*p) return false;
#ifdef _WIN32
  DWORD att = GetFileAttributesA(p);
  return att != INVALID_FILE_ATTRIBUTES && (att & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static void mk_one_dir(const char *p) {
#ifdef _WIN32
  if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) CreateDirectoryA(p, NULL);
#else
  if (mkdir(p, 0775) != 0) { /* already there, or no permission: reported via capture failure */ }
#endif
}

static void mkdir_p(const char *path) {
  if (!path || !*path) return;
  char b[1024];
  size_t n = strlen(path);
  if (n >= sizeof b) return;
  memcpy(b, path, n + 1);
  for (size_t i = 1; i <= n; i++) {
    if (i == n || b[i] == '/' || b[i] == '\\') {
      char save = b[i];
      b[i] = 0;
      mk_one_dir(b);
      b[i] = save;
    }
  }
}

static void uniq_tag(char *dst, size_t n) {
#ifdef _WIN32
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  snprintf(dst, n, "%lu-%lld", (unsigned long)GetCurrentProcessId(), (long long)q.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  snprintf(dst, n, "%ld-%lld-%ld", (long)getpid(), (long long)ts.tv_sec, (long)ts.tv_nsec);
#endif
}

/* <root>/.vxa/tmp, created if missing. Falls back to ./.vxa/tmp and finally
 * the system temp dir so a read-only workspace still captures output instead
 * of running with inherited handles. */
static const char *sh_tmp_dir(Ctx *c, char *out, size_t n) {
  const char *root = c ? ctx_root(c) : NULL;
  if (root && *root) {
    int k = snprintf(out, n, "%s/.vxa/tmp", root);
    if (k > 0 && (size_t)k < n) {
      mkdir_p(out);
      if (dir_exists(out)) return out;
    }
  }
  mkdir_p(".vxa/tmp");
  if (dir_exists(".vxa/tmp")) { snprintf(out, n, ".vxa/tmp"); return out; }
  const char *t = getenv("TEMP");
  if (!t || !*t) t = getenv("TMP");
  if (t && *t && dir_exists(t)) { snprintf(out, n, "%s", t); return out; }
  snprintf(out, n, ".");
  return out;
}

/* paths[0]=stdout paths[1]=stderr paths[2]=stdin, bat = batch file if wanted */
static void tmp_paths(Ctx *c, char (*paths)[1024], char *bat) {
  char dir[512];
  const char *d = sh_tmp_dir(c, dir, sizeof dir);
  char tag[64];
  uniq_tag(tag, sizeof tag);
  snprintf(paths[0], 1024, "%.500s/vxa-sh-%.60s-out.tmp", d, tag);
  snprintf(paths[1], 1024, "%.500s/vxa-sh-%.60s-err.tmp", d, tag);
  snprintf(paths[2], 1024, "%.500s/vxa-sh-%.60s-in.tmp", d, tag);
  if (bat) snprintf(bat, 1024, "%.500s/vxa-sh-%.60s.cmd", d, tag);
}

/* =====================================================================
 * 8. Windows engine: CreateProcessA + job object + two capture files
 * ===================================================================== */
#ifdef _WIN32

static HANDLE open_tmp(const char *path, DWORD access, bool inheritable, DWORD disposition) {
  SECURITY_ATTRIBUTES sa, *psa = NULL;
  if (inheritable) {
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    psa = &sa;
  }
  return CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     psa, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
}

static bool write_whole(const char *path, const char *data, size_t n) {
  HANDLE h = open_tmp(path, GENERIC_WRITE, false, CREATE_ALWAYS);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  if (n) {
    DWORD w = 0;
    ok = WriteFile(h, data, (DWORD)n, &w, NULL) != 0 && w == (DWORD)n;
  }
  CloseHandle(h);
  return ok;
}

static size_t read_at(HANDLE h, long long off, size_t want, char *dst) {
  LARGE_INTEGER li;
  li.QuadPart = off;
  if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return 0;
  size_t got = 0;
  while (got < want) {
    DWORD rd = 0;
    size_t want_now = want - got > 65536 ? 65536 : want - got;
    if (!ReadFile(h, dst + got, (DWORD)want_now, &rd, NULL) || rd == 0) break;
    got += rd;
  }
  return got;
}

/* reads at most cap bytes: head + in-band marker + tail, never the whole file */
static void read_capped(const char *path, size_t cap, char **out, size_t *out_len,
                        size_t *total_out) {
  *out = dupz("");
  *out_len = 0;
  if (total_out) *total_out = 0;
  HANDLE h = open_tmp(path, GENERIC_READ, false, OPEN_EXISTING);
  if (h == INVALID_HANDLE_VALUE) return;
  LARGE_INTEGER sz;
  size_t total = (GetFileSizeEx(h, &sz) && sz.QuadPart > 0) ? (size_t)sz.QuadPart : 0;
  if (total_out) *total_out = total;
  char *buf = NULL;
  size_t len = 0;
  if (total <= cap) {
    buf = (char*)malloc(total + 1);
    if (buf) len = read_at(h, 0, total, buf);
  } else {
    size_t hl, tl, toff;
    sh_cap_spans(total, cap, &hl, &toff, &tl);
    char mark[192];
    size_t ml = sh_cap_marker(mark, sizeof mark, hl + tl, total);
    buf = (char*)malloc(hl + ml + tl + 1);
    if (buf) {
      size_t g1 = read_at(h, 0, hl, buf);
      memcpy(buf + g1, mark, ml);
      size_t g2 = read_at(h, (long long)toff, tl, buf + g1 + ml);
      len = g1 + ml + g2;
    }
  }
  CloseHandle(h);
  if (!buf) return;
  buf[len] = 0;
  free(*out);
  *out = buf;
  *out_len = len;
}

/* removes the capture files; a spawn that never started must not litter the
 * workspace, and a run that timed out with a hung grandchild must not either */
static void clear_tmp(char paths[3][1024], const char *bat) {
  const char *all[4];
  all[0] = paths[0]; all[1] = paths[1]; all[2] = paths[2]; all[3] = bat;
  for (int i = 0; i < 4; i++) {
    if (!all[i]) continue;
    if (DeleteFileA(all[i])) continue;
    DWORD e = GetLastError();
    if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
      fprintf(stderr, "vxa sh: capture file left behind (%s, win32 %lu)\n", all[i], (unsigned long)e);
  }
}

/* The one thing an ANSI environment block cannot carry is a byte sequence the
 * system codepage does not accept: CreateProcessA converts it with CP_ACP, and a
 * dangling multi-byte tail is not merely re-coded, it EATS the next byte -- which
 * in a NUL-separated block means two entries merge into one and the child gets an
 * environment nobody asked for. VXA's strings are UTF-8 bytes passed through
 * unchanged (SPEC 9: no conversion), so on a non-UTF-8 codepage this is a real
 * possibility. Refusing beats a silent corruption: the caller learns which name is
 * at fault and can re-run with an ASCII value or a UTF-8 system codepage.
 * Returns the offending name, or NULL when every pair converts. */
static const char *env_unrepresentable(const ShEnv *env) {
  if (!env) return NULL;
  for (int i = 0; i < env->n; i++) {
    const char *k = env->p[i].k, *v = env->p[i].v;
    /* cchWideChar 0 = measure only; 0 then means the byte sequence itself is bad */
    if (k && MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, k, -1, NULL, 0) == 0) return k;
    if (v && MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, v, -1, NULL, 0) == 0) return k;
  }
  return NULL;
}

static bool win_run(Arena *a, const char *line, const char *cwd, const char *stdin_data,
                    size_t stdin_len, const ShEnv *env, int timeout_ms, ShRes *r,
                    char paths[3][1024], const char *bat) {
  if (!a || !line || !*line) { res_fail(r, SH_SPAWN_FAIL, "no command line (arena unavailable)"); return false; }
  const char *bad = env_unrepresentable(env);
  if (bad) {
    char msg[256];
    snprintf(msg, sizeof msg,
             "env name '%s' has a value that is not a valid byte sequence in the system "
             "codepage (ACP %u); the child's block would be corrupt - use an ASCII value "
             "or a UTF-8 codepage", bad ? bad : "?", (unsigned)GetACP());
    clear_tmp(paths, bat);
    res_fail(r, SH_SPAWN_FAIL, msg);
    return false;
  }
  /* stdin is always a file we control: an inherited console is how a command
   * waits forever for input nobody is going to type */
  write_whole(paths[2], stdin_data ? stdin_data : "", stdin_data ? stdin_len : 0);
  /* The child's environment arrives as a block built for this one process.
   * sh_env_parent()'s array is not modified and _putenv/setenv is never called,
   * so nothing here is visible to the next command this script runs. */
  size_t blk_n = 0;
  char *blk = sh_env_block(a, sh_env_parent(), env, &blk_n);

  HANDLE ho = open_tmp(paths[0], GENERIC_WRITE, true, CREATE_ALWAYS);
  HANDLE he = open_tmp(paths[1], GENERIC_WRITE, true, CREATE_ALWAYS);
  HANDLE hi = open_tmp(paths[2], GENERIC_READ, true, OPEN_EXISTING);
  bool hi_ours = hi != INVALID_HANDLE_VALUE;
  if (!hi_ours) hi = GetStdHandle(STD_INPUT_HANDLE);
  if (ho == INVALID_HANDLE_VALUE || he == INVALID_HANDLE_VALUE) {
    if (ho != INVALID_HANDLE_VALUE) CloseHandle(ho);
    if (he != INVALID_HANDLE_VALUE) CloseHandle(he);
    if (hi_ours) CloseHandle(hi);
    clear_tmp(paths, bat);
    res_fail(r, SH_SPAWN_FAIL, "cannot create capture files under .vxa/tmp");
    return false;
  }

  STARTUPINFOA si;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = hi;
  si.hStdOutput = ho;
  si.hStdError = he;
  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof pi);

  char *mut = (char*)arena_alloc(a, strlen(line) + 1);
  strcpy(mut, line);

  /* a job with KILL_ON_JOB_CLOSE takes the whole tree (cmd.exe + its child)
   * when we close it; if job assignment is refused we still TerminateProcess
   * the direct child. */
  HANDLE job = CreateJobObjectA(NULL, NULL);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl;
    memset(&jl, 0, sizeof jl);
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof jl);
  }

  unsigned long long t0 = GetTickCount64();
  BOOL ok = CreateProcessA(NULL, mut, NULL, NULL, TRUE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW,
                           blk, (cwd && *cwd) ? cwd : NULL, &si, &pi);
  if (!ok) {
    DWORD gle = GetLastError();
    if (job) CloseHandle(job);
    CloseHandle(ho);
    CloseHandle(he);
    if (hi_ours) CloseHandle(hi);
    r->ms = (long)(GetTickCount64() - t0);
    char msg[128];
    snprintf(msg, sizeof msg, "cannot start program (win32 error %lu)", (unsigned long)gle);
    res_fail(r, SH_SPAWN_FAIL, msg);
    clear_tmp(paths, bat);
    return false;
  }
  if (job) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(ho);
  CloseHandle(he);
  if (hi_ours) CloseHandle(hi);

  bool running = true;
  int budget = timeout_ms > 0 ? timeout_ms : SH_DEFAULT_TIMEOUT_MS;
  while (running && budget > 0) {
    DWORD slice = (DWORD)(budget > 200 ? 200 : budget);
    DWORD w = WaitForSingleObject(pi.hProcess, slice);
    if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) { running = false; break; }
    budget -= (int)slice;                                /* hard cap: never more than timeout */
  }
  bool timed_out = running;
  if (timed_out) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, SH_KILL_GRACE_MS);
  }
  DWORD excode = 0;
  GetExitCodeProcess(pi.hProcess, &excode);
  r->ms = (long)(GetTickCount64() - t0);
  if (job) CloseHandle(job);                             /* kills any survivors of the tree */
  r->timed_out = timed_out;
  r->code = timed_out ? SH_TIMED_OUT : (excode == STILL_ACTIVE ? SH_TIMED_OUT : (int)excode);
  if (excode == STILL_ACTIVE && !timed_out) r->code = SH_TIMED_OUT;

  size_t t_out = 0, t_err = 0;
  read_capped(paths[0], SH_BYTE_CAP, &r->out, &r->out_len, &t_out);
  read_capped(paths[1], SH_BYTE_CAP, &r->err, &r->err_len, &t_err);
  (void)t_out; (void)t_err;            /* the byte cap states its own totals in-band */

  clear_tmp(paths, bat);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

bool sh_spawn_argv(Ctx *c, char *const *argv, int argc, const char *cwd,
                   const char *stdin_data, size_t stdin_len, const ShEnv *env,
                   int timeout_ms, ShRes *r) {
  Arena *a = c ? ctx_arena(c) : NULL;
  if (!a) { res_fail(r, SH_SPAWN_FAIL, "no arena: cannot build the argv command line"); return false; }
  if (argc < 1 || !argv || !argv[0]) { res_fail(r, SH_SPAWN_FAIL, "empty command"); return false; }
  Str line = sh_command_line(a, argv, argc);
  char paths[3][1024];
  tmp_paths(c, paths, NULL);
  return win_run(a, line.p, cwd, stdin_data, stdin_len, env, timeout_ms, r, paths, NULL);
}

bool sh_spawn_shell(Ctx *c, const char *cmd, const char *cwd, const char *stdin_data,
                    size_t stdin_len, const ShEnv *env, int timeout_ms, ShRes *r) {
  Arena *a = c ? ctx_arena(c) : NULL;
  if (!a) { res_fail(r, SH_SPAWN_FAIL, "no arena: cannot build the shell command line"); return false; }
  char paths[3][1024];
  char batname[1024];
  const char *bat = NULL;
  Str line;
  if (cmd && strchr(cmd, '\n')) {
    /* cmd.exe stops parsing a command line at a newline, so a multi-line
     * script is written to a generated batch file and run as a batch file.
     * Note: inside a batch file cmd uses the batch parser ('%' doubles). */
    tmp_paths(c, paths, batname);
    Buf b;
    buf_init(&b, a);
    for (const char *p = cmd; *p; p++) {
      if (*p == '\n') buf_puts(&b, "\r\n");
      else if (*p != '\r') buf_putc(&b, *p);
    }
    buf_puts(&b, "\r\n");
    Str text = buf_take(&b);
    if (!write_whole(batname, text.p, text.len)) {
      res_fail(r, SH_SPAWN_FAIL, "cannot write batch file under .vxa/tmp");
      return false;
    }
    bat = batname;
    line = sh_bat_line(a, batname);
  } else {
    tmp_paths(c, paths, NULL);
    line = sh_shell_line(a, cmd);
  }
  return win_run(a, line.p, cwd, stdin_data, stdin_len, env, timeout_ms, r, paths, bat);
}

#else /* ------------------------- POSIX engine ------------------------- */

static int write_all_fd(int fd, const char *p, size_t n) {
  while (n) {
    ssize_t w = write(fd, p, n);
    if (w <= 0) return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static void read_capped(const char *path, size_t cap, char **out, size_t *out_len,
                        size_t *total_out) {
  *out = dupz("");
  *out_len = 0;
  if (total_out) *total_out = 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return;
  struct stat st;
  size_t total = (fstat(fd, &st) == 0 && st.st_size > 0) ? (size_t)st.st_size : 0;
  if (total_out) *total_out = total;
  char *buf = NULL;
  size_t len = 0;
  if (total <= cap) {
    buf = (char*)malloc(total + 1);
    if (buf) {
      ssize_t g = read(fd, buf, total);
      len = g > 0 ? (size_t)g : 0;
    }
  } else {
    size_t hl, tl, toff;
    sh_cap_spans(total, cap, &hl, &toff, &tl);
    char mark[192];
    size_t ml = sh_cap_marker(mark, sizeof mark, hl + tl, total);
    buf = (char*)malloc(hl + ml + tl + 1);
    if (buf) {
      ssize_t g1 = pread(fd, buf, hl, 0);
      if (g1 < 0) g1 = 0;
      memcpy(buf + g1, mark, ml);
      ssize_t g2 = pread(fd, buf + g1 + ml, tl, (off_t)toff);
      if (g2 < 0) g2 = 0;
      len = (size_t)g1 + ml + (size_t)g2;
    }
  }
  close(fd);
  if (!buf) return;
  buf[len] = 0;
  free(*out);
  *out = buf;
  *out_len = len;
}

static long mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void posix_run(char *const *argv, int argc, const char *cmd, bool use_sh,
                      const char *cwd, const char *stdin_data, size_t stdin_len,
                      const ShEnv *env, int timeout_ms,
                      ShRes *r, char paths[3][1024]) {
  int wfd = open(paths[2], O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (wfd >= 0) {
    if (stdin_data && stdin_len) write_all_fd(wfd, stdin_data, stdin_len);
    close(wfd);
  }
  int ofd = open(paths[0], O_CREAT | O_TRUNC | O_WRONLY, 0600);
  int efd = open(paths[1], O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (ofd < 0 || efd < 0) {
    if (ofd >= 0) close(ofd);
    if (efd >= 0) close(efd);
    res_fail(r, SH_SPAWN_FAIL, "cannot create capture files under .vxa/tmp");
    return;
  }
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    close(ofd);
    close(efd);
    res_fail(r, SH_SPAWN_FAIL, "fork failed");
    return;
  }
  if (pid == 0) {
    int fd = open(paths[2], O_RDONLY);
    if (fd >= 0) { dup2(fd, 0); close(fd); }
    dup2(ofd, 1);
    dup2(efd, 2);
    if (ofd > 2) close(ofd);
    if (efd > 2) close(efd);
    setpgid(0, 0);
    if (cwd && *cwd && chdir(cwd) != 0) _exit(127);
    /* execvpe semantics without an execvpe: the child already carries the parent's
     * environment (it is a fork), so applying the overrides here is exactly what
     * an envp-taking exec would do -- and this process replaces itself a moment
     * later, so nothing set here can leak back into the script. */
    if (env) {
      for (int i = 0; i < env->n; i++) {
        const char *k = env->p[i].k;
        if (!k || !*k) continue;
        setenv(k, env->p[i].v ? env->p[i].v : "", 1);
      }
    }
    if (use_sh) execl("/bin/sh", "sh", "-c", cmd ? cmd : "", (char*)NULL);
    else {
      char *av[SH_MAXARGS + 1];
      int i = 0;
      for (; i < argc && i < SH_MAXARGS; i++) av[i] = argv[i];
      av[i] = NULL;
      execvp(av[0], av);
    }
    _exit(127);
  }
  close(ofd);
  close(efd);
  setpgid(pid, pid);
  int budget = timeout_ms > 0 ? timeout_ms : SH_DEFAULT_TIMEOUT_MS;
  long t0 = mono_ms();
  int status = 0;
  bool done = false, timed_out = false;
  for (;;) {
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) { done = true; break; }
    if (w < 0 && errno != EINTR) { done = true; break; }
    long used = mono_ms() - t0;
    if (budget - (int)used <= 0) break;
    struct timespec sl;
    sl.tv_sec = 0;
    sl.tv_nsec = 20 * 1000 * 1000;
    nanosleep(&sl, NULL);
  }
  if (!done) {
    timed_out = true;
    kill(-pid, SIGKILL);
    waitpid(pid, &status, 0);
  }
  r->ms = mono_ms() - t0;
  r->timed_out = timed_out;
  if (timed_out) r->code = SH_TIMED_OUT;
  else if (WIFEXITED(status)) r->code = WEXITSTATUS(status);
  else r->code = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  size_t t1 = 0, t2 = 0;
  read_capped(paths[0], SH_BYTE_CAP, &r->out, &r->out_len, &t1);
  read_capped(paths[1], SH_BYTE_CAP, &r->err, &r->err_len, &t2);
  unlink(paths[0]);
  unlink(paths[1]);
  unlink(paths[2]);
}

bool sh_spawn_argv(Ctx *c, char *const *argv, int argc, const char *cwd,
                   const char *stdin_data, size_t stdin_len, const ShEnv *env,
                   int timeout_ms, ShRes *r) {
  if (argc < 1 || !argv || !argv[0]) { res_fail(r, SH_SPAWN_FAIL, "empty command"); return false; }
  char paths[3][1024];
  tmp_paths(c, paths, NULL);
  posix_run(argv, argc, NULL, false, cwd, stdin_data, stdin_len, env, timeout_ms, r, paths);
  return true;
}

bool sh_spawn_shell(Ctx *c, const char *cmd, const char *cwd, const char *stdin_data,
                    size_t stdin_len, const ShEnv *env, int timeout_ms, ShRes *r) {
  char paths[3][1024];
  tmp_paths(c, paths, NULL);
  posix_run(NULL, 0, cmd, true, cwd, stdin_data, stdin_len, env, timeout_ms, r, paths);
  return true;
}

#endif

/* =====================================================================
 * 9. public entry
 * ===================================================================== */
void sh_exec_env(Ctx *c, const char *cmd, const char *cwd, const char *stdin_data,
                 size_t stdin_len, const ShEnv *env, int timeout_ms, int tail_lines,
                 bool redact, ShRes *r) {
  if (!r) return;
  res_init(r);
  if (stdin_len == 0 && stdin_data) stdin_len = strlen(stdin_data);
  if (timeout_ms <= 0) timeout_ms = SH_DEFAULT_TIMEOUT_MS;
  if (!cmd || !*cmd) { res_fail(r, SH_SPAWN_FAIL, "empty command"); return; }
  if (cwd && *cwd && !dir_exists(cwd)) { res_fail(r, SH_NO_CWD, "cwd not found"); return; }

  bool ran = false;
  if (sh_has_meta(cmd)) {
    ran = sh_spawn_shell(c, cmd, cwd, stdin_data, stdin_len, env, timeout_ms, r);
  } else {
    char stackbuf[2048], *sbuf = stackbuf, *heap = NULL;
    size_t need = strlen(cmd) + 1;
    if (need > sizeof stackbuf) {
      heap = (char*)malloc(need);
      if (!heap) { res_fail(r, SH_SPAWN_FAIL, "out of memory for the argv buffer"); return; }
      sbuf = heap;
    }
    char *argv[SH_MAXARGS];
    int argc = sh_split(cmd, sbuf, need, argv, SH_MAXARGS);
    if (argc < 0) {
      res_fail(r, SH_SPAWN_FAIL, argc == -4 ? "unbalanced quote in command"
                                            : argc == -2 ? "too many arguments for the argv path"
                                                           : "command too long for the argv buffer");
    } else if (argc == 0) {
      res_fail(r, SH_SPAWN_FAIL, "empty command");
    } else {
      ran = sh_spawn_argv(c, argv, argc, cwd, stdin_data, stdin_len, env, timeout_ms, r);
    }
    free(heap);
  }
  if (ran) finish(r, tail_lines, redact);
}

/* The pre-env/pre-length entry, unchanged in shape and meaning: a NUL-terminated
 * stdin string and an inherited environment. */
void sh_exec(Ctx *c, const char *cmd, const char *cwd, const char *stdin_data,
             int timeout_ms, int tail_lines, bool redact, ShRes *r) {
  sh_exec_env(c, cmd, cwd, stdin_data, 0, NULL, timeout_ms, tail_lines, redact, r);
}
