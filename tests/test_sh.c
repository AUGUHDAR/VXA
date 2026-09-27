/* test_sh.c - shell.c: the most dangerous surface, tested byte-exactly.
 *
 * interp.c is deliberately NOT in this test's link set, so the Ctx below is a
 * tiny local shim supplying a real arena and a real workspace root. That lets
 * sh_exec / sh_result_v be exercised for real (quoting, temp-file capture,
 * timeout, shaping, cleanup) while every security-relevant decision is also
 * tested as a standalone pure function.
 */
#include "vxa.h"
#include "shell_int.h"
#include "t.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
static bool dirok(const char *p) {
  DWORD a = GetFileAttributesA(p);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
static void mk_dir(const char *p) { CreateDirectoryA(p, NULL); }
static void abs_path(const char *p, char *out, size_t n) { GetFullPathNameA(p, (DWORD)n, out, NULL); }
#else
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
static bool dirok(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static void mk_dir(const char *p) { mkdir(p, 0775); }
static void abs_path(const char *p, char *out, size_t n) {
  char cwd[512];
  if (getcwd(cwd, sizeof cwd) && snprintf(out, n, "%s/%s", cwd, p) < 0) out[0] = 0;
}
#endif

static Arena A;

/* Ctx plumbing, dual-mode by design:
 *   - the link set for this module is "util val shell" (see build.sh), which
 *     has no interpreter, so the definitions below supply a real arena and a
 *     real workspace root: sh_exec / sh_result_v then run for real here;
 *   - if src/interp.c is ever added to deps(shell), build this test with
 *     -DVXA_NO_CTX_SHIM and interp.c's ctx_new/ctx_arena/ctx_root plus
 *     plan.c's plan_disp are used instead. shell.c only ever reaches the Ctx
 *     through those accessors, so nothing else has to change.
 * (Weak definitions were tried first: this gcc/ld pair emits them as
 *  ".weak.name" aliases on COFF and then reports the references undefined.) */
#ifndef VXA_NO_CTX_SHIM

void plan_disp(Plan *p, Buf *b, bool compact) {
  (void)p; (void)compact;
  buf_puts(b, "<plan>");
}

typedef struct TestCtx { Arena *a; char root[256]; } TestCtx;
static TestCtx g_shim;

Ctx *ctx_new(Arena *a, const char *root, const char *script_path) {
  (void)script_path;
  g_shim.a = a;
  snprintf(g_shim.root, sizeof g_shim.root, "%s", root && *root ? root : ".");
  return (Ctx*)&g_shim;
}
Arena *ctx_arena(Ctx *c) { return ((TestCtx*)c)->a; }
const char *ctx_root(Ctx *c) { return ((TestCtx*)c)->root; }
void ctx_free(Ctx *c) { (void)c; }

#endif /* !VXA_NO_CTX_SHIM */

static Ctx *CT;
static char ROOT[256];

/* ---------------- helpers ---------------- */
static char *dupz_local(const char *s) {
  size_t n = strlen(s);
  char *p = (char*)malloc(n + 1);
  memcpy(p, s, n + 1);
  return p;
}
static Str rd(const char *s) { return sh_redact(&A, s_wrap(s)); }
static bool sub(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }
static char *cl(char *const *av, int n) { return sh_command_line(&A, av, n).p; }
static int sp(const char *cmd, char **av, int max) {
  static char buf[8192];
  return sh_split(cmd, buf, sizeof buf, av, max);
}
static void ex(const char *cmd, const char *cwd, const char *in, int tmo, int tail,
               bool red, ShRes *r) {
  memset(r, 0, sizeof *r);
  sh_exec(CT, cmd, cwd, in, tmo, tail, red, r);
}
/* the env-carrying entry, so every pre-existing ex() case above keeps meaning
 * "no environment overrides, NUL-terminated stdin" */
static void exenv(const char *cmd, const char *cwd, const char *in, size_t in_len,
                  const ShEnv *env, int tmo, int tail, bool red, ShRes *r) {
  memset(r, 0, sizeof *r);
  sh_exec_env(CT, cmd, cwd, in, in_len, env, tmo, tail, red, r);
}
/* overrides for one run: pairs of (name, value), arena-owned so the ShEnv can be
 * returned by value */
static ShEnv envof(int n, ...) {
  va_list ap;
  ShEnv e;
  e.p = (ShEnvPair*)arena_alloc(&A, sizeof(ShEnvPair) * (size_t)(n > 0 ? n : 1));
  e.n = n;
  va_start(ap, n);
  for (int i = 0; i < n; i++) {
    e.p[i].k = va_arg(ap, const char*);
    e.p[i].v = va_arg(ap, const char*);
  }
  va_end(ap);
  return e;
}
/* a child on Windows writes CRLF; compare the LF form, exactly */
static const char *crz(const char *s) {
  if (!s) return NULL;
  char *o = arena_strndup(&A, s, strlen(s)), *w = o;
  for (char *r = o; *r; r++) if (*r != '\r') *w++ = *r;
  *w = 0;
  return o;
}
/* Set a variable in THIS process, as the parent of the child under test. Only the
 * test does this: the point of the merge cases is that sh_exec_env must never. */
static void proc_set(const char *k, const char *v) {
#ifdef _WIN32
  char b[512];
  snprintf(b, sizeof b, "%s=%s", k, v);
  _putenv(b);
#else
  setenv(k, v, 1);
#endif
}
/* how many entries the block holds */
static int blk_entries(const char *b, size_t n) {
  int c = 0;
  for (size_t i = 0; i < n && b[i]; ) { i += strlen(b + i) + 1; c++; }
  return c;
}
/* The byte sequence a child can echo back for an ANSI environment value: the OS
 * reads the block in the system codepage and the (UTF-8 native) child writes the
 * characters back as UTF-8. On POSIX there is no hop at all. */
static const char *ansi_hop(const char *s) {
#ifdef _WIN32
  int w = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
  if (w <= 0) return s;
  wchar_t *wb = (wchar_t*)arena_alloc(&A, sizeof(wchar_t) * (size_t)w);
  if (!MultiByteToWideChar(CP_ACP, 0, s, -1, wb, w)) return s;
  int n = WideCharToMultiByte(CP_UTF8, 0, wb, -1, NULL, 0, NULL, NULL);
  if (n <= 0) return s;
  char *o = (char*)arena_zalloc(&A, (size_t)n + 1);
  WideCharToMultiByte(CP_UTF8, 0, wb, -1, o, n, NULL, NULL);
  return o;
#else
  return s;
#endif
}
static long numof(V v, const char *k, bool *ok) {
  V *p = rec_getz(v.u.r, k);
  if (ok) *ok = p != NULL && p->t == V_NUM;
  return p && p->t == V_NUM ? (long)p->u.n : -999999;
}
static bool boolof(V v, const char *k, bool *ok) {
  V *p = rec_getz(v.u.r, k);
  if (ok) *ok = p != NULL && p->t == V_BOOL;
  return p && p->t == V_BOOL ? p->u.b : false;
}
static Str nameof(V v, const char *k) {
  V *p = rec_getz(v.u.r, k);
  return p && p->t == V_STR ? p->u.s : s_null();
}
static void qcase(const char *arg, const char *want, const char *who) {
  Buf b; buf_init(&b, &A);
  sh_quote(&b, arg);
  Str s = buf_take(&b);
  CHECK(s.len == (int)strlen(want) && !memcmp(s.p, want, (size_t)s.len),
        "%s: got \"%.*s\" want \"%s\"", who, s.len, s.p ? s.p : "", want);
}
static void rocase(const char *cmd, bool want) {
  bool got = sh_is_readonly(cmd);
  CHECK(got == want, "readonly(\"%s\")=%d want %d", cmd, (int)got, (int)want);
}
static void redcase(const char *in, const char *want) {
  Str s = rd(in);
  CHECK(s.len == (int)strlen(want) && !memcmp(s.p, want, (size_t)s.len),
        "redact(\"%s\")=\"%.*s\" want \"%s\"", in, s.len, s.p ? s.p : "", want);
}
static int count_char(const char *s, char c) {
  int n = 0;
  for (const char *p = s; *p; p++) if (*p == c) n++;
  return n;
}
/* quote characters that are not escaped by an odd run of backslashes: an
 * argument that stayed inside its own quotes contributes exactly two */
static int count_real_quotes(const char *s) {
  int n = 0;
  for (const char *p = s; *p; p++) {
    if (*p != '"') continue;
    int bs = 0;
    const char *q = p - 1;
    while (q >= s && *q == '\\') { bs++; q--; }
    if ((bs & 1) == 0) n++;
  }
  return n;
}
/* direct directory read: an in-process check cannot see the files a child
 * would have to list while its own capture files still exist */
static int count_dir(const char *p) {
#ifdef _WIN32
  char pat[800];
  snprintf(pat, sizeof pat, "%s/*", p);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return -1;
  int n = 0;
  do {
    if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) n++;
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return n;
#else
  DIR *d = opendir(p);
  if (!d) return -1;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
  closedir(d);
  return n;
#endif
}

/* =====================================================================
 * 1. argv quoting: exact command lines, including a genuine injection try
 * ===================================================================== */
static void t_quote(void) {
  T("quote: plain");
  qcase("echo", "\"echo\"", "simple");
  qcase("hello world", "\"hello world\"", "space");
  qcase("", "\"\"", "empty arg stays an argument");
  qcase("a\tb", "\"a\tb\"", "tab kept inside quotes");

  T("quote: backslashes (MSVC argv rules)");
  qcase("dir\\", "\"dir\\\\\"", "one trailing backslash doubles");
  qcase("ends\\\\", "\"ends\\\\\\\\\"", "two trailing become four");
  qcase("a\\b", "\"a\\b\"", "lone inner backslash passes through");
  qcase("a\\\"b", "\"a\\\\\\\"b\"", "backslashes before a quote: 2N+1");

  T("quote: metacharacters stay literal");
  qcase("a\"b", "\"a\\\"b\"", "embedded quote");
  qcase("x\"; calc.exe; \"y", "\"x\\\"; calc.exe; \\\"y\"", "INJECTION TRY");
  qcase("a&&b|c", "\"a&&b|c\"", "&& and |");
  qcase("$(whoami)", "\"$(whoami)\"", "command substitution");
  qcase("a`b`c", "\"a`b`c\"", "backticks");
  qcase("a>b<c;", "\"a>b<c;\"", "redirects and semicolon");
  qcase("line1\nline2", "\"line1\nline2\"", "newline injection stays quoted");
  qcase("\"leading", "\"\\\"leading\"", "leading quote");
  qcase("trailing\"", "\"trailing\\\"\"", "trailing quote");

  T("quote: joined command line");
  char *av1[] = { (char*)"git", (char*)"commit", (char*)"-m", (char*)"fix a & b" };
  CKS(cl(av1, 4), "\"git\" \"commit\" \"-m\" \"fix a & b\"");
  char *av2[] = { (char*)"calc.exe", (char*)"x\"; calc.exe; \"y" };
  char *line2 = cl(av2, 2);
  CKS(line2, "\"calc.exe\" \"x\\\"; calc.exe; \\\"y\"");
  /* structural proof that it is still ONE argument: exactly two quotes per
   * argv entry, and re-splitting the line round-trips to the original bytes */
  CKI(count_real_quotes(line2), 4);
  char *back[4];
  CKI(sp(line2, back, 4), 2);
  CKS(back[0], "calc.exe");
  CKS(back[1], "x\"; calc.exe; \"y");
  char *av3[] = { (char*)"echo", (char*)"" };
  CKS(cl(av3, 2), "\"echo\" \"\"");
  char *av4[] = { (char*)"seq", (char*)"1", (char*)"500" };
  CKS(cl(av4, 3), "\"seq\" \"1\" \"500\"");

  T("shell lines");
  CKS(sh_shell_line(&A, "make && rm -rf /").p, "\"cmd.exe\" \"/d\" \"/v:off\" \"/c\" make && rm -rf /");
  CKS(sh_shell_line(&A, "echo a").p, "\"cmd.exe\" \"/d\" \"/v:off\" \"/c\" echo a");
  CKS(sh_bat_line(&A, "C:\\ws\\.vxa\\tmp\\vxa-sh.cmd").p,
      "\"cmd.exe\" \"/d\" \"/v:off\" \"/c\" \"C:\\ws\\.vxa\\tmp\\vxa-sh.cmd\"");
  CK(count_char(sh_bat_line(&A, "a b&c;d").p, '"') == 10);

  T("cmd /c: the one argument cmd parses itself stays verbatim, a chainable one does not");
  char *cv1[] = { (char*)"cmd", (char*)"/c", (char*)"echo %VXATEST%" };
  CKS(cl(cv1, 3), "\"cmd\" \"/c\" echo %VXATEST%");
  char *cv2[] = { (char*)"cmd", (char*)"/c", (char*)"echo a & del b" };
  CKS(cl(cv2, 3), "\"cmd\" \"/c\" \"echo a & del b\"");     /* '&' keeps its quotes: cmd cannot
                                                           * start it, so argv never became a shell */
  char *cv3[] = { (char*)"cmd", (char*)"/c", (char*)"echo a;b" };
  CKS(cl(cv3, 3), "\"cmd\" \"/c\" \"echo a;b\"");
  char *cv4[] = { (char*)"cmd", (char*)"/c", (char*)"dir", (char*)"x" };
  CKS(cl(cv4, 4), "\"cmd\" \"/c\" \"dir\" \"x\"");         /* two tail args: nothing is special */
  char *cv5[] = { (char*)"cmd", (char*)"/c", (char*)"" };
  CKS(cl(cv5, 3), "\"cmd\" \"/c\" \"\"");
  char *cv6[] = { (char*)"C:\\Windows\\System32\\cmd.exe", (char*)"/C", (char*)"set PATH" };
  CKS(cl(cv6, 3), "\"C:\\Windows\\System32\\cmd.exe\" \"/C\" set PATH");
  char *cv7[] = { (char*)"notcmd", (char*)"/c", (char*)"echo hi" };
  CKS(cl(cv7, 3), "\"notcmd\" \"/c\" \"echo hi\"");        /* the rule is cmd's, not any /c */
  char *cv8[] = { (char*)"cmd", (char*)"/c", (char*)"echo \"x\"" };
  CKS(cl(cv8, 3), "\"cmd\" \"/c\" \"echo \\\"x\\\"\"");    /* a quoted tail stays quoted */
}

/* =====================================================================
 * 2. word splitting and the shell decision
 * ===================================================================== */
static void t_split(void) {
  T("split: plain argv");
  char *av[16];
  CKI(sp("cat two  words", av, 16), 3);
  CKS(av[0], "cat"); CKS(av[1], "two"); CKS(av[2], "words");
  CKI(sp("  ls   -la  ", av, 16), 2);
  CKS(av[0], "ls"); CKS(av[1], "-la");
  CKI(sp("cmd.exe /c echo hello-argv", av, 16), 4);
  CKS(av[3], "hello-argv");

  T("split: quoting");
  CKI(sp("git commit -m \"fix spaces\"", av, 16), 4);
  CKS(av[3], "fix spaces");
  CKI(sp("cat \"\" x", av, 16), 3);
  CKS(av[1], "");
  CKI(sp("echo 'raw text'", av, 16), 2);
  CKS(av[1], "raw text");
  CKI(sp("echo a\"b c\"", av, 16), 2);
  CKS(av[1], "ab c");

  T("split: failure modes (all fail closed)");
  CKI(sp("echo \"unbalanced", av, 16), -4);
  CKI(sp("", av, 16), 0);
  CKI(sp("   ", av, 16), 0);
  char many[512];
  many[0] = 0;
  for (int i = 0; i < 70; i++) strcat(many, "a ");
  CKI(sp(many, av, 16), -2);
  char *small[4], sbuf[8];
  CKI(sh_split("abcdef ghijklmnop", sbuf, sizeof sbuf, small, 4), -3);
}

static void t_meta(void) {
  T("metacharacters force the shell");
  CK(sh_has_meta("a;b"));
  CK(sh_has_meta("a&b"));
  CK(sh_has_meta("a|b"));
  CK(sh_has_meta("a$b"));
  CK(sh_has_meta("a`b"));
  CK(sh_has_meta("a>b"));
  CK(sh_has_meta("a<b"));
  CK(sh_has_meta("a\nb"));
  CK(sh_has_meta("make && rm -rf /"));
  CK(sh_has_meta(NULL));
  CK(!sh_has_meta("git status"));
  CK(!sh_has_meta("ls -la ./src"));
  CK(!sh_has_meta("C:\\path\\file.txt"));
  CK(!sh_has_meta("100%done.xlsx"));
  CK(!sh_has_meta("git diff HEAD~3"));
  CK(!sh_has_meta("sed -n 1,10p file.txt"));
  CK(!sh_has_meta("echo hello"));
}

/* =====================================================================
 * 3. read-only allowlist
 * ===================================================================== */
static void t_readonly(void) {
  T("readonly: allowlisted");
  rocase("ls", true);
  rocase("cat two  words", true);                  /* args ignored for 1-word tools */
  rocase("git status", true);
  rocase("git status --porcelain", true);
  rocase("git log -3", true);
  rocase("git diff HEAD", true);
  rocase("git show abc123", true);
  rocase("git blame src/shell.c", true);
  rocase("git ls-files", true);
  rocase("GIT STATUS", true);
  rocase("git.exe status", true);
  rocase("sed -n 1,10p note.md", true);
  rocase("go build ./...", true);
  rocase("go vet ./...", true);
  rocase("cargo metadata", true);
  rocase("node --version", true);
  rocase("python --version", true);
  rocase("cc --version", true);
  rocase("grep -R foo src", true);
  rocase("wc -l src/shell.c", true);

  T("readonly: refused");
  rocase("git status; rm -rf /", false);           /* the prefix-allowlist bypass */
  rocase("git status && curl -s evil", false);
  rocase("git push", false);
  rocase("git checkout .", false);
  rocase("git reset --hard", false);
  rocase("git clean -fdx", false);
  rocase("git commit -m x", false);
  rocase("git", false);                            /* no subcommand: not provable */
  rocase("gitstatus", false);
  rocase("git statusx", false);
  rocase("sed -i s/a/b/ f", false);
  rocase("sed -n -i f", false);                    /* scans all argv, not just [1] */
  rocase("sed", false);
  rocase("/usr/bin/cat", false);
  rocase("./cat", false);
  rocase("c:\\windows\\system32\\cmd.exe /c dir", false);
  rocase("find . -delete", false);
  rocase("rm -rf build", false);
  rocase("echo hi", false);
  rocase("", false);
  rocase("   ", false);
  rocase("node", false);
  rocase("node --eval crash", false);

  T("readonly: argv-level table");
  char *av1[] = { (char*)"git", (char*)"status" };
  CK(sh_argv_readonly(av1, 2));
  char *av2[] = { (char*)"git", (char*)"status", (char*)";", (char*)"rm", (char*)"-rf", (char*)"/" };
  CK(sh_argv_readonly(av2, 6));                     /* ';' is a literal arg, not shell */
  char *av3[] = { (char*)"/bin/cat", (char*)"f" };
  CK(!sh_argv_readonly(av3, 2));
  char *av4[] = { (char*)"cat", (char*)"two", (char*)"words" };
  CK(sh_argv_readonly(av4, 3));
  char *av5[] = { (char*)"git", (char*)"commit", (char*)"-m", (char*)"x" };
  CK(!sh_argv_readonly(av5, 4));
  CK(sh_prog_is_path("/usr/bin/cat"));
  CK(sh_prog_is_path("./cat"));
  CK(!sh_prog_is_path("cat"));
}

/* =====================================================================
 * 4. redaction
 * ===================================================================== */
static void t_redact(void) {
  T("redact: key = value");
  redcase("TOKEN=abc123def456", "TOKEN=abc1***");
  redcase("token = short", "token = shor***");
  redcase("PASSWORD=hunter2", "PASSWORD=hunt***");
  redcase("passwd=x", "passwd=***");
  redcase("api_key: sk-live-1234567890", "api_key: sk-l***");
  redcase("access_key=AKIAIOSFODNN7EXAMPLE", "access_key=AKIA***");
  redcase("my_secret=Zm9vYmFyYmF6cXV1", "my_secret=Zm9v***");
  redcase("{\"access_token\":\"zzzzzzzzzzzz\"}", "{\"access_token\":\"zzzz***\"}");
  redcase("Set-Cookie: sid=abcdef123456; Path=/", "Set-Cookie: sid=***; Path=/");
  redcase("cookie=ABCDEFGHIJKLMNOP", "cookie=ABCD***");

  T("redact: Authorization / Bearer");
  redcase("Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5", "Authorization: Bearer eyJh***");
  redcase("authorization=bearer abcdef1234567890", "authorization=bearer abcd***");
  redcase("Authorization: Basic QWxhZGRpbjpPc2Vlc29uZGU", "Authorization: Basic QWxh***");

  T("redact: high-entropy blob");
  redcase("blob aGVsbG9XkQ1zZm9vYmFyYmF6cXV1eA end", "blob aGVs*** end");
  CK(sh_blob_secret("aGVsbG9XkQ1zZm9vYmFyYmF6cXV1eA", 30));
  CK(!sh_blob_secret("Abcdefghijklmnopqrstuvwxyz", 26));        /* no digit */
  CK(!sh_blob_secret("abcdefghijklmnopqrstuvwxy1", 26));        /* no uppercase */
  CK(!sh_blob_secret("aB1cdefghijklmnopqrs", 19));              /* too short */
  CK(!sh_blob_secret("C:/Program/Java/jdk-25/bin/java", 29));   /* path: contains ":/" */
  CK(!sh_blob_secret("usr/lib/x86_64-mingw/catalog.h", 28));    /* path: two slashes */

  T("redact: false-positive control");
  redcase("compiled 42 files in 3.1s", "compiled 42 files in 3.1s");
  redcase("secretary=office", "secretary=office");              /* not a key segment */
  redcase("tokenizer=hello world", "tokenizer=hello world");
  redcase("username=admin", "username=admin");
  redcase("key: value", "key: value");                          /* "key" is not a key name */
  redcase("built ok, 7 warnings", "built ok, 7 warnings");
  redcase("x86_64-w64-mingw32-cpp.exe -o out", "x86_64-w64-mingw32-cpp.exe -o out");

  T("redact: purity and edges");
  char src[] = "TOKEN=abc123def456";
  Str out = sh_redact(&A, s_wrap(src));
  CKS(src, "TOKEN=abc123def456");                              /* input untouched */
  CK(out.p != src);
  CK(out.len == 13 && !memcmp(out.p, "TOKEN=abc1***", 13));
  CK(s_eqz(sh_redact(&A, s_null()), ""));
  CK(s_eqz(rd("no secrets at all"), "no secrets at all"));
  CK(sh_key_run("token", 5));
  CK(sh_key_run("ACCESS_TOKEN", 12));
  CK(sh_key_run("api-key", 7));
  CK(sh_key_run("private_key", 11));
  CK(!sh_key_run("tokenizer", 9));
  CK(!sh_key_run("keyboard", 8));
  CK(!sh_key_run("username", 8));
}

/* =====================================================================
 * 5. line and byte shaping
 * ===================================================================== */
static char lines[4096];
static void fill_lines(int n) {
  size_t off = 0;
  lines[0] = 0;
  for (int i = 1; i <= n; i++) {
    int k = snprintf(lines + off, sizeof lines - off, "L%03d\n", i);
    if (k <= 0) break;
    off += (size_t)k;
  }
}

static void t_shape(void) {
  T("count_lines");
  CKI(sh_count_lines("", 0), 0);
  CKI(sh_count_lines("a\nb\nc\n", 6), 3);
  CKI(sh_count_lines("a\nb", 3), 2);
  CKI(sh_count_lines("\n", 1), 1);
  CKI(sh_count_lines("a\n\nb", 4), 3);
  CKI(sh_count_lines(NULL, 10), 0);

  T("tail_offset");
  fill_lines(500);
  size_t len = strlen(lines);
  CKI(len, 2500);
  CKI(sh_tail_offset(lines, len, 500), 0);
  CKI(sh_tail_offset(lines, len, 501), 0);
  CKI(sh_tail_offset(lines, len, 0), 0);
  CKI(sh_tail_offset(lines, len, 10), 490 * 5);
  CKI(sh_tail_offset(lines, len, 1), 499 * 5);
  CK(!memcmp(lines + sh_tail_offset(lines, len, 10), "L491\n", 5));

  T("shape: tail keeps the last N lines and reports the truth");
  char tmp[4096];
  int total = -1;
  bool trunc = false;
  memcpy(tmp, lines, len + 1);
  size_t nl = sh_shape(tmp, len, 10, &total, &trunc);
  CKI(total, 500);
  CK(trunc);
  CKI(nl, 50);
  CKS(tmp, "L491\nL492\nL493\nL494\nL495\nL496\nL497\nL498\nL499\nL500\n");
  memcpy(tmp, lines, len + 1);
  trunc = true;
  nl = sh_shape(tmp, len, 0, &total, &trunc);
  CKI(nl, 2500);
  CKI(total, 500);
  CK(!trunc);
  memcpy(tmp, lines, len + 1);
  nl = sh_shape(tmp, len, 500, &total, &trunc);
  CKI(nl, 2500);
  CK(!trunc);
  CKI(sh_shape(NULL, 0, 10, &total, &trunc), 0);
  CKI(total, 0);

  T("cap: spans and marker");
  size_t hl, tl, toff;
  sh_cap_spans(1000, 128, &hl, &toff, &tl);
  CKI(hl, 64); CKI(tl, 64); CKI(toff, 936); CKI(hl + tl, 128);
  sh_cap_spans(100, 128, &hl, &toff, &tl);
  CKI(hl, 100); CKI(tl, 0); CKI(toff, 100);
  sh_cap_spans(2u * 1024u * 1024u, SH_BYTE_CAP, &hl, &toff, &tl);
  CKI(hl + tl, SH_BYTE_CAP);
  CKI(toff + tl, 2u * 1024u * 1024u);
  char mark[192];
  size_t ml = sh_cap_marker(mark, sizeof mark, 128, 1000);
  CKS(mark, "\n...[vxa: 128 of 1000 bytes kept (head+tail), 872 elided]...\n");
  CKI(ml, strlen(mark));

  T("cap: head+tail assembly");
  char src2[1001];
  for (int i = 0; i < 1000; i++) src2[i] = (char)('a' + i % 26);
  src2[1000] = 0;
  size_t ol = 0;
  char *small = sh_cap_apply("short text", 10, 128, &ol);
  CK(small != NULL);
  CKI(ol, 10);
  CKS(small, "short text");
  CK(!sub(small, "elided"));
  free(small);
  char *capped = sh_cap_apply(src2, 1000, 128, &ol);
  CK(capped != NULL);
  CKI(ol, 64 + (long long)ml + 64);
  CK(!memcmp(capped, src2, 64));                     /* head preserved */
  CK(!memcmp(capped + 64 + ml, src2 + 936, 64));     /* tail preserved */
  CK(sub(capped, "872 elided"));
  CKI(count_char(capped, '\n'), 2);                  /* the marker states itself */
  free(capped);
}

/* =====================================================================
 * 6. real execution
 * ===================================================================== */
static void t_exec(void) {
  T("exec: argv path (no shell) captures stdout");
  ShRes r;
  ex("echo hello-argv", NULL, NULL, 20000, 0, false, &r);
  CKI(r.code, 0);
  CK(!r.timed_out);
  CK(sub(r.out, "hello-argv"));
  CKI(r.err_len, 0);
  CK(!r.truncated);
  CK(r.total_lines_set);
  CKI(r.total_lines, 1);
  CK(r.ms >= 0 && r.ms < 20000);
  long ms_echo = r.ms;
  sh_free(&r);
  CK(r.out == NULL);
  CK(r.err == NULL);
  CK(r.out_len == 0);
  CK(ms_echo < 5000);                  /* a trivial echo must not look slow */

  T("exec: stdout and stderr are captured separately");
  ex("echo VXA-OUT-MARK & echo VXA-ERR-MARK 1>&2", NULL, NULL, 20000, 0, false, &r);
  CKI(r.code, 0);
  CK(sub(r.out, "VXA-OUT-MARK"));
  CK(!sub(r.out, "VXA-ERR-MARK"));
  CK(sub(r.err, "VXA-ERR-MARK"));
  CK(!sub(r.err, "VXA-OUT-MARK"));
  sh_free(&r);
  ex("cat no-such-file-vxa-zz.txt", NULL, NULL, 20000, 0, false, &r);
  CKI(r.code, 1);
  CKI(r.out_len, 0);
  CK(sub(r.err, "No such file"));
  sh_free(&r);

  T("exec: exit code is the program's, not a shell's");
  ex("exit 9 & echo never-ran", NULL, NULL, 20000, 0, false, &r);
  CKI(r.code, 9);
  CK(!sub(r.out, "never-ran"));
  sh_free(&r);

  T("exec: a hostile argument stays one literal argv entry");
  char *av[] = { (char*)"echo", (char*)"x\"; calc.exe; \"y" };
  memset(&r, 0, sizeof r);
  bool started = sh_spawn_argv(CT, av, 2, NULL, NULL, 0, NULL, 20000, &r);
  CK(started);
  CKI(r.code, 0);
  CKS(r.out, "x\"; calc.exe; \"y\n");
  CK(!sub(r.out, "Windows"));
  CKI(r.err_len, 0);
  sh_free(&r);

  T("exec: timeout terminates, code is the -1 sentinel");
  ex("ping -n 30 127.0.0.1", NULL, NULL, 200, 0, false, &r);
  CK(r.timed_out);
  CKI(r.code, SH_TIMED_OUT);
  CK(r.ms >= 150 && r.ms < 6000);
  CK(r.total_lines_set);             /* the run still reports what it captured */
  long ms_to = r.ms;
  sh_free(&r);
  CK(ms_to >= 150);

  T("exec: output written before a timeout is still returned");
  char *av2[] = { (char*)"sh", (char*)"-c", (char*)"seq 1 3; sleep 30" };
  memset(&r, 0, sizeof r);
  started = sh_spawn_argv(CT, av2, 3, NULL, NULL, 0, NULL, 700, &r);
  CK(started);
  CK(r.timed_out);
  CKI(r.code, SH_TIMED_OUT);
  CK(sub(r.out, "3\n"));              /* seq had already exited and flushed */
  sh_free(&r);

  T("exec: tail truncation is never silent");
  ex("seq 1 500", NULL, NULL, 20000, 10, false, &r);
  CKI(r.code, 0);
  CK(r.truncated);
  CKI(r.total_lines, 500);
  CKI(sh_count_lines(r.out, r.out_len), 10);
  CK(sub(r.out, "491"));
  CK(sub(r.out, "500"));
  CK(!sub(r.out, "\n2\n"));
  {
    V v = sh_result_v(CT, &r, "seq 1 500");
    bool ok = false;
    CK(v.t == V_REC);
    CK(boolof(v, "truncated", &ok) && ok);
    CKI(numof(v, "total_lines", &ok), 500); CK(ok);
    CKI(numof(v, "code", &ok), 0);
    CKI(numof(v, "signalless", &ok), 0);
    CK(rec_getz(v.u.r, "err") == NULL);   /* empty stream => key omitted */
    CK(rec_getz(v.u.r, "out") != NULL);
  }
  sh_free(&r);
  ex("seq 1 500", NULL, NULL, 20000, 0, false, &r);
  CK(!r.truncated);
  CKI(r.total_lines, 500);
  sh_free(&r);

  T("exec: the byte cap holds 1 MiB per stream and says so");
  ex("seq 1 200000", NULL, NULL, 60000, 0, false, &r);
  CKI(r.code, 0);
  CK(r.out_len > 1000000);
  CK(r.out_len < SH_BYTE_CAP + 2048);
  CK(sub(r.out, "bytes kept (head+tail)"));
  CK(sub(r.out, "1048576 of"));
  sh_free(&r);

  T("exec: stdin is a file we control, never an inherited console");
  ex("cat", NULL, "hello-stdin-line\n", 20000, 0, false, &r);
  CKI(r.code, 0);
  CK(sub(r.out, "hello-stdin-line"));
  sh_free(&r);

  T("exec: cwd is honored");
  ex("ls -A", "build/testws", NULL, 20000, 0, false, &r);
  CKI(r.code, 0);
  CK(sub(r.out, ".vxa"));
  sh_free(&r);

  T("exec: bad cwd is an error, not an abort");
  ex("echo should-not-run", "build/testws/definitely-not-a-dir", NULL, 5000, 0, false, &r);
  CKI(r.code, SH_NO_CWD);
  CKS(r.err, "cwd not found");
  CKI(r.out_len, 0);
  CK(!r.timed_out);
  CK(!r.total_lines_set);
  CK(r.ms >= 0 && r.ms < 2000);
  sh_free(&r);

  T("exec: garbage in, error out (no crash, no silent shell fallback)");
  ex("echo \"unbalanced", NULL, NULL, 5000, 0, false, &r);
  CKI(r.code, SH_SPAWN_FAIL);
  CK(sub(r.err, "unbalanced quote"));
  sh_free(&r);
  ex("", NULL, NULL, 5000, 0, false, &r);
  CKI(r.code, SH_SPAWN_FAIL);
  CK(r.err_len > 0);
  sh_free(&r);
  ex(NULL, NULL, NULL, 5000, 0, false, &r);
  CKI(r.code, SH_SPAWN_FAIL);
  sh_free(&r);
  ex("nonexistent-program-vxa-zz arg", NULL, NULL, 5000, 0, false, &r);
  CKI(r.code, SH_SPAWN_FAIL);
  CK(sub(r.err, "cannot start"));
  sh_free(&r);

  T("exec: capture files live under .vxa/tmp and are cleaned up");
  char tmpdir[700];
  snprintf(tmpdir, sizeof tmpdir, "%s/.vxa/tmp", ROOT);
  CK(dirok(tmpdir));                     /* created even though it was missing */
  CKI(count_dir(tmpdir), 0);             /* and every capture file deleted */
  ex("echo cleanup-probe", NULL, NULL, 20000, 0, false, &r);
  CKI(r.code, 0);
  CKI(count_dir(tmpdir), 0);
  sh_free(&r);

  T("exec: redact=true keeps secrets out of the transcript");
  ex("echo TOKEN=abc123def456", NULL, NULL, 20000, 0, true, &r);
  CKI(r.code, 0);
  CK(sub(r.out, "TOKEN=abc1***"));
  CK(!sub(r.out, "abc123def456"));
  sh_free(&r);
  ex("echo TOKEN=abc123def456", NULL, NULL, 20000, 0, false, &r);
  CK(sub(r.out, "TOKEN=abc123def456"));   /* off means off */
  sh_free(&r);
  ex("echo SECRET=s3cr3tvalue99 1>&2", NULL, NULL, 20000, 0, true, &r);
  CK(sub(r.err, "SECRET=s3cr***"));
  CK(!sub(r.err, "s3cr3tvalue99"));
  sh_free(&r);
}

/* =====================================================================
 * 6b. the child's environment block (pure: nothing is spawned here)
 *     CreateProcessA wants KEY=VALUE entries, each NUL-terminated, sorted
 *     case-insensitively by name with a leading '=' ignored, and the whole block
 *     closed by an EXTRA NUL. Every case below compares the bytes.
 * ===================================================================== */
static void blkcase(char *const *base, const ShEnv *ovr, const char *want, size_t wantn,
                    const char *who) {
  size_t n = 0;
  char *b = sh_env_block(&A, base, ovr, &n);
  if (!want) {
    CHECK(b == NULL && n == 0, "%s: expected no block (child inherits), got %p/%zu",
          who, (void*)b, n);
    return;
  }
  CK(b != NULL);
  if (!b) return;
  CKI(n, (long long)wantn);
  CHECK(n == wantn && !memcmp(b, want, wantn), "%s: block bytes wrong, got [%s|%zu]", who, b, n);
  /* the extra NUL, stated separately so a short block cannot pass by accident */
  CK(n >= 2 && b[n - 1] == 0 && b[n - 2] == 0);
}

static void t_envblock(void) {
  T("env block: sorted by name, NUL per entry, extra NUL at the end");
  char *b1[] = { (char*)"B=1", (char*)"A=2", (char*)"C=3", NULL };
  static const char w1[] = "A=2\0" "B=1\0" "C=3\0" "\0";
  blkcase(b1, NULL, w1, sizeof w1 - 1, "three parents");
  /* an override-only run: the block is still terminated twice */
  ShEnv e1 = envof(1, "VXAK", "v");
  static const char w2[] = "VXAK=v\0" "\0";
  blkcase(NULL, &e1, w2, sizeof w2 - 1, "no parent at all");
  CKI(blk_entries(sh_env_block(&A, b1, NULL, NULL), 12), 3);

  T("env block: merge replaces by name, case-insensitively, keeping the caller's spelling");
  char *b3[] = { (char*)"Path=/bin", (char*)"HOME=/h", NULL };
  ShEnv e3 = envof(1, "PATH", "/x");
  static const char w3[] = "HOME=/h\0" "PATH=/x\0" "\0";
  blkcase(b3, &e3, w3, sizeof w3 - 1, "PATH replaces Path");
  size_t n3 = 0;
  char *got3 = sh_env_block(&A, b3, &e3, &n3);
  CK(got3 != NULL && !sub(got3, "/bin"));            /* the replaced value is gone */
  CKI(blk_entries(got3, n3), 2);                     /* and no duplicate was added */
  CK(!strcmp(b3[0], "Path=/bin"));                   /* the parent's array is untouched */
  CK(!strcmp(b3[1], "HOME=/h"));

  T("env block: a new name is added, every parent entry survives");
  ShEnv e4 = envof(2, "VXAA", "1", "VXAZ", "2");
  char *b4[] = { (char*)"MID=0", (char*)"ZZ=9", (char*)"AA=0", NULL };
  size_t n4 = 0;
  char *got4 = sh_env_block(&A, b4, &e4, &n4);
  CK(got4 != NULL);
  CKI(blk_entries(got4, n4), 5);                     /* 3 parents + 2 overrides */
  static const char w4[] = "AA=0\0" "MID=0\0" "VXAA=1\0" "VXAZ=2\0" "ZZ=9\0" "\0";
  CHECK(n4 == sizeof w4 - 1 && !memcmp(got4, w4, sizeof w4 - 1),
        "merge order/bytes wrong: [%s|%zu]", got4 ? got4 : "", n4);

  T("env block: a leading '=' is ignored when sorting (drive-map entries are real)");
  char *b5[] = { (char*)"=C:=C:\\dir", (char*)"A=1", NULL };
  static const char w5[] = "A=1\0" "=C:=C:\\dir\0" "\0";
  blkcase(b5, NULL, w5, sizeof w5 - 1, "=C: sorts as the name C:");
  /* ... and it is not sorted to the front, which is what a plain byte compare
   * of the whole entry would have done ('=' is 0x3D, below every letter) */
  CK(memcmp(w5, "A=1", 3) == 0);

  T("env block: names compare up to the first '=', values do not matter to order");
  char *b6[] = { (char*)"AB=1", (char*)"A=x=b", NULL };
  static const char w6[] = "A=x=b\0" "AB=1\0" "\0";
  blkcase(b6, NULL, w6, sizeof w6 - 1, "A before AB");
  ShEnv e7 = envof(1, "VXAW", "a b=c");
  static const char w7[] = "VXAW=a b=c\0" "\0";
  blkcase(NULL, &e7, w7, sizeof w7 - 1, "value keeps its spaces and its second '='");

  T("env block: UTF-8 bytes in a value pass through untouched");
  ShEnv e8 = envof(1, "VXAU", "\xc3\xa9");
  static const char w8[] = "VXAU=\xc3\xa9\0" "\0";
  blkcase(NULL, &e8, w8, sizeof w8 - 1, "two-byte UTF-8 value");
  size_t n8 = 0;
  char *g8 = sh_env_block(&A, NULL, &e8, &n8);
  CKI(n8, 9);                                        /* "VXAU=" 5 + 2 + entry NUL + extra NUL */
  CK(g8 && (unsigned char)g8[5] == 0xc3 && (unsigned char)g8[6] == 0xa9);
  CK(g8 && g8[7] == 0 && g8[8] == 0);

  T("env block: empty and absent are both 'nothing to add'");
  ShEnv e9 = envof(0);
  blkcase(NULL, &e9, NULL, 0, "no parent, no override");
  blkcase(NULL, NULL, NULL, 0, "both NULL");
  blkcase(b1, &e9, w1, sizeof w1 - 1, "empty override is the parent, sorted");
  /* the entry count comes from the block, not from the caller's claim */
  size_t n9 = 0;
  char *g9 = sh_env_block(NULL, b1, NULL, &n9);
  CK(g9 == NULL && n9 == 0);
}

/* =====================================================================
 * 6c. env + stdin through a real spawn
 * ===================================================================== */
static void t_envspawn(void) {
  ShRes r;
  /* one parent-only variable, set by the test, that every merge case below proves
   * survives; the child reads it back with printenv (argv, no shell) */
  proc_set("VXASH_PARENT", "from-parent");

  T("spawn: an override reaches the child AND the parent's other vars are still there");
  ShEnv e1 = envof(1, "VXASH_KID", "kid-value");
  exenv("printenv VXASH_PARENT VXASH_KID", NULL, NULL, 0, &e1, 20000, 0, false, &r);
  CKI(r.code, 0);
  CKS(crz(r.out), "from-parent\nkid-value\n");       /* merge, not replace, byte-exact */
  sh_free(&r);

  T("spawn: the parent process environment is NOT mutated");
  CK(getenv("VXASH_KID") == NULL);                   /* no _putenv behind our back */
  ShRes e2;
  memset(&e2, 0, sizeof e2);
  ex("printenv VXASH_KID", NULL, NULL, 20000, 0, false, &e2);
  CKI(e2.code, 1);                                   /* undefined for a second child too */
  CKI(e2.out_len, 0);
  sh_free(&e2);

  T("spawn: a variable the parent already has is overridden for this child only");
  proc_set("VXASH_LEAK", "orig");
  ShEnv e3 = envof(1, "VXASH_LEAK", "over");
  exenv("printenv VXASH_LEAK", NULL, NULL, 0, &e3, 20000, 0, false, &r);
  CKI(r.code, 0);
  CKS(crz(r.out), "over\n");
  sh_free(&r);
  CKS(getenv("VXASH_LEAK"), "orig");                 /* and the parent still has its own */
  ex("printenv VXASH_LEAK", NULL, NULL, 20000, 0, false, &r);
  CKS(crz(r.out), "orig\n");                          /* next command sees no leak */
  sh_free(&r);

  T("spawn: '=', spaces and a non-ASCII UTF-8 byte survive one child intact");
  /* What the child can echo back is limited by the ANSI hop CreateProcessA does
   * (VXA hands the block over as bytes; the OS reads them in the system codepage
   * and the child re-encodes as UTF-8), so the expectation is computed through
   * that same hop. What this pins is VXA's own part: the value arrives whole --
   * not cut at the '=', not split at the space, not merged with its neighbour. On
   * a UTF-8-codepage box the hop is the identity and the same assertion then
   * demands the original bytes back. */
  ShEnv e4 = envof(1, "VXASH_WEIRD", "a b=c \xc3\xa9");
  char want[64];
  snprintf(want, sizeof want, "%s\n", ansi_hop("a b=c \xc3\xa9"));
  exenv("printenv VXASH_WEIRD", NULL, NULL, 0, &e4, 20000, 0, false, &r);
  CKI(r.code, 0);
  CKS(crz(r.out), want);
  CK(sub(r.out, "a b=c "));                           /* the '=' and the space are inside ONE value */
  sh_free(&r);
  /* A byte sequence the codepage cannot represent is refused, not smuggled in:
   * a dangling tail byte would swallow the next entry's first byte. */
  ShEnv e5 = envof(1, "VXASH_BAD", "a\xc3\xa9\xe4\xb8\xad");
  exenv("printenv VXASH_BAD", NULL, NULL, 0, &e5, 20000, 0, false, &r);
  CKI(r.code, SH_SPAWN_FAIL);
  CK(sub(r.err, "codepage"));
  CK(sub(r.err, "VXASH_BAD"));
  CKI(r.out_len, 0);                                  /* nothing ran */
  sh_free(&r);

  T("spawn: PATH survives an override, so a later argv lookup still resolves");
  const char *pp = getenv("PATH");
  if (pp && *pp) {
    ShEnv e5 = envof(1, "VXASH_ONLY", "1");
    exenv("printenv VXASH_ONLY", NULL, NULL, 0, &e5, 20000, 0, false, &r);
    CKI(r.code, 0);
    CKS(crz(r.out), "1\n");                          /* printenv was FOUND: PATH was merged in */
    sh_free(&r);
  } else CK(getenv("PATH") != NULL);

  T("spawn: env on the shell path too (a string command gets the same block)");
  ShEnv e6 = envof(1, "VXASH_STR", "s1");
  exenv("printenv VXASH_STR", NULL, NULL, 0, &e6, 20000, 0, false, &r);
  CKI(r.code, 0);
  CKS(crz(r.out), "s1\n");
  sh_free(&r);

  T("spawn: cmd /c echo %VAR% sees the override (the form an agent writes for a build)");
  ShEnv ec = envof(1, "VXATEST", "abc123");
  exenv("cmd /c echo %VXATEST%", NULL, NULL, 0, &ec, 20000, 0, false, &r);
  CKI(r.code, 0);
  CKS(crz(r.out), "abc123\n");
  sh_free(&r);
  CK(getenv("VXATEST") == NULL);                     /* and this process never had it */

  T("spawn: stdin bytes arrive exactly, including a second line and a blank line");
  const char *patch = "diff --git a/x b/x\n\n+hello \xc3\xa9\n";
  exenv("cat", NULL, patch, strlen(patch), NULL, 20000, 0, false, &r);
  CKI(r.code, 0);
  CKS(crz(r.out), patch);
  sh_free(&r);

  T("spawn: empty stdin and no stdin both reach immediate EOF, and neither hangs");
  exenv("cat", NULL, "", 0, NULL, 5000, 0, false, &r);
  CKI(r.code, 0);
  CKI(r.out_len, 0);
  sh_free(&r);
  exenv("cat", NULL, NULL, 0, NULL, 5000, 0, false, &r);
  CKI(r.code, 0);
  CKI(r.out_len, 0);
  sh_free(&r);

  T("spawn: env + stdin together, and the capture files are still cleaned up");
  char tmpdir[700];
  snprintf(tmpdir, sizeof tmpdir, "%s/.vxa/tmp", ROOT);
  ShEnv e7 = envof(2, "VXASH_A", "1", "VXASH_B", "2");
  exenv("printenv VXASH_A", NULL, "ignored-stdin\n", 14, &e7, 20000, 0, false, &r);
  CKI(r.code, 0);
  CKS(crz(r.out), "1\n");
  sh_free(&r);
  CKI(count_dir(tmpdir), 0);                         /* including the stdin file */

  T("spawn: a stdin handle we redirected does not defeat the timeout");
  /* reads one line from the file we handed it, prints it, then hangs forever:
   * the run must still be killed on budget AND return what it had written */
  char *av8[] = { (char*)"sh", (char*)"-c",
                  (char*)"read a; echo got-$a; echo ${VXASH_T:-notset}; sleep 30" };
  ShEnv e8 = envof(1, "VXASH_T", "env-in-timeout-child");
  memset(&r, 0, sizeof r);
  bool started8 = sh_spawn_argv(CT, av8, 3, NULL, "x-line\ny-line\n", 14, &e8, 900, &r);
  CK(started8);
  CK(r.timed_out);
  CKI(r.code, SH_TIMED_OUT);
  CK(r.ms >= 700 && r.ms < 8000);
  CKS(crz(r.out), "got-x-line\nenv-in-timeout-child\n");   /* input AND env, then killed */
  sh_free(&r);
  CKI(count_dir(tmpdir), 0);                         /* killed child released the files */
}

/* =====================================================================
 * 7. sh_result_v record shape
 * ===================================================================== */
static void t_result_v(void) {
  T("result_v: record keys and omission");
  ShRes f;
  memset(&f, 0, sizeof f);
  f.code = 3; f.ms = 17; f.truncated = true; f.total_lines_set = true; f.total_lines = 500;
  f.out = dupz_local("out-text"); f.out_len = 8;
  V v = sh_result_v(CT, &f, NULL);
  bool ok = false;
  CK(v.t == V_REC);
  CKI(numof(v, "code", &ok), 3); CK(ok);
  CKI(numof(v, "ms", &ok), 17); CK(ok);
  CKSTR(nameof(v, "out"), "out-text");
  CK(rec_getz(v.u.r, "err") == NULL);
  CK(boolof(v, "truncated", &ok) && ok);
  CKI(numof(v, "total_lines", &ok), 500);
  CKI(numof(v, "signalless", &ok), 0);
  CKS(v_tojson(&A, v).p,
      "{\"code\":3,\"ms\":17,\"out\":\"out-text\",\"truncated\":true,"
      "\"total_lines\":500,\"signalless\":0}");
  free(f.out);

  ShRes g;
  memset(&g, 0, sizeof g);
  g.code = 0; g.ms = 4; g.total_lines_set = true;
  g.err = dupz_local("boom"); g.err_len = 4;
  V w = sh_result_v(CT, &g, NULL);
  CK(rec_getz(w.u.r, "out") == NULL);      /* absence carries information */
  CKSTR(nameof(w, "err"), "boom");
  CK(!boolof(w, "truncated", &ok) && ok);
  CK(rec_getz(w.u.r, "timed_out") == NULL);
  CKI(numof(w, "total_lines", &ok), 0);
  free(g.err);

  ShRes h;
  memset(&h, 0, sizeof h);
  h.code = SH_TIMED_OUT; h.timed_out = true; h.out = dupz_local("partial"); h.out_len = 7;
  V x = sh_result_v(CT, &h, NULL);
  CK(boolof(x, "timed_out", &ok) && ok);
  CKI(numof(x, "code", &ok), SH_TIMED_OUT);
  CKI(numof(x, "ms", &ok), 0);
  CK(rec_getz(x.u.r, "total_lines") == NULL);
  free(h.out);

  CK(sh_result_v(NULL, &f, "x").t == V_NULL);

  T("result_v: strings are copied into the arena");
  ShRes k;
  ex("echo arena-copy", NULL, NULL, 20000, 0, false, &k);
  V y = sh_result_v(CT, &k, "echo arena-copy");
  Str s = nameof(y, "out");
  sh_free(&k);                              /* malloc buffers gone ... */
  CK(s.p != NULL);
  CK(s.len >= 10 && !memcmp(s.p, "arena-copy", 10));  /* ... arena copy survives */
  CK(v_tok_est(y) > 0);
}

int main(void) {
  arena_init(&A, 0);
  mk_dir("build");
  mk_dir("build/testws");
  abs_path("build/testws", ROOT, sizeof ROOT);
  CT = ctx_new(&A, ROOT, "tests/test_sh.c");

  t_quote();
  t_split();
  t_meta();
  t_readonly();
  t_redact();
  t_shape();
  t_envblock();
  t_exec();
  t_envspawn();
  t_result_v();

  T_REPORT("sh");
}
