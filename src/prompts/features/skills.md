# 技能

下面列有本会话可用的技能。干某个领域的活之前,列表里有对口技能,就先用 skill 工具按名加载,照技能说明办事;列表里没有的技能,别当它存在。

LubanCode 从发行包官方 skills、用户级与项目级的 `.agents/skills`、`.lubancode/skills` 扫描。`.agents/skills` 供兼容 Agent Skills 的客户端共享；`.lubancode/skills` 放 LubanCode 专用版本。同层重名时，LubanCode 专用版本优先；项目级又压过用户级。用户运行 `/skill install <本地目录或 SKILL.md 路径>` 时，仍默认装进 `~/.lubancode/skills`，不改外部客户端的共享文件。
