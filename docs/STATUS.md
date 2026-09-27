# 构建状态 / 决策日志（自动开发会话产出）

## 一夜结论（09:10 收尾，上下文耗尽）

可运行：`build/vxa.exe` 803 KB 静态单文件。核心主张**已用真二进制复验**：
新建即执行+回执 / 覆盖需令牌(exit 3) / `--confirm` 重放生效 / 篡改后令牌失效 / 删除进回收站需确认。

绿：util 66、val 329、lex 203、parse 319、sim 187、xdiff 303 = **1427 断言**。
红（未修完，不是假绿）：`lib_fs / lib_tx / lib_sh / sh / shell` 五个测试目标**链接期重复符号**
（各自在测试里 stub 了 `main/ctx_arena/lib_method/t_sh`，而对应实现文件已落地）——是脚手架收敛问题，
不是逻辑问题；`outline` 有真 bug（python/js/rust 符号行号 off-by-one、类自身重复计数）。
四个子 Agent 撞到 150 轮上限中途停止（outline / lib_tx / shell / CLI 与文档样例），它们最后的写入未完成；
`src/main.c` 与 CLI 命令面是可用的（eval/run/fs.*/sh/doc/lang/check/doctor 均落地），
但 `tests/e2e.sh` 里几处含反斜杠的用例没写完。

### 五个红套件的确切成因（已诊断，别再猜）

它们**不是逻辑失败，是链接集不对称**：

- `tests/test_sh.c`、`test_shell.c`：`build.sh` 只给 `util val shell`，于是测试文件自己 stub 了
  `ctx_arena/ctx_free/set_error` 等；而真实现已在 `interp.c` 里 -> `multiple definition`。
  修法：删掉测试内的这些 stub，deps 改成完整集合（去掉 `src/main.c`）。
- `tests/test_lib_fs.c`：同理自带 `lib_method` 与 `t_sh` stub，真实现在 `lib_tx.c`/`lib_sh.c` -> 冲突。
  修法：删 stub，deps 补 `src/lib_tx.c src/lib_sh.c`。
- `tests/test_lib_tx.c`、`test_lib_sh.c`：`main` 与真 `main.c` 冲突 -> deps 里必须排除 `src/main.c`
  （`pick()` 已过滤，但这两行走的是 `echo $CORE ...` 分支，`CORE` 本身不含 main，冲突来自测试自带 main
  与被 `ALL` 分支拉进来的 main；确认一次实际展开列表再动刀）。
- `outline`：**真 bug**，不是脚手架问题。python/js/rust 符号行号 off-by-one、类被重复计数。
  根因在扫描器"消费换行"的时机与行号自增的先后，写它的那个 Agent 撞到轮次上限没收尾。

### 下一步顺序

1. 先修 `outline` 行号（唯一的功能性缺陷，影响 `fs.outline`/`fs.bundle` 的可信度——锚点错一位，
   Agent 就会改错行）。
2. 再按上面三条删 stub、对齐 deps，让 `bash build.sh test` 全绿。
3. 最后补 `examples/` 与 `tests/cases/`：目前 6 个样例只跑通 `audit.vxa`；`docs/LEARN.md` 已写好。


时间：2026-09-27 05:00 起，全自动无人参与会话。目标：一门**只给 AI Agent 用**的语言 + 工具链。

## 当前测试（`bash build.sh test`）

| 套件 | 断言 | 说明 |
|---|---|---|
| util | 66 | 内存池/Str/Buf/sha256/glob/路径；sha256 与 Python hashlib 逐字节一致，含 1e6 字节多块向量 |
| val | 329 | 值模型 + 紧凑输出格式 + 确定性（同输入同字节） |
| lex | 203 | 词法：注释/CRLF/括号内换行抑制/插值拆分/错误位置 |
| parse | 222 | 语法：A 组 23 例全通过，B 组 12 个坏程序全被正确拒绝 |
| sim | 187 | 相似度，3330 条向量与独立参考实现零失配 |
| outline | 173 | 六语言符号抽取 + bundle 预算 |
| xdiff / sh / lib_fs / lib_tx / lib_sh | 进行中 | 对应实现落地后补 |

## 关键决策（按被证据推翻的顺序）

1. **实现语言 = C17**，零第三方依赖，`-static` 单文件。Go 被否：只是性能换来的体积与运行时占用，这门语言的性能根本不是瓶颈。
2. **锚点不含行号**。原设计 `sha256(path+行号+文本)` 被推翻：任何上游插入会让后续锚点全部失效。现为 `sha256(前一行, 本行, 后一行)[:8]`，重复靠出现序号 `n:` 消歧，而不是改哈希长度（改长度会破坏令牌稳定性）。
3. **紧凑输出保留 `{}`/`[]` 定界**。原设计把嵌套展平成 `files.path=a` 并换行，一条记录里有两个列表时会歧义。压缩率让位于可判定性。
4. **确认协议按影响面分级**：工作区内**新建**文件立即执行并出回执；**替换/删除**已有内容必须令牌。这样"二次确认"只花在真正不可逆的动作上，否则每写一个文件都要多一轮往返，与省 token 的立项目身冲突。
5. **令牌一次性 + 绑定旧哈希**：`.vxa/journal.ndjson` 记消费过的令牌（重放返回 `replayed=t` 不重复执行）；确认前重算目标哈希，变了就 `STALE_PLAN`。
6. **命令走 argv 数组，白名单只看 argv 前两项**。`sh.ro("git status; rm -rf x")` 这类字符串前缀匹配是假的白名单。字符串形式恒需确认。
7. **体积自述用 `bytes/lines/truncated/cursor`，不用 `tok`**。token 数依赖具体分词器，声明它是撒谎；`est=` 明确标注为 `ceil(bytes/4)` 估算。
8. **删掉 `out.ask`、`tx.resolve`**。无头 Agent 不会回答提问；靠相似度猜字段名违反"宁报错不猜测"。`~=` 保留为**显式测量**，不做隐式纠错。
9. **失速检测**：同一 (脚本, 参数, 退出码, 首错误码) 连续第 3 次 → `LOOP_DETECTED`。Agent 最容易浪费的正是重复同一失败步骤。

## 已用真二进制验证过的行为（不是"应该能"，是跑过了）

```
$ vxa fs.outline m.go
path=m.go,lang=go,bytes=74,lines=5,hash=a5b6b904246b,symbols=[{kind=fn,name=alpha,line=3,at=0dbceab0,sig="func alpha() int { return 1 }"},...],tok=55
$ vxa fs.read m.go --anchors
path=m.go,hash=...,lines=1-5,total=5,bytes=74,shown=5,truncated=f,est=35,text="L1:5cc54716| package main
L3:311470d8| func alpha()..."
$ vxa eval 'fs.outline("m.go").symbols.map(s => s.line)'   ->  [3,5]
$ vxa eval '"a,b" | tx.split(",")'                         ->  [a,b]
$ vxa lang                                                 ->  1453 字节（约 360 token）学会全语言
$ vxa doc fs.read                                          ->  从注册表生成，含 sig/doc/ex
```

两阶段确认（需求 a/b）实测：
- 新建工作区内文件：`p.apply()` 立即落盘，返回 `path=...,bytes=5,token=546ff48b7050`
- 覆盖已有文件：`!ERR code=NEED_CONFIRM ... files=[{path,kind=modify,from=a7937b64b8ca,to=16367aacb67a,bytes=6}]`，exit 3，**磁盘未动**
- 带 `--confirm <token>` 重放：写入生效，内容变 `second`
- 中途人工改文件：令牌不再匹配 -> 拒绝，文件保持被改后的内容（不覆盖别人的改动）
- `fs.delete`：恒需确认；进 `.vxa/trash/`，`fs.restore(seq)` 可回
- `fs.write("../../../Windows/x.txt")`：`OUTSIDE_JAIL`
- `sh.run("echo hi")`：返回 PLAN 需确认；`sh.ro([...])` 白名单不过的一律拒绝（fail-closed）

单元套件当前：util 66 / val 329 / lex 203 / parse 222 / sim 187 / xdiff 303 全绿；
outline、lib_fs、lib_tx、lib_sh 的测试脚手架仍在收尾（链接期 main 符号重复、t_sh 重复定义）。

## 本轮新增能力

- `fs.read(..., {cursor:N})`：截断后按页续读；`cursor` 恒为"下一未读行"，用 `cursor >= total` 判读完。
  实测 867 行文件按 300 字节分页：`1-7` → `8-867`，两页 shown 之和恰等于 total（不重不漏）。
- `sh.run(argv, {env:{...}, stdin:"..."})`：环境变量合并注入（不改父进程）、stdin 喂入。
  令牌绑定 env 的**键+值哈希**与 stdin 的**长度+哈希**，明文不进计划、不进 journal、不进 audit。

## 已知缺陷（下一轮先修这个）

- 链式 `sh.run(...).apply()` 带 `--confirm TOKEN` 时报 `cannot call a null`；
  写成 `p = sh.run(...); p.apply()` 正常。根因在 V_PLAN 作为方法接收者的链式路径上，
  `field_of` 先查计划字段失败、落到 call_fn 时函数值为空。需修并补一条链式用例。

## 已知未完成 / 坑

- `main.c`、`lib_sh.c` 落地中；`lib_fs.c` 有 `s_trim` 参数个数的改动冲突待清。
- `anchors_of` 把 CRLF 当两行 → 行号与锚点错位（安全相关，须修）。
- `bundle` 的字节预算统计没覆盖锚点前缀（超预算）。
- `sh_redact` 只匹配 `KEY=VALUE`，`Bearer xxx` 形态漏。
- Windows 路径归一：`fs.write("../x")` 在盘根附近可能漏判，需按大小写不敏感 + 分隔符统一比较。
- 递归/并行 `task.run` 目前顺序执行，接口已留。
- 机器坑：`cc.exe` 不在 PATH 上就**静默 exit 1**；TMPDIR 不能指向含中文路径。

## 复现

```sh
cd vxa && bash build.sh test        # 单元套件
bash build.sh                       # build/vxa.exe（静态）
bash build.sh release               # -Os -flto -s，体积优先
./build/vxa.exe lang                # 语言速查（注册表生成，不会与实现漂移）
./build/vxa.exe doctor              # 环境自检，一行
```
