#!/bin/sh
# build.sh - VXA build/test driver. Usage:
#   ./build.sh              build build/vxa.exe (static, -O2)
#   ./build.sh test [name]  run one or all module tests (tests/test_<name>.c)
#   ./build.sh syntax FILE  compile a single TU with warnings (no link)
#   ./build.sh clean
set -e
cd "$(dirname "$0")"

CC=${CC:-/c/msys64/mingw64/bin/cc.exe}
[ -x "$CC" ] || CC=cc
# cc.exe is a wrapper that needs its own bin dir on PATH for libgcc/isl DLLs,
# otherwise it exits 1 with no diagnostic.
case "$CC" in
  */*) export PATH="$(dirname "$CC"):$PATH" ;;
esac
mkdir -p build

# suites that carry link-set stubs behind a guard compile against the real modules now
XDEFS() {
  case "$1" in
    lib_fs|lib_sh|lib_tx) echo "-DVX_NO_FS_TEST_STUBS" ;;
    *) echo "" ;;
  esac
}

CFLAGS="-std=c17 ${VXA_OPT:--O2} -g0 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -Isrc -D_GNU_SOURCE"
LIBS="-lm"

have() { [ -f "$1" ]; }

# the language core: every module except the CLI and the fs/tx/sh namespaces
CORE="src/util.c src/val.c src/lex.c src/parse.c src/interp.c src/plan.c src/lib.c src/lib_core.c src/fsx.c src/shell.c src/xdiff.c src/outline.c src/sim.c"

# module -> sources needed to link its test
deps() {
  case "$1" in
    util)          echo src/util.c ;;
    val)           echo src/util.c src/val.c ;;
    xdiff)         echo tests/stubs.c src/util.c src/val.c src/xdiff.c ;;
    sim)           echo tests/stubs.c src/util.c src/val.c src/sim.c ;;
    outline)       echo tests/stubs.c src/util.c src/val.c src/xdiff.c src/outline.c ;;
    sh|shell)      echo src/util.c src/val.c src/shell.c ;;
    lib_fs)        echo $CORE src/lib_fs.c src/lib_tx.c src/lib_sh.c ;;
    lib_tx|lib_sh) echo $CORE src/lib_fs.c src/lib_tx.c src/lib_sh.c ;;
    lex)           echo tests/stubs.c src/util.c src/val.c src/lex.c src/parse.c ;;
    parse)         echo tests/stubs.c src/util.c src/val.c src/lex.c src/parse.c ;;
    interp)        echo $CORE src/lib.c src/lib_core.c src/fsx.c src/shell.c src/xdiff.c src/outline.c src/sim.c src/lib_fs.c src/lib_tx.c src/lib_sh.c ;;
    plan)          echo src/util.c src/val.c src/lex.c src/parse.c src/interp.c src/plan.c src/lib.c src/fsx.c src/shell.c src/xdiff.c src/outline.c src/sim.c ;;
    e2e)           echo ALL ;;
    *)             echo ALL ;;
  esac
}

ALLSRC="src/util.c src/val.c src/lex.c src/parse.c src/interp.c src/plan.c src/lib.c src/lib_core.c src/lib_fs.c src/lib_tx.c src/lib_sh.c src/fsx.c src/shell.c src/xdiff.c src/outline.c src/sim.c src/main.c"

link_srcs() {
  d=$(deps "$1")
  if [ "$d" = ALL ]; then d="$ALLSRC"; fi
  out=""
  for f in $d; do have "$f" && out="$out $f"; done
  echo $out
}

case "${1:-build}" in
  clean) rm -rf build; echo "cleaned" ;;
  syntax) shift; for f in "$@"; do echo "-- $f"; $CC $CFLAGS -c -o build/_syn.o "$f"; done; echo "syntax ok" ;;
  build)
    srcs=""
    for f in $ALLSRC; do have "$f" && srcs="$srcs $f"; done
    $CC $CFLAGS -static -o build/vxa.exe $srcs $LIBS
    sz=$(stat -c %s build/vxa.exe)
    echo "built build/vxa.exe ($sz bytes)"
    ;;
  test)
    name="${2:-}"
    fail=0
    if [ -n "$name" ]; then list="tests/test_$name.c"; else list=$(ls tests/test_*.c 2>/dev/null || true); fi
    for tf in $list; do
      [ -f "$tf" ] || { echo "no test: $tf"; fail=1; continue; }
      n=$(basename "$tf" .c); n=${n#test_}
      srcs=$(link_srcs "$n")
      if ! $CC $CFLAGS $(XDEFS "$n") -o "build/t_$n.exe" "$tf" $srcs $LIBS 2>"build/$n.log"; then
        echo "COMPILE-FAIL $n"; sed -n '1,20p' "build/$n.log"; fail=1; continue
      fi
      if "./build/t_$n.exe" > "build/$n.out" 2>&1; then
        printf "%-10s %s\n" "$n" "$(tail -1 build/$n.out)"
      else
        echo "RUN-FAIL $n"; tail -25 "build/$n.out"; fail=1
      fi
    done
    [ $fail -eq 0 ] && echo "ALL TESTS PASS" || echo "SOME FAILED"
    exit $fail
    ;;
  release)
    # size-first static binary: one file, no runtime deps, no installer
    srcs=""
    for f in $ALLSRC; do have "$f" && srcs="$srcs $f"; done
    $CC -std=c17 -Os -flto -fno-asynchronous-unwind-tables -s -static -Isrc -o build/vxa.exe $srcs $LIBS
    sz=$(stat -c %s build/vxa.exe)
    echo "release build/vxa.exe ($sz bytes, static, lto)"
    ;;
  *) echo "usage: build.sh [build|release|test [name]|syntax FILE|clean]"; exit 1 ;;
esac
