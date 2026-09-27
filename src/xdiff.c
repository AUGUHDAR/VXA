/* xdiff.c - SPEC 4.2: content-hash anchors, anchor patching, and diffs.
 *
 * The one idea this module exists for: an anchor that contains a line number
 * goes stale for every later line as soon as anything upstream is inserted or
 * deleted, so an anchor is a hash over the neighbouring context instead. Path
 * and line index are carried through this API for debug/verification only.
 *
 * Contract note: every entry point here returns arena-owned values, which is
 * why they take Arena* rather than Ctx* (vxa.h's xdiff block says so;
 * anchors_of(), anchor_for(), PatchOp and tx_diff_stats() have always been
 * Arena-first).
 */
#include "vxa.h"
#include <string.h>

#define XD_ANCH_LEN  8                         /* hex chars kept from sha256 */
#define XD_MAX_CELLS ((long long)4000 * 4000)  /* LCS work guard, then approx */
#define XD_HIT_SHOW  100                       /* cap on listed line indices */
#define XD_PREVIEW   60                        /* max chars of a line in a preview */
static const char *const XD_OP_NAMES = "set|del|ins_before|ins_after";

enum { KD_SET = 1, KD_DEL, KD_BEFORE, KD_AFTER };
enum { BRK_NONE = 0, BRK_LF = 1, BRK_CRLF = 2, BRK_CR = 3 };

typedef struct { char *p; int len; unsigned char brk; } Raw;
typedef struct { int kind; Raw *w; int nw; Str deltext; int first; } P2;

static Str raws(const Raw *r) { Str s; s.p = r->p; s.len = r->len; return s; }
static const char *sp(Str s) { return s.p ? s.p : ""; }
static V f_num(V rec, const char *k) {
  V *p = (rec.t == V_REC && rec.u.r) ? rec_getz(rec.u.r, k) : NULL;
  return (p && p->t == V_NUM) ? *p : v_num(0);
}
static V xd_no_arena(void) {
  return v_err_hint(E_INTERNAL, s_wrap("xdiff: no arena"), s_wrap(err_hint(E_INTERNAL)));
}

/* ---------------- line splitting ---------------- */
/* \n, \r\n and a lone \r are each one break. The final line has no break and is
 * returned iff non-empty, so "a\n" is 1 line, "a\n\n" is 2, "" is 0. */
static int split_raw(Arena *a, Str text, Raw **out) {
  int cap = 16, n = 0, i = 0;
  Raw *v;
  if (out) *out = NULL;
  if (!a || !out || !text.p || text.len <= 0) return 0;
  v = (Raw*)arena_alloc(a, (size_t)cap * sizeof(Raw));
  while (i < text.len) {
    int j = i;
    while (j < text.len && text.p[j] != '\n' && text.p[j] != '\r') j++;
    if (n == cap) {
      Raw *nv;
      cap *= 2;
      nv = (Raw*)arena_alloc(a, (size_t)cap * sizeof(Raw));
      memcpy(nv, v, (size_t)n * sizeof(Raw));
      v = nv;
    }
    v[n].p = text.p + i;
    v[n].len = j - i;
    v[n].brk = BRK_NONE;
    if (j < text.len) {
      if (text.p[j] == '\r' && j + 1 < text.len && text.p[j + 1] == '\n') { v[n].brk = BRK_CRLF; j += 2; }
      else { v[n].brk = (text.p[j] == '\r') ? BRK_CR : BRK_LF; j += 1; }
    }
    n++;
    i = j;
  }
  *out = v;
  return n;
}
static bool dominant_crlf(const Raw *L, int n) {
  int term = 0, crlf = 0;
  for (int i = 0; i < n; i++) { if (L[i].brk) term++; if (L[i].brk == BRK_CRLF) crlf++; }
  return term > 0 && crlf * 2 > term;             /* strict majority, else lf */
}
static void brk_put(Buf *b, unsigned char k) {
  if (k == BRK_LF) buf_putc(b, '\n');
  else if (k == BRK_CRLF) buf_puts(b, "\r\n");
  else if (k == BRK_CR) buf_putc(b, '\r');
}
/* Patch text is split like a file; the empty string means exactly one empty
 * line, otherwise "set this line to nothing" could not be expressed. */
static int op_lines(Arena *a, Str t, Raw **out) {
  int n = split_raw(a, t, out);
  if (n == 0) {
    Raw *one = (Raw*)arena_alloc(a, sizeof(Raw));
    one[0].p = (char*)"";
    one[0].len = 0;
    one[0].brk = BRK_NONE;
    *out = one;
    n = 1;
  }
  return n;
}
/* Joining lines for a diff op must survive re-splitting, and a trailing break
 * is not a line, so a run ending in a blank line needs one extra break. */
static Str join_lines(Arena *a, const Raw *L, int from, int cnt) {
  Buf b;
  buf_init(&b, a);
  for (int i = 0; i < cnt; i++) {
    if (i) buf_putc(&b, '\n');
    if (L[from + i].len > 0) buf_put(&b, L[from + i].p, (size_t)L[from + i].len);
  }
  if (cnt > 0 && L[from + cnt - 1].len == 0) buf_putc(&b, '\n');
  return buf_take(&b);
}

/* ---------------- anchors ---------------- */
Str anchor_at(Arena *a, const char *path, int idx, Str prev, Str cur, Str next) {
  /* path and idx are debug-only and deliberately NOT hashed: hashing the line
     number would stale every later anchor after one upstream insert/delete. */
  Sha256 s;
  uint8_t d[32];
  char hx[XD_ANCH_LEN + 1];
  const char nul = '\0';
  (void)path; (void)idx;
  if (!a) return s_null();
  sha256_init(&s);
  sha256_update(&s, "vxa1", 4);
  sha256_update(&s, &nul, 1);
  if (prev.p && prev.len > 0) sha256_update(&s, prev.p, (size_t)prev.len);
  sha256_update(&s, &nul, 1);
  if (cur.p && cur.len > 0) sha256_update(&s, cur.p, (size_t)cur.len);
  sha256_update(&s, &nul, 1);
  if (next.p && next.len > 0) sha256_update(&s, next.p, (size_t)next.len);
  sha256_final(&s, d);
  base16(d, XD_ANCH_LEN / 2, hx);
  return s_from(a, hx, XD_ANCH_LEN);
}

Anch *anchors_of(Arena *a, const char *path, Str text, int *count_out, ErrCode *err) {
  Raw *L = NULL;
  Anch *A;
  int n;
  if (count_out) *count_out = 0;
  if (err) *err = E_NONE;
  if (!a) { if (err) *err = E_BAD_INPUT; return NULL; }
  n = split_raw(a, text, &L);
  if (n <= 0) return NULL;                        /* empty input: count 0, E_NONE */
  A = (Anch*)arena_alloc(a, (size_t)n * sizeof(Anch));
  for (int i = 0; i < n; i++) {
    Str prev = (i > 0) ? raws(&L[i - 1]) : s_null();
    Str next = (i + 1 < n) ? raws(&L[i + 1]) : s_null();
    A[i].n = i;
    A[i].text = s_from(a, L[i].p, (size_t)L[i].len);   /* owned, without the break */
    A[i].at = anchor_at(a, path, i, prev, raws(&L[i]), next);
  }
  if (count_out) *count_out = n;
  return A;
}

/* vxa.h form for callers that already hold a line number (fs.read prints
 * "L<n>:<at>|"). lineno is not hashed either, so this agrees with anchor_at(). */
Str anchor_for(Arena *a, const char *path, int lineno, Str line, Str text) {
  Raw *L = NULL;
  int n = split_raw(a, text, &L);
  if (!a || lineno < 0 || lineno >= n) return s_null();
  return anchor_at(a, path, lineno,
                   lineno > 0 ? raws(&L[lineno - 1]) : s_null(),
                   line.p ? line : raws(&L[lineno]),
                   lineno + 1 < n ? raws(&L[lineno + 1]) : s_null());
}

/* ---------------- anchor lookup ---------------- */
/* An 8-hex-char at must match exactly; a shorter at (1..7 chars) is accepted as
 * a prefix, which is what the ANCHOR_DUP hint "pass n or a longer anchor" means.
 * occ is the 1-based occurrence selector (the op record's n); occ <= 0 means
 * "unspecified", legal only when there is exactly one match. */
static bool at_match(Str cand, Str at) {
  if (!cand.p || !at.p || at.len <= 0 || at.len > cand.len) return false;
  return memcmp(cand.p, at.p, (size_t)at.len) == 0;
}
static Str xd_trim60(Arena *a, Str t) { return s_ellide(a, s_trim(a, t), XD_PREVIEW); }
static void xd_put_cand(Buf *b, Anch *A, int i) {
  Str t = xd_trim60(b->a, A[i].text);
  buf_fmt(b, "L%d: ", i + 1);
  if (t.len) buf_put(b, t.p, (size_t)t.len); else buf_puts(b, "<empty>");
}
/* ANCHOR_MISS candidates: up to 3 anchors whose hash shares a 4-hex-char prefix
 * with the wanted at, else the file's first 3 anchors (an empty file has none). */
static int xd_pick_cands(Arena *a, Anch *A, int n, Str at, int **out) {
  int *v = (int*)arena_alloc(a, 3 * sizeof(int));
  int k = 0;
  *out = v;
  if (A && at.p && at.len >= 4)
    for (int i = 0; i < n && k < 3; i++)
      if (A[i].at.p && A[i].at.len >= 4 && !memcmp(A[i].at.p, at.p, 4)) v[k++] = i;
  if (k) return k;
  for (int i = 0; i < n && k < 3; i++) v[k++] = i;
  return k;
}
static void xd_put_hits(Buf *b, const int *hit, int nh) {
  for (int i = 0; i < nh && i < XD_HIT_SHOW; i++) buf_fmt(b, "%sL%d", i ? "," : "", hit[i] + 1);
  if (nh > XD_HIT_SHOW) buf_fmt(b, ",+%d more", nh - XD_HIT_SHOW);
}
static V xd_miss(Arena *a, Str at, Anch *A, int n, int occ, const int *hit, int nh) {
  Buf b;
  buf_init(&b, a);
  buf_puts(&b, "at=");
  buf_put(&b, at.p, (size_t)(at.len > 0 ? at.len : 0));
  if (nh > 0 && occ > nh) {
    buf_fmt(&b, ": asked for occurrence %d but only %d exist (", occ, nh);
    xd_put_hits(&b, hit, nh);
    buf_puts(&b, ")");
  } else if (n <= 0) {
    buf_puts(&b, ": no anchor matches, the text has no lines");
  } else {
    int *c = NULL, nc = xd_pick_cands(a, A, n, at, &c);
    buf_fmt(&b, ": no anchor matches %d lines; nearest", n);
    if (at.len < XD_ANCH_LEN) buf_puts(&b, " (prefix form)");
    buf_puts(&b, ":");
    for (int i = 0; i < nc; i++) { buf_putc(&b, ' '); xd_put_cand(&b, A, c[i]); }
  }
  return v_err_hint(E_ANCHOR_MISS, buf_take(&b), s_wrap(err_hint(E_ANCHOR_MISS)));
}

V tx_find_anchor(Arena *a, Anch *A, int n, Str at, int occ) {
  int *hit = NULL, nh = 0;
  if (!a) return xd_no_arena();
  if (!at.p || at.len <= 0)
    return v_err(a, E_BAD_INPUT, "at must be a %d-char anchor hash, got %d bytes", XD_ANCH_LEN, at.len);
  if (at.len > XD_ANCH_LEN)
    return v_err(a, E_BAD_INPUT, "at=\"%.*s\" is longer than %d chars; use the hash as printed",
                 at.len > 64 ? 64 : at.len, sp(at), XD_ANCH_LEN);
  /* two passes: size the hit list by the match count, so a big batch of ops on
   * a big file costs matches, not ops*lines, of arena */
  for (int i = 0; i < n; i++) if (A && at_match(A[i].at, at)) nh++;
  if (nh > 0) {
    hit = (int*)arena_alloc(a, (size_t)nh * sizeof(int));
    nh = 0;
    for (int i = 0; i < n; i++) if (at_match(A[i].at, at)) hit[nh++] = i;
  }
  if (nh == 0) return xd_miss(a, at, A, n, occ, hit, 0);
  if (nh > 1 && occ <= 0) {
    Buf b;
    buf_init(&b, a);
    buf_puts(&b, "at=");
    buf_put(&b, at.p, (size_t)at.len);
    buf_fmt(&b, ": matches %d lines (", nh);
    xd_put_hits(&b, hit, nh);
    buf_fmt(&b, "); add n:1..%d to pick one", nh);
    return v_err_hint(E_ANCHOR_DUP, buf_take(&b), s_wrap(err_hint(E_ANCHOR_DUP)));
  }
  if (occ > nh) return xd_miss(a, at, A, n, occ, hit, nh);   /* out-of-range n */
  return v_rec(a, 3, "idx", v_num(occ >= 1 ? hit[occ - 1] : hit[0]),
               "count", v_num(nh), "n", v_num(occ));
}

/* ---------------- patch ---------------- */
typedef struct { char *p; int len; int op; int orig; } Pc;
typedef struct { Pc *v; int len, cap; } Pcs;
static void pcs_push(Arena *a, Pcs *s, char *p, int len, int op, int orig) {
  Pc *q;
  if (s->len == s->cap) {
    int cap = s->cap ? s->cap * 2 : 32;
    Pc *nv = (Pc*)arena_alloc(a, (size_t)cap * sizeof(Pc));
    if (s->len) memcpy(nv, s->v, (size_t)s->len * sizeof(Pc));
    s->v = nv;
    s->cap = cap;
  }
  q = s->v + s->len++;
  q->p = p; q->len = len; q->op = op; q->orig = orig;
}
static void xd_emit_new(Arena *a, Pcs *s, const P2 *p, int op) {
  for (int j = 0; j < p->nw; j++) pcs_push(a, s, p->w[j].p, p->w[j].len, op, -1);
}
static int xd_kind(Str op) {
  if (s_eqz(op, "set")) return KD_SET;
  if (s_eqz(op, "del")) return KD_DEL;
  if (s_eqz(op, "ins_before")) return KD_BEFORE;
  if (s_eqz(op, "ins_after")) return KD_AFTER;
  return -1;
}
static bool xd_writes_line(int k) { return k == KD_SET || k == KD_DEL; }

V tx_patch_apply(Arena *a, Str text, const char *path, V ops) {
  List *L, *ck;
  int nop, nr, na = 0, i, k;
  Raw *RL = NULL;
  Anch *A;
  Str nat;
  bool crlf, orig_nl, null_used = false;
  int null_op = -1;
  PatchOp *P;
  P2 *R;
  char *act;
  int *actop, *hB, *hA, *tB, *tA, *nxt;
  Pcs ps;
  Buf b;
  Rec *out;
  V ckv;

  if (!a) return xd_no_arena();
  if (ops.t != V_LIST || !ops.u.l)
    return v_err(a, E_BAD_INPUT, "ops must be a list of records, got %s", v_typename(ops));
  L = ops.u.l;
  nop = L->len;
  nr = split_raw(a, text, &RL);
  A = anchors_of(a, path, text, &na, NULL);
  if (na != nr) return v_err(a, E_INTERNAL, "xdiff: anchor count %d != line count %d", na, nr);
  nat = anchor_at(a, NULL, 0, s_null(), s_null(), s_null());   /* the empty-file position */
  crlf = dominant_crlf(RL, nr);
  orig_nl = nr > 0 && RL[nr - 1].brk != BRK_NONE;

  P = (PatchOp*)arena_zalloc(a, (size_t)(nop ? nop : 1) * sizeof(PatchOp));
  R = (P2*)arena_zalloc(a, (size_t)(nop ? nop : 1) * sizeof(P2));
  act = (char*)arena_zalloc(a, (size_t)(nr ? nr : 1));
  actop = (int*)arena_alloc(a, (size_t)(nr ? nr : 1) * sizeof(int));
  hB = (int*)arena_alloc(a, (size_t)(nr ? nr : 1) * sizeof(int));
  hA = (int*)arena_alloc(a, (size_t)(nr ? nr : 1) * sizeof(int));
  tB = (int*)arena_alloc(a, (size_t)(nr ? nr : 1) * sizeof(int));
  tA = (int*)arena_alloc(a, (size_t)(nr ? nr : 1) * sizeof(int));
  nxt = (int*)arena_alloc(a, (size_t)(nop ? nop : 1) * sizeof(int));
  for (i = 0; i < nr; i++) { actop[i] = -1; hB[i] = hA[i] = tB[i] = tA[i] = -1; }
  for (k = 0; k < nop; k++) { R[k].first = -1; nxt[k] = -1; }

  /* resolve all anchors against the ORIGINAL text before touching anything: an
   * unresolvable, duplicate or conflicting op aborts the whole batch */
  for (k = 0; k < nop; k++) {
    V ev = L->v[k];
    Rec *r;
    V *av, *ov, *tv, *nv;
    int kd, ix = -1;
    if (ev.t != V_REC || !ev.u.r)
      return v_err(a, E_BAD_INPUT, "op #%d: %s where a record was expected", k + 1, v_typename(ev));
    r = ev.u.r;
    av = rec_getz(r, "at"); ov = rec_getz(r, "op"); tv = rec_getz(r, "text"); nv = rec_getz(r, "n");
    if (!av) return v_err(a, E_BAD_INPUT, "op #%d: missing at; ops are %s", k + 1, XD_OP_NAMES);
    if (av->t != V_STR) return v_err(a, E_BAD_INPUT, "op #%d: at must be a string, got %s", k + 1, v_typename(*av));
    if (!ov) return v_err(a, E_BAD_INPUT, "op #%d: missing op; ops are %s", k + 1, XD_OP_NAMES);
    if (ov->t != V_STR) return v_err(a, E_BAD_INPUT, "op #%d: op must be a string, got %s", k + 1, v_typename(*ov));
    P[k].at = av->u.s;
    P[k].op = ov->u.s;
    if (nv && nv->t != V_NUM)
      return v_err(a, E_BAD_INPUT, "op #%d: n must be a number (1-based occurrence), got %s", k + 1, v_typename(*nv));
    P[k].n = nv ? (int)nv->u.n : 0;
    if (tv) {
      if (tv->t != V_STR) return v_err(a, E_BAD_INPUT, "op #%d: text must be a string, got %s", k + 1, v_typename(*tv));
      P[k].text = tv->u.s;
      P[k].used = true;                       /* vxa.h's PatchOp.used: text was given */
    }
    kd = xd_kind(P[k].op);
    if (kd < 0)
      return v_err(a, E_BAD_INPUT, "op #%d: unknown op \"%.*s\"; ops are %s", k + 1, P[k].op.len, sp(P[k].op), XD_OP_NAMES);
    if (kd != KD_DEL && !P[k].used)
      return v_err(a, E_BAD_INPUT, "op #%d (%.*s): requires a text field", k + 1, P[k].op.len, sp(P[k].op));
    R[k].kind = kd;
    if (kd != KD_DEL) R[k].nw = op_lines(a, P[k].text, &R[k].w);

    if (nr == 0) {
      if (!s_eq(P[k].at, nat)) return tx_find_anchor(a, NULL, 0, P[k].at, P[k].n);   /* ANCHOR_MISS */
      if (kd == KD_DEL) return v_err(a, E_BAD_INPUT, "op #%d (del): the text has no lines to delete", k + 1);
    } else {
      V f = tx_find_anchor(a, A, na, P[k].at, P[k].n);
      V *iv;
      if (f.t == V_ERR) return f;
      iv = (f.t == V_REC && f.u.r) ? rec_getz(f.u.r, "idx") : NULL;
      if (!iv || iv->t != V_NUM) return v_err(a, E_INTERNAL, "op #%d: anchor lookup returned no idx", k + 1);
      ix = (int)iv->u.n;
    }
    R[k].deltext = (ix >= 0) ? A[ix].text : s_null();

    if (ix < 0) {                                  /* the empty text is one position */
      if (null_used)
        return v_err(a, E_BAD_INPUT, "op #%d (%.*s) conflicts with op #%d (%.*s): both write the empty text",
                     k + 1, P[k].op.len, sp(P[k].op), null_op + 1, P[null_op].op.len, sp(P[null_op].op));
      null_used = true;
      null_op = k;
    } else if (xd_writes_line(kd)) {
      if (act[ix]) {
        int j = actop[ix];
        return v_err(a, E_BAD_INPUT,
                     "op #%d (%.*s@%.*s) conflicts with op #%d (%.*s@%.*s): both rewrite line %d; keep one",
                     k + 1, P[k].op.len, sp(P[k].op), P[k].at.len, sp(P[k].at),
                     j + 1, P[j].op.len, sp(P[j].op), P[j].at.len, sp(P[j].at), ix + 1);
      }
      act[ix] = (char)(kd == KD_SET ? 1 : 2);
      actop[ix] = k;
    } else if (kd == KD_BEFORE) {
      if (tB[ix] >= 0) nxt[tB[ix]] = k; else hB[ix] = k;
      tB[ix] = k;
    } else {
      if (tA[ix] >= 0) nxt[tA[ix]] = k; else hA[ix] = k;
      tA[ix] = k;
    }
  }

  /* one ordered pass over the original lines */
  ps.v = NULL; ps.len = 0; ps.cap = 0;
  for (i = 0; i < nr; i++) {
    for (int q = hB[i]; q >= 0; q = nxt[q]) { R[q].first = ps.len; xd_emit_new(a, &ps, &R[q], q); }
    if (act[i] == 1) { int q = actop[i]; R[q].first = ps.len; xd_emit_new(a, &ps, &R[q], q); }
    else if (act[i] == 2) { R[actop[i]].first = ps.len; }
    else pcs_push(a, &ps, RL[i].p, RL[i].len, -1, i);
    for (int q = hA[i]; q >= 0; q = nxt[q]) { R[q].first = ps.len; xd_emit_new(a, &ps, &R[q], q); }
  }
  if (nr == 0 && null_op >= 0) { R[null_op].first = 0; xd_emit_new(a, &ps, &R[null_op], null_op); }

  buf_init(&b, a);
  for (i = 0; i < ps.len; i++) {
    Pc *x = ps.v + i;
    buf_put(&b, x->p, (size_t)(x->len > 0 ? x->len : 0));
    if (i + 1 < ps.len) {
      Pc *y = ps.v + i + 1;
      /* lines that stayed adjacent keep the ending they had in the file */
      if (x->orig >= 0 && y->orig == x->orig + 1 && RL[x->orig].brk != BRK_NONE) brk_put(&b, RL[x->orig].brk);
      else buf_puts(&b, crlf ? "\r\n" : "\n");      /* new lines inherit the dominant eol */
    } else if (orig_nl) {
      buf_puts(&b, crlf ? "\r\n" : "\n");           /* the file's trailing-newline habit */
    }
  }

  /* the self-verification contract: re-read the NEW text instead of trusting the
   * apply bookkeeping; ok=false means "unproven", and is not an error */
  {
    Raw *NL = NULL;
    int nn = split_raw(a, buf_str(&b), &NL);
    ck = list_new(a);
    for (k = 0; k < nop; k++) {
      Rec *c = rec_new(a);
      int line = R[k].first + 1;
      bool ok, have = line >= 1 && line <= nn;
      Str got = have ? raws(&NL[line - 1]) : s_null();
      if (R[k].kind == KD_DEL) ok = !(have && s_eq(got, R[k].deltext));
      else {
        ok = true;
        for (int j = 0; j < R[k].nw; j++) {
          int g = line - 1 + j;
          if (g >= nn || !s_eq(raws(&NL[g]), raws(&R[k].w[j]))) { ok = false; break; }
        }
      }
      rec_setz(a, c, "at", v_str(P[k].at));
      rec_setz(a, c, "ok", v_bool(ok));
      rec_setz(a, c, "line", v_num(line));
      {
        Buf p;
        Str t;
        buf_init(&p, a);
        buf_fmt(&p, "L%d: ", line);
        if (have) {
          t = xd_trim60(a, got);
          if (t.len) buf_put(&p, t.p, (size_t)t.len); else buf_puts(&p, "<empty>");
        } else buf_puts(&p, "<none>");
        rec_setz(a, c, "preview", v_str(buf_take(&p)));
      }
      list_push(a, ck, rec_to_v(c));
    }
  }
  ckv.t = V_LIST; ckv.u.l = ck;

  out = rec_new(a);
  rec_setz(a, out, "text", v_str(buf_take(&b)));
  rec_setz(a, out, "applied", v_num(nop));
  rec_setz(a, out, "checks", ckv);
  rec_setz(a, out, "eol", v_str(s_lit(a, crlf ? "crlf" : "lf")));
  if (path) rec_setz(a, out, "path", v_str(s_lit(a, path)));
  return rec_to_v(out);
}

/* ---------------- diff ---------------- */
typedef struct { char k; int ai, bi; } Ev;   /* ' ' keep, '-' del, '+' add */

#define D_DIAG 0
#define D_UP   1
#define D_LEFT 2
static unsigned char dget(const unsigned char *d, size_t cell) {
  return (unsigned char)((d[cell >> 2] >> ((cell & 3) * 2)) & 3u);
}
static void dset(unsigned char *d, size_t cell, unsigned v) {
  int sh = (int)((cell & 3) * 2);
  d[cell >> 2] = (unsigned char)((d[cell >> 2] & ~(3u << sh)) | ((v & 3u) << sh));
}

/* Real LCS over whole lines: two int rows plus 2 direction bits per cell, so a
 * 4000x4000 middle costs ~4MB and still backtracks an exact script. Common
 * prefix/suffix lines are trimmed first, so the DP only sees changed middle.
 * Rows are the longer side to keep the row arrays small; the tie-break prefers
 * deletions before additions, which is what makes the emitted ops stable. */
static int lcs_middle(Arena *a, const Raw *Am, int nx, const Raw *Bm, int ny, char *mid) {
  const Raw *X, *Y;
  int nX, nY, i, j, w = nx + ny;
  bool swapped;
  int *prev, *cur;
  unsigned char *dir;
  if (nx < ny) { X = Bm; nX = ny; Y = Am; nY = nx; swapped = true; }
  else { X = Am; nX = nx; Y = Bm; nY = ny; swapped = false; }
  prev = (int*)arena_alloc(a, (size_t)(nY + 1) * sizeof(int));
  cur = (int*)arena_alloc(a, (size_t)(nY + 1) * sizeof(int));
  dir = (unsigned char*)arena_zalloc(a, ((size_t)nX * (size_t)nY) / 4 + 8);
  memset(prev, 0, (size_t)(nY + 1) * sizeof(int));
  for (i = 1; i <= nX; i++) {
    Str xs = raws(&X[i - 1]);
    cur[0] = 0;
    for (j = 1; j <= nY; j++) {
      size_t cell = (size_t)(i - 1) * (size_t)nY + (size_t)(j - 1);
      if (s_eq(xs, raws(&Y[j - 1]))) { cur[j] = prev[j - 1] + 1; dset(dir, cell, D_DIAG); }
      else if (prev[j] >= cur[j - 1]) { cur[j] = prev[j]; dset(dir, cell, D_UP); }
      else { cur[j] = cur[j - 1]; dset(dir, cell, D_LEFT); }
    }
    { int *t = prev; prev = cur; cur = t; }
  }
  i = nX; j = nY;
  while (i > 0 && j > 0) {
    unsigned char d = dget(dir, (size_t)(i - 1) * (size_t)nY + (size_t)(j - 1));
    if (d == D_DIAG) { mid[--w] = ' '; i--; j--; }
    else if (d == D_UP) { mid[--w] = swapped ? '+' : '-'; i--; }
    else { mid[--w] = swapped ? '-' : '+'; j--; }
  }
  while (i > 0) { mid[--w] = swapped ? '+' : '-'; i--; }
  while (j > 0) { mid[--w] = swapped ? '-' : '+'; j--; }
  /* one step per consumed row/col, so the match lines leave w unused slots at
   * the front: slide the script down before reporting its length */
  if (w > 0) memmove(mid, mid + w, (size_t)(nx + ny - w));
  return nx + ny - w;
}

/* ai/bi are re-derived by walking the script, so they are trustworthy for every
 * event kind: for a '+' the ai is how many a-lines precede the insertion. */
static void fixup(Ev *E, int ne) {
  int ai = 0, bi = 0;
  for (int i = 0; i < ne; i++) {
    if (E[i].k == ' ') { E[i].ai = ai++; E[i].bi = bi++; }
    else if (E[i].k == '-') { E[i].ai = ai++; E[i].bi = bi; }
    else { E[i].ai = ai; E[i].bi = bi++; }
  }
}
static Ev *script_of(Arena *a, const Raw *A, int na, const Raw *B, int nb, int *ne_out, bool *approx_out) {
  int p = 0, s = 0, nx, ny, n = 0, i, got = 0;
  Ev *E;
  *approx_out = false;
  while (p < na && p < nb && s_eq(raws(&A[p]), raws(&B[p]))) p++;
  while (s < na - p && s < nb - p && s_eq(raws(&A[na - 1 - s]), raws(&B[nb - 1 - s]))) s++;
  nx = na - p - s;
  ny = nb - p - s;
  E = (Ev*)arena_alloc(a, (size_t)(p + nx + ny + s + 1) * sizeof(Ev));
  for (i = 0; i < p; i++) { E[n].k = ' '; n++; }
  if (nx + ny > 0) {
    if ((long long)nx * (long long)ny > XD_MAX_CELLS) {
      *approx_out = true;                        /* bounded-work guard */
      for (i = 0; i < nx; i++) { E[n].k = '-'; n++; }
      for (i = 0; i < ny; i++) { E[n].k = '+'; n++; }
    } else {
      char *mid = (char*)arena_alloc(a, (size_t)(nx + ny));
      got = lcs_middle(a, A + p, nx, B + p, ny, mid);
      for (i = 0; i < got; i++) { E[n].k = mid[i]; n++; }
    }
  }
  for (i = 0; i < s; i++) { E[n].k = ' '; n++; }
  fixup(E, n);
  *ne_out = n;
  return E;
}

/* Hunk bounds: change runs expanded by ctxl keep-lines, then merged when the
 * gap between them is not bigger than the context they would both claim. */
static int hunks_of(Arena *a, const Ev *E, int ne, int ctxl, int **hs, int **he) {
  int cap = ne / 2 + 2, *rs, *re, nr = 0, nh, i = 0, *H1, *H2;
  rs = (int*)arena_alloc(a, (size_t)cap * sizeof(int));
  re = (int*)arena_alloc(a, (size_t)cap * sizeof(int));
  H1 = (int*)arena_alloc(a, (size_t)cap * sizeof(int));
  H2 = (int*)arena_alloc(a, (size_t)cap * sizeof(int));
  *hs = H1; *he = H2;
  while (i < ne) {
    if (E[i].k == ' ') { i++; continue; }
    rs[nr] = i;
    while (i < ne && E[i].k != ' ') i++;
    re[nr++] = i;
  }
  if (nr == 0) return 0;
  H1[0] = rs[0] - ctxl > 0 ? rs[0] - ctxl : 0;
  H2[0] = re[0] + ctxl < ne ? re[0] + ctxl : ne;
  nh = 1;
  for (i = 1; i < nr; i++) {
    int st = rs[i] - ctxl > 0 ? rs[i] - ctxl : 0;
    int en = re[i] + ctxl < ne ? re[i] + ctxl : ne;
    if (st <= H2[nh - 1]) { if (en > H2[nh - 1]) H2[nh - 1] = en; continue; }
    H1[nh] = st;
    H2[nh] = en;
    nh++;
  }
  return nh;
}
static void put_line(Buf *b, char pfx, const Raw *L) {
  buf_putc(b, pfx);
  if (L->len > 0) buf_put(b, L->p, (size_t)L->len);
  buf_putc(b, '\n');
}
static Str render_diff(Arena *a, const Ev *E, int ne, const Raw *A, int na,
                       const Raw *B, int nb, int ctxl, int *hs, int *he, int nh) {
  Buf b;
  int h;
  buf_init(&b, a);
  if (nh == 0) return s_lit(a, "");
  buf_puts(&b, "--- a\n+++ b\n");
  for (h = 0; h < nh; h++) {
    int st = hs[h], en = he[h], ac = 0, bc = 0, a0 = -1, b0 = -1;
    for (int i = st; i < en; i++) {
      if (E[i].k != '+') { ac++; if (a0 < 0) a0 = E[i].ai + 1; }
      if (E[i].k != '-') { bc++; if (b0 < 0) b0 = E[i].bi + 1; }
    }
    if (a0 < 0) a0 = (en > st) ? E[st].ai : na;   /* nothing removed: line it follows */
    if (b0 < 0) b0 = (en > st) ? E[st].bi : nb;
    buf_fmt(&b, "@@ -%d,%d +%d,%d @@\n", a0, ac, b0, bc);
    for (int i = st; i < en; i++) {
      if (E[i].k == ' ') put_line(&b, ' ', &A[E[i].ai]);
      else if (E[i].k == '-') put_line(&b, '-', &A[E[i].ai]);
      else put_line(&b, '+', &B[E[i].bi]);
    }
  }
  return buf_take(&b);
}
/* how many lines share this anchor, and which occurrence idx is: an op only
 * carries n when the anchor is ambiguous, so patch never sees a false DUP */
static int occ_of(Anch *A, int n, int idx, int *cnt_out) {
  int cnt = 0, pos = 0;
  for (int i = 0; i < n; i++) if (s_eq(A[i].at, A[idx].at)) { if (i == idx) pos = cnt; cnt++; }
  *cnt_out = cnt;
  return cnt > 1 ? pos + 1 : 0;
}
static void push_op(Arena *a, List *out, Anch *A, int na, int idx, Str at,
                    const char *op, bool has_text, Str text) {
  Rec *r = rec_new(a);
  int cnt = 0, occ;
  if (idx >= 0) occ = occ_of(A, na, idx, &cnt);
  else { cnt = 1; occ = 0; }
  rec_setz(a, r, "at", v_str(at));
  rec_setz(a, r, "op", v_str(s_lit(a, op)));
  if (has_text) rec_setz(a, r, "text", v_str(text));
  if (occ > 0) rec_setz(a, r, "n", v_num(occ));
  (void)cnt;
  list_push(a, out, rec_to_v(r));
}
/* The diff as patch ops: one op group becomes a set on its first removed line
 * (carrying every added line) plus a del per remaining removed line, so the ops
 * stay anchor-addressed and tx_patch_apply can replay them in one pass. */
static V ops_list(Arena *a, Str ta, const Ev *E, int ne, const Raw *B, int nb) {
  List *out = list_new(a);
  ErrCode e = E_NONE;
  int na = 0, i = 0;
  Anch *A = anchors_of(a, NULL, ta, &na, &e);
  Str nat = anchor_at(a, NULL, 0, s_null(), s_null(), s_null());
  (void)nb;
  while (i < ne) {
    int r0, ndel = 0, nadd = 0, d0 = -1, b0 = -1, pos, j;
    if (E[i].k == ' ') { i++; continue; }
    r0 = i;
    while (i < ne && E[i].k != ' ') i++;
    for (j = r0; j < i; j++) {
      if (E[j].k == '-') { if (ndel == 0) d0 = E[j].ai; ndel++; }
      else if (E[j].k == '+') { if (nadd == 0) b0 = E[j].bi; nadd++; }
    }
    pos = E[r0].ai;                                /* a-line index or insertion point */
    if (ndel > 0) {
      if (nadd > 0) {
        push_op(a, out, A, na, d0, A[d0].at, "set", true, join_lines(a, B, b0, nadd));
        for (j = 1; j < ndel; j++) push_op(a, out, A, na, d0 + j, A[d0 + j].at, "del", false, s_null());
      } else {
        for (j = 0; j < ndel; j++) push_op(a, out, A, na, d0 + j, A[d0 + j].at, "del", false, s_null());
      }
    } else if (na == 0) {
      push_op(a, out, A, 0, -1, nat, "ins_before", true, join_lines(a, B, b0, nadd));
    } else if (pos > 0) {
      push_op(a, out, A, na, pos - 1, A[pos - 1].at, "ins_after", true, join_lines(a, B, b0, nadd));
    } else {
      push_op(a, out, A, na, 0, A[0].at, "ins_before", true, join_lines(a, B, b0, nadd));
    }
  }
  { V v; v.t = V_LIST; v.u.l = out; return v; }
}

V tx_diff(Arena *a, Str ta, Str tb, int ctxl, bool unified) {
  Raw *A = NULL, *B = NULL;
  int na, nb, ne = 0, adds = 0, dels = 0, *hs = NULL, *he = NULL, nh;
  bool approx = false;
  Ev *E;
  Rec *r;
  if (!a) return xd_no_arena();
  if (ctxl <= 0) ctxl = 3;
  na = split_raw(a, ta, &A);
  nb = split_raw(a, tb, &B);
  E = script_of(a, A, na, B, nb, &ne, &approx);
  for (int i = 0; i < ne; i++) { if (E[i].k == '+') adds++; else if (E[i].k == '-') dels++; }
  nh = hunks_of(a, E, ne, ctxl, &hs, &he);
  r = rec_new(a);
  if (unified) rec_setz(a, r, "text", v_str(render_diff(a, E, ne, A, na, B, nb, ctxl, hs, he, nh)));
  else rec_setz(a, r, "ops", ops_list(a, ta, E, ne, B, nb));
  rec_setz(a, r, "adds", v_num(adds));
  rec_setz(a, r, "dels", v_num(dels));
  rec_setz(a, r, "hunks", v_num(nh));
  if (approx) rec_setz(a, r, "approx", v_bool(true));
  return rec_to_v(r);
}

/* vxa.h helper: exact counts without an Arena parameter, so it runs on its own
 * bounded scratch arena and keeps only ints. Returns 0, or -1 on bad arguments. */
int tx_diff_stats(const char *pa, size_t na, const char *pb, size_t nb, int *adds, int *dels, int *hunks) {
  Arena t;
  V r, x;
  Str a, b;
  if (!adds || !dels || !hunks) return -1;
  if (na > (size_t)INT32_MAX || nb > (size_t)INT32_MAX) return -1;
  arena_init(&t, 0);
  a.p = (char*)pa; a.len = (int)na;
  b.p = (char*)pb; b.len = (int)nb;
  r = tx_diff(&t, a, b, 3, false);
  if (r.t != V_REC || !r.u.r) { arena_free(&t); return -1; }
  x = f_num(r, "adds"); *adds = (int)x.u.n;
  x = f_num(r, "dels"); *dels = (int)x.u.n;
  x = f_num(r, "hunks"); *hunks = (int)x.u.n;
  arena_free(&t);
  return 0;
}
