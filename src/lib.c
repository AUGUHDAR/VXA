/* lib.c - builtin registry and the built-in documentation.
 * One row per builtin carries code + signature + doc + example, so `vxa doc`
 * and `vxa lang` are generated from what actually runs. A hand-written manual
 * would drift; this one cannot. */
#include "vxa.h"
#include "interp.h"
#include "lib.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>

V list_of(List *l) { V r; r.t = V_LIST; r.u.l = l; return r; }

static const Builtin *g_all = NULL;
static int g_n = 0;

static void build(void) {
  if (g_all) return;
  int cap = 512, n = 0;
  Builtin *out = (Builtin*)malloc(sizeof(Builtin) * (size_t)cap);
  static const Builtin* (*tabs[])(int*) = { t_fs, t_tx, t_sh, t_out, t_cfg, t_misc, t_json };
  for (size_t k = 0; k < sizeof(tabs)/sizeof(tabs[0]); k++) {
    int m = 0;
    const Builtin *t = tabs[k](&m);
    for (int i = 0; i < m && n < cap; i++) out[n++] = t[i];
  }
  g_all = out;
  g_n = n;
}

const Builtin *lib_all(int *count) {
  build();
  if (count) *count = g_n;
  return g_all;
}

const Builtin *lib_find(const char *ns, const char *name) {
  build();
  for (int i = 0; i < g_n; i++)
    if (!strcmp(g_all[i].ns, ns) && !strcmp(g_all[i].name, name)) return &g_all[i];
  return NULL;                                 /* dispatch never guesses a name */
}

/* Nearest registered name, for error text only. A suggestion in a message is a
 * lookup aid; a suggestion in dispatch would run the wrong function silently. */
const Builtin *lib_suggest(const char *ns, const char *name) {
  build();
  const Builtin *best = NULL;
  double bestsc = 0.62;
  for (int i = 0; i < g_n; i++) {
    if (strcmp(g_all[i].ns, ns)) continue;
    double s = sim_ratio(name, (int)strlen(name), g_all[i].name, (int)strlen(g_all[i].name));
    if (s > bestsc) { bestsc = s; best = &g_all[i]; }
  }
  return best;
}

V lib_call(Ctx *c, const Builtin *b, V *args, int nargs) {
  if (nargs < b->min || (b->max >= 0 && nargs > b->max)) {
    set_error(c, E_ARITY, "%s.%s takes %d..%s%d argument(s), got %d - sig: %s",
              b->ns, b->name, b->min, b->max < 0 ? "or more, " : "", b->max < 0 ? b->min : b->max,
              nargs, b->sig);
    return VN;
  }
  for (int i = 0; i < nargs; i++) {
    if (v_is_err(args[i])) return args[i];      /* never run on a failed input */
  }
  V r = b->fn(c, args, nargs);
  if (ctx_has_err(c)) return ctx_err(c);
  return r;
}

/* ---------- doc rendering ---------- */
static const char *short_of(const char *sig, const char *ns, const char *name) {
  (void)sig; (void)ns; (void)name;
  return NULL;
}

void doc_one(Arena *a, Buf *b, const char *qualified) {
  const char *dot = strchr(qualified, '.');
  char ns[32];
  const char *nm = qualified;
  if (dot) {
    size_t n = (size_t)(dot - qualified);
    if (n >= sizeof ns) n = sizeof ns - 1;
    memcpy(ns, qualified, n); ns[n] = 0;
    nm = dot + 1;
  } else ns[0] = 0;
  if (!strcmp(ns, "global") || !strcmp(ns, "misc")) ns[0] = 0;   /* ns "" holds them */
  const Builtin *bl = lib_find(ns, nm);
  if (!bl) {
    buf_fmt(b, "!ERR code=NOENT name=%s hint=\"list the namespace with: vxa doc %s\"",
            qualified, ns[0] ? ns : "all");
    return;
  }
  buf_fmt(b, "%s.%s sig=\"%s\" doc=\"%s\" ex=\"%s\" pure=%s",
          bl->ns, bl->name, bl->sig, bl->doc, bl->ex ? bl->ex : "-",
          (bl->flags & BF_PURE) ? "t" : "f");
}

void doc_namespace(Arena *a, Buf *b, const char *ns, bool full) {
  if (!ns || !*ns || !strcmp(ns, "global") || !strcmp(ns, "misc")) ns = "";
  int n = 0;
  const Builtin *all = lib_all(&n);
  int bytes = 0;
  for (int i = 0; i < n; i++) {
    if (strcmp(all[i].ns, ns)) continue;
    if (full)
      buf_fmt(b, "%s.%s sig=\"%s\" doc=\"%s\" ex=\"%s\"\n", all[i].ns, all[i].name,
              all[i].sig, all[i].doc, all[i].ex ? all[i].ex : "-");
    else
      buf_fmt(b, "%s.%s %s\n", all[i].ns, all[i].name, all[i].doc);
    bytes++;
  }
  if (!bytes) {
    buf_fmt(b, "!ERR code=NOENT ns=%s hint=\"namespaces:", ns);
    static const char *names[] = { "fs", "tx", "sh", "out", "cfg", "task", "json", "misc" };
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) buf_fmt(b, "%s%s", i ? "," : "", names[i]);
    buf_puts(b, "\"");
    return;
  }
  (void)a;
}

/* the cheat sheet: an agent should be able to write correct VXA after ONE call.
 * Budget is enforced by tests/test_doc.c so this cannot silently grow. */
static const char *CHEAT =
"VXA 0.1 - language for agents. One call, then write.\n"
"stmt: NAME = expr | def NAME(params){...} | if e {} elif e {} else {} | while e {} | for v in e {} | for k,v in e {} | return e? | break | continue\n"
"expr: x | y  (pipe: value becomes FIRST arg) | a..b range | == != < <= > >= ~= + - * / % and or not -\n"
"val:  12 1.5 \"tpl{x}y\" 'raw' ```block``` [1,2,] {k:1,k2:\"v\"} null true false def(a,b){a+b} a=>a*2\n"
"call: fs.read(\"a.c\",{lines:\"10-40\"})  list=[1,2]  rec.x=1  rec[\"x\"]  s[0] s[2..5]\n"
"out:  k=v,k2=v2  nested keeps {}[]  list=[1,2]  null=- bool=t/f  err=!ERR code=X msg=\"..\" hint=\"..\"\n"
"side effects need confirmation: run -> prints PLAN(op,token,files=[{path,kind,from,to,bytes}]) exit 3; rerun with --confirm TOKEN. the CLI never applies a plan for you: a script ending on an unapplied plan => code=UNAPPLIED exit 2, zero writes; several writes need several .apply(). token is bound to old content hashes (file changed => STALE_PLAN) and is single use (journal).\n"
"exit: 0 ok 1 usage 2 script 3 need-confirm 4 policy 5 io 6 internal 7 timeout\n"
"cli:  vxa eval 'expr' | run f.vxa [--confirm T] | plan f.vxa | fs.read P [--lines a-b --grep s --anchors --max-bytes N] | fs.ls DIR [--glob G --recursive] | fs.outline P | fs.bundle P --query Q | sh 'cmd' | diff A B | patch P --ops ops.json | find RE [DIR] | check [dir] | doc fs | lang | doctor\n"
"rules: no fuzzy syntax, no brace repair, no implicit calls. errors are values: r=fs.read(x) if r.code? {..}. missing record key reads as -. never guess a path: fs.ls first.\n";

void doc_lang(Arena *a, Buf *b, int level) {
  buf_puts(b, CHEAT);
  int n = 0;
  const Builtin *all = lib_all(&n);
  if (level >= 2) {
    buf_puts(b, "\nbuiltins:\n");
    for (int i = 0; i < n; i++)
      buf_fmt(b, "%s.%s %s\n", all[i].ns, all[i].name, all[i].doc);
  }
  if (level >= 3) {
    buf_puts(b, "\nsignatures:\n");
    for (int i = 0; i < n; i++)
      buf_fmt(b, "%s.%s sig=\"%s\" ex=\"%s\"\n", all[i].ns, all[i].name, all[i].sig,
              all[i].ex ? all[i].ex : "-");
  }
  (void)short_of(NULL, NULL, NULL);
}
