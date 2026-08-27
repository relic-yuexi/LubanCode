---
name: gui-agent
description: 用 gui-agent-example 插件操作 Windows 桌面程序:list→snapshot(结构路)→按 ref 动作,截图只作视觉复核。凡要"点某软件某按钮/往某窗口填字"的桌面任务,先读这份。
---

# GUI Agent 操作纪律

桌面程序的状态在窗口里,不在你手里。找控件有两条路:**结构路**(gui_snapshot
给控件名+类型+矩形,又省又准)是正路;**视觉路**(截图看图)留给复核与
自绘界面。纪律一条:**先看一眼,动一下,再看一眼**。盲点两下,是死罪。

## 开工三步

1. 先调 `plugin__gui-agent-example__gui_status`:确认平台是 win32、
   DPI 感知不是 unaware、dry-run 是开是关。dry-run 开着就明说:本次
   任务只校验不真点,请用户决定是否关掉再跑。
2. 再 `gui_list_windows`(可带 `title_filter`)找到目标窗口,记下
   `window_id` 与 `rect`。窗口 id 只在本次桌面现场有效,不写进长期
   记忆当永久凭据。**刚 run_command 起的程序,窗口可能要几秒才成**:
   首查扑空别下结论,隔两秒重查,最多五次。
3. `gui_focus_window` 聚焦,然后 `gui_snapshot` 拿控件树:每行
   `ref | 类型 | Name | rect`。按 Name 找目标控件,动作取 rect 中心;
   树被截断就用更小的 `depth` 收窄重拍。

## 找控件:结构路优先

- 快照是文本,比截图省 token 一个量级,坐标直接给到手上。点按钮、
  填输入框,先想快照,别先想截图。
- ref 只在本份快照内有效(快照重拍即换号);跨调用引用 rect 时,
  窗口可能已挪——动作带 `expected_window_rect`(快照 structured 里有
  `window_rect`)可拦住过期坐标。
- **快照收 0 项或找不到目标**:多半是自绘界面(游戏、部分 Electron、
  老自绘 Win32)没给 UIA 暴露控件——这是结构路的盲区,别反复重拍,
  回视觉路(`gui_focus_window` → `gui_screenshot`)。
- 有名无名有讲究:交互控件(按钮/输入/勾选/下拉/页签/列表项/菜单项)
  无名也收;文本与容器要带 Name 才收。快照里没看见的控件,就是 UIA
  看不见,不是不存在。

## 视觉路:截图复核

- 协议 v2 起,截图随结果回喂——**你已经看见图**。直接描述画面、
  直接据图决策,别再让用户人工核对文件。
- 截图用来干三件事:自绘界面上找控件(结构路的盲区)、动作后的视觉
  复核(按钮状态、颜色、画面变化)、确认布局与排版。纯找位置就别烧
  这枚 token,快照够了。
- observation 里的 `window_rect` 是这一刻的现场。动作时把它原样填进
  `expected_window_rect`:窗口挪过,工具会拒(stale_observation),
  这是保护,不是故障。
- 坐标口径两种:`virtual_screen`(全桌面物理像素,可含负值)与
  `window_client`(目标窗口客户区)。快照 rect 与截图像素对应
  virtual_screen 坐标;用 client 坐标必须带 window_id。

## 动作

- **一次只做一项动作**,做完立刻重新观察(快照或截图)再决定下一步。
  不许连点五下不看一眼。
- 坐标以最近一次快照/截图与元数据为准,不凭上一轮记忆猜。
- `gui_click` 只报告"点击已发送",不代表按钮生效。生效与否,下一步
  快照或截图说了算。
- `gui_type_text` 输中文没问题(Unicode 直注);但密码、验证码、OTP
  一律不得经工具输入——遇到密码框就停下,请用户自己填。
- `gui_key` 只用枚举键名。Win 组合与 Alt+F4 默认被拦,别绕。

## 停手与复验

- 见"提交/删除/付款/发送/上传"字样,先停下向用户确认,得到明确
  同意才点。
- 见 `stale_observation`、`window_not_found`、`focus_failed`:
  不是重试同一份旧参数,而是重新 `gui_list_windows` → `gui_snapshot`,
  从新现场重新走。
- 同一动作失败两次,停下来向用户报告现象,不要换着花样硬闯。

## 收工

向用户报清:每一步做了什么动作、哪几步经过快照/截图复验、哪些结果
**没有**复验过。没复验的不说成功。
