# 命令与按键参考

[文档首页](../README.md) · [功能全览](feature-index.md) · [终端交互](../features/terminal/README.md) · [配置手册](configuration.md)

本页按当前主线源码整理。启动参数只在进程起手时解析；slash 命令只在交互会话里解析。命令词大小写不敏感，参数原样保留。版本号只认 `src/app/version.hpp`；CMake 从这里读取项目版本。

## 启动形式

```text
lubancode [options]
lubancode "问题"
lubancode --output <path> "问题"
command | lubancode "补充要求"
lubancode app-server
lubancode trajectory <verb> <session-id|workspace-key>
lubancode gateway <run|status|stop> [options]
lubancode archive <id|标题>
lubancode unarchive <id>
lubancode delete <id|标题> [--force]
```

| 形式 | 行为 |
| --- | --- |
| 无参数 | 进入交互会话；缺少模型配置时可直接添加 Provider，也可跳过后稍后配置。 |
| 一个普通参数 | 作为单发问题；模型仍可调用适用工具。 |
| 单发加 `--output <path>` | 回合收口后，把这一场派生为 Harness JSONL 写到指定路径；只认带任务正文的单发形态。 |
| stdin 有管道数据 | 管道正文与命令行问题一同交给模型；输出降为 plain。 |
| `app-server` | 无界面后台协议模式：stdin/stdout 上逐行 JSON-RPC，不碰终端、不画界面（见 [app-server 协议](../features/app-server/README.md)）。SSH 远程项目经 `ssh <host> lubancode app-server` 拉起。 |
| `trajectory` | 验证、回放、用量、清理与导出本地 Trajectory；不发模型请求。 |
| `gateway` | 前台运行、查询或停止 Gateway 骨架；当前只落 `run/status/stop`，后续运维面不冒充已成。 |
| `archive` / `unarchive` / `delete` | 会话管理子命令，打完结果就退（见下）。 |
| EOF | 交互模式退出；空行只重新显示提示符。 |

### 会话管理子命令

- `lubancode archive <SESSION>`：把一场会话归档（状态图转入 archived，字节原样）。`SESSION` 认完整 id、唯一 id 前缀或唯一命中的标题；重名列短 id 叫你点明，绝不猜。归档后 `--continue`、`/sessions`、裸 `/resume` 略过它。
- `lubancode unarchive <SESSION>`：取消归档，搬回 sessions 根，`/resume` 又能续聊。
- `lubancode delete <SESSION>`：永久删除一场会话（根或归档里的都行）。交互终端先走确认屏（标题/完整 id/目录/「永久删除」，缺省取消，EOF、空答、别的答案都算取消）。`--force` 跳过确认——只给脚本显式使用，不可恢复。删除只碰目标那一份 `.jsonl`；artifact blob 按内容寻址、可能被别的会话引用，不连坐删。

## 启动参数

| 参数 | 说明 |
| --- | --- |
| `--version` | 打印版本后退出。 |
| `--check-update` | 查询 GitHub 最新 Release，打印当前/最新版本与发布页后退出；不自动安装。 |
| `--help` | 打印运行方式、参数、配置与交互命令后退出。 |
| `--yes` | 起手即用 `yolo`，自动放行所有需要确认的工具。 |
| `--continue` | 进入交互界面前恢复本目录最近一场会话；没有便开新会话。 |
| `--output <path>` | 单发模式结束后导出 Harness v1 JSONL；交互模式、app-server 与别的子命令带上会拒绝。常与脚本中的 `--yes` 合用。 |
| `--config` | 打印最终配置、字段来源、权限摘要和模型目录命中情况；密钥打码。 |
| `--system-prompt <file>` | 用 UTF-8 `.md/.txt` 替换人格段；运行所需环境与工具规则仍会追加。 |
| `--reset-system-prompt` | 把 `~/.lubancode/system_prompt.md` 还原成内置版，旧文件留作 `.bak`。 |

内部还使用 `--memory-worker <主目录>` 处理记忆队列。它不是日常用户入口。

## Slash 命令规则

- 单行首个非空字符是 `/`，才当命令。
- 多行 composer 里即使首行写 `/`，整段仍当普通消息发送。
- `/quit` 是 `/exit` 别名；`/lang` 是 `/language` 别名；`/effort` 是 `/think` 别名。
- 未知命令不会发给模型，会在本地报“不认得”。
- 输入 `/` 后可用 Tab 补全，也可按方向键进入命令直选。排队输入框（模型正忙时）同样支持；仅交互终端，管道输入没有逐键补全。

## 模型与 Provider

### `/help`

打印交互命令、键位、配置入口和常用环境变量。它不清会话，也不请求模型。

### `/model [模型名]`

- 裸敲：请求当前端点的模型列表，标出当前项，以它作默认编号。
- Enter 空行：沿用当前项。
- Esc：取消，不误选默认项。
- 带名字：直接切换，不先拉列表。
- 切换只影响本会话；随后可选择是否写回配置。

### `/provider`

```text
/provider
/provider list
/provider refresh
/provider add [名字]
/provider add <名字> <base_url> <wire> [--key-env ENV] [--key KEY]
              [--model MODEL] [--effort LEVEL] [--window SIZE]
/provider switch <名字> [模型]
/provider remove <名字>
/provider set <名字> native_web_search on|off
/provider set <名字> extra_body <JSON object>
/provider set <名字> extra_header <头名> <值>
```

裸敲与 `list` 同义。`add` 参数少于一整行时进入分步向导；向导先给厂家预设，末项才是自定义。会话还没有可用连接时，添加成功便自动切到新 Provider；已有连接时只添加，不抢当前端。当前 provider 不许直接删除。`switch` 成功后保存 `active_provider`。

### `/think [档位]`、`/effort [档位]`

裸敲显示当前档位与档位声明。声明按三层找：模型目录（`models.json` 条目的 `supported_think_levels`）最准；没有条目再看 provider 配置的 `supported_think_levels`；都没有就明说"未经能力验证"，不甩一句"以服务商为准"完事。带参数则切本场档位。档位名字不强行锁死为四档，厂商声明什么便可用什么；表外档位照发，但会标注不在声明表内。

"不填"是正式状态：请求里没有这个字段，文案写"未发送参数"，不偷偷映射成任何默认档。provider 可在配置里声明 `supported_think_levels`（档位数组）、`think_param`（请求参数名，默认 `reasoning_effort`）、`think_passthrough`（是否原样透传）。

### `/doctor [effort|cache|instructions|agents|shell]`

本地兼容端诊断（vLLM/Qwen 一类自建端）。裸敲看概要，不发请求。

```text
/doctor effort [档位|unset]    发一只极小探针:报告 HTTP 状态、清洗后的服务端
                                错误、请求侧实际发送值(参数名+档位;unset 时
                                字段确实缺席)、finish_reason、usage 的
                                reasoning/output 拆账。
/doctor cache                  metrics_url 明配后读服务端 /metrics,报
                                enable_prefix_caching 与 query/hit/cached
                                计数;没配就说明怎么配,不擅自探。
/doctor cache probe            两轮固定前缀测试:同 system、同历史前缀,只换
                                最后一句。报前缀字节是否稳定、服务端 query/hit
                                增量、第二轮 cached_tokens。
/doctor cache usage            stream_usage 能力探针:发一只带
                                stream_options.include_usage 的极小请求,结论
                                写回 provider 配置。
/doctor instructions           AGENTS.md cwd 基线全账:逐 source 一行 + 字节帽
                                计费口径 + fallback 名单 + 缓存口径 + 分型诊断
                                (不发请求、不泄正文)。
```

诊断只对本地兼容端动手：`probe`/`usage` 两个动作挡在"base_url 是本机地址，或 metrics_url 已明配"这道闸后面，公网 provider 不发。密钥与正文不进报告——只摆参数名、档位值、token 数与错误摘要。

裸敲概要第一行就是审批档：起手档与来源（`CLI --yes` / `env LUBANCODE_CONFIRM_MODE=…` / `project permissions.default_confirm_mode=…` / 内置默认）一行说清；会话里 Shift+Tab 切过档的另起一行标"本会话"当前档。

usage 账分四态：`not_reported`（服务端没回 usage）/ `disabled`（metrics 明说没启用，状态栏写"服务端未启用缓存"）/ `enabled_no_hit`（已启用未命中）/ `hit`。同一个 0 不糊。

### `/config`

打印配置诊断。它与 `lubancode --config` 共用格式，另会标出当前会话临时切过的模型。

审批档一行与 `/doctor` 同一份账：列出最终起手档及四源来源（CLI/env/project/builtin，带原文值）；本会话切过档的再列一行当前档。`permissions` 摘要行仍照抄 `settings.local.json` 里的原文——配置写的什么、最终生效的是哪档，两笔账分开看。

### `/update [check]`

查询 GitHub `releases/latest`，按 SemVer 比较当前版本与最新正式 Release。裸敲与 `check` 同义；别的参数会打印用法。

发现新版时，只打印版本、发布页和安装提示，不在运行中的进程里覆盖自己。下载新版发行包，再运行包内安装脚本；程序与官方 Skills 一并更新，`~/.lubancode/skills` 下的用户技能不动。检查失败只报网络、HTTP、JSON 或版本错误。

## 项目与工作目录

### `/init`

在 Git 根生成 `AGENTS.md`；非 Git 目录写在 cwd。已有 `AGENTS.md` 或 `AGENTS.override.md` 时不覆盖，只重载。详见[项目指令](../features/project-instructions/README.md)。

### `/instructions`

```text
/instructions
/instructions path <路径>
/instructions reload
```

AGENTS.md 指令链的逐 source 账：项目根、目标、上限、每份文档一行（路径、`AGENTS`/`OVERRIDE`/`FALLBACK`/`GLOBAL` 类型、字节数、摘要前 8 位、离目标最近标注）、合计、指纹与装载状态。`path` 看任意目标链（嵌套 `AGENTS.md` 从仓库根也能查）；`reload` 重载后亮新基线。诊断按 code 分型（空文件/遮蔽/读错/坏 UTF-8/越界 symlink/超预算/fallback/迁移提示），输出不泄正文。诊断全账与计费口径另见 `/doctor instructions`。

### `/worktree`

```text
/worktree new [名字]
/worktree list
/worktree exit keep
/worktree exit remove
```

`new` 创建隔离分支与工作树，随后切换会话 cwd、项目指令、Skill、权限和记忆身份。`remove` 遇到脏工作树会拒绝强删；`keep` 只退出，不移目录。

### `/clear`

清空当前对话历史并重建屏面。它不删除磁盘会话文件，也不删除项目记忆。

## 上下文与会话

### `/context [窗口]`

裸敲展示系统提示、工具 schema、历史与总占用。带 `256k`、`512k`、`1m` 或正整数时，只改本场 token 窗口。

### `/usage [session <id>] [--by model|purpose|run|outcome] [--json]`

Token 账本报告（只读，只摆事实）。裸敲看当前会话：coverage（几笔有 provider usage、几笔 unknown）、输入与 cache 读写、输出（reasoning 注明已含在内）、按模型/用途的 token 占比、估算费用、cache 行为观察与账的成色。`session <id>` 看同 workspace 的指定场；`--by` 换分账表；`--json` 出机器可读账（`lubancode.usage.report` v1）。当前会话恒标 `provisional`（未封口，读已提交高水位）。费用来自 `~/.lubancode/pricing.json` 价格表（没配则 token 照报、费用 `not_priced`；本地估算，非账单）。`features.trajectory` 关时账未开：明说之后降级给内存角色粗账并注明口径差异。`day`/`week`/`workspace`/`all` 跨场汇总属后续批次。累计账与 `/context` 的"下一请求快照"是两本账，不混。

### `/compact [重点说明]`

立刻压缩旧历史。可在参数里补一句“这次必须保住什么”。压缩模型由 `compact_model` 决定，留空沿用会话模型。

### `/sessions [all|archived]`

默认列 cwd 下最近 20 场；`all` 跨目录列；`archived` 只读列已归档的场子。列表含时间、标题、模型与 id。归档的在 `sessions/archive/` 下，字节原样；想续聊先 `lubancode unarchive <id>`。

### `/resume [编号|id]`

裸敲打开全屏会话台账：输入即搜（标题/首句/id/目录），Tab 轮换 Search/Filter/Sort 焦点，左右键改筛选与排序，上下浏览、PageUp/PageDown 翻页。三种查看态：`Ctrl+T` 看所选会话的转录（大文件取头尾，Esc 收起回原行）；`Ctrl+E` 摊开选中场的标题、目录、id、模型、消息数与创建/更新时间；`Ctrl+O` 在紧凑行与舒展行间切换，只改画法不动筛选与选中。Enter 恢复时重放消息、工具摘要与压缩点，随后继续写回原 JSONL；Esc 原路返回不动盘。直接给编号或 id 可跳过台账。

### `/archive`

归档当前会话（状态图转入 archived），成功后退出交互。字节、id、标题、时间一字不动；`--continue`、`/sessions`、裸 `/resume` 从此略过它。后台子代理还在跑时拒绝。别的场子用顶层命令 `lubancode archive <id|标题>`。

### `/delete`

永久删除当前会话：先过确认屏（标题/完整 id/目录/「永久删除」四行，缺省取消，EOF、空答、别的答案都算取消），确认后关句柄、删文件、退出。后台子代理还在跑时拒绝。别的场子用顶层命令 `lubancode delete <id|标题>`。归档的场子也能删。artifact blob 按内容寻址、可能被别的会话引用，不连坐删。

### `/export [路径]`

把当前会话导成 Markdown。默认写到 sessions 目录的 `<id>.md`；标题来自 `/title`，压缩点会留标记。

### `/title [标题]`

裸敲查看；带内容则写入 title 事件。最后一条标题供 `/sessions` 和 `/export` 使用。

详见[会话与上下文](../features/sessions/README.md)。

## 工具与扩展

| 命令 | 行为 |
| --- | --- |
| `/tools` | 按核心、已加载、延迟未加载三态列工具。 |
| `/todos` | 查看 `todo_write` 维护的当前清单。 |
| `/skills` | 列出官方级、用户级与项目级 Skill，并标明来源。 |
| `/skill` | 裸敲看用法；支持 `list/install/update/remove`。 |
| `/mcp` | 列 MCP 服务存活状态与发现的工具。 |
| `/lsp` | 列语言服务器的未启动、运行、闲置关闭状态。 |
| `/plugins` | 列三路插件（native/Lua/process）的工具与加载警告。 |
| `/plugin` | 管单枚插件：`inspect <id>` 看详情、`doctor <id>` 探环境（不执行工具）、`reload`/`enable`/`disable` 以重启为口径。 |
| `/channels` | 列渠道账号：全局配置里的渠道 × 本进程运行态。渠道默认关闭，普通交互进程不启 sidecar，只显示 `gateway not running` 与引导。 |
| `/channel` | 管单渠道账号：`show <渠道> [账号]` 看状态机/水位/迁移账，`doctor <渠道> [账号]` 体检（密钥只报来源不报值，不发平台请求），`start`/`stop`/`restart` 在普通交互进程只给 `lubancode gateway run` 引导，不改变进程形态；`pairing list <渠道> [账号]` 看待审配对，`pairing approve|reject <渠道> [账号] <code>` 批/拒（只认宿主看到的 sender id）。 |

`/skill install` 可收 HTTP(S) 地址、本地目录、`SKILL.md` 或独立 Markdown。安装统一落到 `~/.lubancode/skills/<name>`，成功后本场立即刷新。

生成 Python 插件脚手架走命令行子命令：`lubancode plugin init python [名字]`（plugin.json + runner.py + test_runner.py 三件套，落 `~/.lubancode/plugins/<名字>/`）。

## 项目记忆

```text
/memory
/memory on|off
/memory use on|off
/memory learn off|review|auto
/memory review
/memory accept <id>
/memory edit <id> 标题 [:: 正文]
/memory reject <id> [理由]
/memory list
/memory remember fact|preference|feedback 标题 [:: 正文]
/memory forget <id>
/memory rebuild
/memory stale
/memory verify <id>
/memory refresh <id>
/memory migrate
/memory show <id>
/memory open [id]
/memory why [id]
```

`on/off` 改本场总状态；`use` 控召回；`learn` 控学习档位(只能降到全局授权以内)。`review` 看待审候选,`accept`/`edit`/`reject` 处置。`remember` 排后台任务(feedback 只收用户明说的纠正)，`forget` 归档而非直接抹账，`rebuild` 从主题 Markdown 重建 catalog 与 index，`stale` 查陈旧，`verify`/`refresh` 续命，`migrate` 把旧格式主题批迁 front matter(先列账、备份、确认后动盘)，`show` 看单份主题，`open` 用 `$VISUAL/$EDITOR` 编辑主题或索引(编辑回来先校验,坏 YAML 不覆盖原件)，`why` 对上一轮召回对账。详见[项目记忆](../architecture/memory/design.md)。

## 个性与语言

### `/soul`

```text
/soul
/soul <直接内容>
/soul <souls 目录里的名字>
/soul clear
/soul off
/soul default
```

直接内容写进 `SOUL.md` 并即时生效。名字切换 `souls/<name>.md`。`off` 关闭叠加，`default` 回到 `SOUL.md`。

### `/prompt [reset]`

裸敲显示人格来源和字数。`reset` 恢复 `system_prompt.md` 内置脚手架，并保留备份。人格只替换 core；环境、工具和协议段仍在。

### `/prompt audit static | runtime [session-id] | outcome [--since 30d] | all | explain <finding-id> [--json]`

Prompt 三层审计（只读，只摆事实，主人裁决）。`static` 审眼前配置：五层来源与 override 链、同文重复（hash 相同）、n-gram 高重合、token 占比、工具描述与 schema 形状、动态段插中段、用户模块漂移——不碰 Journal。`runtime [session-id]` 审真实请求：Journal 里 `model.request.prepared` 的 request snapshot（manifest+toolset+形状）逐请求对账，看 toolset 抖动、稳定前缀断在哪、system 增长与 cache 命中的同向观察。`outcome [--since Nd]` 扫当前 workspace 的封口 session，把工具终态/验证/结果的摩擦抬成 finding（措辞只说信号，不断言 prompt 错；active/incomplete/corrupt 单列排除）。`all` 三层一起；`explain <finding-id>` 按稳定 id 重跑并打一条 finding 的全账（证据/反证/规则版本）。`--json` 出机器可读账（`lubancode.prompt.audit` v1）。报告不含 prompt 正文与绝对路径；语义类检查（疑似指令冲突/口号段）只给 `suspected`（confidence=low），复核才作数。`features.trajectory` 关时 static 照常，runtime/outcome 明说"账未开"。`--model-review` 属后续批次，敲了明说。裸敲 `/prompt` 与 `reset` 行为不变。

### `/language [语言码]`

裸敲列语言并选择；带语言码直接切。切换即时作用于命令说明与界面，可写回配置。内置 `zh-CN`、`en`，其余来自 `languages/*.json`。

## 图片与退出

### `/image <路径...>`

附一张或多张本地图片。普通消息也可写 `@image.png`，带空格路径写 `@"a b.png"`。支持 PNG、JPG/JPEG、GIF、WebP；每张不超过 5 MiB，并校验文件头与尺寸。

### `/exit`、`/quit`

结束交互会话。裸词 `exit`、`quit` 也认。EOF 同样退出。

## 编辑按键

| 按键 | 普通编辑状态 |
| --- | --- |
| `Enter` | 发送当前整段；空行重新提示。 |
| `Shift+Enter` | 插入换行。 |
| `Alt+Enter` | 同上；Windows Terminal 常把它绑成全屏，故不推荐。 |
| `← / →` | 按 Unicode 字符移动。 |
| `↑ / ↓` | 单行状态翻历史；Slash 候选打开时移动选择。 |
| `Home / End` | 到当前逻辑行首/尾。 |
| `Backspace / Delete` | 删除字符；碰到大段粘贴占位时整枚删除。 |
| `Tab` | 有输入时补全；空输入时进入工具条目焦点。 |
| `Shift+Tab` | 非焦点状态按 `default -> accept_edits -> yolo -> auto -> dont_ask` 循环审批档；中文 UI 名依次为“默认模式 / 接受编辑 / YOLO / 自动模式 / 不询问”。切档后黄色说明显示约 6 秒。 |
| `Ctrl+C` | 非空输入时清行；空输入时按 EOF 处理。 |
| `Ctrl+D` / Windows `Ctrl+Z` | EOF，结束读取。 |

## 工具条目按键

| 按键 | 行为 |
| --- | --- |
| `Ctrl+O` | 紧凑/详细全局切换；回合执行中也会按新档重打当前转录快照。 |
| 空输入 `Tab` | 选最近工具条目；焦点态继续按 Tab 往旧走。 |
| 焦点态 `Shift+Tab` | 往新条目走，此时不切确认档。 |
| `Ctrl+E` | 看焦点条目全文；无焦点时看最近一条。 |
| `Esc` / `Enter` | 退出焦点或全文画面。 |

流式正文正在落字时，焦点查看按键暂不响应；回合监听线程仍接收 Esc、排队输入与 `Ctrl+O`。

## 菜单按键

| 场景 | 按键 |
| --- | --- |
| 单选 | `↑/↓` 移动，Enter 确认，Esc 取消。 |
| 多选 | `↑/↓` 移动，Space 勾选，Enter 提交，Esc 取消。 |
| 自由填写 | 移到“自己填写”后直接输入，Backspace 删除，Enter 提交。 |
| `/model` 编号选择 | 空行选当前项，Esc 取消。 |
| `/resume` 会话台账 | 输入即搜；`Tab`/`Shift+Tab` 轮换 Search/Filter/Sort 焦点，`←/→` 改选项；`↑/↓` 浏览，`PageUp/PageDown` 翻页，`Home/End` 到头尾；Enter 恢复，Esc 原路返回。查看态：`Ctrl+T` 转录（Esc/Ctrl+T 收起回原行），`Ctrl+E` 摊开选中场详情，`Ctrl+O` 紧凑/舒展切换。台账里没有删除键。 |

## 非交互降级

- 管道与重定向不用原地菜单、动画、ANSI 重画和 bracketed paste。
- `ask_user` 不在单发/管道入口挂载，免得无人应答时挂死。
- 图片、slash 命令、焦点按键属于交互入口；脚本场景应把要求直接写进问题。
- 需要无确认自动化时用 `--yes`（即 `yolo`），或设 `LUBANCODE_CONFIRM_MODE=default|accept_edits|yolo|auto|dont_ask`；旧 `confirm` 仍兼容为 `default`，但已弃用。
