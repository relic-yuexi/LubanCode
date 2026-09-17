# QQ 助手：会话、工具、附件与菜单

QQ 命令由宿主处理，无须先配置 `commands`。未知 slash 返回帮助提示，不交给模型猜。

| 命令 | 行为 |
|---|---|
| `/help` | 列出内置命令与自定义别名 |
| `/session`、`/sessions` | 列出当前 QQ 聊天、当前工作区的逻辑会话 |
| `/session current` | 查看当前逻辑会话编号 |
| `/new`、`/clear`、`/session new` | 选择一场新上下文；首次发言才创建 V3 session |
| `/session switch default` | 切回原会话；其他编号从 `/session` 列表取 |
| `/status`、`/tools`、`/skills` | 查看当前工具与技能装配状态 |
| `/files` | 文件与图片说明 |
| `/reminders` | 查看自己的提醒 |
| `/menu` | 查看菜单启用方式 |

新会话不删除历史，不取消提醒。选择写进账号的 `sessions.jsonl`，重启后恢复；无法输入他人的 V3 session id 越界切换。群聊仍遵循原有 group scope，共享群会话不等于个人私有会话。逻辑编号与 V3 session id 分开：缓存淘汰后恢复可能生成新的 V3 id，但逻辑编号保持。

## 配置位置

只改启动目录不会改变全局配置位置。未设置 `LUBANCODE_HOME` 时，读用户目录下 `.lubancode/config.json`；设置后读 `<LUBANCODE_HOME>/config.json`。以下字段合入已有配置，别覆盖凭据或其他账号。

联网搜索需配置顶层 `search.provider`（tavily、brave、serper 之一）和 `search.api_key`，并在已声明的各层工具上限中放行 `web_search`。`/status` 会区分工具未装配、渠道未放行与须审批。

技能与 `skill` 工具使用同一份启动快照。个人布局加载官方、用户和项目 `.agents/skills`、`.lubancode/skills`；应用根布局只加载 `<LUBANCODE_HOME>/skills`。每份技能放在独立目录，入口为 `SKILL.md`。修改后重启 Gateway。还须让渠道策略允许 `skill`；技能正文不会替宿主授予 shell 权限。

要回传文件，可把 `send_file` 加入账号 `tools.approve`，执行前通过 QQ 按钮批准；如需预授权，可放进 `tools.allow`。各显式层取交集，deny 始终优先。旧账号权限不自动放宽。

```json
{
  "channels": {
    "qqbot": {
      "accounts": {
        "main": {
          "menu": {"publish": true, "preset": "assistant"}
        }
      }
    }
  }
}
```

`assistant` 预设包含帮助、新会话、会话列表、我的提醒、文件说明、功能状态。不可同时写 `preset` 与 `items`。不开 `publish` 就不发布；已有自定义菜单仍按发布器冲突规则处理。重启后检查 Gateway 发布诊断。代码支持不代表平台账号已获权限或客户端已经展示；须用真实 QQ 验收。点击菜单填入命令后，由用户发送。

## 收发附件

QQ 原生文件/图片入口可发附件。每条最多 4 件、单件下载上限 20 MiB。宿主保存原件、净化文件名，给模型路径与最多 2 KiB 文本预览。PNG/JPEG/GIF/WebP 校验字节、摘要、格式和尺寸后，以图片块接入模型输入与持久输入账；所选模型仍须支持图片。

PDF、Office、音视频、ZIP 只收原件，不自动解析或执行。处理正文需另配解析技能/工具；普通 `read_file` 不能代替 OCR、文档解析或语音识别。类型不支持或图片校验失败会如实说明。

`send_file` 每轮可暂存一件当前工作目录内文件（不超过 20 MiB）。路径规范化后仍须落在工作目录内，受保护凭据路径拒绝。宿主冻结文件副本；原件随后变动不影响投递。工具成功只表示待投递，本轮成功完成后由持久 outbox 送回当前 QQ 聊天。选择事实落盘后发生重启，可沿工作键读回同一附件。未显式回传文件时，长回复仍附完整 `.txt`。平台上传/投递失败不能当作用户已经收件。

提醒工具支持 `delay_seconds`，以原消息时间为锚；本轮时间由宿主注入，不要求模型猜当前时间。

## 验证范围

回归用例覆盖无配置命令分派、上下文隔离与切回、会话选择重放、文件快照/跨目录拒绝、图片块校验与菜单预设。CI 在 GitHub Actions 执行；真实 QQ 菜单、视觉模型与文件上传仍须账号联调。
