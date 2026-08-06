# 钩子与本地权限

### hooks

`hooks` 可有 `pre_tool`、`post_tool`、`session_start`、`session_end` 四个数组。每一项都要有字符串 `command`。命令交给 `cmd.exe` 执行。

`pre_tool` 和 `post_tool` 可再写字符串 `matcher`：精确工具名，或 `"*"` 匹配全部工具；省略或空串也当 `"*"`。`session_start` 与 `session_end` 不看 `matcher`。

```json
{
  "hooks": {
    "pre_tool": [
      { "matcher": "write_file", "command": "echo about to write" }
    ],
    "post_tool": [],
    "session_start": [
      { "command": "echo session started" }
    ],
    "session_end": []
  }
}
```

## settings.local.json：项目级本地权限（不进版本库）

`<cwd>/.lubancode/settings.local.json` 存本项目的权限约定，照 Claude Code / Codex 的路数——这是**本地文件，不该提交**（首次落地时 lubancode 会尽力在 `<cwd>/.gitignore` 补一行 `.lubancode/settings.local.json`；没有 `.gitignore` 就只给一句提示，请手动加）。结构：

```json
{
  "permissions": {
    "allow_tools": ["write_file"],
    "allow_commands": ["npm test", "git status"],
    "deny_commands": ["rm -rf"],
    "default_confirm_mode": "auto"
  }
}
```

全部字段可选；坏 JSON 只告警跳过、不崩。各字段用途：

- `allow_tools`：这些工具启动即进会话「总是允许」集合，本会话直接免确认（跟按 `a` 落进来的是同一个集合）。
- `allow_commands`：`run_command` 命令前缀白名单。auto 档里命中前缀等价 `command_safety` 判成 Safe（补充白名单，不改内置判定）。
- `deny_commands`：`run_command` 命令前缀黑名单。命中前缀就**永远问一句**，压过 `allow_commands`、压过会话「总是允许」。只在 confirm / auto 档生效；`--yes` / yolo 是显式全放，`deny` 不拦。
- `default_confirm_mode`：起手确认档 `auto` / `yolo` / `confirm`。优先级低于 `--yes` / `LUBANCODE_CONFIRM_MODE`，高于内置默认 `confirm`。

`run_command` 传 `run_in_background: true` 起的后台命令，安全判定/确认流程跟前台命令一模一样——`allow_commands` / `deny_commands` / `command_safety` 照样按 `command` + `shell` 两个字段判，不会因为放后台就绕过确认。

前缀判定：命令去掉前导空白后，以某条前缀打头就算命中（原始命令串直接比，不做 shell 解析）。

### 按 a 持久化

确认某个工具时按 `a`（本会话总是允许）之后，真控制台里会多问一句「也永久写进项目 settings.local.json?[y/N]」。选 `y` 就把工具名写进 `permissions.allow_tools`（去重，项目级 `.lubancode/` 目录按需创建），下次进这个项目即免确认。管道 / `--yes` 下不追问，只进本会话集合。

`/config` 会打一行 `permissions` 摘要：`allow_tools` 几个、`allow_commands` / `deny_commands` 几条、`default_confirm_mode` 是什么。

### 加 pre_tool 钩子

编辑 `~/.lubancode/config.json`，在 `hooks.pre_tool` 加一项。`matcher` 填工具精确名，或填 `"*"`：

```json
{
  "hooks": {
    "pre_tool": [
      { "matcher": "write_file", "command": "echo about to write" }
    ]
  }
}
```

`command` 是交给 `cmd.exe` 的整条命令。它会在工具之前执行，先审命令内容，再落配置。
