/* sim.c - sim_ratio / sim_jaro: pure, deterministic similarity for ~= (SPEC 1.2)
 * and the candidates side of tx.resolve (SPEC 4.2). No globals, no I/O, no exit.
 *
 * WHY CODE POINTS AND NOT BYTES: the data agents fuzzy-match is full of Chinese
 * keys and values, and "词法分析" vs "词法分祈" is ONE glyph apart to a reader
 * but THREE byte edits to a byte-wise metric (and "好" vs "妈" would score .33
 * instead of 0). Scoring per code point keeps ~= meaningful for CJK; a
 * malformed byte is its own character and is never merged with a neighbour.
 */
#include <stdlib.h>
#include <string.h>
#include "vxa.h"

#define SIM_SMALL 256

static unsigned cp_dec(const char *p, int n, int *adv) {
  unsigned b0 = (unsigned)(unsigned char)p[0], b1, b2, b3;
  *adv = 1;
  if (b0 < 0x80u) return b0;
  if (n < 2) return 0x110000u + b0;
  b1 = (unsigned)(unsigned char)p[1];
  if ((b0 & 0xE0u) == 0xC0u) {
    if (b0 >= 0xC2u && (b1 & 0xC0u) == 0x80u) {
      *adv = 2; return ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
    }
    return 0x110000u + b0;
  }
  if ((b0 & 0xF0u) == 0xE0u) {
    if (n >= 3) {
      b2 = (unsigned)(unsigned char)p[2];
      if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u &&
          (b0 != 0xE0u || b1 >= 0xA0u) && (b0 != 0xEDu || b1 <= 0x9Fu)) {
        *adv = 3; return ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
      }
    }
    return 0x110000u + b0;
  }
  if ((b0 & 0xF8u) == 0xF0u) {
    if (n >= 4) {
      b2 = (unsigned)(unsigned char)p[2]; b3 = (unsigned)(unsigned char)p[3];
      if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u &&
          (b0 != 0xF0u || b1 >= 0x90u) && (b0 != 0xF4u || b1 <= 0x8Fu)) {
        *adv = 4; return ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) |
                         ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
      }
    }
    return 0x110000u + b0;
  }
  return 0x110000u + b0;
}

static int sim_cp_len(const char *p, int n) {
  int i = 0, c = 0;
  if (!p || n <= 0) return 0;
  while (i < n) { int adv; cp_dec(p + i, n - i, &adv); i += adv; c++; }
  return c;
}

static int cp_load(const char *p, int n, unsigned *small, unsigned **out, void **heap) {
  int c, i = 0, adv;
  unsigned *q = small;
  *heap = NULL; *out = small;
  if (!p || n < 0) n = 0;
  c = sim_cp_len(p, n);
  if (c > SIM_SMALL) {
    q = (unsigned *)malloc(sizeof(unsigned) * (size_t)c);
    if (!q) { *out = NULL; return -1; }
    *out = q; *heap = q;
  }
  while (i < n) { *q++ = cp_dec(p + i, n - i, &adv); i += adv; }
  return c;
}

static int lev_dist(const char *a, int na, int ca, const char *b, int nb, int cb) {
  int sb[SIM_SMALL + 1], *row = sb, i, j, res;
  void *heap = NULL;
  int ab = 0;
  if (cb > SIM_SMALL) {
    heap = malloc(sizeof(int) * (size_t)(cb + 1));
    if (!heap) return -1;
    row = (int *)heap;
  }
  for (j = 0; j <= cb; j++) row[j] = j;
  for (i = 1; i <= ca; i++) {
    int adv, prev = i - 1, bb = 0;
    unsigned ac = cp_dec(a + ab, na - ab, &adv);
    ab += adv;
    row[0] = i;
    for (j = 1; j <= cb; j++) {
      unsigned bc = cp_dec(b + bb, nb - bb, &adv);
      int del, ins, sub, d;
      bb += adv;
      del = row[j] + 1;
      ins = row[j - 1] + 1;
      sub = prev + (ac != bc ? 1 : 0);
      prev = row[j];
      d = del;
      if (ins < d) d = ins;
      if (sub < d) d = sub;
      row[j] = d;
    }
  }
  res = row[cb];
  free(heap);
  return res;
}

static double clamp01(double v) {
  if (!(v == v)) return 0.0;
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

double sim_ratio(const char *a, int na, const char *b, int nb) {
  int ca, cb, d;
  if (!a || na < 0) { a = ""; na = 0; }
  if (!b || nb < 0) { b = ""; nb = 0; }
  if (na == nb && memcmp(a, b, (size_t)na) == 0) return 1.0;
  ca = sim_cp_len(a, na);
  cb = sim_cp_len(b, nb);
  if (ca == 0 || cb == 0) return 0.0;
  if (ca < cb) {
    const char *tp = a; int tn = na; int tc = ca;
    a = b; na = nb; ca = cb; b = tp; nb = tn; cb = tc;
  }
  d = lev_dist(a, na, ca, b, nb, cb);
  if (d < 0) return 0.0;
  return clamp01(1.0 - (double)d / (double)ca);
}

static double jaro_core(const unsigned *x, int nx, const unsigned *y, int ny) {
  unsigned char ym_small[SIM_SMALL], *ym = ym_small;
  int yj_small[SIM_SMALL], *yj = yj_small;
  void *hym = NULL, *hyj = NULL;
  int win, i, j, t = 0, h = 0, lo, hi;
  double r;
  if (nx == 0 && ny == 0) return 1.0;
  if (nx == 0 || ny == 0) return 0.0;
  if (ny > SIM_SMALL) {
    hym = malloc((size_t)ny);
    if (!hym) return 0.0;
    ym = (unsigned char *)hym;
  }
  if (nx > SIM_SMALL) {
    hyj = malloc(sizeof(int) * (size_t)nx);
    if (!hyj) { free(hym); return 0.0; }
    yj = (int *)hyj;
  }
  for (i = 0; i < ny; i++) ym[i] = 0;
  win = (ny > nx ? ny : nx) / 2 - 1;
  if (win < 0) win = 0;
  for (i = 0; i < nx; i++) {
    lo = (i - win > 0) ? i - win : 0;
    hi = (i + win < ny - 1) ? i + win : ny - 1;
    yj[i] = -1;
    for (j = lo; j <= hi; j++) {
      if (ym[j]) continue;
      if (x[i] != y[j]) continue;
      ym[j] = 1; yj[i] = j; t++;
      break;
    }
  }
  if (t == 0) { free(hym); free(hyj); return 0.0; }
  {
    int ai = 0, bi = 0;
    while (ai < nx) {
      while (ai < nx && yj[ai] < 0) ai++;
      if (ai >= nx) break;
      while (bi < ny && !ym[bi]) bi++;
      if (bi >= ny) break;
      if (x[ai] != y[bi]) h++;
      ai++; bi++;
    }
  }
  r = ((double)t / (double)nx + (double)t / (double)ny +
       (double)(t - h / 2) / (double)t) / 3.0;
  free(hym); free(hyj);
  return clamp01(r);
}

double sim_jaro(const char *a, int na, const char *b, int nb) {
  unsigned xs[SIM_SMALL], ys[SIM_SMALL];
  unsigned *x = xs, *y = ys;
  void *hx = NULL, *hy = NULL;
  int nx, ny;
  double r;
  nx = cp_load(a, na, xs, &x, &hx);
  if (nx < 0) return 0.0;
  ny = cp_load(b, nb, ys, &y, &hy);
  if (ny < 0) { free(hx); return 0.0; }
  r = jaro_core(x, nx, y, ny);
  free(hx); free(hy);
  return r;
}
