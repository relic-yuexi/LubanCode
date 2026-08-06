---
name: lubancode-config
description: LubanCode 官方配置与功能索引。用户问及或要改 soul/魂、提示词、主题、模型/provider、MCP、钩子、权限、搜索、LSP、技能、会话、快捷键、版本检查或升级时使用；只按问题加载对应 reference。
---
<!-- lubancode 系统维护,随版本自动更新;自定义请另建技能 -->

# LubanCode 文档索引

这是一张目录，不是整部手册。先看用户问哪一件，再从技能目录下只读对应一卷。不要一气读完全部 references。

## 按需取卷

- soul、魂、SOUL.md、souls/、提示词模块：`references/soul-and-prompts.md`
- 配置层级、字段、环境变量、目录、主题：`references/configuration.md`
- 模型、provider、models.json、特殊请求参数：`references/providers-and-models.md`
- MCP：`references/mcp.md`
- hooks、工具确认、settings.local.json、权限：`references/hooks-and-permissions.md`
- 搜索服务、web_search：`references/search.md`
- LSP、语言服务器：`references/lsp.md`
- 技能目录、安装、更新、覆盖顺序：`references/skills.md`
- 会话、上下文、快捷键、常用斜杠命令：`references/sessions-and-ui.md`
- 检查新版、升级程序、同步官方技能：`references/update.md`

## 规矩

1. 先定主题，只开最少一卷。问题横跨两处，再开第二卷。
2. 路径都以工具返回的“技能目录”为根。用 `read_file` 读 reference。
3. 改 JSON 时保留无关字段。真实密钥不进仓库、回答、测试或截图。
4. 用户自建技能放用户层或项目层。不要改发行包里的官方副本。
