/* test_util.c - foundation must be right before anything else builds on it. */
#include "vxa.h"
#include "t.h"
#include <stdlib.h>

static Arena A;

static void hexvec(const char *in, const char *want) {
  uint8_t d[32]; char hx[65];
  sha256_buf(in, strlen(in), d);
  base16(d, 32, hx);
  CHECK(!strcmp(hx, want), "sha256(\"%.12s...\")=%s want %s", in, hx, want);
}

int main(void) {
  arena_init(&A, 0);
  T("sha256");
  hexvec("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  hexvec("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  hexvec("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  hexvec("The quick brown fox jumps over the lazy dog",
         "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
  { /* 1e6 'a' - official NIST-style multi-block vector */
    static char big[1000001]; memset(big, 'a', 1000000);
    uint8_t d[32]; char hx[65]; sha256_buf(big, 1000000, d); base16(d, 32, hx);
    CKS(hx, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }
  {
    char big[2000]; memset(big, 'a', sizeof big);
    uint8_t d[32]; sha256_buf(big, 1000, d);
    uint8_t e[32]; sha256_buf(big, 1001, e);
    CK(memcmp(d, e, 32) != 0);
  }
  { /* incremental == one-shot */
    Sha256 s; uint8_t out[32], one[32];
    sha256_init(&s);
    sha256_update(&s, "abc", 1); sha256_update(&s, "abc" + 1, 2);
    sha256_final(&s, out);
    sha256_buf("abc", 3, one);
    CK(!memcmp(out, one, 32));
  }
  T("hash");
  Str h = hash12(&A, "abc", 3);
  CKSTR(h, "ba7816bf8f01");
  CKSTR(hash4(&A, "abc", 3), "ba78");
  CK(fnv1a("abc", 3) != fnv1a("abd", 3));

  T("str");
  Str s = s_lit(&A, "hello world");
  CKI(s.len, 11);
  CK(s_findz(s, "world", 0) == 6);
  CK(s_findz(s, "z", 0) == -1);
  CK(s_eqz(s, "hello world"));
  CK(!s_eqz(s, "hello"));
  CKSTR(s_lower(&A, s), "hello world");
  CKSTR(s_upper(&A, s_lit(&A, "Ab")), "AB");
  CKSTR(s_trim(&A, s_lit(&A, "  x y \n")), "x y");
  CKSTR(s_slice(s, 6, 11), "world");
  CKSTR(s_slice(s, 0, 999), "hello world");
  CKSTR(s_concat(&A, s_lit(&A, "a"), s_lit(&A, "b")), "ab");
  long long iv;
  CK(s_is_int(s_lit(&A, "-42"), &iv) && iv == -42);
  CK(!s_is_int(s_lit(&A, "4.2"), &iv));
  CK(!s_is_int(s_lit(&A, ""), &iv));
  CKSTR(s_replace(&A, s_lit(&A, "aXbXc"), s_lit(&A, "X"), s_lit(&A, "--")), "a--b--c");
  CKI(s_ucount(s_lit(&A, "\xe4\xbd\xa0\xe5\xa5\xbd")), 2);
  CKI(s_ucount(s_lit(&A, "abc")), 3);
  CKSTR(s_char_at(&A, s_lit(&A, "\xe4\xbd\xa0x"), 1), "x");
  {
    Str big = s_lit(&A, "0123456789012345678901234567890123456789");
    Str e = s_ellide(&A, big, 20);
    CK(e.len <= 20 + 1 && s_findz(e, "...", 0) >= 0);
  }
  T("buf");
  Buf b; buf_init(&b, &A);
  buf_puts(&b, "x="); buf_fmt(&b, "%d", 42);
  CKSTR(buf_str(&b), "x=42");
  buf_clear(&b); buf_json_str(&b, s_lit(&A, "a\"b\nc\xff"));
  { Str j = buf_take(&b); CHECK(strncmp(j.p, "\"a\\\"b\\nc", 7) == 0, "json esc=%.*s", j.len, j.p); }
  buf_clear(&b); fmt_num(&b, 3.0); CKSTR(buf_str(&b), "3");
  buf_clear(&b); fmt_num(&b, 3.5); CKSTR(buf_str(&b), "3.5");
  buf_clear(&b); fmt_num(&b, -0.0); CKSTR(buf_str(&b), "0");
  buf_clear(&b); fmt_num(&b, 1e9); CKSTR(buf_str(&b), "1000000000");

  T("glob");
  CK(glob_match("*.c", "main.c"));
  CK(!glob_match("*.c", "main.cpp"));
  CK(glob_match("a?c", "abc"));
  CK(!glob_match("a?c", "ac"));
  CK(glob_match("[ab]x", "ax"));
  CK(!glob_match("[!ab]x", "ax"));
  CK(glob_path("src/**/t.h", "src/a/b/t.h"));
  CK(glob_path("**/*.c", "deep/path/x.c"));
  CK(!glob_path("src/*.c", "src/a/x.c"));

  T("path");
  CKSTR(path_norm(&A, "src//./a/../b.c"), "src/b.c");
  CKSTR(path_norm(&A, "/x/y/../z"), "/x/z");
  CKSTR(path_norm(&A, "."), ".");
  CKSTR(path_join(&A, "a/b", "c"), "a/b/c");
  CKSTR(path_join(&A, "a/b/", "c"), "a/b/c");
  CK(path_is_abs("C:/x"));
  CK(!path_is_abs("C:x"));
  CK(path_within("D:/proj", "D:/proj/src/a.c"));
  CK(!path_within("D:/proj", "D:/other/a.c"));
  CK(!path_within("D:/proj", "D:/projectx/a.c"));
  { char ex[16]; path_ext("a/b/main.c", ex, sizeof ex); CKSTR(s_lit(&A, ex), ".c");
    path_ext("Makefile", ex, sizeof ex); CKSTR(s_lit(&A, ex), ""); }
  CKSTR(s_lit(&A, path_base("a/b/c.h")), "c.h");

  T("arena");
  for (int i = 0; i < 5000; i++) { volatile void *p = arena_alloc(&A, 100); (void)p; }
  CK(A.total >= 5000 * 100);
  CK(A.nalloc >= 5000);

  T("errmeta");
  CKSTR(s_lit(&A, err_name(E_NEED_CONFIRM)), "NEED_CONFIRM");
  CKI(err_exit(E_NEED_CONFIRM), X_CONFIRM);
  CKI(err_exit(E_OUTSIDE_JAIL), X_POLICY);
  CKI(err_exit(E_ANCHOR_MISS), X_IO);
  CK(strlen(err_hint(E_STALE_PLAN)) > 10);
  CKI(tok_est("abcd", 4), 1);
  CKI(tok_est("abcde", 5), 2);

  T_REPORT("util");
}
