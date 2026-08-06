# 功能全览

[文档首页](README.md) · [命令参考](commands.md) · [工具参考](tools.md) · [架构说明](architecture.md)

本页对应 `v0.24.0`。它是一张功能总账：一项能力从哪里进入，什么时候出现，数据落在哪里，细节该去哪一页查。当前 Release 测试共 `1151` 例、`5239` 条断言。

## 运行方式

| 方式 | 入口 | 交互能力 | 会话存档 | 适合场景 |
| --- | --- | --- | --- | --- |
| 交互会话 | `lubancode` | 完整：编辑器、菜单、确认、排队、聚焦 | 有 | 日常开发与长任务 |
| 续接会话 | `lubancode --continue` | 同交互会话 | 续写原 JSONL | 接着本目录最近一场工作 |
| 单发任务 | `lubancode "问题"` | 无逐键菜单；工具仍可调用 | 不落交互会话 | 脚本、短任务、CI 辅助 |
| 管道输入 | `git diff | lubancode "审查"` | plain 输出；不能现场提问 | 不落交互会话 | 串接 shell 工作流 |
| 诊断模式 | `--config`、`--help`、`--version`、`--check-update` | 不请求模型 | 无 | 排错、版本检查与自动化探测 |

## 模型与协议

| 功能 | 现状 | 入口与说明 |
| --- | --- | --- |
| Anthropic Messages | 原生请求与流事件解析 | `wire=anthropic` |
| OpenAI Responses | 原生请求、工具调用、服务端搜索事件 | `wire=responses` |
| Chat Completions | OpenAI 兼容聊天与工具调用 | `wire=chat_completions`，`chat` 也认 |
| 多 provider | 每条保存协议、地址、密钥来源、模型、窗口与私有参数 | `/provider`、[配置手册](configuration.md) |
| Provider 目录 | 内置快照、在线缓存、ETag、断网回退 | `/provider add`、`/provider refresh`、[Provider 目录](provider-catalog.md) |
| 模型目录 | 展示名、上下文窗口、推理档位、模型指令与 variant 参数 | `~/.lubancode/models.json` |
| 模型切换 | 拉端点模型列表；当前模型作默认项；Esc 取消 | `/model` |
| 推理档位 | 会话级切换；具体档位由 provider/模型声明 | `/think`，`/effort` 同义 |
| 厂商参数透传 | 请求体浅合并，HTTP 头追加或覆盖 | `extra_body`、`extra_headers` |
| 原生联网搜索 | Anthropic/Responses 可声明服务端搜索工具 | provider 的 `native_web_search` |

模型地址与密钥没有内置默认值。交互模式缺配置会开向导；单发与管道模式直接报缺项。配置来源与覆盖顺序见[配置手册](configuration.md)。

## 代码与系统工具

| 工具 | 能做什么 | 是否常驻 | 默认确认 |
| --- | --- | --- | --- |
| `read_file` | 按行号读文件，支持 offset/limit 与截断续读 | 是 | 否 |
| `search` | 正则搜内容，或 glob 找文件 | 是 | 否 |
| `write_file` | 新建或整篇覆盖 UTF-8 文件，自动建父目录 | 是 | 是 |
| `edit_file` | 唯一字符串替换，兼容换行、统一缩进与行尾空白 | 是 | 是 |
| `run_command` | 前台命令、后台服务、超时、日志与整树回收 | 是 | 是 |
| `web_fetch` | 抓 HTTP(S) 文本，HTML 转正文 | 是 | 否 |
| `web_search` | 调 Tavily、Brave 或 Serper 搜索 | 配 `search` 后 | 否 |
| `lsp` | 定义、引用、文件符号、诊断 | 配 `lsp` 后 | 否 |

参数、上限、失败路与平台差异见[工具参考](tools.md)。

## 代理工作流

| 功能 | 行为 |
| --- | --- |
| 多轮工具循环 | 模型可连续调用工具，工具结果写回下一次协议请求，直到产出答案或被打断。 |
| 子代理 | `agent` 给独立上下文与工具表，只把结论带回主会话；适合大范围搜索与长资料归纳。 |
| 待办清单 | `todo_write` 全量维护 `pending / in_progress / completed`，终端原位更新变化项。 |
| 用户选择 | `ask_user` 一次问 1 到 4 题，支持单选、多选和自由填写；只在交互模式挂载。 |
| 工具延迟挂载 | 工具总数超过阈值时，MCP/插件等先留索引；`tool_search` 命中后再放进请求 schema。 |
| Skills | 官方级、用户级、项目级三层 `SKILL.md`；项目 > 用户 > 官方。可列出、安装、更新、删除用户技能。 |
| 项目指令 | 从 Git 根到 cwd 分层加载 `AGENTS.override.md` / `AGENTS.md`；主代理与子代理共用。 |
| 隔离 worktree | 新建、列出、保留或移除工作树；会话切换到新 cwd 后重建项目上下文。 |
| 项目记忆 | 默认关闭；同步本地召回，写入走后台队列；事实与偏好分开。 |

## 终端界面

| 功能 | 行为 |
| --- | --- |
| 多行编辑 | 方向键、Home/End、历史、插入删除；`Shift+Enter` 换行，Enter 发送。 |
| Slash 候选 | `/` 开头实时提示；Tab 补全；按下方向键可进入直选菜单。 |
| 大段粘贴 | 1000 字符以内原样编辑；更长内容折成占位，提交时还原全文。 |
| 消息排队 | 模型工作时仍可输入；本轮结束后按顺序发送。 |
| 工作状态 | 等首字节时显示动画、耗时、取消提示与状态面板。 |
| 流式 Markdown | 标题、强调、列表、表格、代码块分段收束；宽度不足时按显示列截断。 |
| 终端公式 | `$...$` 压成紧凑 Unicode；`$$...$$` 展开分式、根式、上下限、伸缩括号、矩阵和数组。 |
| 工具转录 | 运行态原位换成终态；紧凑档藏子工具，详细档显示参数、全文与 diff。 |
| 回合中切档 | `Ctrl+O` 会重打当前快照，不只改一行提示；旧屏幕锚点随即作废，后续改为安全追加。 |
| 聚焦查看 | 空输入时 Tab 选条目，`Ctrl+E` 看全文，Esc/Enter 返回。 |
| 三档确认 | `confirm / auto / yolo` 用 `Shift+Tab` 切换；状态栏实时显示。 |
| 图片输入 | `/image` 或消息里的 `@路径`；PNG/JPEG/GIF/WebP，每张最多 5 MiB。 |
| 主题与语言 | dark/light/plain；内置中英文，外部 JSON 语言包可扩展。 |

详见[终端交互](terminal-ui.md)与[命令参考](commands.md)。

## 上下文、会话与记忆

| 功能 | 数据与边界 |
| --- | --- |
| token 统计 | 汇总输入、缓存命中、输出与上下文百分比；不同协议字段先归一。 |
| 自动压缩 | 接近窗口时压缩旧历史，保留最近消息与压缩摘要。 |
| 手工压缩 | `/compact [重点]` 可补一句本次压缩必须保住的内容。 |
| 独立压缩模型 | `compact_model` 留空则沿用会话模型。 |
| 字符硬限 | `max_context_chars` 是 token 窗口之外的第二道安全网。 |
| JSONL 存档 | 会话事件逐行追加，含 meta、消息、工具、usage、标题与压缩点。 |
| 列出与恢复 | `/sessions`、`/resume`；按 cwd 筛选，恢复后继续写回原文件。 |
| Markdown 导出 | `/export` 导出全量流水，压缩点保留标记。 |
| 项目记忆 | 住在用户目录，不进仓库，不混进 session，也不随导出外带。 |

详见[会话与上下文](sessions-and-context.md)和[项目记忆](memory-system-design.md)。

## 扩展与定制

| 扩展点 | 形式 | 进程边界 |
| --- | --- | --- |
| Skill | `SKILL.md` + 资源 | 提示与文件，不执行本地二进制 |
| MCP | stdio JSON-RPC 服务 | 独立子进程 |
| LSP | 标准语言服务器 | 独立子进程 |
| Lua | 返回工具表的 `.lua` | 宿主进程内 |
| C ABI 插件 | `luban_plugin_entry` DLL | 宿主进程内，仅 Windows |
| hooks | session/tool 前后执行命令 | 外部子进程 |
| Prompt 模块 | `prompts/core|features|platforms/*.md` | 构建嵌入，运行时文件可覆盖 |
| 法与魂 | `system_prompt.md` 与 `SOUL.md` | 人格替换与风格叠加分开 |
| 主题/i18n | 内置主题、`languages/*.json` | 本地资源 |

详见[扩展指南](extensions.md)。

## 权限与安全

- `confirm`：需要确认的工具逐次询问。
- `auto`：文件工具与判定为安全的命令可放行；危险命令与外挂工具仍问。
- `yolo` / `--yes`：显式全放，不受项目 deny 兜底拦截。
- `.lubancode/settings.local.json` 可设工具白名单、命令 allow/deny 前缀和默认确认档；文件不该进版本库。
- API key 可走 `key_env`；诊断与列表会打码。
- 外部 Skill、MCP、Lua、DLL、hooks 都应当按代码执行能力审查。Lua 与 DLL 没有沙箱。
- 未知 LaTeX 命令、坏公式与坏 Markdown 会退回原文，不能把用户内容悄悄吞掉。

## 平台与交付

Windows 用 `CreateProcessW`、Job Object、UTF-16 路径与 Win32 控制台；Linux/macOS 用 `fork/exec`、进程组、`poll` 与 POSIX 终端。业务层只看统一的进程结果与终端能力。

`/update` 与 `--check-update` 显式查询 GitHub 最新 Release，按 SemVer 比较，不自动安装。新版仍由发行包安装脚本落地；程序和官方 Skills 同步，用户 Skills 不动。

CI 在 Windows/MSVC、Ubuntu/GCC、macOS/Clang 三路编译并测试。`v*` 标签从干净 runner 重编、打包并创建 Release。构建、测试与发包细节见[架构说明](architecture.md#15-测试与交付)。

## 当前边界

- C ABI 原生插件只在 Windows 加载。
- 复杂原地重画以 Windows 真控制台路径最完整；管道与重定向主动降级为 plain。
- `web_fetch` 只收文本，不做浏览器执行、登录态与 JavaScript 渲染。
- `web_search` 要用户自备搜索服务 key；服务端原生搜索与本地 `web_search` 是两条独立路径。
- LSP 只提供 definition、references、symbols、diagnostics，未做编辑器全功能替代。
- 项目记忆先用本地词法检索，不含向量库与用户级跨项目记忆。
- 进程内 Lua/DLL 插件没有权限隔离；不可信插件应改走进程外 MCP。
