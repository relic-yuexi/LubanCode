# Agent 与 Prompt Profile 契约

[文档首页](../README.md) · [功能目录](../features/README.md) · [扩展指南](../features/extensions/README.md) · [Workflow Schema](workflow-schema.md) · [配置手册](configuration.md)

本页定两样东西的格式契约：Agent Definition（一份 YAML，写谁来做、带什么能力）
与 Prompt Profile（一套稀疏覆盖，写系统提示词怎样拼）。

本页先于实现落纸（2026-08-28）。解析器、目录与命令分阶段实现；落地之前，本页
写的命令与扫描行为尚未可用。第 8 节的对照表钉住现有代码字段名，实现不得另造名。

## 1. 术语边界

LubanCode 里有几套东西，各管一摊，不要混账。

| 名称 | 管什么 | 不管什么 |
| --- | --- | --- |
| Agent | 谁来做、带什么上下文、用什么模型和工具、受什么限制 | 不实现工具，不收藏大段通用教程 |
| Prompt Profile | Agent 怎样拼系统提示词 | 不调度步骤，不授予权限 |
| Skill | 某类事怎样做，可带说明、脚本、模板和资料 | 不占一套独立上下文，不决定模型 |
| Workflow | 先做什么、后做什么、何时分支 | 不另造一套 Agent 运行时 |
| Plugin | 可执行工具组件，承载 native、Lua 或 process 工具 | 不打包 Agent、Skill、Workflow |
| Package | 安装与分发单位，可收纳 Agent、Prompt、Skill、Workflow、Plugin、MCP | 不参与执行，不授予权限 |
| MCP | 把外部工具接进来 | 不决定谁能调用工具 |

关系如下：

```text
Package -> Skill -------------+
       -> Plugin tools -------+
       -> MCP tools ----------+----> Agent Definition
       -> Prompt Profile -----+
Model catalog ----------------+
                               |
                               v
                         Resolved AgentProfile
                               |
                         Agent + AgentLoop
                               |
                  AgentTool / Workflow Executor
```

一句话：Agent 选能力，也选 Prompt；Skill 教它办事；MCP 给它工具；Workflow
排它出场。

### 1.1 父代理怎样交任务

`agent` 工具的新调用只收一条任务正门：

```json
{
  "title": "复核迁移安全",
  "prompt": "背景：……\n\n## 任务\n……\n\n## 报告\n……",
  "agent_type": "general-purpose"
}
```

`title` 给面板与日志看，须短小；`prompt` 给子代理干活，由父代理临场写成。
子代理看不见父会话，父代理须把它当作刚走进屋子的聪明同事：交代目的与缘由、
已经查明或排除的事、必要路径/行号/错误/命令、任务边界，以及收工要带回什么。
“背景—任务—报告”只是一副推荐骨架，一句自包含任务也能派出。宿主不解析
Markdown，不把正文拆成 `goal/context/scope/acceptance/deliverable`，也不拿模板
重写它。

角色、工具、权限、模型、预算、`effective_cwd` 与 worktree 隔离另属宿主运行账，
进 system 环境与执行快照，不混进任务语义合同。旧 session/trajectory 里的嵌套
`task` 参数仍可读取、展示与导出，但不可重放成新调用。

`solutions/` 只作教程、配方、样例集合，不进运行时类型系统。Package 外壳见
`todos/统一Package封装与组件挂载系统设计.todo`（`docs/packages.md` 落纸后以它为准）。

### 1.2 派工任务书六件套模板

大单子（修 `todos/` 单、多步改码）光靠"背景—任务—报告"三段套话容易裸奔：无环境实情、
无纪律、无验收线，子代理全程自己摸环境。对照实战一次跑成的任务书，推荐六件套排布：

```text
## 单子路径
（任务出自哪张 todos/*.todo 单或哪段需求；没有单子就写需求出处）

## 范围红线
（只许动哪些文件/模块；哪些明确不许碰；本单不做版本号改动之类的边界）

## 环境实情
（本机构建环境——宿主会自动在派工 prompt 尾部附 [宿主注入·本机环境附录]
（CMake preset、离线 _deps 拷贝路、ctest 临时 USERPROFILE、动头文件
--clean-first），照附录办即可；此处只写本单特有的环境实情）

## 纪律
（提交信息语言与署名、不 push 不合 main、只勾真验证过的批次等）

## 完工标准
（怎样算修完：测试全绿、回归零、验收命令过、新增测试钉住）

## 回报格式
（完工回报带什么：分支名、commit 号、ctest 汇总原文一行、落点 文件:行、
新增测试册清单；半路卡住照样回报卡点）
```

模板只引导不强制。派工时给 `agent` 工具加可选参数 `template: "full"`，宿主直接给
prompt 套这副六件套引导壳（任务原文逐字节保留，缺的项提示子代理补齐或问明）；
环境附录由宿主自动探测注入（会话启动探测一次并缓存，重复派工不重复探测），不占
任务正文。`/help` 里也留了这条模板的出处指引。

## 2. 定义放哪里

一个 Agent 一份 YAML，单文件。不要套 `browser-tester/agent.yaml` 这类二层目录。
Agent 文件不收藏脚本与资料——那些归 Skill 或 Plugin。单文件便于列举、覆盖、
校验和展示来源。

```text
~/.lubancode/agents/browser-tester.yaml          用户级
<project>/.lubancode/agents/browser-tester.yaml  项目级
<embedded resources>/agents/*.yaml               内置（general-purpose、Explore）
<package>/agents/*.yaml                          Package 内（见第 11 节）
```

文件名须与 `name` 字段一致（去 `.yaml` 后缀）。不一致时以 `name` 为准，给
warning，日后可升为错误。

加载优先级：**project > user > builtin**。同层重名直接报错；跨层同名属于显式
覆盖，`/agent inspect` 须列出被盖住的来源。首版不做热重载：启动时加载，另有
显式 reload 命令。

## 3. YAML 总览

完整样例收在 `tests/fixtures/agents/complete.yaml`，最小样例收在
`tests/fixtures/agents/minimal.yaml`。一份全字段定义长这样：

```yaml
schema: 1
name: browser-tester
description: 查验网页应用，复现界面故障，回报带截图的证据。

prompt:
  profile: browser-tester
  project_instructions: inherit
  soul: inherit

model:
  role: inherit
  effort: inherit

skills:
  preload:
    - browser-testing

tools:
  allow:
    - read_file
    - search
    - context_read
    - todo_write
    - mcp__browser__navigate
    - mcp__browser__screenshot
  deny:
    - run_command

mcp_servers:
  - browser

requires:
  tools:
    - mcp__browser__navigate
    - mcp__browser__screenshot

runtime:
  max_output_tokens: 8192
  max_turns: 24
  max_context_chars: 600000
  context_window_tokens: 0
  length_continuations: 1
  execution_mode: auto
  isolation: none

permissions:
  mode: inherit
```

最小可用定义只要三行：

```yaml
schema: 1
name: note-taker
description: 记录会话要点，整理成简短纪要。
```

首版不收 Markdown 正文，也不收 `developer_instructions`。临时提示词由调用
任务传入；稳定提示词放 Prompt Profile；可复用做法放 Skill。

## 4. 逐字段规矩

### 4.1 顶层字段

| 字段 | 必填 | 类型 | 默认 | 约束 |
| --- | --- | --- | --- | --- |
| `schema` | 是 | 整数 | 无 | 只收 `1`，见第 7 节 |
| `name` | 是 | 字符串 | 无 | 稳定 ID，规矩见下 |
| `description` | 是 | 字符串 | 无 | 1 到 1024 字符 |
| `prompt` | 否 | 映射 | 全继承 | 见 4.2 |
| `model` | 否 | 映射 | 全继承 | 见 4.3 |
| `skills` | 否 | 映射 | 无预装 | 见 4.4 |
| `tools` | 否 | 映射 | 全继承 | 见 4.5 |
| `mcp_servers` | 否 | 字符串数组 | 空 | 见 4.6 |
| `requires` | 否 | 映射 | 无 | 见 4.7 |
| `runtime` | 否 | 映射 | 逐字段继承 | 见 4.8 |
| `permissions` | 否 | 映射 | `mode: inherit` | 见 4.9 |

`name` 用小写 kebab-case：1 到 64 字符，只用小写字母、数字、单横线；横线不
可顶头、收尾或连写（与 Skill 同规矩）。它是稳定 ID，改了名等于换了 Agent。

`description` 写何时派它出场，不写宣传话。主 Agent 派活就靠这一句。

### 4.2 prompt

| 字段 | 必填 | 类型 | 默认 | 取值 |
| --- | --- | --- | --- | --- |
| `profile` | 否 | 字符串 | 继承父 | Profile 名，`default` 或 kebab-case 名 |
| `project_instructions` | 否 | 字符串 | `inherit` | `inherit`、`omit` |
| `soul` | 否 | 字符串 | `inherit` | `inherit`、`off` |

`profile` 省略 = 继承父 Agent 当前 Profile；父也没有，落回 `default`。显式写
`default` = 强制用内置默认，不继承。Profile 本身见第 6 节。

`project_instructions` 只收 `inherit`、`omit`。省了等于 `inherit`。

`soul` 首版只收 `inherit`、`off`。不要在 Agent 文件里另塞 Soul 正文——Soul 归
`souls/` 目录与 `/soul` 命令管。

### 4.3 model

| 字段 | 必填 | 类型 | 默认 | 取值 |
| --- | --- | --- | --- | --- |
| `role` | 否 | 字符串 | `inherit` | `inherit`、`normal`、`cheap`、`lao`（`plan` 是 `lao` 别名） |
| `effort` | 否 | 字符串 | `inherit` | `inherit`，或 provider 声明的思考档 |

`role` 引用现有三档模型角色（`/model roles` 可查）。`inherit` 沿用父 Agent
的有效模型。未配置的角色回落 `normal`，照抄现有路由链。

`effort` 引用 provider 声明的思考档（provider 的 `supported_think_levels`）。
解析时对照 provider 能力，越界报错，不悄悄降档。

### 4.4 skills

| 字段 | 必填 | 类型 | 默认 | 取值 |
| --- | --- | --- | --- | --- |
| `preload` | 否 | 字符串数组 | 空 | Skill 名，去重保序 |

`preload` 里的 Skill 在 Agent 启动时把正文装进上下文。名字须真实存在；缺了
标 unavailable，不静默跳过。

### 4.5 tools

| 字段 | 必填 | 类型 | 默认 | 取值 |
| --- | --- | --- | --- | --- |
| `allow` | 否 | 字符串数组 | 继承父 | 完整工具名，非空字符串 |
| `deny` | 否 | 字符串数组 | 继承父 | 完整工具名，非空字符串 |

只收完整工具名：内置工具用裸名（`read_file`、`run_command`、`todo_write`），
MCP 工具用 `mcp__<server>__<tool>`，插件工具用 `plugin__<plugin>__<tool>`。
首版不做 glob。

语义：

- 只写 `allow`：白名单，名单外的全不可见。
- 只写 `deny`：黑名单，其余继承父。
- 都写：先 `allow` 后 `deny`；交叠时 **deny 胜出**。
- 都不写：全继承父 Agent 的有效工具表。
- `allow` 点到一个父 Agent 没有的工具，不算授权；解析时报清楚（见 9.2）。
- 两份数组各自去重、保住原次序。

`allow: []`（空数组）不合法。要表达"不给工具"，写 `allow` 并列出确要保留的
名字；空数组与"不写"分不清意图，按格式错报。

### 4.6 mcp_servers

字符串数组。只引用**已配置、已信任**的服务名——用户或项目 `config.json` 里
`mcpServers` 的键，或所在 Package `mcp/` 目录的 id。

不得内联 `command`、`args`、env 或密钥。Agent 文件不是 MCP 启动器。元素不是
字符串时，判别有次序：元素是映射且带 `command`、`args`、`env` 任一键，报
`agent.mcp_inline`；其余非字符串元素报 `agent.type_mismatch`。

### 4.7 requires

| 字段 | 必填 | 类型 | 默认 | 取值 |
| --- | --- | --- | --- | --- |
| `tools` | 否 | 字符串数组 | 空 | 完整工具名，非空字符串 |

`requires.tools` 只作启动前断言：这些工具必须在 Agent 的有效工具表里。缺项
就报错，绝不悄悄放宽工具，也不退化成全工具 Agent。

它与 `tools.allow` 分工不同：`allow` 是过滤，`requires` 是断言。`requires.tools`
与 `tools.deny` 交叠是自相矛盾，解析时报错。

### 4.8 runtime

对齐现有 `AgentRuntimeProfile`（`src/agent/runtime_profile.hpp`），字段名一
致，不另造。

| 字段 | 必填 | 类型 | 默认 | 语义 |
| --- | --- | --- | --- | --- |
| `max_output_tokens` | 否 | 正整数 | 继承三级解析 | 显式声明输出上限 |
| `max_turns` | 否 | 非负整数 | 走宿主默认（`subagent.default_max_turns`，未设 = `0` 不限） | **任务总 turn**：从接到任务到交回终态，最多准入几次逻辑模型请求；父代理补话、孩子回信、Stop 钩子续跑共一本账，`0` = 不限 |
| `max_steps_per_turn` | 否 | 非负整数 | 继承父 | **已弃用**（legacy）：每个 input round （一次 `Agent::Run()`）各自的模型请求数上限，续投会重领额度；单独使用仍按旧义生效并给 `agent.legacy_step_budget` 警告，与 `max_turns` 同现按 `agent.turn_budget_conflict` 拒载 |
| `max_context_chars` | 否 | 正整数 | `600000` | history 字符安全网 |
| `context_window_tokens` | 否 | 非负整数 | 继承父 | 上下文窗口 token 数，`0` = 未知 |
| `length_continuations` | 否 | 非负整数 | `1` | max_tokens 打断在思考段时的续跑次数，`0` = 不续 |
| `execution_mode` | 否 | 字符串 | `auto` | `auto`、`foreground`、`background` |
| `isolation` | 否 | 字符串 | `none` | `none`、`worktree` |

`max_output_tokens` 省略时走现有三级解析：config 显式 > provider 声明 > 模型
目录声明，全缺席交服务端默认。Agent YAML 里写了正整数，视同 config 级显式
声明。

预算合同的迁移（turn 预算单 §5）：`runtime.max_turns` 是任务总闸——一道闸
管到终态，续投、孩子回流、Stop 钩子续跑都从这本账扣；`max_steps_per_turn`
是兼容窗里的旧键，只限单个 input round，已弃用。旧定义迁移时把
`max_steps_per_turn: N` 改写成 `max_turns: M`——语义从"每轮各自 N"变"整任务
合计 M"，按任务实际规模调数值；两者同现直接拒载（`agent.turn_budget_conflict`），
不静默选边。`/agent doctor <名字>` 列明当前生效路并给迁移建议，
`/agent inspect <名字>` 给可复制的迁移片段。

`execution_mode` 是这份定义的**缺省执行档**：调用方（`agent` 工具、Workflow
节点）显式给值时压过它。取值与 `agent` 工具现有参数一致。

`isolation: worktree` 表示默认住进隔离 git worktree，写操作不越出房门，语义
同现有子代理隔离。

### 4.9 permissions

| 字段 | 必填 | 类型 | 默认 | 取值 |
| --- | --- | --- | --- | --- |
| `mode` | 否 | 字符串 | `inherit` | `inherit`、`default`、`accept_edits`、`yolo`、`auto`、`dont_ask`（`confirm` 兼容为 `default`，已弃用） |

权限不能用单一 rank/min 排序：父子按两部分求交——可自动执行的能力集合取交集，是否保留询问资格也取交集。`inherit` 原样继承父档。于是父 `yolo`（自动能力为 All）配子 `default` 时，子定义会收窄为默认审批并可向主会话询问；父 `dont_ask` 配任何会询问的子档时，询问资格被拿掉，未获自动许可的动作直接拒绝。父 `auto` 配子 `accept_edits` 只自动接受文件编辑。工具白名单仍另行取交集，不能借档位拿回父 Agent 没有的工具。

子 Agent 想要"只读"，不用另造权限档——用 `tools.allow` 收成只读白名单（现有
Explore 即此做法）。

## 5. 严格解析

解析用 `yaml-cpp`。不拿 Skill 那套简易 frontmatter 扫描器凑。

- 未知字段报错，不可静默吞掉。
- 类型不合报文件、行、列。
- 缺必填时报字段路径与期望类型。
- 同层重名报冲突来源。
- 枚举值非法报字段路径与合法取值表。
- 数组去重，同时保住原次序。
- 所有路径经规范化与越界检查（`..`、符号链接、绝对路径逃逸）。
- 诊断打印来源层（builtin / user / project / package），不打印密钥与环境变量值。

## 6. Prompt Profile

### 6.1 Profile 不是一段字符串

Prompt Profile 是一组**稀疏覆盖**。它沿用 `src/prompts` 现有模块树，只改自己
点名的文件，没点名的模块原样走默认。

```text
src/prompts/profiles/browser-tester/core/10-identity.md   内置 Profile
~/.lubancode/prompts/profiles/browser-tester/core/10-identity.md   用户覆盖
<project>/.lubancode/prompts/profiles/browser-tester/core/10-identity.md   项目覆盖
<package>/prompts/profiles/browser-tester/core/10-identity.md   Package 覆盖
```

没有 `profiles/default/` 也能跑。现有 `src/prompts/{core,features,platforms}`
就是隐式 `default`，老配置不受影响。

### 6.2 覆盖与拼装次序

同一模块按下列次序找。越往后，权越大：

```text
内置 default 模块
-> 用户全局 default 覆盖
-> 内置选中 Profile 覆盖
-> 用户选中 Profile 覆盖
-> 项目选中 Profile 覆盖
```

`system_prompt.md` 与 CLI persona 仍可替换 core。它们只替 core，不抹掉动态
环境、有效能力说明、platform、mode、model instructions、tool index 和 Soul。

系统提示的拼装总次序：

```text
resolved core/persona
-> runtime environment
-> project instructions（若继承）
-> effective feature modules
-> platform module
-> host-owned mode module
-> deferred tool index
-> model instructions
-> Soul（若启用）
```

这条次序要写成测试，不可只写在注释里。

### 6.3 哪些可以换，哪些不能换

Agent Profile 可以换：

- core 模块。
- feature 模块文案。
- platform 文案。
- 是否继承 project instructions。
- 是否启用 Soul。

Agent Profile 不可以换：

- `modes/`。Mode 是宿主策略，项目 Agent 改写不得。
- 模型能力与模型专属指令。它们仍从 `models.json` 与 provider 解析。
- 工具 schema。它归工具注册表。
- 权限判断、沙箱、审批、步数硬上限。

### 6.4 只给有效能力配说明

`PromptAssembler` 眼下常驻装入 files、shell、delegation、todo 等 feature 文案。
自定义 Agent 一旦裁掉工具，便说一套、做一套。

为此引入 `PromptCapabilities`：从过滤后的有效 ToolRegistry 推导。

```text
effective tools -> PromptCapabilities -> feature modules
```

例：Agent 没有 `run_command`，就不装 shell feature；没有 `agent` 工具，就
不装 delegation feature；没有 `todo_write`，就不装 todo feature。

这条首版只用于自定义 Agent。验证稳了，再决定是否收拢主 Agent 旧逻辑。

### 6.5 来源账本

解析结果不只是一份最终字符串，须带一份 `PromptSourceLedger`：

```text
core/10-identity.md    <- project profile browser-tester
core/20-workflow.md    <- user global default
features/web.md        <- embedded profile browser-tester
platforms/responses.md <- embedded default
modes/default.md       <- embedded host policy
```

这份账本供 `/agent inspect`、`/prompt`、日志与测试使用。出了 Prompt 覆盖
问题，一眼看见是谁压了谁。

## 7. schema 版本与兼容

写法定死：顶层 `schema: <整数>`。当前唯一合法值 `1`。

- **未知高版本**（如 `schema: 2`）：拒绝加载，报错附升级提示。不降级猜解。
- **非整数**（如 `schema: "1"`、`schema: 1.0`）：按类型错报。
- **加字段**：只许加可选字段，不动版本号。老程序遇新字段按"未知字段"拒绝并
  提示升级——这是有意的，宁拒勿猜。加字段须同时补 schema 测试与本页文档。
- **改字段**：改名、改类型、改语义、删字段、改默认值行为，一律升版本号。
  旧版本号保留至少一代的解析能力，再旧的拒绝。

Agent 定义不写进会话历史正文。历史只记稳定 `name`、定义来源摘要与解析后的
版本标识。reload 后同名 Agent 变了，已运行的持旧快照，新任务才用新定义。

## 8. YAML 字段与 C++ 对照

实现以现有代码字段名为准。下表逐条对齐；有出入处以本表为准改 todo 样例。

| YAML | C++ 落点 | 说明 |
| --- | --- | --- |
| `schema` | （解析器层，无对应字段） | 版本门 |
| `name` | Catalog 键；运行时即 hooks 与面板的 `agent_type` | `src/tools/agent_tool.hpp` |
| `description` | Catalog 元数据；`agent` 工具 schema 的类型描述 | 同上 |
| `prompt.profile` | `PromptProfileResolver`（规划中） | overlay roots 喂给 `AssembleSystemPrompt` |
| `prompt.project_instructions` | `agent::PromptOptions::project_instructions` | `src/agent/prompt_assembler.hpp` |
| `prompt.soul` | `AgentProfile::soul` | `src/agent/agent.hpp` |
| `model.role` | `ModelRolesConfig{normal,cheap,lao}` 解析出 `AgentProfile::provider` + `AgentProfile::request.model` | `src/config/config.hpp`、`src/agent/model_router.hpp` |
| `model.effort` | `AgentProfile::request.reasoning_effort`（`api::RequestProfile`） | `src/api/types.hpp` |
| `skills.preload` | `agent::PromptOptions::skills_segment` | `src/agent/prompt_assembler.hpp` |
| `tools.allow` / `tools.deny` | `AgentProfile::tool_filter` / `AgentProfile::tool_filter_denial` | `src/agent/agent.hpp` |
| `mcp_servers` | MCP 服务引用 -> 过滤 `mcp__<server>__*` 工具；段开关 `PromptSectionSwitches::mcp` | `src/agent/agent.hpp` |
| `requires.tools` | （解析期断言，无运行时字段） | Catalog / doctor |
| `runtime.max_output_tokens` | `AgentRuntimeProfile::max_output_tokens`（配 `max_output_tokens_source`） | `src/agent/runtime_profile.hpp` |
| `runtime.max_turns` | `AgentTurnBudgetProfile::max_turns`（Resolver 合并；任务注册时冻进 `TaskRecord::turn_account`） | `src/agent/turn_budget.hpp`、`src/tools/task_ledger.hpp` |
| `runtime.max_steps_per_turn` | `AgentRuntimeProfile::max_steps_per_turn`（legacy；`AgentTurnBudgetProfile::legacy_max_steps_per_input` 另记一笔） | 同上 |
| `runtime.max_context_chars` | `AgentRuntimeProfile::max_context_chars` | 同上 |
| `runtime.context_window_tokens` | `AgentRuntimeProfile::context_window_tokens` | 同上 |
| `runtime.length_continuations` | `AgentRuntimeProfile::length_continuations` | 同上 |
| `runtime.execution_mode` | `agent` 工具 `execution_mode` 参数（`auto`/`foreground`/`background`） | `src/tools/agent_tool.cpp` |
| `runtime.isolation` | `tools::IsolationScope`（worktree 房） | `src/tools/isolation.hpp` |
| `permissions.mode` | `runtime::PermissionMode`（五档，见 §4.9） | `src/runtime/turn_runtime.hpp` |

反向也有一条边界：C++ 有、YAML 没有的字段，同样写明。

- `AgentProfile::system_prompt`：拼装结果，来自 Prompt Profile 与运行环境，
  不许在 Agent 文件里直写。
- `AgentProfile::model_instructions`：来自 `models.json`，Agent 文件写不得。
- `AgentProfile::deferred_index_provider`：请求期活口，宿主注入。
- `PromptSectionSwitches`（mcp/web/lsp/wire 四段开关）：解析器按有效工具与
  配置推导，首版不开放 YAML 直写。

合并档（阶段 3，`AgentProfileResolver` 是唯一权威，AgentTool 派发与 Workflow
绑定同一口进）：runtime 五枚预算字段各按
**入参显式 > YAML runtime > 父值/配置默认**落档；`max_output_tokens` 的 YAML
显式值视同 config 级声明（来源标 `ConfigFile`），YAML 缺席时父值连同来源枚举
原样继承；`context_window_tokens` 在父值之前另有一级"会话同步的窗口"（发轮
前再同步的活值）。合并期诊断用 §9.2/§9.3 的稳定码（`agent.tool_not_granted`、
`agent.permission_widening`、`agent.effort_not_supported`、
`agent.missing_dependency`），派发口结构化明拒，不悄悄放宽。

与 todo 样例的三处出入，已在本文档订正：

1. 工具名对齐现有注册表：命令工具叫 `run_command`（不叫 `shell`）；待办工具
   只有 `todo_write`，没有 `todo_read`。
2. `permissions.mode: read_only` 不进首版。现有权限档为 `default` / `accept_edits` / `yolo` / `auto` / `dont_ask`（另有 `inherit`）；只读限制用 `tools.allow` 表达。
3. `runtime` 补全 `AgentRuntimeProfile` 全部字段（`max_output_tokens`、
   `max_context_chars`、`context_window_tokens`、`length_continuations`），
   名字与 C++ 一字不差。

## 9. 诊断一览

### 9.1 解析错（读单份文件即知）

| 错误码 | 触发 | 夹具 |
| --- | --- | --- |
| `agent.schema_unsupported` | `schema` 不是 `1`（含未知高版本） | `invalid/schema-too-new.yaml` |
| `agent.missing_field` | 缺 `schema`、`name` 或 `description` | `invalid/missing-required.yaml` |
| `agent.bad_name` | `name` 不合 kebab-case 规矩 | `invalid/bad-name.yaml` |
| `agent.bad_description` | `description` 空白或超 1024 字符 | 无 |
| `agent.type_mismatch` | 字段类型不对（如 `schema` 写成字符串、`tools.allow` 写成字符串） | `invalid/type-error.yaml` |
| `agent.bad_enum` | 枚举字段收了非法值（如 `soul: custom`、`permissions.mode: read_only`） | `invalid/bad-enum.yaml` |
| `agent.unknown_field` | 未知字段（如顶层写 `system_prompt` 塞提示词正文） | `invalid/unknown-field.yaml` |
| `agent.mcp_inline` | `mcp_servers` 元素是映射且带 `command`/`args`/`env` 任一键 | `invalid/mcp-inline.yaml` |
| `agent.empty_list` | 数组字段写成 `[]`（`tools.allow`、`tools.deny`、`skills.preload`、`mcp_servers`、`requires.tools`） | `invalid/empty-list.yaml` |
| `agent.requires_conflict` | `requires.tools` 与 `tools.deny` 交叠 | `invalid/requires-conflict.yaml` |

### 9.2 解析错（合并父子、跨文件才知）

| 错误码 | 触发 |
| --- | --- |
| `agent.duplicate_same_layer` | 同一层目录两份定义同名 |
| `agent.tool_not_granted` | `tools.allow` 点到父 Agent 没有的工具 |
| `agent.permission_widening` | 旧版兼容诊断名；当前 `permissions.mode` 采用自动能力集合与询问资格求交，不因单一 rank 判定父子宽窄 |
| `agent.effort_not_supported` | `model.effort` 不在 provider 声明的思考档里 |

### 9.3 不可用（不报错，标状态）

| 状态码 | 触发 | 行为 |
| --- | --- | --- |
| `agent.missing_dependency` | 缺 Skill、MCP、工具或模型角色 | Agent 标 unavailable；调用时报缺什么，绝不退化成全工具 Agent |

### 9.4 警告

| 警告码 | 触发 | 夹具 |
| --- | --- | --- |
| `agent.name_mismatch` | 文件名与 `name` 不一致 | `invalid/name-mismatch.yaml` |

所有诊断带来源层与文件、行、列（能定位时）。不打印密钥与环境变量值。

## 10. 安全边界

- 项目 Agent 只能引用已启用、已信任的 Plugin、MCP 和工具。
- Agent 文件不得内联 MCP 启动命令、环境变量或密钥。
- 子 Agent 的权限、工具、沙箱只能继承或收窄。
- `allow` 点到父 Agent 没有的工具，不算授权；解析时报清楚。
- 依赖缺失时标 unavailable，不偷着降级。
- Prompt Profile 只改模型所见文字，改不了宿主 ModePolicy 与权限门。
- 每次解析保留定义来源、Prompt 来源、有效工具与有效权限，方便审计。

## 11. 与 Package 的咬合

Package 的 `agents/` 目录里，每份 Agent 一只 `.yaml`，格式与本页完全相同。

- 名字两枚：local id（`browser-tester`）与 canonical id
  （`moontide.browser-suite:browser-tester`）。包外引用写全名。
- 包内引用可写短名：`prompt.profile`、`skills.preload`、`mcp_servers` 先在
  本包里找，找不到再按显式全名解析。
- Package manifest 不重抄组件清单，也不得借清单放宽本页任何一条权限规矩。
- 整包校验有一处错，Agent 与同包其余组件一件也不挂载。

规矩全文见 `todos/统一Package封装与组件挂载系统设计.todo`。

## 12. 夹具

`tests/fixtures/agents/` 收三样：

```text
complete.yaml             全字段合法样例
minimal.yaml              最小合法样例
invalid/*.yaml            故意出错的诊断样例，错误码见第 9 节
```

夹具里不写真实密钥、账号与绝对路径。合法样例的工具名、服务名、Skill 名都是
演示用的占位名，解析测试只验格式，不依赖这些名字真实存在。
