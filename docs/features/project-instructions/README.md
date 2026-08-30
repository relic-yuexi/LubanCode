# 项目指令

[文档首页](../../README.md) · [命令手册](../../reference/commands.md) · [架构说明](../../architecture/README.md) · [提示词模块](../../../src/prompts/README.md)

`AGENTS.md` 写仓库规矩。它告诉代理项目怎么搭、命令怎么跑、哪些边界不能碰。它不是聊天记录，也不是答话口吻。

主代理和子代理都读这套指令。新会话启动时读一次；`/init` 新建或发现现有文件后，会当场重载，不必 `/clear`，也不必重启。

## 1. 一分钟上手

进入仓库，执行：

```text
/init
```

随后打开生成的 `AGENTS.md`，把三件事写准：

1. 源码、测试、生成物各放哪儿。
2. 构建、格式化、测试该跑什么命令。
3. 哪些文件不能改，哪些改动必须补测试。

写完重新执行 `/init`。程序见文件已在，不会覆盖，只会重载。

## 2. `/init` 写到哪里

LubanCode 从当前目录往上找 `.git`：

- 找到 Git 根，就在根目录写 `AGENTS.md`。
- 找不到，就把当前目录当项目根。
- 根目录若已有非空 `AGENTS.override.md`，优先认它。
- 否则若已有 `AGENTS.md`，也不覆盖。

脚手架会查看根目录文件，猜一套起步命令：

| 标志文件 | 生成的命令 |
| --- | --- |
| `CMakeLists.txt` | `cmake -S . -B build`、`cmake --build build`、`ctest --test-dir build --output-on-failure` |
| `package.json` | `npm install`、`npm run build`、`npm test` |
| `Cargo.toml` | `cargo build`、`cargo test` |
| `go.mod` | `go build ./...`、`go test ./...` |
| `pyproject.toml` 或 `pytest.ini` | `python -m pip install -e .`、`python -m pytest` |
| `Makefile` | `make`、`make test` |

没认出项目类型时，脚手架只留待填说明，不凭空编命令。生成结果是一张底稿，仍要由项目维护者校准。

## 3. 查找顺序

程序从项目根一路走到目标。启动会话时目标就是当前工作目录；写文件时目标是那个文件——写前闸按文件的祖先链重新解析，嵌套的 `AGENTS.md` 就算在仓库根启动也能生效。每层只取一份：

1. 非空 `AGENTS.override.md`。
2. 若没有非空 override，再取非空 `AGENTS.md`。
3. （显式配置 `project_doc_fallback_filenames` 后）两枚主名都没命中时，按名单顺序取第一份非空文件。

根目录先拼，近处目录后拼。越靠近目标，越能补充或压过上层规矩。

```text
repo/
  AGENTS.md                 # 全仓通则
  src/
    AGENTS.override.md      # src 内采用这份，不读同层 AGENTS.md
    parser/
      AGENTS.md             # parser 再添细则
```

改 `repo/src/parser/token.cpp` 时，生效链如下（无论会话从仓库根还是从 `src/parser` 启动）：

```text
repo/AGENTS.md
repo/src/AGENTS.override.md
repo/src/parser/AGENTS.md
```

`override` 只压过同层 `AGENTS.md`，不会抹掉父目录指令。空文件跳过。路径会写进拼装后的标题，模型知道每段从哪儿来。

首次往一个新作用域写文件时，写前闸先拦一次，把该链的完整规则随工具结果注入，模型读过原样重试即放行——第一次拦住是协议握手，不是错误。`AGENTS.md` 被改动后指纹即变，下一次写会重新握手。

## 4. 32 KiB 上限与计费口径

上限 32 KiB 管"段间分隔 + 来源标题（`## Instructions from ...`）+ 正文"三样合计。拼完后再在最前头加 `# Project Instructions` 与一行固定说明（约 100 bytes）——这截包装**不计入帽**，最终串可略超 32 KiB，超出量即包装本身。

系统提示里的基线投影沿这条旧口径：达到上限时沿 UTF-8 字符边界截断，免得切坏中文，后续文件不再读入，状态明标"截断"，不冒充"全部已加载"。

写前闸另按"整份文档"计：目标链在预算内装不下时**直接拒写**并说明每份多大、总额多少、怎样拆——不腰斩（截断在半条禁令中间比没有更糟）。

这有个实在后果：根目录若写成万言书，近处模块规矩可能挤不进来。通则写短，细则下沉。大段背景材料放普通文档，再从 `AGENTS.md` 指明何时去读。

## 4b. 查账与诊断

```text
/instructions                      看 cwd 基线:逐 source 一行(路径/类型/字节数/摘要/最近标注)
/instructions path src/a.cpp       看目标链(嵌套 AGENTS.md 从仓库根也能查)
/instructions reload               显式重载,会话立即采用
/doctor instructions               cwd 基线全账 + 计费口径 + 分型诊断
```

诊断分型，不再把"打不开"与"空文件"都写成没发现：

| code | 含义 |
| --- | --- |
| `empty_skipped` | 文件在但为空，跳过 |
| `shadowed_same_directory` | 同层非空 override 压住了这份 `AGENTS.md` |
| `read_error` | 文件在却读不动（权限/短暂 I/O 错） |
| `invalid_utf8` | 内容不是合法 UTF-8，拒收 |
| `symlink_outside_project` | 符号链接解析到项目外，拒读 |
| `symlink_broken` | 符号链接断链或成环 |
| `symlink_inside_project` | 链到项目内：允许，账里同时记 link 与真实路径 |
| `over_budget` | 拼装投影撞了字节帽，有文档没装下 |
| `fallback_used` | 显式配置的 fallback 文件名命中 |
| `migration_hint` | 发现 `AGENT.md`/`CLAUDE.md`/`GEMINI.md`，只提示不读 |

诊断输出只列路径与状态，不泄正文。

## 5. 推荐结构

```markdown
# Repository Guidelines

## Project Layout
- Product code lives in `src/`; tests mirror it under `tests/`.
- Do not edit generated files under `build/`.

## Build and Test
- Configure: `cmake -S . -B build`
- Build: `cmake --build build --config Release`
- Test: `ctest --test-dir build -C Release --output-on-failure`

## Working Agreements
- Keep protocol parsing in `src/api/`.
- Preserve unrelated changes in a dirty worktree.
- Add a regression test for every user-visible bug fix.
- Report checks that could not be run.
```

一条规矩最好能回答“何时、在哪儿、做什么”。例如“改 SSE 时补 `tests/test_*_events.cpp`”就比“确保高质量”有用。

## 6. 分层写法

根目录放全仓通则：

- 构建与测试入口。
- 代码生成规则。
- 安全边界。
- 提交与发布规矩。

模块目录放局部细则：

- 该模块的抽象边界。
- 专属测试命令。
- 文件命名与兼容约束。
- 必须阅读的设计文档。

临时迁移期间，可用 `AGENTS.override.md` 盖住同层旧规则。迁移完就删，免得后来的人只见新规，不知为何压住旧规。

## 7. 适合写什么

适合写：

- 可直接执行的命令。
- 路径与模块归属。
- 测试门槛。
- 用户已有改动如何保留。
- 生成文件、密钥、迁移数据等禁区。
- 格式化、兼容性、文档同步要求。

不适合写：

- API key、令牌、内网口令。
- 一次会话才有用的临时安排。
- 长篇产品介绍。
- 与仓库无关的个人偏好。
- 互相打架、又不说明作用域的命令。

`AGENTS.md` 通常会进 Git。凡写进去的字，都按公开给项目协作者看待。

## 8. 与其他提示层的关系

| 文件 | 职分 |
| --- | --- |
| `~/.lubancode/system_prompt.md` 或自定 system prompt | 代理身份与总工作方式 |
| `~/.lubancode/SOUL.md` | 口吻、措辞、个人偏好 |
| `~/.lubancode/AGENTS.md` | 跨仓库的通用工作法（全局层，优先级最低，项目层永远能盖过） |
| `AGENTS.md` | 仓库规矩 |
| `SKILL.md` | 某类任务的具体流程与资源 |

全局层（`~/.lubancode/AGENTS.md`）与 SOUL 分工明确：SOUL 管"怎么说"，全局 AGENTS 管"怎么干活"（比如"改 POSIX 代码先用 WSL 验证"这类对所有仓库都成立的规矩）。它垫在项目根之前，最先拼进提示、最先被预算挤掉；不创建这份文件就没有这一层，行为与从前一字不差。

项目指令不能越过程序权限。它可以要求模型调用工具，却不能绕开确认、路径校验、命令黑名单和输出上限。

## 9. 当前会话何时生效

- 新启动会话：启动时加载。
- `/init`：无论新建还是发现现有文件，都会重建主代理提示。
- `/instructions reload`：同 `/init` 的重载线，随后亮出新基线账。
- `/init` 之后调用子代理：子代理拿到新指令。
- 在外部编辑器里直接改文件：系统提示里的基线不自动重读（重跑 `/init`、`/clear` 或重启）；写前闸侧按"惰性发现"办——下一次写文件时先查 mtime，变了就重读规则、重新握手。改的是"确认权"而非"提示词"，所以不需要重启就能挡住没看新规则的写入。

已经写进历史的旧系统提示不会逐条改写。后续请求使用重建后的提示。

## 10. fallback 文件名与迁移

默认只认 `AGENTS.override.md` 与 `AGENTS.md`。想让它顺手读别的文件（比如团队只有一份 `TEAM.md`），在配置文件里显式点名：

```json
{ "project_doc_fallback_filenames": ["TEAM.md", "CONTRIBUTING.md"] }
```

每层先按老规矩选主名，两枚主名都没命中（不在/读错/空）才按名单顺序取第一份非空文件，命中记 `fallback_used` 诊断。名单元素必须是纯文件名，不得与主名撞车。默认空 = 不启用——多读规则文件必须用户显式点名。

仓库里若有 `AGENT.md`（少个 S）、`CLAUDE.md`、`GEMINI.md`，`/instructions` 与 `/doctor instructions` 会给一条 `migration_hint`，只提示、不自动读——默认把四五套规则全拼进提示词只会平白制造冲突。要迁移，把规矩并进 `AGENTS.md`。

## 11. 排错

**子目录指令没生效**

先跑 `/instructions path <文件>` 看目标链。写前闸按目标文件解析，嵌套 `AGENTS.md` 会自动进链；若链上没有，检查文件名与所在层级。

**同层 `AGENTS.md` 没读**

看看同层是否有非空 `AGENTS.override.md`。有它便只取 override。`shadowed_same_directory` 诊断会说清。

**`/init` 没覆盖旧文件**

这是刻意的。项目规矩不能由脚手架悄悄洗掉。手工编辑旧文件，再跑 `/init` 重载。

**近处规矩不见了**

多半撞上 32 KiB。`/instructions` 会标"截断"并列出没装下的文档；写前闸遇超预算链会拒写并说明每份多大。删短根目录长文，把背景移到 `docs/`。

**找错项目根**

确认上级目录哪一层有 `.git`。Git worktree 的 `.git` 可以是文件，程序同样按存在处理。
