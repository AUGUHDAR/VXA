/* fsx.c - workspace-jailed file access.
 * Rules that make agent writes survivable:
 *   - every write resolves inside the workspace root (the "jail")
 *   - writes are atomic (tmp + rename)
 *   - deletes go to .vxa/trash and are restorable, never unlinked
 *   - reads report bytes/lines/truncated instead of pretending to be complete
 * Known limitation: normalization is lexical, so a symlink that escapes the root
 * is not detected here; the plan layer re-hashes targets before applying, which
 * bounds the damage to a file the agent could already name. */
#include "vxa.h"
#include "interp.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <stdio.h>
#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #define MKDIR(p) _mkdir(p)
#else
  #include <dirent.h>
  #include <unistd.h>
  #define MKDIR(p) mkdir((p), 0777)
#endif
#include <dirent.h>
#include <unistd.h>

#define TRASH_KEEP_DAYS 30
#define READ_CAP (4u * 1024u * 1024u)

static void split_lines(Arena *a, Str t, Str *l, int *n, int max) {
  int start = 0, k = 0;
  for (int i = 0; i <= t.len && k < max; i++) {
    bool brk = (i == t.len) || t.p[i] == '\n' || t.p[i] == '\r';
    if (!brk) continue;
    int end = i;
    if (t.p[i] == '\r' && i + 1 < t.len && t.p[i+1] == '\n') i++;
    if (i == t.len && end == start) break;         /* no trailing empty line */
    l[k++] = s_slice(t, start, end);
    start = i + 1;
  }
  *n = k;
}

bool fs_exists(const char *p) { struct stat st; return p && !stat(p, &st); }
bool fs_is_dir(const char *p) { struct stat st; return p && !stat(p, &st) && S_ISDIR(st.st_mode); }
long fs_size(const char *p) { struct stat st; if (!p || stat(p, &st)) return -1; return (long)st.st_size; }

char *fs_slurp(Arena *a, const char *p, size_t *len_out, ErrCode *err) {
  if (err) *err = E_NONE;
  struct stat st;
  if (!p || !*p) { if (err) *err = E_BAD_INPUT; return NULL; }
  if (stat(p, &st)) { if (err) *err = E_NOENT; return NULL; }
  if (S_ISDIR(st.st_mode)) { if (err) *err = E_ISDIR; return NULL; }
  if (st.st_size > (off_t)READ_CAP) { if (err) *err = E_LIMIT; return NULL; }
  FILE *f = fopen(p, "rb");
  if (!f) { if (err) *err = E_DENIED; return NULL; }
  size_t n = (size_t)st.st_size;
  char *buf = (char*)arena_alloc(a, n + 1);
  size_t got = n ? fread(buf, 1, n, f) : 0;
  fclose(f);
  buf[got] = 0;
  if (len_out) *len_out = got;
  return buf;
}

Str fs_hash_file(Arena *a, const char *p, ErrCode *err) {
  size_t n = 0;
  char *d = fs_slurp(a, p, &n, err);
  if (!d) return s_null();
  return hash12(a, d, n);
}

/* ---------- paths ---------- */
static char *get_cwd(Arena *a) {
  char buf[4096];
  if (!getcwd(buf, sizeof buf)) return arena_strdup(a, ".");
  return arena_strdup(a, buf);
}

Str ws_resolve(Arena *a, Ctx *c, const char *p, ErrCode *err) {
  if (err) *err = E_NONE;
  if (!p || !*p) { if (err) *err = E_BAD_INPUT; return s_null(); }
  char *root = (char*)ctx_root(c);
  Str abs;
  if (path_is_abs(p)) abs = path_norm(a, p);
  else abs = path_norm(a, path_join(a, get_cwd(a), p).p);
  /* drive letters are compared case-insensitively on Windows inside path_within */
  char *rs = path_norm(a, root).p;
  if (!path_within(rs, abs.p)) { if (err) *err = E_OUTSIDE_JAIL; return s_null(); }
  return abs;
}

const char *ws_trash(Ctx *c) {
  Arena *a = ctx_arena(c);
  return path_join(a, ctx_root(c), ".vxa/trash").p;
}
const char *ws_tmp(Ctx *c) {
  Arena *a = ctx_arena(c);
  return path_join(a, ctx_root(c), ".vxa/tmp").p;
}
void ensure_dir_for(const char *p) {
  if (!p || fs_is_dir(p)) return;
  char *dup = strdup(p);
  for (char *s = dup + 1; *s; s++) {
    if (*s == '/' || *s == '\\') { *s = 0; MKDIR(dup); *s = '/'; }
  }
  MKDIR(dup);
  free(dup);
}

/* ---------- writes ---------- */
void ensure_parent_of(const char *path) {
  char *d = strdup(path);
  char *sl = strrchr(d, '/');
  char *bs = strrchr(d, 92);
  char *cut = sl > bs ? sl : bs;
  if (cut) { *cut = 0; if (*d) ensure_dir_for(d); }
  free(d);
}

static ErrCode write_now(const char *path, const char *data, size_t n) {
  ensure_parent_of(path);
  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s.vxatmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return E_DENIED;
  size_t w = n ? fwrite(data, 1, n, f) : 0;
  if (fflush(f) != 0) { fclose(f); remove(tmp); return E_IO; }
  fclose(f);
  if (w != n) { remove(tmp); return E_IO; }
  if (rename(tmp, path) != 0) {
    /* Windows rename refuses an existing target */
    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return E_IO; }
  }
  return E_NONE;
}

V fs_write_atomic(Ctx *c, const char *path, const char *data, size_t n, bool append, ErrCode *err) {
  Arena *a = ctx_arena(c);
  if (err) *err = E_NONE;
  if (append && fs_exists(path)) {
    size_t on = 0;
    char *old = fs_slurp(a, path, &on, err);
    if (!old) return VN;
    char *cat = (char*)arena_alloc(a, on + n + 1);
    memcpy(cat, old, on);
    memcpy(cat + on, data, n);
    cat[on + n] = 0;
    *err = write_now(path, cat, on + n);
    return *err == E_NONE ? v_ok(a, 2, "path", v_str(s_lit(a, path)), "bytes", v_num((double)(on + n))) : VN;
  }
  *err = write_now(path, data, n);
  if (*err != E_NONE) return VN;
  return v_ok(a, 2, "path", v_str(s_lit(a, path)), "bytes", v_num((double)n));
}

V fs_move(Ctx *c, const char *from, const char *to, ErrCode *err) {
  Arena *a = ctx_arena(c);
  if (err) *err = E_NONE;
  if (!fs_exists(from)) { *err = E_NOENT; return VN; }
  ensure_parent_of(to);
  if (rename(from, to) != 0) {
    if (fs_exists(to)) remove(to);
    if (rename(from, to) != 0) { *err = E_IO; return VN; }
  }
  return v_ok(a, 3, "from", v_str(s_lit(a, from)), "path", v_str(s_lit(a, to)),
              "bytes", v_num((double)fs_size(to)));
}

/* ---------- trash ---------- */
static void trash_index_add(Ctx *c, long seq, const char *orig, const char *stored, long bytes) {
  const char *dir = ws_trash(c);
  ensure_dir_for(dir);
  char idx[4096];
  snprintf(idx, sizeof idx, "%s/index.ndjson", dir);
  FILE *f = fopen(idx, "ab");
  if (!f) return;
  fprintf(f, "{\"seq\":%ld,\"path\":\"%s\",\"stored\":\"%s\",\"bytes\":%ld}\n",
          seq, orig, stored, bytes);
  fclose(f);
}

V fs_to_trash(Ctx *c, const char *path, long *seq_out, ErrCode *err) {
  Arena *a = ctx_arena(c);
  if (err) *err = E_NONE;
  struct stat st;
  if (stat(path, &st)) { *err = E_NOENT; return VN; }
  if (S_ISDIR(st.st_mode)) { *err = E_ISDIR; return VN; }   /* directory deletes unsupported: too easy to wreck a tree */
  const char *dir = ws_trash(c);
  ensure_dir_for(dir);
  long seq = (long)time(NULL);
  char stored[4096];
  snprintf(stored, sizeof stored, "%s/%ld__%s", dir, seq, path_base(path));
  if (rename(path, stored) != 0) { *err = E_IO; return VN; }
  trash_index_add(c, seq, path, stored, (long)st.st_size);
  if (seq_out) *seq_out = seq;
  return v_ok(a, 3, "path", v_str(s_lit(a, path)), "trash", v_str(s_lit(a, stored)),
              "seq", v_num((double)seq));
}

V fs_restore(Ctx *c, long seq, ErrCode *err) {
  Arena *a = ctx_arena(c);
  if (err) *err = E_NONE;
  char idx[4096];
  snprintf(idx, sizeof idx, "%s/index.ndjson", ws_trash(c));
  FILE *f = fopen(idx, "rb");
  if (!f) { *err = E_NOENT; return VN; }
  char line[4096];
  long found = -1;
  char orig[2048] = "", stored[2048] = "";
  while (fgets(line, sizeof line, f)) {
    char *ps = strstr(line, "\"seq\":");
    if (!ps) continue;
    long s = atol(ps + 6);
    char *po = strstr(line, "\"path\":\""), *pd = strstr(line, "\"stored\":\"");
    if (s == seq && po && pd) {
      po += 8; pd += 10;
      char *e1 = strchr(po, '"'), *e2 = strchr(pd, '"');
      if (e1 && e2) {
        size_t n1 = (size_t)(e1 - po), n2 = (size_t)(e2 - pd);
        if (n1 < sizeof orig && n2 < sizeof stored) {
          memcpy(orig, po, n1); orig[n1] = 0;
          memcpy(stored, pd, n2); stored[n2] = 0;
          found = s;
        }
      }
    }
  }
  fclose(f);
  if (found < 0) { *err = E_NOENT; return VN; }
  ErrCode e2 = E_NONE;
  V r = fs_move(c, stored, orig, &e2);
  if (e2 != E_NONE) { *err = e2; return VN; }
  (void)a;
  return r;
}

/* ---------- reads ---------- */
static bool line_matches(Str l, const char *needle, bool icase) {
  if (!needle || !*needle) return true;
  Str nd = s_wrap(needle);
  if (!icase) return s_find(l, nd, 0) >= 0;
  for (int i = 0; i + nd.len <= l.len; i++) {
    int ok = 1;
    for (int j = 0; j < nd.len; j++) {
      char x = (char)tolower((unsigned char)l.p[i+j]), y = (char)tolower((unsigned char)nd.p[j]);
      if (x != y) { ok = 0; break; }
    }
    if (ok) return true;
  }
  return false;
}

V fs_read_range(Ctx *c, const char *path, int from, int to, const char *grep,
               bool anchors, int ctx_lines, int max_bytes, ErrCode *err) {
  Arena *a = ctx_arena(c);
  if (err) *err = E_NONE;
  size_t n = 0;
  ErrCode se = E_NONE;
  char *data = fs_slurp(a, path, &n, &se);
  if (!data) { *err = se; return VN; }
  Str text = s_wrap(data);
  int cap = 200000;
  Str *lines = (Str*)arena_zalloc(a, sizeof(Str) * (size_t)cap);
  int nl = 0;
  split_lines(a, text, lines, &nl, cap);
  int total_bytes = (int)n;
  int total_lines = nl;

  /* select ranges */
  int *keep = (int*)arena_zalloc(a, sizeof(int) * (size_t)(nl ? nl : 1));
  int nkeep = 0;
  if (grep && *grep) {
    for (int i = 0; i < nl; i++) {
      if (!line_matches(lines[i], grep, true)) continue;
      int lo = i - (ctx_lines > 0 ? ctx_lines : 0); if (lo < 0) lo = 0;
      int hi = i + (ctx_lines > 0 ? ctx_lines : 0); if (hi >= nl) hi = nl - 1;
      for (int k = lo; k <= hi; k++) if (!keep[k]) { keep[k] = 1; }
    }
    for (int i = 0; i < nl; i++) if (keep[i]) keep[nkeep++] = i;
    /* keep[] now holds indices; rebuild in order */
  } else {
    int lo = from > 0 ? from - 1 : 0;
    int hi = to > 0 ? to : nl;
        if (lo > nl) lo = nl;
    if (hi > nl) hi = nl;
    if (hi < lo) hi = lo;
    for (int i = lo; i < hi; i++) keep[nkeep++] = i;
  }

  /* anchors for the kept lines */
  int acount = 0;
  Anch *A = anchors_of(a, path, text, &acount, err);
  if (anchors && *err != E_NONE) return VN;

  Buf b; buf_init(&b, a);
  int emitted = 0, bytes = 0;
  bool truncated = false;
  for (int i = 0; i < nkeep; i++) {
    int idx = keep[i];
    Str l = lines[idx];
    char pre[32];
    pre[0] = 0;
    if (anchors) {
      /* never degrade silently: a read that claims anchors but hands back bare
       * line numbers would feed tx.patch anchors that do not exist */
      if (acount != nl) { *err = E_INTERNAL; set_error(c, E_INTERNAL, "fs.read(%s): anchor table has %d entries for %d lines; refusing to emit partial anchors", path, acount, nl); return VN; }
      snprintf(pre, sizeof pre, "L%d:%s| ", idx + 1, A[idx].at.p);
    }
    int need = (int)strlen(pre) + l.len + 1;
    if (max_bytes > 0 && bytes + need > max_bytes) { truncated = true; break; }
    buf_puts(&b, pre);
    buf_put(&b, l.p, (size_t)l.len);
    buf_putc(&b, '\n');
    bytes += need;
    emitted++;
  }
  Str out = buf_take(&b);
  ErrCode he = E_NONE;
  Str h = hash12(a, data, n);
  (void)he;
  int lo = nkeep ? keep[0] + 1 : 0, hi = nkeep ? keep[nkeep-1] + 1 : 0;
  return v_ok(a, 9,
    "path", v_str(s_lit(a, path)),
    "hash", v_str(h),
    "lines", v_str(s_fmt(a, "%d-%d", lo, hi)),
    "total", v_num(total_lines),
    "bytes", v_num(total_bytes),
    "shown", v_num(emitted),
    "truncated", truncated ? VT : VF,
    "est", v_num((double)((bytes + 3) / 4)),
    "text", v_str(out));
  (void)cap; (void)a;
}

static int count_lines_file(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f) return -1;
  int lines = 0, c, last = -1;
  long bytes = 0;
  while ((c = fgetc(f)) != EOF) {
    bytes++;
    if (c == 10) lines++;
    last = c;
  }
  fclose(f);
  if (bytes == 0) return 0;
  if (last != 10) lines++;
  return lines;
}



V fs_ls(Ctx *c, const char *dir, const char *globpat, bool recurse, int max, ErrCode *err) {
  Arena *a = ctx_arena(c);
  if (err) *err = E_NONE;
  const char *d = (dir && *dir) ? dir : ".";
  int limit = max > 0 ? max : 2000;
  DIR *dp = opendir(d);
  if (!dp) { *err = fs_is_dir(d) ? E_DENIED : E_NOENT; return VN; }
  List *l = list_new(a);
  struct dirent *en;
  while ((en = readdir(dp)) && l->len < limit) {
    const char *nm = en->d_name;
    if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
    if (nm[0] == '.' && !(globpat && globpat[0] == '.')) continue;   /* dotfiles only when asked for */
    Str full = s_fmt(a, "%s/%s", d, nm);
    if (fs_is_dir(full.p)) {
      if (recurse) {
        ErrCode sub = E_NONE;
        V inner = fs_ls(c, full.p, globpat, true, limit - l->len, &sub);
        if (sub == E_NONE && inner.t == V_LIST)
          for (int i = 0; i < inner.u.l->len; i++) list_push(a, l, inner.u.l->v[i]);
      }
      continue;
    }
    if (globpat && *globpat && !glob_match(globpat, nm)) continue;
    Rec *r = rec_new(a);
    rec_setz(a, r, "path", v_str(full));
    rec_setz(a, r, "bytes", v_num((double)fs_size(full.p)));
    rec_setz(a, r, "lines", v_num((double)count_lines_file(full.p)));
    list_push(a, l, rec_to_v(r));
  }
  closedir(dp);
  for (int i = 1; i < l->len; i++) {              /* stable order: never depend on readdir */
    V key = l->v[i];
    V *kp = rec_getz(key.u.r, "path");
    if (!kp) continue;
    int j = i - 1;
    while (j >= 0) {
      V *pj = rec_getz(l->v[j].u.r, "path");
      if (!pj || s_cmp(pj->u.s, kp->u.s) <= 0) break;
      l->v[j + 1] = l->v[j]; j--;
    }
    l->v[j + 1] = key;
  }
  return list_of(l);
}

V fs_glob(Ctx *c, const char *pattern, const char *root, int max, ErrCode *err) {
  Arena *a = ctx_arena(c);
  ErrCode e = E_NONE;
  V all = fs_ls(c, root && *root ? root : ".", NULL, true, 0, &e);
  if (e != E_NONE) { if (err) *err = e; return VN; }
  char *base = path_norm(a, root && *root ? root : ".").p;
  size_t bl = strlen(base);
  List *out = list_new(a);
  for (int i = 0; i < all.u.l->len; i++) {
    V *pv = rec_getz(all.u.l->v[i].u.r, "path");
    if (!pv) continue;
    const char *rel = pv->u.s.p;
    if (!strncmp(rel, base, bl)) rel += bl;
    while (*rel == '/' || *rel == 92)  rel++;
    if (!*pattern || glob_path(pattern, rel)) {
      list_push(a, out, v_str(s_lit(a, rel)));
      if (max > 0 && out->len >= max) break;
    }
  }
  if (err) *err = E_NONE;
  return list_of(out);
}
