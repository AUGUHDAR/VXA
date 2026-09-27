/* stubs.c - only for unit-test targets that link a subset of the program
 * (language tests do not pull in plan/fs/CLI). Never used by build.sh build. */
#include "vxa.h"
void plan_disp(Plan *p, Buf *b, bool compact) { (void)p; buf_puts(b, "PLAN(-)"); (void)compact; }
