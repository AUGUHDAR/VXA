# VXA - read once, then write it

The only reader and writer of VXA is an agent. Real cost = round trips x tokens.
VXA never guesses: an ambiguous form is a hard error, not a chance to repair it.
Every builtin returns a machine-readable value; side effects are planned, confirmed, journaled.

## 1 syntax

Every form, one line each. Anything else is not VXA.

```
x = 1                                  # newline or ";" separates statements; never whitespace
def add(a, b) { a + b }                # last expression in the block is the value; return exits early
def sq(a, b) { a * b }                 # anonymous lambda as an expression value
f = x -> x * 2                         # lambda: name, (a,b), or () on the left of ->
g = (a, b) -> a + b                    # arrow binds at the value, body may be {block}
if a > 1 { 1 } elif a { 2 } else { 3 } # "else if" is not a form
for v in [1,2] { v }                   # one name over list/str/rec/num
for k, v in {a:1,b:2} { out.kv(k, v) } # two names: rec gives key+value, list gives index+value
while i < 10 { i = i + 1 }             # break | continue | return [expr]
```

Scoping: `{}` is a new scope. Assigning a name that already exists outside writes through to it.
Assigning a new name makes it local. There is no `global` keyword - `def` is already global.

Operators, tightest first:

```
() [] . call/index/field      -x  !x  not x      * / %      + -
< <= > >= == != ~=            and      or      |      =      .. (between + and |)
```

```
a = 1 + 2 * 3          # 7, never left-to-right
s = "abc"[1..2]        # slice; s[2..] runs to the end; s[..2] is not a form
t = x | f(a, b)        # == f(x, a, b): the piped value is always arg 0, needs a CALL with parens
n = name ~= "tx.patch" # similarity 0..1, for matching DATA only
r = fs.read(p)         # ERR is a value: if r.code { ... } - there is no postfix ? form
m = rec.missing        # an absent key is null, printed as "-"; it is not an error
```

Strings: `"..."` interpolates `{expr}` and unescapes `\n \t \" \{ \\ \x41`. A literal `{` needs `\{`,
or use `'raw'` / ```` ```block``` ````, which never interpolate. `{}` empty is an error.
Lists and records allow trailing commas; parameter lists do not. Record keys are a bare name, a
`"string"`, or a number - `{k:1}` is the key `k`, not the variable `k`, and `{{k}:1}` is refused.

## 2 values and the exact bytes you will read

Six data types: `num str bool null list rec`. Machinery values: `plan err fn`.
Compact form (`--fmt auto`, the default) is one line; structure lives in `{}`/`[]`, never in line position.

```
42  3.5  t  f  -  bare-when-safe  "needs quoting"  [1,2,3]  []  {}
```

`null` prints as `-`, `true`/`false` as `t`/`f`, integers without `.0`. A string is bare unless it
contains a space, control char, or one of `" = , { } [ ] # : \` or a non-ASCII byte - then it is escaped.
Top level drops its outer braces; nested records keep them. These are real emitted bytes:

```
path=src/main.c,hash=7c19f3a1b2c3,lines=10-40,total=312   # a record at top level
[1,2,3]  [1,2,x,-]  [{path=a.c,kind=modify},{path=b.c,kind=delete}]
op=fs.write,confirm=jail,token=9f2c1a4b88d1,files=[{path=a.c,kind=modify,from=e1b2,to=77aa,bytes=512}],count=1
!ERR code=NOENT msg="no such file nope.txt" hint="path missing: run fs.ls/fs.glob to confirm the real path"
```

Keys stay in insertion order: same value, same bytes - and a plan hashes those bytes.

**Property or method.** A zero-arg method reads as a property (`xs.len` is a number, `r.keys` a list);
anything taking arguments needs parens (`xs.map(f)`). In a record the field wins: `{keys:9}.keys` is 9.
Globals too: `len(x) keys(r) sum(xs)` - see `vxa doc global`.

## 3 exit codes - branch on these, not on prose

| code | meaning | do |
|---|---|---|
| 0 | ok | continue |
| 1 | usage / bad flag | fix the arguments |
| 2 | parse, type, arity, undef, guard, limit, UNAPPLIED | fix the script at the reported line |
| 3 | NEED_CONFIRM | read the plan, decide, rerun with `--confirm TOKEN` |
| 4 | OUTSIDE_JAIL, DENIED, POLICY | use a path inside the root, or change manifest policy |
| 5 | NOENT, IO, NOTDIR, ISDIR, ANCHOR_MISS, ANCHOR_DUP, STALE_PLAN, BAD_TOKEN, SH | re-check existence, anchors, freshness |
| 6 | OOM | shrink the input or raise --max-mem |
| 7 | TIMEOUT | raise --timeout or split the work |

## 4 confirm protocol (operating procedure)

A side effect never happens on the first pass. `fs.write/append/delete/move`, `sh.run`,
`cfg.save`, `task.run` all return a PLAN.

1. **run** `vxa run task.vxa` - you get the plan and exit 3. Nothing changed.
2. **read the plan**: `op`, `confirm`, `token`, `files=[{path,kind,from,to,bytes}]`. That list is the
   whole impact surface: every file created, replaced, or deleted.
3. **decide**: surface the impact, or accept it. Never confirm blind.
4. **rerun** `vxa run task.vxa --confirm TOKEN`. The script rebuilds the identical plan, the token
   matches, it executes, exit 0.

Policy `confirm=` on each plan: `none` = act at once (only `.vxa/tmp/`); `jail` = **creating a new file
inside the workspace runs at once with a receipt, replacing or deleting anything needs the token**
(fs.write default); `ask` = always needs the token (sh.run, fs.delete default); `all` = token for everything.

Token = 12 hex chars of sha256 over `vxa1|op|sorted path:kind:from_hash:to_hash|arg fingerprints`:
bound to the old content hashes and to the workspace root, and single use.

* `STALE_PLAN` (exit 5) - a target changed between the plan and the confirm, so the token's `from` hash
  no longer matches. Do not reuse the token: rerun without `--confirm` to get a fresh plan, re-read the
  diff, then decide again.
* `LOOP_DETECTED` - the same (script, args, exit code, first error) three times in a row is a stall, and
  the journal says so. A fourth identical retry is never the fix. Change something: path, args, range,
  the whole approach - or stop and report.
* `OUTSIDE_JAIL` (exit 4) - the resolved path escapes the workspace root. Run `fs.ls` to find the real
  in-root path, pass `--root`, or accept the refusal. Nothing was touched.
* `BAD_TOKEN` - the token is for a different plan or root. Regenerate; do not edit it.

Script side: `p = fs.write(path, text)` -> `p | out.emit()` -> `r = p.apply()` yields the NEED_CONFIRM
value; `p.apply(tok)` runs it, with `tok = cfg.get("confirm", "")` from `--confirm`.
Plans are queryable: `p.op p.confirm p.token p.files`, so `if p.files.len > 5 { out.die("too wide") }`
bounds blast radius. **Nothing runs unless the script applies it** - the CLI never executes a trailing plan
for you (`code=UNAPPLIED`, exit 2, zero writes), so several writes need several `.apply()`s.
A consumed token replayed returns the cached result with `replayed=t` instead of writing twice.

## 5 the five things an agent always needs

**find** - never assume a path exists.
`fs.ls(dir, {recursive:true, glob:"*.c", max:200})` -> `[{path,bytes,lines}]` sorted by path.
`fs.glob("**/*.md", ".")` -> `[str]` relative paths. `fs.outline(p)` -> symbols only:
`{path,hash,lang,symbols:[{kind,name,line,hash,sig}],est}` - one call replaces ten reads.
`fs.bundle([paths], {query:"parser", max_bytes:3000, only:"symbols"})` -> `{bundle:[...],dropped:[...]}`.

**read cheaply** - `fs.read(path, {lines:"10-40", grep:"re", ctx:2, max_bytes:2000, anchors:true})`
-> `{path,hash,lines,total,bytes,shown,truncated,est,text}`. `truncated=t` means you got a **prefix**, not
the file: raise `max_bytes`, narrow with `lines`/`grep`, or read the rest from `cursor`. Never treat a
truncated body as the whole thing. `est` is `ceil(bytes/4)`, an estimate only.

**edit safely** - read with `anchors:true`, every line arrives as `L12:c3f2| text`. The 8-char hash is
`sha256("vxa1" + prev + cur + next)`: content-addressed, so an insert upstream does not shift it.
Build ops from those anchors, never from line numbers:
```
ops = [{at:"c3f2a1b0", op:"set", text:"..."}, {at:"7a1b2c3d", op:"del"},
       {at:"9de0f1a2", op:"ins_after", text:"...", n:2}]
new = tx.patch(text, ops, {path:"src/a.c"})   # {text, applied, checks:[{at,ok,preview}]}
w = fs.write("src/a.c", new.text)
```
`ANCHOR_MISS` = the anchor is gone (re-read, the file moved). `ANCHOR_DUP` = the same three lines appear
several times: pass `n` for the occurrence you mean. `checks` re-reads every edited line for you, so do
not read the file again to confirm your own patch.

**run commands** - `sh.ro(["git","status"], {timeout:10})` is the read-only fast path: argv, no shell,
no token. `sh.run(["go","test","./..."], {cwd, timeout, tail:40, redact:true})` is a PLAN -> `apply` ->
`{code,out,err,ms,out_tail,truncated,total_bytes,cursor}`. argv never passes a shell, so `; && | $()` are
literal arguments. A **string** `sh.run("make && rm -rf x")` asks for a shell: always token, never
whitelist. `sh.allowed(argv)` checks first. `code` is the exit status and `0` is falsy, so
`if r.code { ... }` is the failure branch. Failure text is at the tail: read `out_tail`, not `out`.

**verify** - the edit is not done until the tree checks. `task.verify([{name,cmd}])` runs checks in
dependency order and returns only each failure's `first_fail`, not the whole log. Compare `fs.hash(p)`
before and after, or against the `to` hash the plan promised.

## 6 do not

1. **Do not read whole files to change three lines.** `fs.outline`, then `fs.read` with `lines`/`grep`.
2. **Do not edit by line number.** Line numbers drift the moment anything is inserted; anchors do not.
3. **Do not ignore `truncated=`.** The read is a prefix; treating it as whole is how a wrong edit gets
   confirmed.
4. **Do not retry an identical failing command.** Same args + same error = `LOOP_DETECTED`. Change the
   plan or stop and report.
5. **Do not glob and then assume the file is there.** `fs.glob` returns names; `fs.exists`/`fs.hash`
   return facts. Reading a stale name costs a round trip and a NOENT.
6. **Do not hardcode absolute paths.** They break the jail, the token and the next machine; use
   `cfg.get("k", default)` / `env("VXA_ROOT")` for what varies.

## 7 typical task

```
path = cfg.get("target", "src/parse.c")
r = fs.read(path, {anchors:true, grep:"perr", ctx:2, max_bytes:4000})
if r.code { out.die(r) }                             # NOENT / ISDIR / OUTSIDE_JAIL
if r.truncated { out.die("raise max_bytes: anchors are partial") }

ops = []
for h in tx.find(r.text, "perr") {
  ops[len(ops)] = {at:h.at, n:h.n, op:"set", text:h.text | tx.subst("perr", "report_err")}
}
if len(ops) == 0 { out.die({code:"NOHIT", path:path, hash:r.hash}) }

new = tx.patch(r.text, ops, {path:path})
if new.code { out.die(new) }                         # ANCHOR_MISS / ANCHOR_DUP
w = fs.write(path, new.text)
out.emit({token:w.token, files:w.files, checks:new.checks})
given = cfg.get("confirm", "")
if given == "" { out.emit(w.apply()) }                # -> !ERR NEED_CONFIRM, exit 3
out.emit(w.apply(given))                              # second pass applies, exit 0
```

For this content as machine-readable text: `vxa lang` (whole cheat sheet) and `vxa doc fs`
(one namespace with signatures, docs and one example each; `vxa doc fs.read` for a single builtin).
