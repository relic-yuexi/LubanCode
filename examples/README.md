# 示例

| 目录 | 内容 |
| --- | --- |
| [packages/](packages/README.md) | Package 示例:整箱分发,带 package.yaml 清单——browser(官方浏览器包:Agent 加 Skill 加四只 Workflow,浏览器本体归核心)、gui-agent(code-bearing:process 插件加 Skill,默认只发现不挂载) |
| [agents/](agents/README.md) | 散装 Agent 定义示例(如 code-reviewer.yaml,一只 YAML 一只 Agent) |
| [plugins/](plugins/README.md) | 散装插件四条路各一枚:Lua、process(Python/Rust/C)、native DLL |
| [workflows/](workflows/README.md) | 散装工作流示例(三省六部) |
| [web-console/](web-console/README.md) | 参考前端(多前端外壳单·阶段 D):纯静态 Web 页四件套(聊天/页签/面板/镜像+审批),全程只走 AppServer 协议——验收协议面的工具,不是产品 |
| [shells/](shells/README.md) | 外壳孵化(多前端外壳单·阶段 E):Tauri 桌面壳与 Android WebView 壳,都不复制参考前端(直指同一份代码),零内核改动 |

两条路怎么选:整箱分发、多件组件搭配、要讲信任门的,走 `packages/`
(Package 契约见 `docs/reference/packages.md`);单件试验、随手放一只的,
旧 standalone 目录(`agents/`、`plugins/`、`workflows/`)照旧扫、照旧用。
新项目宜用 Package;旧目录留作兼容,不删。
