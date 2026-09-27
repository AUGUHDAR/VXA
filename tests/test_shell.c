/* tests/test_shell.c - build.sh keys this module by the name "shell", while the
 * module plan names the suite tests/test_sh.c. This file only forwards, so both
 *   ./build.sh test shell   and   ./build.sh test sh
 * reach the same assertions. Do not put tests here.
 *
 * deps(sh|shell) is the narrow subset (util val shell), so the Ctx and plan_disp
 * the suite runs on come from the local shims inside test_sh.c - the mode they
 * were written for. VXA_NO_CTX_SHIM would swap them for interp.c/plan.c. */
#include "test_sh.c"
