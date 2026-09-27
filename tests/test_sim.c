/* test_sim.c - ~= must mean what a reader sees: exact scores, symmetric, in
 * range, stable across the stack/heap size boundary, sane on CJK and stray bytes.
 * Every expected number here was derived by hand or by an independent reference
 * implementation - never read back out of sim.c.
 */
#include "vxa.h"
#include "t.h"
#include <stdlib.h>

int main(void) {
  T("sim_identity");
  CKD(sim_ratio("", 0, "", 0), 1.0);                      /* both empty -> 1 */
  CKD(sim_ratio("abc", 3, "abc", 3), 1.0);
  CKD(sim_ratio("解析器", 9, "解析器", 9), 1.0);
  CKD(sim_ratio("x", 1, "x", 1), 1.0);
  CKD(sim_jaro("", 0, "", 0), 1.0);
  CKD(sim_jaro("kitten", 6, "kitten", 6), 1.0);
  CKD(sim_jaro("a", 1, "a", 1), 1.0);
  CKD(sim_ratio("\xff\xfe", 2, "\xff\xfe", 2), 1.0);      /* stray bytes compare pure */

  T("sim_empty_side");
  CKD(sim_ratio("", 0, "a", 1), 0.0);                    /* one empty, one not -> 0 */
  CKD(sim_ratio("a", 1, "", 0), 0.0);
  CKD(sim_ratio("", 0, "解析", 6), 0.0);
  CKD(sim_jaro("", 0, "a", 1), 0.0);
  CKD(sim_jaro("a", 1, "", 0), 0.0);

  T("sim_ratio_single_edits");
  CKD(sim_ratio("abc", 3, "axc", 3), 2.0 / 3.0);         /* 1 substitution / 3 */
  CKD(sim_ratio("abc", 3, "abd", 3), 2.0 / 3.0);
  CKD(sim_ratio("é", 2, "e", 1), 0.0);                    /* 1 glyph apart, fully */
  CKD(sim_ratio("abc", 3, "azbbc", 5), 3.0 / 5.0);       /* 2 edits / 5 */
  CKD(sim_ratio("abc", 3, "abcd", 4), 3.0 / 4.0);        /* 1 insertion / 4 */
  CKD(sim_ratio("a", 1, "ab", 2), 0.5);
  CKD(sim_ratio("ab", 2, "a", 1), 0.5);
  CKD(sim_ratio("abcd", 4, "dcba", 4), 0.0);             /* 4 edits / 4 */
  CKD(sim_jaro("abcd", 4, "dcba", 4), 0.5);

  T("sim_kitten_sitting");                                /* canonical pair, distance 3 */
  CKD(sim_ratio("kitten", 6, "sitting", 7), 1.0 - 3.0 / 7.0);
  CKD(sim_ratio("kitten", 6, "sitting", 7), 4.0 / 7.0);
  CKD(sim_jaro("kitten", 6, "sitting", 7), 47.0 / 63.0);
  CK(sim_ratio("kitten", 6, "sitting", 7) > 0.5714285714);
  CK(sim_ratio("sitting", 7, "kitten", 6) > 0.5714285714);

  T("sim_transposition");
  CKD(sim_ratio("kinds", 5, "kings", 5), 0.8);            /* d->g = 1 edit / 5 */
  CKD(sim_jaro("kinds", 5, "kings", 5), 13.0 / 15.0);
  CKD(sim_ratio("abcd", 4, "abdc", 4), 0.5);              /* Levenshtein charges 2 */
  CKD(sim_jaro("abcd", 4, "abdc", 4), 11.0 / 12.0);       /* Jaro forgives it */
  CK(sim_jaro("abcd", 4, "abdc", 4) > sim_ratio("abcd", 4, "abdc", 4));

  T("sim_cjk");                                            /* scored per glyph, not per byte */
  CKD(sim_ratio("解析器", 9, "解析", 6), 2.0 / 3.0);       /* 1 glyph / 3 */
  CKD(sim_jaro("解析器", 9, "解析", 6), 8.0 / 9.0);
  CKD(sim_ratio("词法分析", 12, "词法分祈", 12), 0.75);    /* 1 glyph / 4 */
  CKD(sim_jaro("词法分析", 12, "词法分祈", 12), 5.0 / 6.0);
  CKD(sim_ratio("好", 3, "妈", 3), 0.0);                   /* byte-wise would be 0.33 */
  CKD(sim_jaro("好", 3, "妈", 3), 0.0);
  CKD(sim_ratio("你好", 6, "你好世界", 12), 0.5);          /* 2 glyphs / 4 */
  CKD(sim_jaro("你好", 6, "你好世界", 12), 5.0 / 6.0);
  CKD(sim_ratio("café", 5, "cafe", 4), 0.75);              /* 4 glyphs in 5 bytes */
  CKD(sim_jaro("café", 5, "cafe", 4), 5.0 / 6.0);
  CKD(sim_ratio("München", 8, "Munchen", 7), 6.0 / 7.0);
  CKD(sim_ratio("\xf0\x9f\x98\x80" "a", 5, "\xf0\x9f\x98\x80" "b", 5), 0.5);  /* emoji = 1 unit */
  CKD(sim_jaro("\xf0\x9f\x98\x80" "a", 5, "\xf0\x9f\x98\x80" "b", 5), 2.0 / 3.0);

  T("sim_case_is_significant");                            /* caller must lowercase first */
  CKD(sim_ratio("ABC", 3, "abc", 3), 0.0);                /* 3 subs / 3 */
  CKD(sim_jaro("ABC", 3, "abc", 3), 0.0);
  CK(sim_ratio("userId", 6, "UserId", 6) < 1.0);
  CKD(sim_ratio("userId", 6, "UserId", 6), 5.0 / 6.0);    /* high, but not 1 */
  CKD(sim_jaro("userId", 6, "UserId", 6), 8.0 / 9.0);

  T("sim_stray_bytes");                                    /* malformed byte = 1 char */
  CKD(sim_ratio("\xff", 1, "", 0), 0.0);
  CKD(sim_ratio("\xff\xff", 2, "\xff", 1), 0.5);
  CKD(sim_ratio("a\xff", 2, "a", 1), 0.5);
  CKD(sim_ratio("a\x80", 2, "ab", 2), 0.5);
  CKD(sim_jaro("a\x80", 2, "ab", 2), 2.0 / 3.0);
  CKD(sim_ratio("\xc3", 1, "ab", 2), 0.0);                /* truncated lead = 1 char */
  CKD(sim_ratio("\xed\xa0\x80", 3, "\xed\xbf\xbf", 3), 1.0 / 3.0);  /* each stray byte counts */
  CKD(sim_jaro("\xed\xa0\x80", 3, "\xed\xbf\xbf", 3), 5.0 / 9.0);

  T("sim_reference_values");
  CKD(sim_ratio("MARTHA", 6, "MARHTA", 6), 1.0 - 2.0 / 6.0);
  CKD(sim_jaro("MARTHA", 6, "MARHTA", 6), 17.0 / 18.0);   /* published Jaro example */
  CKD(sim_jaro("DWAYNE", 6, "DUANE", 5), 37.0 / 45.0);    /* published Jaro example */
  CKD(sim_jaro("CRATE", 5, "TRACE", 5), 11.0 / 15.0);
  CKD(sim_ratio("alpha", 5, "alpa", 4), 0.8);
  CKD(sim_ratio("John Smith", 10, "John Smyth", 10), 0.9);
  CKD(sim_ratio("user_id", 7, "userid", 6), 6.0 / 7.0);

  T("sim_symmetry");
  {
    static const char *pa[] = { "kitten", "abc", "解析器", "userId", "", "a", "\xff\xfe", "café" };
    static const char *pb[] = { "sitting", "azbbc", "词法分析", "UserId", "z", "ab", "ab", "cafe" };
    for (int i = 0; i < 8; i++) {
      int la = (int)strlen(pa[i]), lb = (int)strlen(pb[i]);
      CKD(sim_ratio(pa[i], la, pb[i], lb), sim_ratio(pb[i], lb, pa[i], la));
      CKD(sim_jaro(pa[i], la, pb[i], lb), sim_jaro(pb[i], lb, pa[i], la));
    }
    CKD(sim_ratio("词法分析", 12, "解析器", 9), sim_ratio("解析器", 9, "词法分析", 12));
    CKD(sim_jaro("词法分析", 12, "解析器", 9), sim_jaro("解析器", 9, "词法分析", 12));
  }

  T("sim_range_and_bounds");
  {
    static const char *pa[] = {
      "alpha", "", "a", "ab", "kitten", "abc", "解析器", "好", "GET /a/b",
      "userId", "München", "x", "\xff", "John Smith", "user_id", "café",
      "abcd", "hello world", "\xf0\x9f\x98\x80", "naïve"
    };
    static const char *pb[] = {
      "alpa", "a", "b", "ba", "sitting", "azbbc", "解析", "妈", "GET /a/c",
      "userid", "Munchen", "xaa", "ab", "John Smyth", "userid", "cafe",
      "abdc", "world", "\xf0\x9f\x98\x81", "street"
    };
    for (int i = 0; i < 20; i++) {
      int la = (int)strlen(pa[i]), lb = (int)strlen(pb[i]);
      double r = sim_ratio(pa[i], la, pb[i], lb);
      double j = sim_jaro(pa[i], la, pb[i], lb);
      CHECK(r >= 0.0 && r <= 1.0 && r == r, "ratio(%s,%s)=%g outside 0..1", pa[i], pb[i], r);
      CHECK(j >= 0.0 && j <= 1.0 && j == j, "jaro(%s,%s)=%g outside 0..1", pa[i], pb[i], j);
      CHECK(fabs(r - sim_ratio(pb[i], lb, pa[i], la)) < 1e-12, "ratio asymmetric: %s/%s", pa[i], pb[i]);
      CHECK(fabs(j - sim_jaro(pb[i], lb, pa[i], la)) < 1e-12, "jaro asymmetric: %s/%s", pa[i], pb[i]);
    }
    CKD(sim_ratio("GET /a/b", 8, "GET /a/c", 8), 7.0 / 8.0);
  }

  T("sim_long_strings_heap_path");        /* > SIM_SMALL chars: rolling row + heap arrays */
  {
    static char big[2048], big2[2048];
    double r, j;
    for (int i = 0; i < 2000; i++) { big[i] = (char)('a' + (i % 26)); big2[i] = big[i]; }
    big[2000] = 0; big2[2000] = 0;
    big2[1999] = 'z'; big2[1500] = 'q';                   /* 2 substitutions */
    r = sim_ratio(big, 2000, big2, 2000);
    j = sim_jaro(big, 2000, big2, 2000);
    CKD(r, 1.0 - 2.0 / 2000.0);
    CHECK(j > 0.999 && j <= 1.0, "jaro(2000,2000)=%g want ~0.9992", j);
    CKD(sim_ratio(big, 2000, big, 2000), 1.0);
    CKD(sim_jaro(big, 2000, big, 2000), 1.0);
    for (int i = 0; i < 2000; i++) big[i] = 'a';
    for (int i = 0; i < 1999; i++) big2[i] = 'a';
    big[2000] = 0; big2[1999] = 0;
    CKD(sim_ratio(big, 2000, big2, 1999), 1.0 - 1.0 / 2000.0);
    CKD(sim_jaro(big, 2000, big2, 1999), (1999.0 / 2000.0 + 2.0) / 3.0);
    {                                                      /* long CJK, 3 bytes per glyph */
      int n1 = 0, n2 = 0;
      for (int i = 0; i < 300; i++) { memcpy(big + n1, "词", 3); n1 += 3; }
      for (int i = 0; i < 299; i++) { memcpy(big2 + n2, "词", 3); n2 += 3; }
      CKD(sim_ratio(big, n1, big2, n2), 299.0 / 300.0);
      CK(sim_jaro(big, n1, big2, n2) > 0.998);
      CKD(sim_ratio(big, n1, big2, n2), sim_ratio(big2, n2, big, n1));
    }
  }

  T("sim_defensive_inputs");
  {
    CKD(sim_ratio(NULL, 0, "a", 1), 0.0);
    CKD(sim_ratio("a", 1, NULL, 0), 0.0);
    CKD(sim_ratio(NULL, 0, NULL, 0), 1.0);
    CKD(sim_jaro(NULL, 0, NULL, 0), 1.0);
    CKD(sim_ratio("ab", -5, "ab", -5), 1.0);               /* negative length = empty */
  }

  T("sim_stack_heap_boundary");      /* row/arrays switch strategy past SIM_SMALL */
  {
    char s1[300], s2[300];
    for (int i = 0; i < 256; i++) { s1[i] = (char)('a' + (i % 26)); s2[i] = s1[i]; }
    s2[255] = 'Z';
    CKD(sim_ratio(s1, 256, s2, 256), 255.0 / 256.0);
    CK(sim_jaro(s1, 256, s2, 256) > 0.99);
    for (int i = 0; i < 257; i++) { s1[i] = (char)('a' + (i % 26)); s2[i] = s1[i]; }
    s2[256] = 'Z';
    CKD(sim_ratio(s1, 257, s2, 257), 256.0 / 257.0);
    CK(sim_jaro(s1, 257, s2, 257) > 0.99);
    CKD(sim_ratio(s1, 257, s1, 257), 1.0);
    CKD(sim_jaro(s2, 257, s1, 257), sim_jaro(s1, 257, s2, 257));
  }

  T("sim_deterministic");
  {
    double a = sim_ratio("kitten", 6, "sitting", 7), b = 0.0;
    for (int i = 0; i < 200; i++) b = sim_ratio("kitten", 6, "sitting", 7);
    CKD(a, b);
    CKD(sim_jaro("解析器", 9, "解析", 6), sim_jaro("解析器", 9, "解析", 6));
    CKD(sim_ratio("词法分析", 12, "词法分祈", 12), 0.75);
  }

  T_REPORT("sim");
}
