# VXA

一门**只给 AI Agent 用**的语言和工具链。C17 实现，零第三方依赖，单个静态可执行文件。

人类语言（如本仓库旁的 Vox）追求"语言适应人的直觉"：容忍歧义、自动补全、永不崩溃。
VXA 反过来：**零猜测、机器可读输出、可确认的副作用、按字节计价**。
理由很简单——Agent 的成本不是 CPU，是 `往返次数 × 每轮 token`，以及带着错误前提继续跑。

```sh
bash build.sh            # build/vxa.exe
bash build.sh release    # 体积优先（-Os -flto -s）
bash build.sh test       # 全部单元套件
./build/vxa.exe lang     # 一条命令学会全语言
./build/vxa.exe doc fs   # 内建函数签名表（由注册表生成，不会与实现漂移）
```

## 五件事

1. **上下文获取** `fs.outline` / `fs.bundle --query` — 只喂骨架和命中行，不喂全文；
   `fs.read --lines a-b --grep s --max-bytes N`，超限时 `truncated=t` 并给 `cursor=`，
   绝不给出看起来完整的残缺内容。
2. **锚点编辑** 行锚点 = 上下文内容哈希（前一行/本行/后一行，**不含行号**）。
   在上游插一行不会让其余锚点失效；锚点失配直接报 `ANCHOR_MISS` 并附候选行，
   而不是悄悄改错地方。`tx.patch` 每条 op 都回读改后行内容（`checks=[{at,ok,line,preview}]`），
   Agent 不需要再读一遍确认。
3. **两阶段确认** 副作用先返回计划：`token` + `files=[{path,kind,from,to,bytes}]`，
   不碰磁盘。新建工作区内文件立即执行并出回执；**替换/删除已有内容必须令牌**。
   令牌绑定文件旧哈希（中途被改则失效）且一次性（`journal.ndjson` 记账，重放不重复执行）。
   Agent 无法回答提示，所以"确认"就是带 `--confirm <token>` 重放同一脚本。
4. **命令执行** `sh.run(["go","test","./..."])` 走 argv，不经 shell；
   `sh.ro(["git","status"])` 白名单按 `argv[0..1]` 精确匹配，
   所以 `git status; rm -rf x` 这种字符串绕过无效；字符串形式恒需确认。
   输出默认截尾 + 密钥脱敏，`code=0` 是数据不是结论。
5. **硬止损** 同一 (脚本, 参数, 退出码, 首错误码) 连续第三次 → `LOOP_DETECTED`。
   Agent 最容易浪费的正是重复同一个失败步骤。

## 约定

- stdout 只有机器可读结果：`k=v,k2=v2`，嵌套保留 `{}`/`[]`，`null`→`-`，bool→`t/f`；
  体积自述用 `bytes=/lines=/shown=/truncated=/est=`（`est` 明确是估算，token 数不可测量）。
- 退出码即控制流：`0 成功 1 用法 2 脚本 3 需确认 4 策略 5 IO 6 内部 7 超时`。
- 错误是值，带稳定 `code=` 和一行 `hint=`；**宁报错不猜测**：没有模糊语法匹配、
  没有括号补全、没有参数乱序、没有多语关键字。
- 写操作永远在工作区监狱内；删除进 `.vxa/trash/` 可 `fs.restore`，不硬删。

深入阅读：`docs/LEARN.md`（给 Agent 的学习文档，短）、`SPEC.md`（规格与决策依据）、
`docs/STATUS.md`（构建状态、已知缺陷、为什么这样取舍）。
