# 会话与终端操作

## 常用会话命令与快捷键

- `/context` 看上下文占用；`/context 512k` 临时改窗口（只本会话）。
- `/compact [重点]` 手动压缩历史成存档；占用超 80% 会自动压缩。
- `/clear` 清空历史开新会话；`/resume`、`/sessions` 续聊与列存档；`/export` 导出 Markdown。
- `/model`、`/think`、`/language`、`/soul`、`/prompt`、`/skills`、`/tools`、`/mcp`、`/lsp`、`/todos`、`/config` 各管一摊，`/help` 有全表。
- 快捷键：Ctrl+O 切换紧凑/详细；Tab/Shift+Tab 在工具条目间移焦点；Ctrl+E 聚焦查看全文;ESC 打断当前轮。
- 模型工作时可直接键入下一条并回车；消息留在常驻队列区，本轮收尾后依次发送。模型遇到会改变方向的选择时，可用 `ask_user` 摆出选项，末项由用户自行填写。
