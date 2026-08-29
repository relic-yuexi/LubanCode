# Package 与组件挂载

[文档首页](../README.md) · [功能目录](../features/README.md) · [扩展 LubanCode](../features/extensions/README.md) · [Agent 契约](agents.md) · [Workflow Schema](workflow-schema.md) · [安全模型](../development/security.md)

Package 是一只箱子。它只做五件事：

```text
发现 -> 校验 -> 记账 -> 挂载 -> 卸载
```

它不跑模型，不出工具，不授予权限。箱里装六类组件：Agent、Prompt Profile、Skill、Workflow、Plugin、MCP server，外加各组件要用的脚本、模板与资料。

本文是冻结合同。实现落地之前，本文与 `tests/fixtures/packages/` 下的夹具就是唯一权威：目录怎么摆、清单怎么写、名字怎么起、哪一步要信任，一概照此办理。设计出处见 `todos/统一Package封装与组件挂载系统设计.todo`；Agent 定义格式另见前置单 `todos/自定义Agent与PromptProfile设计.todo`。

自进化闭环（`todos/Package驱动的自进化闭环设计.todo`）产出的候选包，`package/` 一层照抄本文格式，可独立校验；进化账另落 `evolution.json`，不进清单。

## 1. 名字分层：Package 与 Plugin 不可同名

LubanCode 早有 Plugin 运行时：native ABI、Lua、process manifest、工具挂载、项目插件信任账。若把外层安装包也叫 Plugin，两层同名，命令、日志、错误、文档全要打架。

故此定死两层名字：

```text
外层：Package                    安装与分发单位，不参与执行
内层：Agent / Prompt / Skill / Workflow / Plugin / MCP    各自执行或供装配
```

- Package 是箱子。装货、记账、定身份。
- Plugin 仍是工具运行时，会跑代码。MCP 仍是协议服务，会起进程。Skill 是说明，Workflow 是编排，Agent 是运行配置，Prompt Profile 是提示词覆盖——都不出工具。
- Package 不得借清单越过现有工具权限、插件信任与 MCP 安全门。清单里写不出任何能力。

与 Skill、Workflow、Plugin 各自旧目录的关系：旧 standalone 目录照旧扫、照旧用，规矩一行不改。Package 是另一条路，不是替换。

## 2. 标准目录与 ComponentKind

一只完整 Package：

```text
browser-suite/
├── package.yaml
├── README.md
├── LICENSE
├── agents/
│   └── browser-tester.yaml
├── prompts/
│   └── profiles/
│       └── browser-tester/
│           ├── core/
│           │   └── 10-identity.md
│           └── features/
│               └── web.md
├── skills/
│   └── browser-testing/
│       ├── SKILL.md
│       └── references/
├── workflows/
│   └── smoke-test/
│       ├── workflow.yaml
│       └── prompts/
├── plugins/
│   └── dom-analyzer/
│       ├── plugin.json
│       └── runner.py
├── mcp/
│   └── browser/
│       ├── mcp.yaml
│       ├── server.js
│       └── package.json
├── assets/
└── docs/
```

六类组件定成枚举，实现照此落 `ComponentKind`：

```text
ComponentKind ::= agent | prompt_profile | skill | workflow | plugin | mcp_server
```

每类组件的目录与文件名规矩：

| kind | 目录 | 一件组件 | local id 取自 | 入口 |
| --- | --- | --- | --- | --- |
| `agent` | `agents/` | `agents/browser-tester.yaml` | 文件名去扩展名 | 该 YAML（格式见前置 TODO §4.1） |
| `prompt_profile` | `prompts/profiles/` | `prompts/profiles/browser-tester/core/10-identity.md` | profile 目录名 | 目录整体（稀疏覆盖，无单一入口） |
| `skill` | `skills/` | `skills/browser-testing/SKILL.md` | 目录名 | `SKILL.md` |
| `workflow` | `workflows/` | `workflows/smoke-test/workflow.yaml` | 目录名 | `workflow.yaml` |
| `plugin` | `plugins/` | `plugins/dom-analyzer/plugin.json` | `plugin.json` 的 `id`，须与目录名一致 | `plugin.json` |
| `mcp_server` | `mcp/` | `mcp/browser/mcp.yaml` | `mcp.yaml` 的 `id`，须与目录名一致 | `mcp.yaml` |

硬规矩：

- `package.yaml` 必须在包根。
- 六类组件目录认包根这一层，不递归猜目录名。
- `assets/`、`docs/` 不自动加载，只供组件按相对路径读取。
- 一件 Skill 占一层目录，入口 `SKILL.md`；一件 Workflow 占一层目录，入口 `workflow.yaml`；一件 MCP server 占一层目录，入口 `mcp.yaml`。
- 一件 packaged Plugin 必须带 `plugin.json`。裸 DLL、裸 Lua 只留给旧目录兼容，不进 Package。
- local id 一律小写 kebab-case：只用小写字母、数字、单横线；首尾须字母或数字，横线不连写；长 1 到 64。包内比旧 standalone Plugin 的 `[A-Za-z0-9_-]` 更严；旧目录不受影响。
- 未识别的顶层目录可以保留，`doctor` 给提示。`skill/`、`workflow/`、`plugin/`、`agent/` 这类少个字母的近似名要明报，不当作普通未知目录。

## 3. package.yaml（schema 1）

清单要薄。只写身份、版本、兼容，不装能力。

最小清单：

```yaml
schema: 1
id: moontide.browser-suite
version: 0.1.0
name: Browser Suite
description: Browser inspection agents, skills, workflows, plugins, and MCP tools.
```

完整清单：

```yaml
schema: 1
id: moontide.browser-suite
version: 0.1.0
name: Browser Suite
description: Browser inspection agents, skills, workflows, plugins, and MCP tools.

authors:
  - name: Moontide
    url: https://example.com

license: Apache-2.0
homepage: https://example.com/browser-suite
repository: https://example.com/browser-suite.git

compatibility:
  lubancode: ">=0.27.0 <0.28.0"
  platforms:
    - windows
    - linux
    - macos
```

逐字段规矩：

| 字段 | 必填 | 默认 | 规矩 |
| --- | --- | --- | --- |
| `schema` | 是 | — | 首版只认 `1`。别的值拒载，明报，不静默猜结构 |
| `id` | 是 | — | 小写字母、数字、点、连字符。至少两段（建议 `作者.包名`）；段间以 `.` 分隔；每段同 kebab-case 规矩，长 1 到 64。例：`moontide.browser-suite` |
| `version` | 是 | — | 按 SemVer 解析与比较，不拿普通字符串排序 |
| `name` | 是 | — | 展示名，列表与审批页用 |
| `description` | 是 | — | 写装了什么、给谁用，不写宣传话 |
| `authors` | 否 | 空 | 列表，每项 `name` 加可选 `url` |
| `license` | 否 | — | SPDX 标识，如 `Apache-2.0` |
| `homepage`、`repository` | 否 | — | URL，只作溯源 |
| `compatibility.lubancode` | 否 | 不检查 | 版本范围串。写了就严格检查，当前版本不合即整包 invalid |
| `compatibility.platforms` | 否 | 全平台 | `windows` / `linux` / `macos` 的子集。写了就按当前平台检查 |

清单不写——写了按未知字段报错：

- Agent、Skill、Workflow 等逐文件列表。盘点见第 4 节。
- `command`、`args`、`env`、网络权限。这些归组件自己的 manifest。
- 工具 allowlist。归 Agent 定义与宿主权限账。
- 密钥与 token。哪都不许写。
- `enabled` 之类的启停状态。启停账在包外，见第 9 节。

目录名不必等于 id。id 末段与包目录名不同时，`doctor` 给 warning；日后可升为错误。

## 4. 自动盘点

规矩一句话：**目录里有就认，清单不必登记，清单也不得登记。**

扫描六类标准目录，件件过各自的原生 parser，生成一份 `PackageInventory`。这份账只读，不写回源目录：

```text
package id / version / source / root / content hash
agents[]
prompt_profiles[]
skills[]
workflows[]
plugins[]
mcp_servers[]
assets/docs 文件数
code-bearing 文件清单
diagnostics[]
```

盘点须稳定：先规范路径，再按 UTF-8 字节序排序。文件系统枚举次序不可拿来做 ID、覆盖次序或哈希次序。同一只包扫两遍，哈希必须一字不差；内容改一个字节，哈希必须变。

盘点不复制 schema。Agent 字段归 Agent parser，Skill frontmatter 归 Skill loader，Workflow 校验归 Workflow validator，Plugin manifest 归 Plugin runtime，MCP 握手归 MCP runtime。Package 层只记账。

## 5. MCP 组件格式

MCP 眼下只从 `config.json` 的 `mcpServers` 读 command、args、env。Package 里要有独立入口，免得改用户配置才能启包。

`mcp/<id>/mcp.yaml`：

```yaml
schema: 1
id: browser
description: Local Playwright browser server.
transport: stdio

runtime:
  command: node
  args:
    - "${package_dir}/mcp/browser/server.js"
  env:
    BROWSER_TOKEN: "${env:BROWSER_TOKEN}"
  timeout_ms: 30000

permissions:
  network: false
```

字段规矩：

| 字段 | 必填 | 默认 | 规矩 |
| --- | --- | --- | --- |
| `schema` | 是 | — | 只认 `1` |
| `id` | 是 | — | kebab-case，须与所在目录名一致 |
| `description` | 是 | — | 审批页与 `/mcp` 展示用 |
| `transport` | 是 | — | 首版只收 `stdio`。远端 HTTP、OAuth 另开单 |
| `runtime.command` | 是 | — | 可执行文件，走 exec form，不经 Shell |
| `runtime.args` | 否 | 空 | 字符串数组 |
| `runtime.env` | 否 | 空 | 只许 `${env:NAME}` 占位，读宿主已有变量 |
| `runtime.timeout_ms` | 否 | 30000 | 握手与调用的墙钟帽 |
| `permissions.network` | 否 | `false` | 首版无执法能力，只作 doctor 与审批展示，不得谎称已隔离 |

占位符规矩：

- `${package_dir}` 指包根，只在 `args` 与 `env` 值里展开；规范化后逃出包根即拒。
- `${package_data}` 指持久数据目录（见第 9 节），更新包不丢数据。
- `${project_dir}` 只在日后明确允许的字段展开，首版不认。
- `${env:NAME}` 只读宿主环境变量名，值不落清单、不进日志。
- 清单不得写真实密钥。

解析后落为现有 `McpServerConfig`（`src/config/config.hpp`）或其后继。起服、握手、`tools/list`、注册全走现有 MCP runtime。MCP 工具名必须带 Package 命名空间，编码见第 6 节。

## 6. 命名空间

包一多，重名是常事。不再靠“后扫到的盖前面”。

每件 packaged component 有两枚名字：

```text
local id:     browser-tester
canonical id: moontide.browser-suite:browser-tester
```

- canonical id = `<package-id>:<local-id>`，中间一枚冒号。
- canonical id 不带 kind 段。同包里 Agent 叫 `browser-tester`、Prompt Profile 也叫 `browser-tester`，不算撞名，各归各的 Catalog。
- 六类组件一律适用。Plugin 的 canonical id 是 `moontide.browser-suite:dom-analyzer`，MCP 的是 `moontide.browser-suite:browser`。

引用规矩：

- 包内引用写短名：`skills.preload: [browser-testing]`、`mcp_servers: [browser]`、`prompt.profile: browser-tester`。Resolver 先在本包里找。
- 包外引用必须写 canonical id 全名。短名出包即解析失败，不猜。
- 旧 standalone 组件保留裸名。Package 组件不注册裸 alias，不暗中盖住旧名字。日后要短别名，另做显式 alias 与冲突诊断。

### 6.1 工具 wire name

工具名沿用现有前缀，把有效组件 ID 命名空间化：

```text
plugin__<package-id>.<plugin-local-id>__<tool>
mcp__<package-id>.<server-local-id>__<tool>
```

各 provider 对工具名普遍按 `[A-Za-z0-9_-]` 收口、上限约 64 字符；本仓 `IsValidPluginIdentifier`（`src/runtime/plugin_contract.hpp`）同此。点号进不了 wire。故定一枚可逆 `ToolWireName` 编码，全仓一处实现，不许各写各的字符串替换：

**编码规矩：canonical 名里每个落在 `[A-Za-z0-9_-]` 之外的字符，换成 `%HH`（大写十六进制，按字节）。`%` 本身不在合法集里，原文从不出现裸 `%`，故解码唯一、可逆。**

正例（包 id `moontide.browser-suite`）：

| 名目 | 展示名（给人看） | wire 名（发给 provider 与权限账） |
| --- | --- | --- |
| Plugin 工具 | `plugin__moontide.browser-suite.dom-analyzer__inspect` | `plugin__moontide%2Ebrowser-suite%2Edom-analyzer__inspect` |
| MCP 工具 | `mcp__moontide.browser-suite.browser__navigate` | `mcp__moontide%2Ebrowser-suite%2Ebrowser__navigate` |

反例：

```text
plugin__moontide.browser-suite__inspect          # 丢了组件段。两包各带同名工具即撞车
plugin__moontide_browser_suite_dom_analyzer__x   # 点改下划线。下划线本身合法，无法逆推，禁
plugin__Moontide.BrowserSuite__inspect           # 大写点号原样上 wire。provider 直接拒
```

- 展示名仍用 canonical id。`/tools`、`/mcp`、`/package show` 一律展示带点的可读名；wire 名只在发请求与记账时出现。
- Agent 的 `tools.allow`、`requires.tools`、`settings.local.json` 的 `allow_tools` 都写 wire 名——账本按注册名记。
- 长度预算：编码后的完整工具名不超 64 字符，超了 `doctor` 报错。package id 别起长名，每枚 `%2E` 吃三格。
- 编码只施于组件 ID 段。工具短名本就 `[A-Za-z0-9_-]`，不动。

## 7. 发现不等于执行

“放进去就成功”分三步看：

```text
放进目录        -> 立刻被发现，进 list、inspect、doctor
格式无错        -> 立刻可列、可查、可诊断
越过信任门      -> 才能挂载会跑代码的组件
```

Plugin 与 MCP 能起进程、读环境变量、访问网络。扫到目录便执行，等于把项目 checkout 当安装器。故分两档：

**内容组件（content）**：Agent Definition、Prompt Profile、Skill、Workflow。项目 Package 里这些组件静态校验过后即可登记。真执行时仍受 Agent 工具过滤、Workflow capability 校验、沙箱与审批约束——登记不是放行。

**代码组件（code-bearing）**：Plugin 的二进制、Lua、process command 与脚本；MCP 的 command、args 与可执行文件；日后若 Skill 脚本加入自动安装或自动执行，也算。

两档门槛：

- 用户级 Package：用户亲手放进 `~/.lubancode/packages/`，待遇同现有用户插件，视作已安装来源。`inspect` 仍须亮出命令与权限。
- 项目级 Package：代码组件先过 Package 内容指纹信任门。未信任时整包仍可发现、可检查；代码组件一个不启动，依赖它们的 Agent、Workflow 标 unavailable。
- 官方与 dev 层（`--package-dir`）：官方视同已安装来源；dev 是外来源码目录，与项目级同样过门。

批准一只 Package 时须亮出：

- canonical root 与完整内容哈希。
- package id、version、来源。
- Plugin 与 MCP 清单。
- 将启动的 command 与 args；密钥值打码。
- 声明的网络、env、cwd 权限。
- 将新增的工具名（wire 名与展示名并列）。

文件改一个字节，哈希就变，旧信任失效，须重批。

### 7.1 信任账（阶段 4 落地口径）

批准记录落 `<home>/.lubancode/package-trust.json`（与 plugin-trust.json 同惯例，绝不写回包目录）。一条记录绑四样：package id、version、整包内容哈希（阶段 1 的盘点算法，与 Plugin 指纹同一把底座）、何时批的。比对只认 id + 哈希——version 在 package.yaml 里，package.yaml 在哈希里，版本变哈希必变。

命令面（`/package` 的子命令，不新增斜杠命令）：

```text
/package trust <id>   亮全份审批材料（五样回执）后落账；幂等
/package untrust <id> 销该包全部批准（含哈希已对不上的陈账）
```

批准重启后生效——会话启动时折一份只读信任快照钉住，运行中批/销不换本会话的账（与 §9 的会话钉快照同语义）。未过门时：content 组件照挂；code 组件一件不挂不执行；引用包内 Plugin/MCP 的 Agent 标 unavailable（`/agents` 注明缘由），直接引工具或依赖此类 Agent 的 Workflow 同理（Catalog 里 broken 注缘由）。哈希对不上（文件动过）视同未信任：拦下并指路重批，不静默放行也不静默卸载；把文件改回去，旧账自然重新对上。

## 8. 整包成，整包败

Package 不可半挂。激活分七步：

```text
1. Parse package.yaml
2. Build deterministic inventory and hash
3. Parse every component with its native parser
4. Resolve package-local references
5. Check compatibility, trust and collisions
6. Build PackageMountPlan
7. Atomically publish catalogs and runtimes
```

前六步有一处错，整包 invalid。它的 Agent、Skill、Workflow、Plugin、MCP 一件也不露给运行时。诊断从包根一路指到具体文件、行、字段。

第七步若要启动多件 Plugin 或 MCP：

- 先在暂存 ToolRegistry 与暂存 runtime owner 中启动。
- 全部成功，快照一次交给 SessionStack。
- 中途失败，停掉已起进程，卸掉暂存模块，丢掉暂存表。
- 不留半包工具。

旧 standalone MCP 维持“坏一只，跳一只”的兼容语义。Package 走整包事务，两套不混。

## 9. 源码与数据分家、启停账在包外、会话钉快照

本节只定合同，不涉实现细节。

**源码与数据分家。** 包根按只读看待。运行产生的缓存、数据库、下载物、虚拟环境不可写回包内。持久数据放：

```text
~/.lubancode/package-data/<package-id>/
```

组件经 `${package_data}` 或宿主 API 取路径。卸载默认保留数据；`purge` 才删，删前单独确认。

**启停账在包外。** Package 自己不可写 `enabled: false`。启停、信任、固定版本另记：

```text
~/.lubancode/package-state.json
```

状态键至少带：package id、source root、content hash、enabled/disabled、trusted/untrusted、选中的版本与来源。项目共享的启用清单与用户私有的信任账分开记。仓库不可提交某台机器的信任决定。

**会话钉快照。** 一场会话启动时取一份 `PackageSnapshot`，运行中的 Agent、Workflow、Plugin、MCP 都指向它。Reload 之后：新任务用新快照；已在跑的 Agent、Workflow 继续用旧快照。content-only 包可热换 Catalog；带 native Plugin 或常驻 MCP 的包，首版提示重启会话，不冒险热卸 DLL。

## 10. 诊断与夹具

`/package doctor <id|path>` 逐项查：

| 检查 | 报什么 |
| --- | --- |
| 清单 schema 与 SemVer | `manifest_missing`、`manifest_schema_unsupported`、`id_invalid`、`version_not_semver`、`unknown_field` |
| LubanCode 版本与平台 | `incompatible_lubancode`、`incompatible_platform` |
| 标准目录拼写 | `unrecognized_top_level_directory`、`near_miss_directory` |
| 每件组件的原生 schema | 指到文件、行、字段 |
| 相对路径与符号链接 | `path_escape`（`${package_dir}` 或 `../` 逃出包根） |
| 内部引用与工具冲突 | 缺引用、撞 wire 名、`id_invalid`（组件 local id 不守规矩，如含点、下划线、大写） |
| 命名一致性 | `name_mismatch`（manifest id 与目录名、frontmatter name 与目录名不符） |
| command 与 env | command 是否存在；env 只报变量名在不在，不吐值 |
| 信任、启停与 runtime | 未信任、已停用、runtime 不支持 |

退出码区分：清单错、组件错、依赖缺、未信任、runtime 不支持。

夹具在 `tests/fixtures/packages/`，与本文逐条对得上：

| 夹具 | 是什么 |
| --- | --- |
| `minimal-content-only/` | 最小内容包。清单加一件 Skill，两层即成。放进目录、reload，Skill 即可发现，全程零代码组件 |
| `full-stack/` | 完整包。六类组件各至少一件真样例，自述见其 `README.md`。交予没看过代码的人，只照目录也应说清：会发现什么、会执行什么、哪一步要信任 |
| `broken/missing-manifest/` | 缺 `package.yaml`。组件无辜，整包 `manifest_missing` |
| `broken/invalid-manifest/` | 清单四处错：schema 非 1、id 大写带下划线、version 非 SemVer、未知字段 `permissions`。逐条对应上表 |
| `broken/path-escape/` | MCP 的 `${package_dir}/../../` 逃出包根；Workflow 的 `prompt:` 引用 `../` 越界 |
| `broken/bad-names/` | 近似目录 `skill/`、agent 文件名大写且 `name` 带空格、Skill frontmatter 名不合规且与目录不符、Plugin id 带点、MCP id 与目录不符 |

`broken/README.md` 逐条指错。四个坏包各自独立，别把 `broken/` 整个丢进 packages 目录——它自己没有清单。

## 11. 从哪里来，先做什么

首版扫描四层，每个直接子目录是一只 Package：

```text
<official packages>/
~/.lubancode/packages/
<project>/.lubancode/packages/
--package-dir <path>    # 开发调试，可重复
```

同 id 优先级：CLI 开发目录 > project > user > official。被盖住的版本仍进诊断账，`/package inspect` 列出所有候选、实际所用版本与来源。这套优先级只管同一只 Package，不改 standalone Skill、Workflow、Plugin 的旧优先级。

另有第五路 **store 选中版本**（自进化闭环阶段 4）：`~/.lubancode/package-store/` 里 active/canary 指针指到的那一枚，由装配并进挂载与 `/package list`（scope 记 `store`），不参与上面四层的目录扫描。同 id 时 store 压 user/official、让位于 dev 与 project。哈希完好在进化侧验过才递进来；手改 store 内文件，下次启动拒挂并指路。

上手三步：

1. 照 `minimal-content-only/` 摆一只内容包，`/package reload` 后 `/skills` 能看到它。
2. 要编排，加 `workflows/`；要固定角色，加 `agents/` 与 `prompts/`。
3. 真要添工具，才进 `plugins/` 或 `mcp/`——那是代码组件，项目里要过信任门。
