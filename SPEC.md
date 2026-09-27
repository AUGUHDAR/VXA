# VXA 语言规格 v0.1（AI 原生代理语言 / Agent-Native Language）

> 定位：给 **AI Agent** 用的脚本语言 + 操作协议。不是给人看的漂亮语言。
> 人类语言（如本仓库的 Vox）追求"语言适应人的直觉"：容忍歧义、自动补全、永不崩溃。
> VXA 追求相反的极值：**零歧义、机器可读输出、可确认的副作用、按 token 计价**。

## 0. 为什么 AI 需要另一门语言

Agent 的真实成本不是 CPU，是 **往返次数 × 每轮 token**。一次任务的开销结构：

| 成本源 | 占比 | VXA 的对策 |
|---|---|---|
| 读文件（整份读入只改 3 行） | 最大 | `fs.read --lines` / `fs.outline` / `fs.bundle`，返回 `tok=` 自报体积 |
| 盲改：按行号改，行号已漂移 | 高，且引发重试 | **内容哈希锚点** `c3f2`，锚点错乱直接报错而不是改错 |
| 冗长自然语言报错 | 中 | 稳定错误码 + 一行 `k=v` 输出 + `hint=` 可执行修复建议 |
| 语法糖写错 → 解析歧义 → 重试 | 中 | 语法只有一套合法形式，**不做模糊匹配、不做括号补全** |
| 不可逆操作（删文件、跑破坏性命令） | 事故级 | **两阶段确认协议**（§3），令牌绑定计划内容 + 文件旧哈希 |
| 结果非结构化 → 人/模型再解析 | 高 | 所有内建函数返回 record/list，无自然语言 |

设计公理（冲突时按优先级裁决）：
1. **可验证优于宽容**：宁可报错，不可猜测。猜测会让 Agent 带着错误前提继续跑。
2. **token 优于美观**：输出默认单行、去标点、`null` 用 `-` 表示。
3. **幂等优于便利**：同一脚本 + 同一输入 = 同一计划 = 同一令牌。
4. **确定性排序**：record 保持插入序，list 保序，绝不依赖哈希遍历序（否则令牌不稳定）。
5. **不崩溃**：运行期错误是 `ERR` 值，不是进程退出；但**错误必须显式传播**（§1.6）。

---

## 1. 语言

### 1.1 词法
```
注释   # 到行尾；块首 #! 行忽略
空白   换行 = 语句分隔；; 亦可分隔；无缩进语义
标识符 [A-Za-z_][A-Za-z0-9_]*     （区分大小写）
数字   12  3.5  -2  0x1F          （内部 double；整型打印不带 .0）
字符串 "带 {expr} 插值，转义 \n \t \" \{ \\"
      '原始串，不插值不转义'
      ```三引号，跨行，不插值```
布尔   true false      空值 null
列表   [1, 2, 3]       尾逗号允许
记录   {name:"a", n:1} 键可为标识符或字符串，插入序保留
```
**没有**：类型标注、分号强制、缩进块、预处理、多行字符串续行符、隐式全局。

### 1.2 运算符（优先级从高到低）
```
() [] . 取值/调用
- !      一元
* / %
+ -
< <= > >= == != ~=(相似) 
and or
|        管道（最低，含赋值右侧）
=        赋值/声明
```
- `.`：`a.b` 记录字段；`a.0` 列表下标；`fs.read` 命名空间调用。
- `~=`：相似度 0..1（Levenshtein 归一），**仅用于数据匹配，不用于语法容错**。
- `|`：`x | f(a,b)` ≡ `f(x,a,b)`，值永远是第一个实参，无占位符歧义。

### 1.3 语句
```vxa
x = 1                       # 首次赋值即声明
def add(a, b) { a + b }     # 块最后一个表达式即返回值；显式 return 可提前
if c { } elif d { } else { }
for v in list { }           # for k,v in rec { }
while c { }
break continue
return expr?
```
- 作用域：块 `{}` 是新作用域；内层可读写外层已存在的变量，赋值新名则为局部。
- 无递归深度无限：默认步数上限 `steps=2e7`、递归深度 512，超限返回 `ERR GUARD_LIMIT` 并说明如何放开。

### 1.4 值与内建字段
所有值支持 `.type()`（返回字符串）；list 支持 `.len .map .filter .each .join .sort .first .last .slice(a,b) .has(x) .uniq .flat`；
str 支持 `.len .split(sep) .trim .upper .lower .contains(s) .starts(s) .ends(s) .replace(a,b) .re(sep) .find(re) .lines .hash .fmt`；
rec 支持 `.keys .values .has(k) .get(k,default) .del(k) .merge(r) .pick([k..]) .omit([k..])`。

### 1.5 错误值
`ERR` 是值，携带 `code msg hint`。内建函数失败返回 ERR 而非抛异常。
```vxa
r = fs.read("nope.txt")        # -> !ERR code=NOENT
if r.code? { ... }             # r.code 存在即错误
out.dead(r)                    # 或 die(r) 结束脚本
```
未被使用的 ERR 会静默（Agent 自己负责检查），但 `strict=1`（配置项）时首个 ERR 即终止。

### 1.6 真值
`null false 0 "" [] {}` 为假，其余为真。

---

## 2. 输出协议（这是给 AI 读的，不是给人读的）

`--fmt` 三种：`auto`(默认紧凑) / `json` / `text`；`--lines` 用于逐行展开列表。
体积一律用可测量量自述：`bytes=`、`lines=`、`truncated=t`、`total_bytes=`、`cursor=`。
`est=` 只是 `ceil(bytes/4)` 的粗估，标注为估算，不作为预算依据。

### 2.1 紧凑格式 `auto` 规则
```
标量      42  3.5  t/f  null->-   字符串安全时裸写，否则 C 式转义
record    {k=v,k2=v2}          顶层省略最外层花括号
list 标量  [1,2,3]
list 记录  [{a=1,b=2},{a=3,b=4}]
嵌套      永远保留 { } [ ] 定界，不做"a.b.c=v"式的展平
错误      !ERR code=NEED_CONFIRM op=fs.write token=9f2c1a hint="rerun with --confirm 9f2c1a"
```
规则只有一条：**结构靠定界符，不靠换行和位置**。展平格式（`files.path=a` 换行 `files.path=b`）在一条记录含多个列表时会歧义，实测多轮对话中压缩格式省字但增加误读，因此宁可保留 `{}`。
相对 JSON 仍省约 35% 字符（去引号、去 `:`、null->`-`、bool->`t/f`、无缩进）。
示例（`fs.read --lines 10-40`）：
```
path=src/main.c hash=7c19f3 lines=41-71 total=312 tok=214 text=...
```
### 2.2 退出码（Agent 用 `$?` 分流，不需要读文字）
| 码 | 含义 | 应对 |
|---|---|---|
| 0 | 成功 | 继续 |
| 1 | 用法/参数错误 | 修参数 |
| 2 | 脚本解析或执行错误 | 看 `code=` 修脚本 |
| 3 | **需要二次确认** | 读 plan，决定是否 `--confirm <token>` 重跑 |
| 4 | 策略拒绝（越界、禁用） | 换路径或改 manifest 策略 |
| 5 | IO 错误 | 检查存在性/权限 |
| 6 | 内部错误 | 报障 |
| 7 | 超时 | 提高 `--timeout` 或拆分 |

---

## 3. 两阶段确认协议（需求 a、b 的机制化落地）

### 3.1 计划 PLAN
任何**副作用操作**（写文件、删文件、改文件、执行命令）不直接生效，返回 PLAN：
```
{op:"fs.write", confirm:"required", token:"9f2c1a",
 files:[{path:"src/a.c", from:"e1b2..", to:"77aa..", kind:"modify", bytes:512},
        {path:"src/b.c", kind:"delete", bytes:2048}]}
```
`files` 即需求 (b)：**列出将被修改/删除的每个文件及影响**。

### 3.2 令牌
`token = base16(sha256(canonical_plan))[:12]`，`canonical_plan` 为稳定序列化：
`vxa1|op|排序后的 "path:kind:from_hash:to_hash"|其他参数指纹`。
- 令牌绑定 **文件旧内容哈希**：若确认之间文件被改动，令牌自动失效 → `ERR STALE_PLAN`（防 TOCTOU）。
- 令牌与 **工作区根路径** 绑定：换个目录重放同一脚本，令牌不同。

### 3.3 策略 `confirm=`（三值）
| 值 | 行为 | 默认用于 |
|---|---|---|
| `none` | 立即执行，不产令牌（仅临时目录等零风险区） | `.vxa/tmp/` 下写入 |
| `jail` | 由 `apply()` 判定：目标全在工作区内**且**是新建 → 直接执行并出回执（不多花一轮）；替换或删除已有内容 → 要求令牌 | **fs.write 默认** |
| `ask` | 总是要求令牌 | `fs.delete`、`sh.run` 默认 |
| `all` | 同上但连 `none` 区也要令牌 | 审计模式 |

### 3.4 重放语义（关键）
**幂等台账**：令牌一经成功消费即写入 `.vxa/journal.ndjson`（token、目标旧/新哈希、时间、结果）。
带同一令牌重放不再执行，直接返回缓存结果 + `replayed=t`——避免"重试即重复写"这类不可逆事故。

Agent 不可交互回答提示。所以确认 = **重放**：
```
$ vxa run task.vxa                      -> 打印 PLAN，exit 3
$ vxa run task.vxa --confirm 9f2c1a     -> 重建同一 PLAN，令牌匹配，执行，exit 0
```
脚本必须幂等：`fs.read` 等只读操作两次执行结果一致，故令牌一致。

### 3.4b 谁来触发执行（唯一真相）
PLAN 只有两条路径能变成磁盘动作：
1. 脚本自己 `.apply()` —— 同一脚本里的多个写各自 apply，未 apply 的不落地；
2. 带 `--confirm TOKEN` 重放，且该计划本就需要令牌。

**CLI 绝不代替脚本执行"尾值里的计划"。** 曾实现过一次自动执行，后果是
`fs.write(a); fs.write(b)` 只写 b 却退出 0：退出码撒谎，Agent 会去读一个不存在的文件。
现在未 apply 的尾值计划 => `!ERR code=UNAPPLIED` + exit 2（脚本缺陷），零落地。
`vxa plan` 只预览，任何情况下都不写。

### 3.5 语言内 API
```vxa
p = fs.write("a.txt", text)      # 返回 PLAN（未写盘）
p | out.plan                      # 打印影响面
if p.files.len > 5 { die("影响面过大") }
r = p.apply()                     # 策略需确认时 -> !ERR NEED_CONFIRM(含 token)
r = p.apply(p.token)              # 显式给出令牌（用于自测/CI）
fs.write("a.txt", text).auto.apply   # 同上链式
```
`sh.run("rm -rf x")` 同构：PLAN → apply → 结果 record。

---

## 4. 内建库（为 Agent 的六件事而存在）

命名空间：`fs` 文件 · `tx` 文本/锚点 · `sh` 命令 · `out` 输出/交互 · `cfg` 配置 · `sim` 相似度 · `task` 作业。

### 4.1 fs — 上下文获取（省 token 的主战场）
```vxa
fs.cwd() -> str
fs.exists(p) -> bool
fs.glob(pat, root=".") -> [str]
fs.ls(dir, {recursive:false, glob:"*.c", max:200}) -> [{path,bytes,hash,lines}]
fs.hash(p) -> str                 # sha256[:12]，内容指纹
fs.read(p, {lines:"10-40", grep:"foo", max_tok:400, anchors:true, ctx:2})
   -> {path,hash,lines:"10-40",total:312,match:3,tok:180,text}
   # anchors:true 时每行前缀 `L12:d6f21459| `（8 字符锚点），可直接抄进 tx.patch 的 at
fs.write(p, text) -> PLAN
fs.append(p, text) -> PLAN
fs.delete(p) -> PLAN              # 移入 .vxa/trash/<seq>__<name>，可 fs.restore
fs.move(from, to) -> PLAN         # 需求(b)：重命名也列影响
fs.restore(seq) -> PLAN
fs.outline(p) -> {path,hash,symbols:[{kind,name,line,hash,sig}],lang,tok}
fs.bundle([paths], {max_bytes:12000, query:"parser", only:"symbols"}) -> {bundle:[...],dropped:[...],tok}
fs.manifest() -> rec              # 读工作区 .vxa/manifest.json
```
`fs.outline`/`fs.bundle` 是"只喂骨架不喂全文"的实现：一次调用替代十次 read。

### 4.2 tx — 锚点与差异（消灭"盲改"）
锚点 = 上下文哈希，**不含行号**：
```
at = base16(sha256("vxa1"   prev_line   cur_line   next_line))[:8]
```
- 不含行号 => 在上游插入/删除一行不会使其余锚点失效（含行号的方案会让每个后续锚点变成 STALE，实测直接把可用性打掉）。
- 首行 prev 取空串，末行 next 取空串。
- 同文件内锚点可能重复（完全相同的三行上下文）。重复**不改变哈希**，而是在使用时消歧：`{at:"1f2e3d4c", n:2}` 表示第 2 次出现；缺省 `n` 且有多处 => `ERR ANCHOR_DUP(count=3)`，并回列每处的行号与内容。
- 锚点是"内容地址"，因此 `--confirm` 期间文件被改 => 锚点自然失配 => `ERR ANCHOR_MISS`，附最近 3 个候选。
```vxa
tx.anchors(text, path) -> [{n,at,hash}]
tx.patch(text, [{at:"c3f2", op:"set", text:"..."},
                {at:"7a1b", op:"del"},
                {at:"9de0", op:"ins_after", text:"..."}], {path:"a.c"})
   -> {text, applied:3, checks:[{at:"9de0", ok:true, preview:"L42: ..."}]}
   # 锚点找不到 -> ERR ANCHOR_MISS（附最近 3 个候选锚点及其行内容）
   # 锚点重复     -> ERR ANCHOR_DUP
tx.diff(a, b, {ctx:3, unified:true}) -> {text, adds:4, dels:1, hunks:2}
tx.udiff(fileA, fileB, {ignore_ws:true}) -> PLAN 形式? 否，只读 -> {text}
tx.find(text, re, {group:1}) -> [{n,at,match,text}]
tx.similar(a,b) -> 0.0..1.0
tx.subst(text, re, repl) -> str        # repl 支持 ${1}
tx.join(list, sep) -> str
```
`tx.patch` 返回的 `checks` 是**自动二次核验**：每个 op 都回读改后行内容，Agent 无需再 read 一遍确认。

### 4.3 sh — 命令执行（需求 a）
```vxa
sh.run(["go","test","./..."], {cwd:".", timeout:60, env:{}, stdin:"", tail:40, redact:true}) -> PLAN
sh.run("make && rm -rf /")            # 字符串形式 => 永远要令牌，走 shell
PLAN.apply() -> {code, out, err, ms, signal, out_tail, truncated, total_bytes, cursor}
sh.ro(["git","status"], {timeout:10}) -> {code,out,err,ms}   # argv 白名单，不经 shell
sh.allowed(["git","status"]) -> bool
```
- **argv 数组优先**：直接 CreateProcess/execvpe，不经过 shell，`; && | $()` 全部失效。
- 白名单按 `argv[0..1]` 精确匹配，不看字符串前缀（`git status; rm -rf` 这种绕过被封死）。
- 字符串命令 = 显式要求 shell，策略恒为 `ask`，且不能命中白名单。
- `sh.ro` 白名单（默认）：`ls cat head tail find grep rg wc sort uniq file stat git-status git-log git-diff git-show go-build-query cargo-metadata node--version python--version`。**只读命令走快路，写命令走确认**，避免所有操作都被迫二次确认。
- `redact:true`：输出里匹配 `(?i)(token|secret|password|api[-_]?key|authorization)\s*[:=]\s*\S+` 替换为 `$1=***`（配置与日志的密钥必须脱敏）。
- `tail:40`：默认截断长输出为尾 40 行并给 `truncated=true`+`total_lines`，因为失败信息在尾部。
- `sh.run` 默认策略 `ask`；manifest `policies.shell` 可加前缀白名单降级为 `jail`。

### 4.4 out / cfg / task
```vxa
out.emit(v)            # 按 --fmt 输出，可多次
out.log(msg)           # stderr 进度，不污染 stdout
out.plan(p) out.json(v) out.raw(s) out.kv(k,v) out.die(v)
cfg.get(key, default)  cfg.set(key, v) cfg.merge(rec)   # 无交互问值：缺配置就报 ERR 让 Agent 补
cfg.save() -> PLAN     # 写 .vxa/config.json
task.run([{name:"build", cmd:"...", needs:["env"]}], {jobs:4, confirm:"ask"}) -> PLAN/结果
```

---

### 3.7 失速检测（Agent 会在同一个错误上反复撞）
同一 (脚本, 参数, 退出码, 首个错误码) 指纹在 `.vxa/journal.ndjson` 中连续出现 3 次 =>
输出 `!ERR code=LOOP_DETECTED times=3 same_as="#2" hint="change the plan, do not repeat it"` 并退出码 3。
这是给 Agent 的硬止损，不是给人类的提示。

### 3.8 验证是一等操作
`task.verify([{name,cmd}])`：按依赖拓扑跑检查，全部通过才允许标记任务完成；
失败时只回传每条检查的首个失败块（`first_fail=`），不回传整坨日志。

## 5. 工作区、安全与审计
- 工作区根 = `--root` > 最近的 `.vxa/` 目录 > cwd。
- **监狱（jail）**：任何 `fs.*` 写目标经 `realpath` 后必须在根内；越界 → `ERR OUTSIDE_JAIL`（退出码 4）。
- 写文件临时文件 + rename，原子。
- 删除永不 `unlink`：移动到 `.vxa/trash/`，保留 30 天，`fs.restore` 可逆。
- 审计：每次 PLAN 执行追加一行到 `.vxa/audit.ndjson`（含令牌、时间、目标哈希）。
- 权限：`--allow` 外部命令白名单在 manifest 中声明；未声明的绝对路径外执行 → `ERR POLICY`。
- C 盘保护：临时/缓存目录一律可配置，默认 `.vxa/tmp`（工作区内）。

## 6. 配置分层与自愈（"先查、先装、先问，不臆测"）
优先级（只有三层，少一层是一层心智）：`CLI --set` > `.vxa/config.json`（含 manifest 的策略段）> 内建默认。
环境变量 `VXA_*` 仅用于覆盖 `paths.*`，不参与策略。
```vxa
d = sh.ro("python --version")            # 1 查：存在性
if d.code != 0 {
  cfg.set("python.exe", out.ask("python 路径？"))    # 3 问：只问一次，随后落盘
}
```

## 7. 清单文件 `.vxa/manifest.json`
```json
{"name":"vxa","version":"0.1.0","entry":"main.vxa",
 "requires":{"tools":["cc"],"env":["CC"],"files":["SPEC.md"]},
 "policies":{"fs":"jail","shell":"ask","confirm":"auto","jail":true,"trash_days":30},
 "paths":{"tmp":".vxa/tmp","cache":".vxa/cache","log":".vxa/audit.ndjson"},
 "checks":[{"name":"selftest","cmd":"vxa check tests"}],
 "deps":[{"name":"xdiff","src":"internal","version":"0.1"}],
 "vars":{"max_bytes":8000,"fmt":"auto"}}
```
- `deps` 中 `internal` = 随二进制内建，`install` 时校验版本，缺失则报错并给出获取方式（不静默降级）。
- `deps.json` 记录安装来源/哈希/日期，供审计。

## 8. CLI
```
vxa eval '<expr>'                     单表达式
vxa run <file.vxa> [--confirm TOK] [--set k=v] [--jobs n] [--timeout s]
vxa plan <file.vxa>                   强制只出计划不执行
vxa fs.read <p> [--lines a-b] [--grep re] [--anchors] [--max-bytes n]
vxa fs.ls [dir] [--glob g] [--recursive] [--max n]
vxa fs.outline <p>                    vxa fs.bundle <p...> [--query q] [--max-bytes n]
vxa sh '<cmd>' [--confirm TOK] [--tail n] [--timeout s] [--ro]
vxa diff <a> <b> [--unified|--compact]     vxa patch <p> [--ops ops.json] [--confirm TOK]
vxa find <re> [dir] [--glob g] [--max n]
vxa check [dir]                       校验语法 + 跑 tests 用例，紧凑报表
vxa doctor                            环境自检（编译器、路径、缓存、磁盘），退出码可用
vxa repl                              交互式（人用）
vxa help [topic]
通用: --fmt auto|json|text   --quiet   --root dir   --timeout s   --no-audit
```

## 9. 实现约束
- **C17，单静态二进制，无第三方依赖**（仅 libc + win32/posix 必要封装）。目标体积 < 300KB，冷启动 < 15ms。
- 内存：单一 arena 顺序分配 + 世代回收（脚本生命周期短，不做 GC）；上限 `--max-mem`（默认 512MB）超限 `ERR OOM`。
- 平台：Windows 优先（MinGW64/GCC），源码保持 POSIX 可移植。
- 编码：UTF-8 字节串，不做转换；字符串长度=字节数，`.ulen` 给码点数。
- 不做的事（明确排除，防止"人类语言"化）：模糊语法匹配、括号自动补全、参数乱序、多语关键字别名、解释性报错文案、崩溃后继续跑。
