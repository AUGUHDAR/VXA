/* t.h - zero-dependency test harness used by every tests/test_*.c */
#ifndef VXA_T_H
#define VXA_T_H
#include <stdio.h>
#include <string.h>
#include <math.h>

static int t_pass = 0, t_fail = 0;
static const char *t_cur = "";
#ifdef VXA_TRACE
#define T(name) do { t_cur = (name); fprintf(stderr, "TRACE "); fprintf(stderr, "%s", (name)); fprintf(stderr, "\n"); fflush(stderr); } while (0)
#else
#define T(name) (t_cur = (name))
#endif
#define CHECK(cond, ...) do { \
  if (cond) t_pass++; \
  else { t_fail++; printf("FAIL[%s:%d] ", t_cur, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define CK(cond)          CHECK(cond, "expected true: %s", #cond)
#define CKI(a, b)         CHECK((long long)(a) == (long long)(b), "%s=%lld want %lld", #a, (long long)(a), (long long)(b))
#define CKD(a, b)         CHECK(fabs((double)(a) - (double)(b)) < 1e-9, "%s=%.6f want %.6f", #a, (double)(a), (double)(b))
#define CKS(a, b)         do { const char *_x = (a); const char *_y = (b); \
  CHECK(_x && _y && !strcmp(_x, _y), "%s=\"%s\" want \"%s\"", #a, _x ? _x : "(null)", _y ? _y : "(null)"); } while (0)
#define CKSTR(s, b)       do { Str _t = (s); const char *_y = (b); \
  CHECK(_t.p && _t.len == (int)strlen(_y) && !memcmp(_t.p, _y, (size_t)_t.len), \
    "%s=\"%.*s\" want \"%s\"", #s, _t.len, _t.p ? _t.p : "", _y); } while (0)
#define T_REPORT(name)    do { printf("%-10s pass=%d fail=%d\n", name, t_pass, t_fail); \
  return t_fail ? 1 : 0; } while (0)
#endif
