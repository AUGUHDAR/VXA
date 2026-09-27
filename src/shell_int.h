/* shell_int.h - internal surface of shell.c, shared with tests/test_sh.c.
 *
 * Everything here is pure (no process, no filesystem) except the two spawn
 * engines, so the security-relevant logic -- argv quoting, the shell-metachar
 * decision, the read-only allowlist, redaction and the tail/byte-cap shaping --
 * can be unit-tested without the interpreter. Not part of the language API;
 * only src/shell.c and tests/test_sh.c include this file.
 */
#ifndef VXA_SHELL_INT_H
#define VXA_SHELL_INT_H
#include "vxa.h"

/* ---- sentinels for ShRes.code (never a real exit code) ----
 * -1 timed out (SPEC: agent branches on timed_out, code is meaningless)
 * -2 cwd not found (validated before spawn, nothing ran)
 * -3 command could not be parsed or could not be started */
enum { SH_TIMED_OUT = -1, SH_NO_CWD = -2, SH_SPAWN_FAIL = -3 };

/* hard capture limit per stream, kept head+tail of the bytes */
#define SH_BYTE_CAP ((size_t)1024u * 1024u)
#define SH_MAXARGS 64
#define SH_DEFAULT_TIMEOUT_MS 30000
#define SH_KILL_GRACE_MS 2000

/* ---------- word splitting (no arena; caller owns buf) ----------
 * Splits a plain program+args string the way an argv literal would: runs of
 * whitespace separate words, " and ' quote (inside " a \" or \\ escapes), an
 * empty quoted word yields an empty argument. Metacharacters are NOT handled
 * here: sh_exec routes those to the shell before ever calling this.
 * Returns argc (0 = empty command) or negative:
 *   -2 too many arguments   -3 buffer too small   -4 unbalanced quote */
int sh_split(const char *cmd, char *buf, size_t bufsz, char *argv[], int maxargs);

/* ---------- shell metacharacters ----------
 * `; & | $ ` > <` and newline. Any of them => the string MUST go through
 * cmd.exe /c (or sh -c) and can never be considered read-only. */
bool sh_has_meta(const char *cmd);

/* ---------- Windows argv quoting (MSVC/CRT rules, always quoted) ----------
 * Emits "...": every run of N backslashes directly before a " or before the
 * closing quote is doubled (2N, or 2N+1 when it must escape that quote);
 * embedded " becomes \". Always wrapping also neutralizes trailing spaces. */
void sh_quote(Buf *b, const char *arg);
Str  sh_command_line(Arena *a, char *const *argv, int argc);   /* "a" "b" ... */
Str  sh_shell_line(Arena *a, const char *cmd);   /* "cmd.exe" "/d" "/v:off" "/c" <cmd> */
Str  sh_bat_line(Arena *a, const char *bat);     /* same, but runs a temp .cmd file */

/* ---------- read-only allowlist (argv-level, never string prefixes) ------
 * sh_prog_is_path: '/', '\', ':' or '.' in the program name, or a name that is
 * empty/dot-ish => true (absolute or relative path, never allowlisted).
 * sh_argv_readonly works on split argv: argv[0] exact table hit, and for the
 * grouped tools argv[1] must be the exact subcommand. */
bool sh_prog_is_path(const char *prog);
bool sh_argv_readonly(char *const *argv, int argc);

/* ---------- line shaping ----------
 * A "line" is bytes up to and including its '\n'; a final unterminated line
 * still counts. Empty buffer = 0 lines. */
int    sh_count_lines(const char *p, size_t n);
size_t sh_tail_offset(const char *p, size_t n, int keep);   /* first byte of the last keep lines */
/* Truncate buf in place to its last tail_lines lines; returns the new length
 * and reports the pre-truncation line count + whether anything was dropped.
 * tail_lines <= 0 keeps everything and never sets truncated. */
size_t sh_shape(char *buf, size_t len, int tail_lines, int *total_lines, bool *truncated);

/* ---------- byte cap ---------- */
void   sh_cap_spans(size_t total, size_t cap, size_t *head_len, size_t *tail_off, size_t *tail_len);
size_t sh_cap_marker(char *dst, size_t dstsz, size_t kept, size_t total);
/* malloc'd head+marker+tail copy (or a plain copy when n <= cap). Never used
 * for the >cap Windows case (there we seek instead of slurping). */
char  *sh_cap_apply(const char *p, size_t n, size_t cap, size_t *out_len);

/* ---------- redaction ----------
 * Key list is matched case-insensitively against the segments (split on
 * _ - . /) of an identifier run, so access_token, api-key and SECRET_KEY hit
 * too; the run must be a whole segment, so tokenizer/secretary do not.
 * Blob rule (heuristic, one line): a word of >= 24 chars from
 * [A-Za-z0-9+/_.-] holding lowercase AND uppercase AND a digit is a secret,
 * unless the word looks like a path (contains ":/" or "//" or two '/'). */
bool   sh_blob_secret(const char *p, size_t n);
size_t sh_ident_run(const char *p, size_t n, size_t i);   /* end of run, or i */
bool   sh_key_run(const char *p, size_t n);               /* run is a secret key name */

/* ---------- spawn engines (platform internals, used by sh_exec) ----------
 * Return true only if a process was actually started (and so out/err/time
 * mean something). false means nothing ran and code is a SH_* sentinel. */
bool sh_spawn_argv(Ctx *c, char *const *argv, int argc, const char *cwd,
                   const char *stdin_data, int timeout_ms, ShRes *r);
bool sh_spawn_shell(Ctx *c, const char *cmd, const char *cwd,
                    const char *stdin_data, int timeout_ms, ShRes *r);

#endif
