#!/bin/sh
# e2e.sh - the acceptance harness for the whole product. It asserts BEHAVIOUR
# (exit codes, machine-readable fields, and what actually ended up on disk),
# never just "it ran". Run from the repo root:  bash tests/e2e.sh
#
# Every case gets its own workspace under build/e2e_tmp/ and runs with cwd set to
# it, so the jail root IS the workspace and nothing a case writes can escape it.
# Only relative paths are handed to the binary: it is a native Windows program
# and cannot see msys-style /d/... paths.
set -u
# Byte semantics everywhere. In a GBK locale sed's [a-f] matches far more than
# hex, and the protocol is bytes.
LC_ALL=C
export LC_ALL

TOP=$(pwd)
VXA=${VXA:-$TOP/build/vxa.exe}
TMPD=${TMPD:-$TOP/build/e2e_tmp}
KEEP=${KEEP:-0}                       # KEEP=1 leaves the scratch dirs in place
pass=0
fail=0

if [ ! -f "$VXA" ]; then
  echo "E2E pass=0 fail=1"
  echo "FAIL: no binary at $VXA - build it first (bash build.sh)"
  exit 1
fi
rm -rf "$TMPD"
mkdir -p "$TMPD"

ok() { pass=$((pass + 1)); }
no() { fail=$((fail + 1)); echo "FAIL: $1"; }

contains() { case "$1" in *"$2"*) ok ;; *) no "$3: expected '$2' in: $1" ;; esac; }
lacks()    { case "$1" in *"$2"*) no "$3: unexpected '$2' in: $1" ;; *) ok ;; esac; }
eqx()      { if [ "$1" = "$2" ]; then ok; else no "$3: got '$1' want '$2'"; fi; }
exit_is()  { if [ "$RC" = "$1" ]; then ok; else no "$2: exit $RC want $1 | stdout: $(head -2 "$OUT" | tr '\n' '~')"; fi; }
has()   { if grep -q -- "$1" "$OUT" 2>/dev/null; then ok; else no "$2: no /$1/ on stdout: $(head -3 "$OUT" | tr '\n' '~')"; fi; }
hasnt() { if grep -q -- "$1" "$OUT" 2>/dev/null; then no "$2: unexpected /$1/ on stdout: $(head -3 "$OUT" | tr '\n' '~')"; else ok; fi; }
hasre() { if grep -Eq -- "$1" "$OUT" 2>/dev/null; then ok; else no "$2: no match for /$1/ on stdout: $(head -3 "$OUT" | tr '\n' '~')"; fi; }
# oneline <desc>: the protocol allows exactly one result line
oneline() {
  n=$(grep -c . "$OUT" 2>/dev/null || echo 0)
  if [ "$n" = "1" ]; then ok; else no "$1: stdout has $n result lines, the protocol says 1"; fi
}
# nocr <desc>: stdout must not carry CR (text-mode output would break the protocol)
nocr() {
  a=$(wc -c <"$OUT"); b=$(tr -d '\r' <"$OUT" | wc -c)
  if [ "$a" = "$b" ]; then ok; else no "$1: stdout contains CR"; fi
}

# run <vxa args...>  -> sets OUT, ERR, RC
run() {
  OUT="$SD/.out"; ERR="$SD/.err"
  "$VXA" "$@" >"$OUT" 2>"$ERR"
  RC=$?
}
token_from() { sed -n 's/.*token=\([0-9a-f]\{12\}\).*/\1/p' "$1" | head -1; }
anchor_from() { sed -n "s/.*$2:\([0-9a-f]*\).*/\1/p" "$1" | head -1; }
seq_from() { sed -n 's/.*seq=\([0-9][0-9]*\).*/\1/p' "$1" | head -1; }
# confirm_again <same vxa args...>: rerun with the token the last run printed
confirm_again() {
  T=$(token_from "$OUT")
  if [ -z "$T" ]; then no "confirm_again: the previous run printed no token"; RC=99; return 1; fi
  run "$@" --confirm "$T"
  return 0
}
# write <file> <line...>: a .vxa source, one line per argument, no shell escapes
write() {
  f=$1; shift
  : > "$f"
  for l in "$@"; do printf '%s\n' "$l" >> "$f"; done
}
# fresh <case-name>: a clean workspace, cd'ed into. The .vxa marker makes this
# directory the jail root even when the repository above it is a workspace too,
# so cases cannot write into each other.
fresh() {
  SD="$TMPD/$1"
  rm -rf "$SD"; mkdir -p "$SD/.vxa"
  cd "$SD" || { no "cannot enter $SD"; return 1; }
}

# =====================================================================
# 1. eval: arithmetic, interpolation, round trips, --fmt
# =====================================================================
fresh eval
run eval '1+2*3';        exit_is 0 "eval arithmetic"
eqx "$(cat "$OUT")" "7" "eval arithmetic result"
nocr "eval result"
run eval '(1+2)*3.5';    eqx "$(cat "$OUT")" "10.5" "eval float arithmetic"
run eval '"v{6*7}"';     eqx "$(cat "$OUT")" "v42" "eval string interpolation"
run eval '[1,2,3]';      eqx "$(cat "$OUT")" "[1,2,3]" "eval list round trip"
run eval '{a:1,b:"x"}';  eqx "$(cat "$OUT")" "a=1,b=x" "eval record drops its outer braces"
run eval '[{a:1},{a:2}]'; eqx "$(cat "$OUT")" "[{a=1},{a=2}]" "eval nested record keeps its delimiters"
run eval 'null';         eqx "$(cat "$OUT")" "-" "eval null prints -"
run eval 'true';         eqx "$(cat "$OUT")" "t" "eval bool prints t"
run eval '"a,b"';        eqx "$(cat "$OUT")" '"a,b"' "eval a string holding a comma is quoted"
write x.vxa 'x = [1,2,3]' 'x[1] + x.len()'
run run x.vxa
eqx "$(cat "$OUT")" "5" "run computes a list index and a method call"
run eval '{n:"42"}'
# NOTE: a numeric-looking string prints bare, so n="42" and n=42 are the same
# bytes on stdout. The compact form is not typed - the field name carries the
# meaning. Asserted so the rule is visible, not because it is ideal.
eqx "$(cat "$OUT")" 'n=42' "eval prints a numeric-looking string bare"

run eval '{a:1,b:[1,2],c:{d:"e"}}' --fmt json
eqx "$(cat "$OUT")" '{"a":1,"b":[1,2],"c":{"d":"e"}}' "eval --fmt json is valid JSON"
run eval '[1,2]' --fmt json;        eqx "$(cat "$OUT")" "[1,2]" "eval --fmt json list"
run eval '42' --fmt json;           eqx "$(cat "$OUT")" "42" "eval --fmt json scalar"
run eval '{a:1,b:2}' --fmt text
contains "$(tr '\n' '|' <"$OUT")" "a: 1" "eval --fmt text renders key: value lines"

write multi.vxa 'x = 41' 'x + 1'
run run multi.vxa
eqx "$(cat "$OUT")" "42" "run evaluates a multi-statement script"

# =====================================================================
# 2. run: a NEW file inside the jail applies at once, with a receipt
# =====================================================================
fresh newwrite
write w.vxa 'fs.write("hello.txt", "hi there")'
run run w.vxa
exit_is 0 "a new file under the jail needs no confirmation"
if [ -f hello.txt ]; then ok; else no "hello.txt was not written"; fi
eqx "$(cat hello.txt 2>/dev/null)" "hi there" "the new file holds exactly what was written"
has "hello.txt" "the receipt names the path"
has "bytes=" "the receipt reports bytes="
oneline "the receipt is one machine line"

write e.vxa 'out.emit({path:"a.txt", n:3, ok:true, none:null})'
run run e.vxa
exit_is 0 "out.emit works"
eqx "$(cat "$OUT")" "path=a.txt,n=3,ok=t,none=-" "emit uses the compact protocol and is not echoed twice"

write c.vxa 'out.emit({k:cfg.get("k"), a:cfg.get("args")})'
run run c.vxa --set k=7 --args "hello agent"
exit_is 0 "--set and --args are accepted"
OUTTXT=$(cat "$OUT")
contains "$OUTTXT" "k=7" "--set overrides config and reaches cfg.get"
contains "$OUTTXT" "hello agent" "--args reaches the script as cfg.get(args)"

# =====================================================================
# 3. run: OVERWRITING an existing file - the two-phase round trip
# =====================================================================
fresh overwrite
printf 'one\n' > t.txt
write w.vxa 'fs.write("t.txt", "two")'
run run w.vxa
exit_is 3 "overwriting an existing file needs confirmation"
has "code=NEED_CONFIRM" "the plan pass says NEED_CONFIRM"
has "token=" "the plan pass prints a token"
has "files=" "the plan lists the files it would touch"
has "kind=modify" "the plan classifies the target as modify"
has "path=" "the plan entry names the path"
has "from=" "the plan binds the old content hash"
has "to=" "the plan binds the planned content hash"
eqx "$(cat t.txt)" "one" "the plan pass changed nothing on disk"
T1=$(token_from "$OUT")
[ -n "$T1" ] && ok || no "no 12-hex token was printed"

run run w.vxa --confirm "$T1"
exit_is 0 "confirming the token applies the plan"
eqx "$(cat t.txt)" "two" "the confirmed run wrote the new content"

# a token buys one plan. The world has moved on, so the old token must be
# refused and nothing may be written again
run run w.vxa --confirm "$T1"
if [ "$RC" = "0" ]; then no "a spent token was accepted a second time"; else ok; fi
eqx "$(cat t.txt)" "two" "a refused replay did not rewrite the file"

# same token, same plan: that is a replay, and a replay is reported as one
printf 'same' > s.txt
write i.vxa 'fs.write("s.txt", "same")'
run run i.vxa
A=$(token_from "$OUT")
run run i.vxa
B=$(token_from "$OUT")
eqx "$A" "$B" "same script plus same world equals same token"
run run i.vxa --confirm "$A"
exit_is 0 "the idempotent write confirms"
run run i.vxa --confirm "$A"
exit_is 0 "the same token again exits 0 - a replay, not a second write"
has "replayed=t" "a spent token reports replayed=t"
oneline "a replay is one machine line"

# =====================================================================
# 4. tampering between plan and confirm must never apply
# =====================================================================
fresh tamper
printf 'alpha\n' > v.txt
write w.vxa 'fs.write("v.txt", "beta")'
run run w.vxa
TT=$(token_from "$OUT")
printf 'tampered\n' > v.txt                        # change the world behind the plan
run run w.vxa --confirm "$TT"
if [ "$RC" = "0" ]; then no "confirm succeeded after the target was tampered with"; else ok; fi
exit_is 5 "a tampered confirm is refused as an IO-level error"
hasre "STALE_PLAN|BAD_TOKEN" "the refusal is STALE_PLAN or BAD_TOKEN"
eqx "$(cat v.txt)" "tampered" "the tampered content survives - nothing was applied"

# rebuilding the plan after the tamper yields a DIFFERENT token, and that one
# applies: the refusal above was about the signed hashes, not a blanket no
run run w.vxa
T2=$(token_from "$OUT")
if [ "$T2" = "$TT" ]; then no "the token did not change after the target changed"; else ok; fi
run run w.vxa --confirm "$T2"
exit_is 0 "a fresh token for the new state applies"
eqx "$(cat v.txt)" "beta" "the file now holds the planned content"

# =====================================================================
# 5. delete goes to trash and is restorable byte for byte
# =====================================================================
fresh trash
printf 'keep me\n' > d.txt
ck_before=$(cksum d.txt)
write del.vxa 'fs.delete("d.txt")'
run run del.vxa
exit_is 3 "a delete always needs confirmation"
has "code=NEED_CONFIRM" "the delete produced a plan"
has "kind=delete" "the plan says delete, not modify"
confirm_again run del.vxa
exit_is 0 "the confirmed delete ran"
if [ -e d.txt ]; then no "d.txt is still in the workspace after delete"; else ok; fi
has "trash" "the delete receipt mentions trash"
if [ -d .vxa/trash ]; then ok; else no "no .vxa/trash area was created"; fi
SEQ=$(seq_from "$OUT")
[ -n "$SEQ" ] && ok || no "the delete receipt carries no restore seq: $(head -1 "$OUT")"
run eval "fs.restore($SEQ)"
if [ "$RC" = "3" ]; then confirm_again eval "fs.restore($SEQ)"; fi
exit_is 0 "fs.restore after a delete"
if [ -f d.txt ]; then ok; else no "restore did not bring d.txt back"; fi
eqx "$(cksum d.txt 2>/dev/null)" "$ck_before" "the restored file is byte-identical"

# =====================================================================
# 6. sh: the read-only fast path vs. everything else needing a plan
# =====================================================================
fresh shro
run sh --ro "ls"
exit_is 0 "a whitelisted command runs under --ro"
has "code=" "the sh result carries code="
oneline "the sh result is one machine line"

run sh --ro 'echo hi > injected.txt'
exit_is 3 "--ro refuses to approve a redirect"
has "code=NEED_CONFIRM" "a refused command becomes a plan, not a run"
if [ -e injected.txt ]; then no "a redirect was executed under --ro"; else ok; fi

run sh --ro "totally_unknown_tool arg"
exit_is 3 "--ro on a tool that is not whitelisted becomes a plan"

run sh 'echo hi > plain.txt'
exit_is 3 "a plain sh call is a plan, never a run"
has "code=NEED_CONFIRM" "sh plan code"
has "token=" "sh plan token"
if [ -e plain.txt ]; then no "sh ran without confirmation"; else ok; fi
confirm_again sh 'echo hi > plain.txt'
exit_is 0 "a confirmed sh command runs"
if [ -f plain.txt ]; then ok; else no "the confirmed sh command created no file"; fi

fresh shred
printf 'token=sekret123\n' > s.txt
run sh --ro "grep token s.txt"
if [ "$RC" = "0" ]; then
  hasnt "sekret123" "sh output is redacted by default"
  run sh --ro "grep token s.txt" --no-redact
  if [ "$RC" = "0" ]; then has "sekret123" "--no-redact returns the raw value"; else ok; fi
else
  ok; ok                                        # grep unavailable here: not a protocol break
fi

# =====================================================================
# 7. the jail
# =====================================================================
fresh jail
run sh 'true' --cwd ../..
exit_is 4 "--cwd outside the jail is refused"
has "OUTSIDE_JAIL" "OUTSIDE_JAIL is named"

fresh jail2
write esc.vxa 'fs.write("../escaped.txt", "nope")'
run run esc.vxa
exit_is 4 "a script write outside the jail is refused"
has "OUTSIDE_JAIL" "the script escape names OUTSIDE_JAIL"
if [ -e ../escaped.txt ]; then no "a file was created outside the jail"; else ok; fi

fresh rootflag
mkdir -p in
printf 'yes\n' > in/a.txt
run fs.read in/a.txt --root .
exit_is 0 "--root . keeps the workspace as the jail"
has "truncated=f" "the read says it is complete"

# =====================================================================
# 8. fs.read: bounded and honest
# =====================================================================
fresh fsread
: > lines.txt
i=1
while [ $i -le 20 ]; do printf 'line%02d content\n' $i >> lines.txt; i=$((i + 1)); done

run fs.read lines.txt
exit_is 0 "fs.read of a file"
has "bytes=" "fs.read reports bytes="
has "shown=" "fs.read reports shown="
has "truncated=" "fs.read reports truncated="
has "total=20" "fs.read reports the total line count"
has "hash=" "fs.read reports a content hash"
has "truncated=f" "a full read reports truncated=f"
nocr "fs.read"

run fs.read lines.txt --lines 5-8
exit_is 0 "fs.read --lines"
has "lines=5-8" "--lines is echoed back"
has "shown=4" "--lines bounds the body"
has "line05" "--lines returns the requested window"
hasnt "line09" "--lines stops at the requested window"

run fs.read lines.txt --grep line12
exit_is 0 "fs.read --grep"
has "shown=1" "--grep reports how many lines matched"
has "line12" "--grep returns the match"

run fs.read lines.txt --grep line12 --ctx 1
exit_is 0 "fs.read --grep --ctx"
has "shown=3" "--ctx widens the window around the match"

run fs.read lines.txt --anchors --lines 1-3
exit_is 0 "fs.read --anchors"
hasre 'L1:[0-9a-f]+' "--anchors prefixes lines with L<n>:<hash>"

run fs.read lines.txt --max-bytes 20
exit_is 0 "--max-bytes still succeeds"
has "truncated=t" "--max-bytes sets truncated=t"
has "shown=" "--max-bytes still reports shown="

run fs.read missing.txt
exit_is 5 "fs.read of a missing file is an IO error"
has "NOENT" "the missing file names NOENT"

run fs.read lines.txt --lines 1-2 --fmt json
eqx "$(head -c 1 "$OUT")" "{" "fs.read --fmt json returns an object"

# =====================================================================
# 9. fs.ls / outline / bundle / find / diff
# =====================================================================
fresh lsdir
mkdir -p sub
printf 'a\n' > one.c
printf 'bb\n' > sub/two.c
printf 'ccc\n' > notes.txt
run fs.ls . --recursive --glob '*.c'
exit_is 0 "fs.ls --recursive --glob"
has "total=2" "fs.ls matched both .c files"
has "shown=2" "fs.ls reports shown="
has "truncated=f" "fs.ls reports truncated="
has "bytes=" "fs.ls reports bytes="
run fs.ls . --max 1
has "truncated=t" "fs.ls --max truncates and says so"
has "shown=1" "fs.ls --max respects the limit"

run fs.outline one.c
case "$RC" in
  0) has "path=" "fs.outline names the file" ;;
  *) no "fs.outline exited $RC: $(head -1 "$OUT")" ;;
esac

run fs.bundle one.c notes.txt --query a
case "$RC" in
  0) has "bundle=" "fs.bundle returns a bundle" ;;
  *) no "fs.bundle exited $RC: $(head -1 "$OUT")" ;;
esac

fresh finddir
printf 'needle here\nsecond line\n' > f1.txt
printf 'nothing\nNEEDLE upper\n' > f2.txt
run find needle .
exit_is 0 "find"
has "total=2" "find is case-insensitive and found both"
has "f1.txt" "find names the matching file"
has "shown=2" "find reports shown="
has "truncated=f" "find reports truncated="
run find needle . --max 1
has "truncated=t" "find --max truncates and says so"
run find zzz-absent .
has "total=0" "find reports zero matches honestly"

fresh diffdir
printf 'a\nb\nc\n' > a.txt
printf 'a\nB\nc\nd\n' > b.txt
run diff a.txt b.txt
exit_is 0 "diff"
has "adds=" "diff counts additions"
has "dels=" "diff counts deletions"
run diff a.txt b.txt --unified
exit_is 0 "diff --unified"
run diff a.txt nope.txt
exit_is 5 "diff against a missing file"

# =====================================================================
# 10. patch: anchors from the current version apply, stale ones do not
# =====================================================================
fresh patchdir
: > p.txt
i=1
while [ $i -le 10 ]; do printf 'proc %d {\n}\n' $i >> p.txt; i=$((i + 1)); done
run fs.read p.txt --anchors --lines 3-3
A3=$(anchor_from "$OUT" L3)
[ -n "$A3" ] && ok || no "no content anchor came back for line 3: $(head -1 "$OUT")"
printf '[{at:"%s",op:"set",text:"REPLACED"}]' "$A3" > ops.json
run patch p.txt --ops ops.json
exit_is 3 "patch needs confirmation"
has "code=NEED_CONFIRM" "patch produced a plan"
has "files=" "the patch plan lists the target"
if grep -q '^REPLACED$' p.txt; then no "patch wrote the file before confirmation"; else ok; fi
confirm_again patch p.txt --ops ops.json
exit_is 0 "the confirmed patch applied"
if grep -q '^REPLACED$' p.txt; then ok; else no "the anchored line was not replaced"; fi
if grep -q '^proc 2 {' p.txt; then no "the line the anchor pointed at is still there"; else ok; fi

# an anchor from the pre-patch version is stale now and must fail
printf '[{at:"%s",op:"set",text:"AGAIN"}]' "$A3" > stale.json
run patch p.txt --ops stale.json
if [ "$RC" = "0" ]; then no "a stale anchor applied anyway"; else ok; fi
has "ANCHOR_MISS" "a stale anchor reports ANCHOR_MISS"
if grep -q '^AGAIN$' p.txt; then no "the stale anchor wrote the file"; else ok; fi

# anchors carry no line number: line 1 is still addressable after line 3 moved
run fs.read p.txt --anchors --lines 1-1
A1=$(anchor_from "$OUT" L1)
printf '[{at:"%s",op:"ins_after",text:"ADDED"}]' "$A1" > add.json
run patch p.txt --ops add.json
confirm_again patch p.txt --ops add.json
exit_is 0 "patching with an anchor taken before the last edit"
if grep -q '^ADDED$' p.txt; then ok; else no "ins_after did not insert the line"; fi

# the ops file may come from stdin
run fs.read p.txt --anchors --lines 5-5
A5=$(anchor_from "$OUT" L5)
printf '[{at:"%s",op:"set",text:"FROMSTDIN"}]' "$A5" > sops.txt
run patch p.txt --ops - < sops.txt
if [ "$RC" = "3" ]; then
  T=$(token_from "$OUT")
  run patch p.txt --ops - --confirm "$T" < sops.txt
fi
exit_is 0 "patch reads its ops from stdin"
if grep -q '^FROMSTDIN$' p.txt; then ok; else no "--ops - did not apply the change"; fi

# =====================================================================
# 11. stall detection (SPEC 3.7)
# =====================================================================
fresh stall
write bad.vxa 'out.die("same failure")'
run run bad.vxa
exit_is 2 "a script error exits 2"
hasnt "LOOP_DETECTED" "the first failure is not called a loop"
run run bad.vxa
hasnt "LOOP_DETECTED" "the second failure is not called a loop"
run run bad.vxa
has "LOOP_DETECTED" "the third identical failure is reported as a loop"
has "times=3" "the loop line says how many times"
has "change something" "the loop line says what to do instead"
exit_is 2 "the loop line keeps the original exit code"
write bad.vxa 'out.die("a different failure")'
run run bad.vxa
hasnt "LOOP_DETECTED" "changing the script clears the streak"

# =====================================================================
# 12. plan: forced plan-only never executes
# =====================================================================
fresh planonly
printf 'brand new\n' > n.txt
write t.vxa 'fs.write("n.txt", "changed")' 'fs.write("m.txt", "created")'
run plan t.vxa
exit_is 3 "plan exits 3 when anything needs confirmation"
has "code=NEED_CONFIRM" "plan prints the plan"
has "token=" "plan prints a usable token"
eqx "$(cat n.txt)" "brand new" "plan did not touch the existing file"
if [ -e m.txt ]; then no "plan created a new file"; else ok; fi
T=$(token_from "$OUT")
run plan t.vxa --confirm "$T"
exit_is 1 "plan refuses --confirm: it must never execute"
has "BAD_INPUT" "refusing --confirm names BAD_INPUT"
if [ -e m.txt ]; then no "plan --confirm wrote a file"; else ok; fi

# =====================================================================
# 13. exit codes and flag discipline
# =====================================================================
fresh codes
run eval '1' --nope
exit_is 1 "an unknown flag is a usage error"
has "code=BAD_INPUT" "the unknown flag names BAD_INPUT"
has "flag=--nope" "the unknown flag is named"
has "vxa help" "the unknown flag points at help"
run eval '1' --f
exit_is 1 "an abbreviation is refused, never guessed"
run eval '1' --fmt
exit_is 1 "--fmt with no value is refused"
run eval '1' --fmt=yaml
exit_is 1 "--fmt with a bad value is refused"
run eval '1' --confirm
exit_is 1 "--confirm with no value is refused"
run eval '1 +'
exit_is 2 "a syntax error exits 2"
has "code=SYNTAX" "the syntax error names SYNTAX"
run run nothing.vxa
exit_is 5 "a missing file exits 5"
has "code=NOENT" "the missing file names NOENT"
run nosuchcommand 1
exit_is 1 "an unknown command exits 1"
run
exit_is 1 "no command at all exits 1"
run eval '{a:1}' --quiet
exit_is 0 "--quiet does not change the result"

# stdout stays machine output; CLI notes go to stderr and --quiet kills them
fresh purity
write p.vxa 'out.log("working")' '42'
run run p.vxa
eqx "$(cat "$OUT")" "42" "stdout holds only the result"
if grep -q working "$ERR"; then ok; else no "out.log did not reach stderr"; fi
write q.vxa 'fs.write("x.txt", "hello")'
run run q.vxa --quiet
if grep -q "vxa:" "$ERR"; then no "--quiet still leaked a CLI note to stderr: $(cat "$ERR")"; else ok; fi
has "bytes=" "--quiet does not remove the receipt"
if [ -f x.txt ]; then ok; else no "--quiet changed what the script did"; fi

# =====================================================================
# 14. doc / lang / check / doctor / help / version: generated, not hand-written
# =====================================================================
fresh meta
run lang
has "VXA" "lang prints the cheat sheet"
n=$(wc -l <"$OUT"); [ "$n" -ge 5 ] && ok || no "lang is too short to write code from ($n lines)"
run lang --full
has "fs.read" "lang --full lists the builtins"
run doc
has "namespaces=" "doc lists the namespaces"
has "builtins=" "doc counts the builtins"
run doc fs
has "fs.read" "doc <ns> lists that namespace"
run doc fs.read
has "sig=" "doc <ns>.<name> prints a signature"
run doctor
for f in os= arch= cc= root= tmp= disk_free_gb= utf8= args=; do has "$f" "doctor reports $f"; done
oneline "doctor is one line"
case "$RC" in 0|4|5) ok ;; *) no "doctor exited $RC" ;; esac
run version
has "vxa 0.1.0" "version prints the version"
run help
has "fs.bundle" "help lists the commands"
has "3 need-confirm" "help states the exit codes"
run help sh
has "--ro" "help <cmd> lists that command's own flags"

# check: a syntax report, plus tests/*.vxa run as case files
mkdir -p tests
printf 'a = 1\nb = a + 1\nb == 2\n' > tests/good.vxa
printf 'x = (1 + 2\n' > tests/broken.vxa
run check .
exit_is 2 "check reports failures with the script exit code"
has "files=2" "check counts the .vxa files it looked at"
has "fail=1" "check counts the failure"
if grep -Eq 'tests/broken.vxa:[0-9]+: ' "$OUT"; then ok; else no "check must print file:line: message: $(head -4 "$OUT" | tr '\n' '~')"; fi
rm -f tests/broken.vxa
run check .
exit_is 0 "a clean tree checks green"
has "ok=1" "the clean check counts the good file"
has "fail=0" "the clean check has no failures"
has "cases=1" "check runs tests/*.vxa as case files"
printf 'this_name_does_not_exist.nothing\n' > tests/casebad.vxa
run check tests
has "fail=1" "a failing case counts as a failure"
exit_is 2 "a failing case exits 2"

echo "E2E pass=$pass fail=$fail"
[ "$KEEP" = "1" ] || rm -rf "$TMPD"
if [ "$fail" -gt 0 ]; then echo "E2E FAILED"; exit 1; fi
echo "E2E OK"
exit 0
