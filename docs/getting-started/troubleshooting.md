# 排错手册

[文档首页](../README.md) · [命令与按键](../reference/commands.md) · [配置手册](../reference/configuration.md) · [测试指南](../development/testing.md)

这页从症状找入口。字段细节仍回专题页。排错先保现场，再缩范围；不要一见失败便删配置、清 session 或重装。

## 1. 先收五样证据

```powershell
lubancode --version
lubancode --config
git status --short
```

再记：

1. OS、终端与启动命令。
2. 当前 cwd、provider、model、wire、确认档。
3. 最短复现步骤。
4. 完整错误类别与前后几行，密钥打码。
5. 是否只在交互、单发、管道、子代理或 PTC 出现。

不要贴完整配置。`api_key`、Authorization、自定义 header、MCP env、项目路径先遮掉。

## 2. 启动与配置

| 现象 | 先查 | 再做 |
| --- | --- | --- |
| 开场显示“尚未连接” | `--config` 的 provider/model 来源 | 用 `/provider add` 添加，或 `/provider` 切换已有配置 |
| 报缺 key | provider 的 `auth` 三态与 `key_env` | 设环境变量；无鉴权须显式 `auth=none` |
| 项目里行为不同 | cwd 下 `.lubancode/config.json` | 对照来源，不先删全局配置 |
| JSON 解析失败 | 报错路径与行列 | 用 JSON parser 校验，别往 JSON 写注释 |
| 改配置没生效 | 改的是全局还是项目文件 | 重看覆盖顺序与启动 cwd |

配置优先级和默认值见[配置手册](../reference/configuration.md)。

## 3. Provider 与模型请求

### HTTP 401/403

- 检查 `auth` 是否选对。
- `env` 模式确认变量在启动 LubanCode 的同一进程环境里。
- 自定义 `extra_headers` 是否覆盖或删了 Authorization。
- base URL 是否走错区域、网关或协议。

### 404

- 看 base URL 是否已经带 `/v1`，客户端又补了一层。
- 看 `wire` 与端点是否相配。
- `/model` 列表接口与 chat 端点可能不是一套权限。

### 请求能发，工具调用坏

- 先用 `/tools` 看工具是否注册、延迟或禁用。
- 查模型是否真支持所选 wire 的 tool calling。
- Chat 兼容端常只实现半套字段；抓最小请求与响应，对照对应 wire 文档。
- PTC 与 JSON tool calling 分开测，别把模型不支持 PTC 误判成工具全坏。

### usage 或缓存不对

- 服务端不回 usage 时，客户端不能凭空补。
- Chat usage 的 `cached_tokens=0` 不一定代表服务端没命中；若显式配置 `metrics_url`，用 `/doctor cache` 对账。
- 只看本次请求前后增量，不看共享服务累计值。
- 冷、热与只改尾部三种场景分开跑。

### 请求挂死不返回（代理/TUN 截胡回环）

症状：请求发出后再无下文，连接超时与空闲读超时都不触发，主回合冻住只能杀进程；后台子代理的请求也卡在同一枚上。真机现场多伴随本机代理（Clash 一类）或 TUN 模式——回环 `127.0.0.1` 的连接被截胡后偶发重置或吞掉，客户端的连接已建、响应头永不到，两道常规闸都够不着。

排查顺序：

1. 看错误文案里有没有 `request_hard_timeout_secs`——落锤的是每枚请求的硬墙钟（默认 300 秒），说明确实挂到了绝境，不是网络慢。
2. 代理/TUN 环境下把回环与内网地址加直连规则（bypass `127.0.0.1`、`localhost` 与内网段），别让本地假服务或本地网关的流量走代理。
3. 子代理任务另有整轮墙钟 `subagent.wall_clock_timeout_secs`（默认 1800 秒）：请求墙管一枚请求，任务墙管整轮，两道先到先响。
4. 长任务合法撞墙（超长思考、超长流式回复）就显式调大 `request_hard_timeout_secs`，别设 `0` 关墙——关掉后回到旧病：挂死无兜底。
5. 再现与取证：开 `LUBANCODE_DEBUG_SUBAGENT=1`，子代理诊断日志里每枚请求一行 `request`/`stream_end`，硬超时落锤另有 `hard_timeout` 一行带耗时，拿它对照时间线。

详见[会话与上下文](../features/sessions/README.md)与[测试指南](../development/testing.md)。

## 4. 工具

| 现象 | 先查 |
| --- | --- |
| 模型说没有工具 | `/tools` 的 registered/deferred/loaded 三态 |
| 文件读不到 | cwd、相对路径、工作区边界、编码 |
| edit 报多处匹配 | 扩大 `old_string` 上下文，或明确 `replace_all` |
| 命令一直问 | 确认档、工具确认属性、settings allow/deny |
| 命令超时后仍有进程 | 是否收了进程树；后台模式另查 registry 与日志 |
| 网页只有空壳 | 页面依赖 JavaScript，换 API 或浏览器 MCP |
| LSP 无结果 | 扩展名映射、服务命令、cwd、初始化日志 |
| MCP 工具不出现 | `/mcp`、握手、`tools/list`；服务 stdout 不得写日志 |

完整 schema 与上限见[工具参考](../reference/tools.md)。

## 5. 终端与输入框

### 没颜色或没有原地重画

管道、重定向和不支持的终端会降级 plain。这是能力降级，不是主题失效。`LUBANCODE_FORCE_COLOR=1` 只强制颜色，不把管道变成真 TTY。

### 字符错位、边框断、残影

- 记终端、字号、窗口宽高与是否滚离底部。
- 查中文宽字符、emoji、ANSI 与 soft wrap。
- `Ctrl+L` 重画若能暂时救回，说明旧帧账可能失效；仍须留最小复现。
- 终端 bug 用对应 screen driver 验，不只拍图。

### Slash 提示出现，Tab 不补全

先分空闲与忙碌：空闲 composer 应按 `Tab` 补全。当前主线的忙碌排队 composer 存在一处已登记缺口：提示能画出，编辑器候选表却为空。临时可键完整命令，或等当前轮收口后再补全。修复时须让提示与编辑器共用候选状态，不能只改画面。

### 按键被别的面板抢走

`Up/Down`、`Tab`、`Shift+Tab` 会随 composer、焦点、子代理面板和队列编辑态换现职。先看 footer 当前提示，再按 Esc 逐层退出。切换只改选中项；按 Enter 才该真正换视图。

详见[终端交互](../features/terminal/README.md)。

## 6. 子代理与队列

| 现象 | 检查顺序 |
| --- | --- |
| 只见工具，不见完整对话 | 是否切进对应 agent view；transcript 是否按 task 分账 |
| 回不到 main | 当前 viewed task id、Enter/Esc 退出语义、main 项是否仍在导航表 |
| 完成项还挂着 | idle 折叠/清理时机、完成通知是否已交回 main |
| 排队消息没送 | 目标是 main 还是某 task；条目是否 edit_open/failed/target gone |
| 子代理失败 | 看终态原因：取消、step 上限、工具错、服务错，不要一概重派 |
| 定向消息串台 | 按稳定 task id 查，不按列表下标 |

主代理与子代理的 history、transcript、队列目标是三本账。界面挤在一起时先查账是否串，再查绘制。

## 7. 会话与 compact

### `/resume` 找不到

- 默认按 cwd 筛；用 `/sessions all` 看跨目录。
- worktree 路径不同，会话也分开。
- JSONL 坏尾行应停在最后完整事件；看解析告警。

### context 百分比怪

- 窗口来自当前 provider/model 配置。
- usage 可能 stale；状态栏 `~` 标记不能忽略。
- `/context` 估算与服务端 tokenizer 不是同一把精确尺。

### compact 失败

- 看 compact model、它自己的窗口与输出预留。
- manifest 缺 goal/open_items 会拒收，旧 history 应保持不动。
- 单轮巨型历史无法按 episode 分层时会明确拒绝。
- 字符硬裁若接手，终端应显式告警；完整流水仍从 session/export 查。

详见[会话与上下文](../features/sessions/README.md)。

## 8. 项目记忆

| 现象 | 先查 |
| --- | --- |
| `/memory on` 被拒 | 全局是否允许；项目不能反向开启 |
| 没召回 | `use`、query origin、scope、阈值、预算、stale/expired |
| 没生成候选 | `learn` 是否 `off`，回合抽取是否报错 |
| 候选不自动写 | 默认 `review` 要人工 accept；`auto` 须全局授权且证据过闸 |
| pending 不落盘 | worker 是否启动，`memory-jobs/failed/` 是否有坏 job |
| worktree 不共享 | `/memory` 的 project key 与 common git dir |

`/memory why` 应优先给召回 trace，不要靠猜关键词。详见[项目记忆](../architecture/memory/design.md)。

## 9. Hooks 与扩展

### Hook 不跑

- `/hooks` 看事件是否注册、matcher 是否命中、项目定义是否已信任。
- exec form 的 command 是否存在；Windows override 是否写对。
- stdout 是否只回协议 JSON，日志是否误写 stdout。
- async 字段若只是解析展示而未执行，按当前手册边界判断。

### Hook 把操作拦了

- 看事件、handler、退出码、failure policy 与合并后的最终决策。
- 多层 Hook 是相加，不要只查项目文件。
- 定义改动会让项目批准失效，须重审。

### Skill/MCP/Lua/DLL

- Skill 查三层优先级与实际命中路径。
- MCP/LSP 查子进程与协议日志。
- Lua/DLL 崩宿主时先移出插件目录再复现；不要在原现场反复启动。
- 原生插件核 ABI、架构、runtime 与依赖 DLL。

### GUI 插件（gui-agent 一类桌面自动化）

- **点击总落错位置**：先 `gui_status` 看 `dpi_awareness`。见 `unaware` 说明系统拒了 DPI 感知调用，坐标被虚拟化——截图物理像素与注入坐标对不上，先解决它再干活。
- **多屏副屏点不中**：副屏在主屏左侧时坐标是负的（`virtual_screen` 报得出范围），别把它当非法值修掉。
- **`focus_failed` 反复出现**：Windows 限制后台进程抢前台。人工点一下目标窗口，或让用户先激活它；不是插件坏了。
- **`stale_observation` 接连报**：窗口在被挪或自适应布局在抖。重新 `gui_screenshot` 拿新矩形，动作带最新的 `expected_window_rect`；连报就说明窗口不稳定，换 `window_client` 坐标口径。
- **截图黑屏/空白**：目标窗口最小化了（被拒是预期）；GPU 合成窗口一般 PrintWindow 抓得住，仍黑就换 `target=screen`，先想清楚会拍下哪些显示器。
- **`gui_snapshot` 收 0 项**：多半是自绘界面（游戏/部分 Electron/老自绘 Win32）没给 UIA 暴露控件树——结构路的盲区，回 `gui_screenshot` 视觉路；提权窗口探不全也是它。
- **快照里控件没名字**：应用没给 UIA 名字（tkinter 默认就这样，控件画在 Tk 手里、helper HWND 文字是空的）。类型和矩形还在，能按 rect 点；要名字得应用自己补（教学夹具就是这么做的）。
- **中文没输进去**：查 `ensured_foreground` 是否为真——窗口不在前台，键进了别的程序。本插件走 Unicode 键盘事件，与输入法状态无关。

## 10. 构建与测试

| 现象 | 先查 |
| --- | --- |
| Configure 很慢 | vcpkg/FetchContent 路线与网络 |
| Windows CI 最慢 | job 分段；Build 与 Test 分开看 |
| 改一行却重编很多 | 是否碰了 PCH、公共头或大静态库边界 |
| CTest 找不到 exe | `-C Release` 与多配置输出目录 |
| 真终端测试没跑 | driver 是 `EXCLUDE_FROM_ALL`，须点名构建 |
| 真模型测试跳过 | 对应 key/环境变量未设；它本就不进默认 ctest |

详见[开发指南](../development/build-and-release.md)与[测试指南](../development/testing.md)。

## 11. 最小复现模板

```text
版本/commit：
OS 与终端：
启动方式：交互 / 单发 / 管道
provider/model/wire（无 key）：
cwd 类型：普通目录 / Git / worktree
确认档：
最短步骤：
期望：
实际：
是否稳定复现：
相关日志（已打码）：
```

终端问题再附窗口宽高与截图；协议问题附脱敏请求/响应形状；并发问题写时间顺序。证据越短，病根越容易露出来。
