# 检查更新与同步官方技能

## 检查新版

- 交互会话里运行 `/update` 或 `/update check`。
- 不进会话时运行 `lubancode --check-update`。
- 命令只查 GitHub 最新 Release，不会悄悄改文件。若有新版，会打印当前版本、最新版本与发布页。

## 升级

从新版发行包运行对应安装脚本：

- Windows：`powershell -ExecutionPolicy Bypass -File .\install.ps1`
- macOS / Linux：`./install.sh`

安装脚本先换程序，再把发行包里的 `skills/` 同步到官方技能目录。这样程序与官方文档同版。

用户级 `~/.lubancode/skills/` 与项目级 `<cwd>/.lubancode/skills/` 不归安装器改动。用户自建内容会留下。若同名自定义技能存在，仍按覆盖顺序生效。
