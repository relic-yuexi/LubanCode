# 技能安装与覆盖顺序

## 技能安装

LubanCode 扫描三层技能。官方层随发行包安装；用户与项目层留给自定义，不扫描 Codex、Claude 或其他代理的技能目录：

- 官方层：程序旁的 `skills/<技能名>/SKILL.md`；不可用 `/skill remove` 删除。
- 用户级：`~/.lubancode/skills/<技能名>/SKILL.md`。
- 项目级：`<cwd>/.lubancode/skills/<技能名>/SKILL.md`。

用 `/skill install <来源>` 安装到用户级目录。来源既可写 HTTP(S) 地址，也可写本机技能目录、目录里的 `SKILL.md`，或一份独立 `.md`。Windows 路径带空格也可直接放在命令余下部分；外层引号会自动剥掉。安装成功后技能清单当场刷新，不必重启。`/skill list` 查位置与来源，`/skill update [名字]` 更新有远端来源记录的技能，`/skill remove <名字>` 删除。

用户若指着 `.codex/skills`、`.claude/skills` 或 `.agents/skills` 里的现成技能叫你“复制过去”，目标仍须是 `~/.lubancode/skills/<技能名>`，不可在这些外部目录之间互抄。复制整包，且让 `SKILL.md` 直接落在目标根部，不能多套一层同名目录。
