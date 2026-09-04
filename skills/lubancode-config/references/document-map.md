# 官方文档地图

文档根目录：`<技能目录>/../../docs`。下列路径都从该目录起算。

| 用户问题 | 先读 | 需要内部机理时再读 |
| --- | --- | --- |
| 第一次安装、配置与跑任务 | `getting-started/quickstart.md` | `reference/commands.md` |
| 为何用 C++、与 Claude Code/Codex CLI/Pi 的差异 | `getting-started/why-lubancode.md` | `architecture/README.md` |
| 配置来源、字段、环境变量、主题、目录 | `reference/configuration.md` | `architecture/README.md` |
| 模型、Provider、目录、私有请求参数 | `reference/configuration.md`、`features/providers/catalog.md` | `architecture/providers/schema.md` |
| 会话、恢复、compact、缓存、上下文 | `features/sessions/README.md`、`features/context/compaction.md` | `architecture/context/compaction.md` |
| 项目记忆 | `reference/commands.md`、`architecture/memory/design.md` | `architecture/memory/flow.md`、`architecture/memory/context.md` |
| 工具、Agent、后台任务、PTC | `reference/tools.md`、`features/tools/ptc.md` | `architecture/tool-calling-flow.md`、`architecture/agent-loop/reliability.md` |
| Skill、Plugin、MCP、LSP | `features/extensions/README.md` | `architecture/extensions/tool-extension.md`、`architecture/extensions/plugin-runtime.md` |
| Hooks、权限、settings.local.json | `features/extensions/hooks.md`、`development/security.md` | `architecture/hooks-flow.md` |
| 终端、键位、排队输入、面板 | `features/terminal/README.md`、`reference/commands.md` | `architecture/query-data-flow.md` |
| AGENTS、项目指令、prompt、soul | `features/project-instructions/README.md`、`reference/configuration.md` | `architecture/README.md` |
| 安装、更新、发行、排错 | `development/build-and-release.md`、`getting-started/troubleshooting.md` | `development/testing.md` |
| 当前功能总览 | `reference/feature-index.md` | `README.md` 不需要再读；它只是总导航 |

读页面时用真实绝对路径。先把 Skill 工具返回的目录与 `../../docs/<路径>` 合并、
规范化，再调用 `read_file`。不要把 `<技能目录>` 当成字面目录名。
