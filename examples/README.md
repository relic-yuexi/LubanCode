# 示例

| 目录 | 内容 |
| --- | --- |
| [packages/](packages/README.md) | Package 示例:整箱分发,带 package.yaml 清单——browser-agent(内容包:Skill 加练习页)、gui-agent(code-bearing:process 插件加 Skill,默认只发现不挂载) |
| [agents/](agents/README.md) | 散装 Agent 定义示例(如 code-reviewer.yaml,一只 YAML 一只 Agent) |
| [plugins/](plugins/README.md) | 散装插件四条路各一枚:Lua、process(Python/Rust/C)、native DLL |
| [workflows/](workflows/README.md) | 散装工作流示例(三省六部) |

两条路怎么选:整箱分发、多件组件搭配、要讲信任门的,走 `packages/`
(Package 契约见 `docs/reference/packages.md`);单件试验、随手放一只的,
旧 standalone 目录(`agents/`、`plugins/`、`workflows/`)照旧扫、照旧用。
新项目宜用 Package;旧目录留作兼容,不删。
