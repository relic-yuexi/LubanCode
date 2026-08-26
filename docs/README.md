# LubanCode 文档

[根 README](../README.md) · [功能目录](features/README.md) · [参考手册](reference/README.md) · [架构说明](architecture/README.md) · [开发手册](development/README.md)

这里只收产品、架构与开发文档。版本号以 `CMakeLists.txt` 和
`src/app/version.hpp` 为准；已发布变化看 [CHANGELOG](../CHANGELOG.md)。文档若
与程序输出、源码或测试冲突，以后三者为准，并把错页补回来。

求职手册、问题库与复盘已经移到仓库顶层 [interview/](../interview/README.md)。
它们不随发行包装走，也不拿来定义产品现状。未来设计仍放 `todos/`，不混进
功能手册。

## 从哪儿读起

| 想做什么 | 先读 | 再查 |
| --- | --- | --- |
| 安装、启动、排错 | [根 README](../README.md#安装) | [排错手册](getting-started/troubleshooting.md) |
| 看现有功能 | [功能目录](features/README.md) | [功能总账](reference/feature-index.md) |
| 查命令、字段、工具参数 | [参考手册](reference/README.md) | [安全模型](development/security.md) |
| 恢复、压缩或管理上下文 | [会话](features/sessions/README.md) | [压缩机制](features/context/compaction.md) |
| 接 Provider、Skill、Hook、MCP、LSP 或插件 | [扩展指南](features/extensions/README.md) | [配置手册](reference/configuration.md) |
| 看请求、工具、Agent 怎样运转 | [架构说明](architecture/README.md) | [Query 数据流](architecture/query-data-flow.md) |
| 编译、测试、发版 | [开发手册](development/README.md) | [测试指南](development/testing.md) |

## 目录规矩

```text
docs/
  getting-started/   安装后的第一步与排错
  features/          按功能模块写用户可见行为
  reference/         命令、配置、工具等精确契约
  architecture/      内部数据流、状态机与模块边界
  development/       构建、测试、规范、安全与发版
  assets/            文档图片
```

- 一页先认一门职分。使用步骤不与内部机理搅成一锅。
- 模块增页，先挂到本层 `README.md`，再登记进 `catalog.txt`。
- 面试材料不回流 `docs/`。TODO 目标也不能写成现有功能。
- 相对链接、目录收录和官方 Skill 路由由 `bash scripts/check_docs.sh` 查。

## 权威入口

| 事实 | 源码或运行入口 | 文档 |
| --- | --- | --- |
| 启动参数与 slash 命令 | `src/app/cli_options.*`、`src/cli/slash_commands.*` | [命令参考](reference/commands.md) |
| 配置字段与默认值 | `src/config/config.*`、`lubancode --config` | [配置手册](reference/configuration.md) |
| Provider schema | `src/config/provider_catalog.*`、`catalog/providers.json` | [Provider 目录](features/providers/catalog.md) |
| 工具名与 schema | `src/tools/*`、`ToolRegistry` | [工具参考](reference/tools.md) |
| 会话与上下文 | `src/sessions/session_store.*`、`src/agent/compact.*` | [会话](features/sessions/README.md) |
| 构建、测试与发行 | `CMakeLists.txt`、`.github/workflows/*` | [开发手册](development/README.md) |
