# GUI Agent — 本地 process 插件示例(Windows 桌面自动化)

模型反复走"看一眼,动一下,再看一眼":快照(UIA 控件树)、截图、移鼠标、
点击、输文字,十件工具,件件短命——每次调用起一只进程,干完就退。窗口、
光标、前台焦点,全保存在 Windows 桌面手里。**插件自己没有状态,所以不
需要 MCP。**

找控件有两条路:**结构路**(`gui_snapshot` 给控件名+类型+矩形,省 token、
坐标直给)是正路;**视觉路**(截图看图)留给复核与自绘界面——这正是
Browser Agent 的 snapshot/ref 体系在桌面的对影。

完整设计账见 `todos/GUIAgent与BrowserAgent扩展示例.todo`;本 README 教你装、
跑、看懂、改成自己的。

## 1. 它为何不是 MCP

| 疑问 | 答案 |
| --- | --- |
| 截图点击也算"会话"吧,为何不搭 MCP? | 状态在**桌面与目标程序**里,不在插件里。每次调用重新问桌面要窗口矩形,要完就退,没有跨调用要保活的对象。 |
| MCP 有 tools/list 发现,这没有? | manifest `plugin.json` 就是静态真账,十件工具名与 schema 加载期一次读齐。 |
| 进程崩了怎么办? | 只坏本次调用,宿主收到唯一错误码,不带倒 LubanCode。 |
| 什么时候该改用 MCP? | 工具要保存 DOM handle、订阅事件、维持数据库连接或接收异步下载时——状态住进了你的进程,短命进程装不下。那类示例见 Browser Agent(另单在建)。 |

一句话:**一次性本地函数走 process 插件;有独立状态与生命周期的服务走 MCP。**

## 2. 安装(十分钟内到第一项可见结果)

前置:Windows 10/11,Python 3.10+(`python --version` 能出版本号;官方
安装包默认带 ctypes 与 tkinter)。**零第三方依赖**——不装 mss、不装
pyautogui、不装 Pillow,理由见 `requirements.txt`。

```bat
:: 1. 拷目录(整只 gui-agent 一起,runner 要 import 同目录模块)
xcopy /E /I examples\agents\gui-agent %USERPROFILE%\.lubancode\plugins\gui-agent

:: 2. 第一回先用 dry-run:动作只校验不注入
set LUBANCODE_GUI_DRY_RUN=1

:: 3. 起 LubanCode(同这只终端,环境变量才递得进插件)
lubancode
```

进 LubanCode 后:

```text
/plugins                 → 应见 gui-agent-example: 10 个工具
/tools                   → 模型侧工具名 plugin__gui-agent-example__gui_status 等
```

试试:

```text
你:用 gui_status 看一眼桌面环境
模型:调用 plugin__gui-agent-example__gui_status →
      平台 win32(承诺范围 Windows 10/11),Python 3.13,DPI 感知
      per_monitor_v2,显示器 2 台,虚拟屏 [-1920,0,2560,1440],dry-run 开。
```

dry-run 关掉(去掉环境变量重启)后才真点鼠标。教程第一课建议一直开着。

### 装到哪:用户级与项目级(信任门)

上面那条 xcopy 装的是**用户级**——`%USERPROFILE%\.lubancode\plugins\`。你
亲手放进去的,免信任门,拿来即用。

另一路是**项目级**:拷进仓库里的 `<项目>\.lubancode\plugins\gui-agent\`,
随 git 分发,团队人人 clone 即得:

```bat
xcopy /E /I examples\agents\gui-agent <项目>\.lubancode\plugins\gui-agent
```

项目目录里的插件是外来代码,放进目录就是执行代码。LubanCode 启动时按
"插件目录 + 全部文件内容的指纹"过**信任门**,未经信任只警告、不挂载:

```text
[plugin] gui-agent-example: 项目插件未经信任(内容指纹 a1b2c3d4e5f6),跳过——
  ……批准:/plugin trust gui-agent-example(回执亮工具清单与完整指纹,重启后挂载)
```

批准是一条命令的事,不用手改任何 JSON:

```text
/plugin trust gui-agent-example
```

回执先把插件概要亮出来(工具清单、文件数、完整 64 位指纹),再落账:
**已信任,重启后挂载**。插件文件改一个字节,指纹就变,信任失效,须重批
——团队仓里升级插件后,各成员重跑一次 trust 即可。撤销用
`/plugin untrust gui-agent-example`(只销账,不动插件文件)。

怎么选:团队仓里带插件共用走项目级;个人自用装用户级,省一道门。

## 3. 教学夹具与第一课

不拿真实微信、银行、邮箱练手。仓里带一只本地夹具:

```bat
:: 另开一只终端
python examples\agents\gui-agent\fixtures\fixture_app.py
```

窗口标题 `LubanCode GUI Fixture`:名字输入框、颜色下拉、提交/重置按钮、
结果行、事件账。事件账逐笔显示窗口收到的 Button/Key——注入到没到,
一眼对账。加 `--events-file PATH` 可让事件账同步落盘,脚本对账用;
`--state-file PATH` 在提交时写出 `{submitted, name, color}` 供断言。
夹具还在布局落定后把控件文字写进各自的 helper HWND(tkinter 的控件在
Windows 上各有一枚 helper HWND,但 Tk 只管自己画、HWND 文字留空,UIA
快照就只见类型不见名——夹具自己补齐,顺带教这一课;真世界自绘程序
没这份好心,见第 10 节排错)。

任务链(Skill `SKILL.md` 教的就是这条),结构路为主、截图只作复核:

1. `gui_list_windows`(title_filter=LubanCode GUI Fixture)→ 拿 window_id
2. `gui_focus_window` → 前台
3. `gui_snapshot` → 控件树(每行 `ref | 类型 | Name | rect`)
4. `gui_click`(名字输入框 rect 中心 + expected_window_rect)→ 点名字框
5. `gui_type_text`("阿明")→ 中文 Unicode 直注
6. `gui_key`(["down"] / ["up"])→ 颜色选值
7. `gui_click`(提交按钮 rect 中心)→ 提交
8. `gui_snapshot`(结果行文字变了)/`gui_screenshot` → 复验

纯视觉路(截图→看→点)也整条可用,`scripts/manual_e2e.py` 走的就是它;
结构路的对影脚本 `scripts/uia_snapshot_e2e.py` 零截图点提交。

对 LubanCode 说:"用 gui-agent 插件,在 LubanCode GUI Fixture 里填名字
阿明、颜色保持 green、点提交,然后快照确认",模型照 Skill 走。

## 4. 一份真实的工具调用长什么样

宿主发给插件进程的请求(stdin,恰好一份 JSON):

```json
{"protocol": 1, "call_id": "req-17", "plugin": "gui-agent-example",
 "tool": "gui_click",
 "arguments": {"x": 104, "y": 20, "coordinate_space": "window_client",
               "window_id": "0x001A0B0C",
               "expected_window_rect": [100, 80, 580, 440]},
 "context": {"cwd": "D:/project"}}
```

插件进程的响应(stdout,恰好一份 JSON):

```json
{"protocol": 1, "call_id": "req-17", "ok": true,
 "content": [{"type": "text",
   "text": "已发送 left 点击 x1 到 (212,132)。这只是动作事实,不代表界面已变;下一步须 gui_screenshot 复验。"}],
 "structured": {"x": 212, "y": 132, "button": "left", "clicks": 1,
                "ensured_foreground": true, "window_id": "0x001A0B0C",
                "verify_next": "gui_screenshot"}}
```

- `call_id` 宿主生成,响应对不上即协议错——排查串话先看它。
- `arguments` 先过 manifest schema 验证(宿主侧),类型不对根本起不了进程。
- `content[0].text` 是模型读的话;`structured` 是机器账(前端/测试用)。
- 插件日志走 stderr,烂在 stdout 里就是协议错(`bad_json`),不会从字堆里猜。

出错时长这样(`ok=false`,插件自报稳定错误码):

```json
{"protocol": 1, "call_id": "req-18", "ok": false,
 "error": {"code": "stale_observation",
           "message": "观察已过期:截图时窗口矩形 [100,80,580,440],现在是 [220,160,700,520]。请重新 gui_screenshot 再动作。"}}
```

## 5. 观察→动作→再观察的数据流

```text
        ┌─ gui_screenshot ──→ 桌面(PrintWindow 离屏抓窗)
        │      ↓ PNG 编码(手写,zlib) → 落盘 artifact
        │      ↓ observation{window_rect, client_size, dpi_scale, sha256...}
        │
   模型 │  拿 observation 里的 window_rect 填进动作参数
        ↓
   gui_click{x,y,window_client,window_id,expected_window_rect}
        │      ↓ 重查窗口:在?矩形没变?→ 变了 = stale_observation 拒
        │      ↓ 换算客户区坐标 → 全桌面物理像素 → 查界内
        │      ↓ 不在前台?先聚焦(复查成功才算)
        │      ↓ SendInput 注入(或 dry-run:到此为止,只报计划)
        │
   模型 ← 动作事实(不含结果判断)
        │
        └─ gui_screenshot ──→ 复验,才许说"成功"
```

Skill 的死规矩:一次一项动作,动作后必快照/截图;不许连续盲点五次。

## 6. 结构路:gui_snapshot(UIA 控件树快照)

`gui_snapshot` 是桌面版 `browser_snapshot`:Windows UI Automation 把
目标窗口的控件树文本化,每行 `ref | 类型 | Name | rect`,模型按名找
控件、按 rect 中心点击——不烧一枚截图 token,也不依赖多模态。真机
回执长这样(夹具窗口):

```text
窗口 'LubanCode GUI Fixture'(0x04580338)UIA 快照:depth=8,收 13 项,走访 16 节点,42ms。ref 只在本份快照内有效;rect 是全桌面物理像素(virtual_screen 口径,与截图同源),动作取矩形中心。
- e1 | image | 事件账 | rect=[339, 672, 969, 743]
- e2 | image | (未提交) | rect=[339, 510, 517, 581]
- e3 | button | 重置 | rect=[571, 430, 642, 484]
- e4 | button | 提交 | rect=[435, 430, 506, 484]
- e5 | pane | 颜色下拉 | rect=[435, 493, 825, 530]
- e6 | image | 颜色: | rect=[367, 493, 411, 523]
- e7 | pane | 名字输入框 | rect=[435, 353, 825, 390]
- e8 | image | 名字: | rect=[367, 353, 411, 383]
- e9 | menubar | 系统 | rect=[367, 287, 409, 331]
- e10 | menuitem | 系统 | rect=[367, 287, 409, 331]
- e11 | button | 最小化 | rect=[1016, 285, 1110, 342]
- e12 | button | 最大化 | rect=[1110, 285, 1202, 342]
- e13 | button | 关闭 | rect=[1202, 285, 1296, 342]
```

几条设计账:

- **ref 是本份快照内的短码**(e1、e2……,browser 侧同款做法),重拍即
  换号;不做窗口代次失效保护——每次快照重发新 ref,窗口没了靠
  `window_not_found` 兜底,动作坐标防过期靠 `expected_window_rect`。
- **只收模型要动的东西**:交互控件(按钮/输入/勾选/下拉/页签/列表项/
  菜单项)无名也收;文本与容器带 Name 才收(有名的容器是标签化区块,
  比如 tkinter 的输入框在 UIA 里就是一枚带名 pane);纯容器折叠,但
  不占深度。
- **三顶帽子**:深度帽 `depth`(默认 8,最大 24)、元素帽(400 项,
  正文只铺前 60 行,全量在 structured)、时间帽(8s,防无响应窗口)。
  超帽如实报"截断",教模型用 depth 收窄。
- **零第三方依赖的 UIA 接入**:ctypes 手调 IUIAutomation COM 壳
  (`gui_uia.py`),vtable 下标照 Windows SDK 头文件逐条对过。为什么
  不起 PowerShell 子进程跑 System.WindowsAutomation:其一,坐标口径——
  插件进程是 per-monitor v2 DPI 感知的,in-proc UIA 的矩形与截图、
  SendInput 同一物理像素;PowerShell 子进程是 DPI-unaware 的,托管 UIA
  在高 DPI 屏会把矩形虚拟化甚至给空矩形。其二,托管 UIA 微软自己都
  不建议新用。其三,起 powershell.exe 加载 CLR 每回一到三秒,in-proc
  走树毫秒级。代价是手排 vtable——文件头注释里立了账。
- **盲区如实写**:自绘控件(游戏、部分 Electron、老自绘 Win32)UIA
  看不见,收 0 项时回执明说"回视觉路"。tkinter 自己也半瞎:控件有
  helper HWND 但不带文字,名字得应用程序自己补(夹具就是这么做的)。

## 7. 坐标空间、DPI 与 stale observation

- **virtual_screen**:全桌面物理像素联合坐标系,多屏时含负原点(左屏
  x 从 -1920 起)。截图像素与它一一对应。
- **window_client**:目标窗口客户区,左上 (0,0),不含标题栏。执行前
  换算,窗口不给就拒。
- **DPI**:插件进程先调 `SetProcessDpiAwarenessContext`
  (Per-Monitor v2),全链路物理像素,与截图像素同一口径。
  `gui_status` 报 `dpi_awareness`;若见 `unaware`,坐标会被系统虚拟化,
  先排这一条。
- **stale observation**:截图时窗口矩形进 observation;动作带
  `expected_window_rect`,执行前重查,不合即拒。它防的是**坐标过期**,
  不是权限——observation 不是授权凭据。窗口换进程、换标题、换矩形,
  默认都拒。

## 8. 图随结果回喂(协议 v2)

process 插件协议 v2 起,`content` 里可以带 `type=image` 块。
`gui_screenshot` 在文本与 observation 之外,把 PNG 以 path 模式随响应帧
回给宿主:

```json
"content": [
  {"type": "text", "text": "已截图 窗口 'x' ... 图已随结果回喂 ..."},
  {"type": "image", "mime_type": "image/png", "path": "C:\\...\\gui-obs-....png"}
]
```

宿主收到后照 MCP 富结果的同一条规矩验身(魔数复核、大小帽 20MB、内容
寻址落会话 artifact),再由四家 wire 原生上协议(anthropic 的 tool_result
image 块 / responses 的 input_image 数组 / Gemini 3+ 的 inlineData;chat
wire 明降级为路径附注)。也就是说:**截图保存成功 = 模型看见截图**,
视觉判断不再需要人工核对文件。证据文件照旧落盘——路径作附账,artifact
可追。observation 里的标记同步翻真:

```json
"model_visibility": {"rich_result": true, "protocol": 2,
                     "note": "截图经协议 v2 image 块回喂模型,模型已看见;证据文件照旧落盘。"}
```

老宿主(只认 v1)收到 protocol=2 的响应会按 UnknownContent 整帧拒——
本插件随宿主 v2 一起交付,不做双协议回退。

**降采样口径**:截图长边超过 1568px 的,先按整数步长采样缩进帽内
(`png.downscale_to_long_edge`,零依赖、不插值)再回喂与落盘,一份图
两处共用。各家视觉 token 都按分辨率计(anthropic ≈ 宽×高/750,建议
长边 ≤1568;gpt/gemini 按 512/768 像素块计片),3072x1918 的整窗原图
原样上 wire 既烧 token 又顶各家边长帽。要原图附账,调用时传
`keep_original=true`,证据目录另存 `-orig` 后缀一份,默认不双份。

## 9. 安全模型

| 类别 | 工具 | 说明 |
| --- | --- | --- |
| 观察 | `gui_status` / `gui_list_windows` / `gui_snapshot` / `gui_screenshot` | 无输入注入。快照只读控件树,连像素都不碰;截图默认只拍目标窗口,`target=screen` 拍全部显示器,当心隐私。 |
| 低风险动作 | `gui_focus_window` / `gui_move_mouse` | 改前台、挪鼠标,不产生点击。 |
| 写动作 | `gui_click` / `gui_scroll` / `gui_type_text` / `gui_key` | 每次调用前 LubanCode 照常确认;`.lubancode/settings.local.json` 的 `allow_tools` 只按名单放,别开 `plugin__gui-agent-example__*` 通配。 |

- `LUBANCODE_GUI_DRY_RUN=1`:动作类工具全部只校验只报计划,连聚焦都不发。
- Win 组合与 Alt+F4 默认禁(`dangerous_key_blocked`);确要放行须显式
  `LUBANCODE_GUI_ALLOW_DANGEROUS_KEYS=1`。
- 密码、OTP、验证码一律不得经 `gui_type_text`;遇到密码框就停。
- 不碰剪贴板(中文走 KEYEVENTF_UNICODE 键盘事件,非粘贴)、不提权、
  不碰 secure desktop。鼠标移屏幕角落不是本插件的取消机制——ESC 走
  LubanCode 自己的取消链,插件进程随调用被收尸。
- 文本帽 4096 字符、滚轮帽 50 格、连点帽 3 次、截图帽 8MB——全在注入
  前收口。

## 10. 排错

| 症状 | 先查 |
| --- | --- |
| `/plugins` 不见 gui-agent-example | 启动警告点名 manifest 哪项;`plugin.json` 须与 runner.py 同目录整只拷贝 |
| 调用报 `spawn_failed` | `python` 不在 PATH → manifest 的 `runtime.command` 写绝对路径 |
| `gui_status` 报 dpi_awareness=unaware | 系统拒了 DPI 感知调用;坐标会错位,先解决它再干活 |
| `focus_failed` | Windows 限制后台进程抢前台;人工点一下目标窗口,或让用户先激活它 |
| `stale_observation` | 不是故障——窗口挪了。重新 screenshot,拿新矩形再动 |
| `window_not_found` | 窗口关了或换了桌面;重新 list |
| `dangerous_key_blocked` | 预期行为;确认真要用再开环境变量 |
| 中文输入没出现 | 窗口是否前台(`ensured_foreground`);目标框是否只读 |
| 点了按钮没反应 | 多半点开了下拉/浮层没收起——后续点击落在浮层上(夹具事件账里 `widget=str` 就是它)。先 `gui_key` Enter/Esc 收起浮层再点 |
| 截图全黑 | 最小化窗口被拒是预期;若目标用 GPU 合成,PrintWindow + PW_RENDERFULLCONTENT 已覆盖大多数;仍黑就改 `target=screen` 前先想隐私 |
| `gui_snapshot` 收 0 项 | 多半是自绘界面(游戏/部分 Electron/老自绘 Win32)没给 UIA 暴露控件——结构路的盲区,回 `gui_screenshot` 视觉路;提权窗口(管理员跑的)探不全也是它 |
| 快照里控件没名字 | 应用没给 UIA 名字(tkinter 默认就这样:控件画在 Tk 手里,helper HWND 文字是空的)。位置与类型还在,能按 rect 点;要名字得应用自己补(教学夹具就是这么做的) |
| 快照被截断 | 树太宽/太深。带更小的 `depth` 重拍,或换枚更具体的窗口 |
| 点击按快照 rect 落偏 | 先 `gui_status` 查 DPI;快照 rect 与截图、SendInput 同一物理像素口径,DPI unaware 时全链路一起错位 |

## 11. 改造成你自己的工具(五步)

1. **改工具名**:拷走整目录,`plugin.json` 里改 `id` 与各 `tools[].name`
   (模型看到的就是 `plugin__<id>__<name>`)。
2. **改 schema**:每件工具的 `input_schema` 收窄到你真要的参数,
   `additionalProperties: false` 别松。
3. **改实现**:`gui_actions.py` 里每件工具一个函数,签名
   `(backend, arguments, settings)`;Win32 花活进 `gui_backend.py`。
4. **改权限**:dry-run 开关、危险键闸、上限常量,全在 `gui_actions.py`
   顶部;env allowlist 在 `plugin.json` 的 `permissions.env`。
5. **补测试**:`test_runner.py` 加册,注入 `FakeBackend`,断言坐标换算
   与拦截路径;真机链路照 `scripts/manual_e2e.py` 的架子另写。

练习三题(各守边界,不拿真实账号/生产数据练):

- **控制计算器**:用 `gui_key` 打数字与运算符。边界:别开任务管理器,
  别动系统设置。
- **控制自家测试工具**:替你点"跑一遍回归"按钮并截图存证。边界:按钮
  名单写死,提交类动作先问。
- **给图片编辑器添重复动作**:批量"打开→滤镜→另存"。边界:只动你建的
  临时目录,写动作全留确认。

## 12. 源码地图与平台承诺

| 文件 | 管什么 |
| --- | --- |
| `plugin.json` | manifest:id、runtime、十件工具 schema、env allowlist、network=false |
| `runner.py` | 协议 v1:stdin 一份 JSON → stdout 一份 JSON,日志进 stderr |
| `gui_actions.py` | 合同层:坐标换算、stale 拦截、上限、危险键闸、dry-run、observation |
| `gui_backend.py` | Win32 ctypes 层:窗口枚举、DPI、SendInput、BitBlt;`FakeBackend` 供测试 |
| `gui_uia.py` | UIA COM 壳:ctypes 手调 IUIAutomation,控件树折叠规则与帽子 |
| `png.py` | 零依赖 PNG 编码(zlib + CRC32),魔数自检 |
| `test_runner.py` | 离线自测(零真输入):`python test_runner.py`,41 册 |
| `fixtures/fixture_app.py` | 教学夹具:一条命令起的本地小窗 |
| `scripts/manual_e2e.py` | 视觉路 E2E(截图→看→点),默认 SKIP,`--run` 才动鼠标 |
| `scripts/uia_snapshot_e2e.py` | 结构路 E2E(快照→rect 中心点提交,零截图),默认 SKIP,`--run` 才动鼠标 |
| `SKILL.md` | 教模型的操作纪律(拷到 `~/.lubancode/skills/gui-agent/`) |
| `config.example.json` | settings.local.json 样例与环境开关说明 |

平台承诺:首版只承诺 Windows 10/11。`gui_status` 在别的平台如实回
`unsupported_platform`,不装能跑。Linux/macOS 留了接口
(`gui_backend.make_backend` 分派点),欢迎照 Win32 层的样子补 X11/Cocoa。

第三方依赖:无(版本与体积见 `requirements.txt` 的说明)。夹具用 tkinter,
Windows 官方 Python 自带。

## 13. 自测命令速查

```bash
# 离线单测(零真输入,CI 同款)
python test_runner.py

# 干跑 E2E(真枚举真截图,不注入点击)
python scripts/manual_e2e.py --run --dry-run

# 真跑 E2E(会动鼠标,须专用桌面)
python scripts/manual_e2e.py --run

# 结构路 E2E:快照定位、零截图点提交(会动鼠标,须专用桌面)
python scripts/uia_snapshot_e2e.py --run

# 手工灌一份协议请求看响应
echo {"protocol":1,"call_id":"t1","plugin":"gui-agent-example","tool":"gui_status","arguments":{},"context":{}} | python runner.py
```
