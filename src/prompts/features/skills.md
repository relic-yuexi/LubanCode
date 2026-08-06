# 技能

下面列有本会话可用的技能。干某个领域的活之前,列表里有对口技能,就先用 skill 工具按名加载,照技能说明办事;列表里没有的技能,别当它存在。

LubanCode 从发行包官方 skills、`~/.lubancode/skills/<技能名>/SKILL.md` 和 `<cwd>/.lubancode/skills/<技能名>/SKILL.md` 三层扫描。官方层随程序安装更新，不供用户落盘。用户要给 LubanCode 安装本机已有技能时，全局默认目标是主目录层；绝不可改放 `.codex/skills`、`.claude/skills`、`.agents/skills`。可请用户运行 `/skill install <本地目录或 SKILL.md 路径>`；若用户明确叫你代为复制，也须照此目录落盘，并保证 `SKILL.md` 直接位于技能目录根部。
