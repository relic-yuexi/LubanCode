---
name: gui-agent
description: 用 gui-agent-example 插件操作 Windows 桌面程序:截图→看→点→输入的循环。凡要"点某软件某按钮/往某窗口填字"的桌面任务,先读这份。
---

# GUI Agent 操作纪律

桌面程序的状态在窗口里,不在你手里。你每回只拿到一帧画面、一份坐标,
窗口随时会挪、会最小化、会弹新框。所以纪律只有一条:**看一眼,动一下,
再看一眼**。盲点是这条纪律的死罪。

## 开工三步

1. 先调 `plugin__gui-agent-example__gui_status`:确认平台是 win32、
   DPI 感知不是 unaware、dry-run 是开是关。dry-run 开着就明说:本次
   任务只校验不真点,请用户决定是否关掉再跑。
2. 再 `gui_list_windows`(可带 `title_filter`)找到目标窗口,记下
   `window_id` 与 `rect`。窗口 id 只在本次桌面现场有效,不写进长期
   记忆当永久凭据。
3. `gui_focus_window` 聚焦,然后 `gui_screenshot`(target=window)
   拿 observation。

## 观察

- 协议 v2 起,截图随结果回喂——**你已经看见图**。直接描述画面、
  直接据图决策,别再让用户人工核对文件。证据文件照旧落盘(附账),
  需要留档引用路径即可。
- observation 里的 `window_rect` 是这一刻的现场。动作时把它原样填进
  `expected_window_rect`:窗口挪过,工具会拒(stale_observation),
  这是保护,不是故障。
- 坐标口径两种:`virtual_screen`(全桌面物理像素,可含负值)与
  `window_client`(目标窗口客户区)。截图像素对应 virtual_screen 坐标;
  用 client 坐标必须带 window_id。

## 动作

- **一次只做一项动作**,做完立刻重新 `gui_screenshot` 复验,再决定
  下一步。不许连点五下不看一眼。
- 坐标以最近一次截图与 observation 元数据为准,不凭上一轮记忆猜。
- `gui_click` 只报告"点击已发送",不代表按钮生效。生效与否,下一张
  截图说了算。
- `gui_type_text` 输中文没问题(Unicode 直注);但密码、验证码、OTP
  一律不得经工具输入——遇到密码框就停下,请用户自己填。
- `gui_key` 只用枚举键名。Win 组合与 Alt+F4 默认被拦,别绕。

## 停手与复验

- 见"提交/删除/付款/发送/上传"字样,先停下向用户确认,得到明确
  同意才点。
- 见 `stale_observation`、`window_not_found`、`focus_failed`:
  不是重试同一份旧参数,而是重新 `gui_list_windows` → `gui_screenshot`,
  从新现场重新走。
- 同一动作失败两次,停下来向用户报告现象,不要换着花样硬闯。

## 收工

向用户报清:每一步做了什么动作、哪几步经过截图复验、哪些结果**没有**
复验过。没复验的不说成功。
