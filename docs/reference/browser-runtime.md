# BrowserRuntime 与内嵌浏览器边界

[文档首页](../README.md) · [功能目录](../features/README.md) · [Package 与组件挂载](packages.md) · [app-server 协议](../features/app-server/README.md) · [安全模型](../development/security.md)

LubanCode 要在宿主界面里打开网页、操作页面、看截图与 Console、Network，还要把同一份观察送给用户和模型。这套能力**不是一只 Package**。

一句话：**Runtime 是本事，Package 是装法。**

本文是冻结合同。谁持有浏览器状态、谁画界面、谁翻协议、谁装箱子，一概照此办理；实现者不必翻设计 TODO，也不会把 Runtime 塞回 Package 或 MCP handler。设计出处见 `todos/内嵌浏览器调试工作台_BrowserRuntime与Package边界.todo`。

## 1. 四层各守一摊

```text
核心产品层
  BrowserRuntime + App Server Browser 协议 + Desktop Browser Tab

可分发内容层
  Package: Agent + Skill + Workflow + Prompt Profile + 可选兼容适配器
```

| 名称 | 该管什么 | 不该管什么 |
| --- | --- | --- |
| BrowserRuntime | 浏览器进程、Context、页签、状态、动作、事件、截图、下载、崩溃恢复 | 不写领域操作教程，不画界面 |
| Desktop Browser Tab | 页面显示、地址栏、视口、Console/Network UI、截图预览 | 不自己重造模型与权限逻辑，不另立活动页账 |
| App Server | 前端与 Runtime 之间的方法、事件、审批、取消、断线补账 | 不保存浏览器业务规则 |
| Package | 安装、版本、来源、内容哈希、组件组合 | 不持有浏览器窗口，不直接授权 |
| Agent | 谁来测、模型与工具收窄、预装哪些 Skill | 不实现浏览器引擎 |
| Skill | 教模型"快照→动作→复验"、停手线与排错 | 不保存页签与 Cookie |
| Workflow | 排烟测、视觉回归、Console/Network 检查步骤 | 不接管 Playwright |
| MCP adapter | 给第三方客户端与终端版暴露 BrowserRuntime | 不成为内嵌界面的唯一入口 |

不许越的界，反面清单：

- 不把 BrowserRuntime 塞进 Skill、Agent YAML 或 Package manifest。
- 不让 Agent YAML 内联浏览器启动命令。
- 不让 Package manifest 授权浏览器写动作——权限走宿主审批账。
- 前端不另造一份"当前活动页"；MCP adapter 也不另造。
- 不另造一套与现有 Browser MCP 不同的 page/ref 语义。
- 不一上来删 MCP。它仍是 CLI、第三方与远程场景的入口。

## 2. 术语冻结

这些词全仓统一，不许各写各的：

| 术语 | 形状 | 谁发号 | 何时失效 |
| --- | --- | --- | --- |
| `session_id` | `s` + 时间36进制 + 随机段 | Runtime 建 session 时 | 会话结束即永久失效 |
| `page_id` | `p1`、`p2`，递增不复用 | Runtime 开页时 | 关页、浏览器崩溃、会话结束；报 `page_closed` / `unknown_page` |
| `generation` | 整数，开页为 0，首航后为 1 | Runtime，主框架每次导航 +1 | 不失效，只递增；旧 generation 的 ref 全作废 |
| `snapshot_id` | `p1-g2-s3`（页-代-序） | Runtime 每次快照 | 页换代即过期；带着旧 snapshot_id 动作明报 `stale_ref` |
| `ref` | `e12`，绑定 page_id + generation + snapshot_id | 快照时在页面里登记 | 导航换页即 stale；DOM 改动后解析数不唯一即拒 |
| `seq` | 各账本（console/network/download）单调递增序号 | Runtime | 不失效；断线后按 cursor 补账去重 |

快照、截图、Console、Network 条目一律带 `page_id` 与 `generation`，不帯的回执算坏账。

## 3. 状态真值只有一本

Runtime 持有：

```text
browser_session_id
engine / headed / profile
page_id / generation / active
snapshot_id / ref map
viewport
console_seq / network_seq / download_seq
crash_epoch
```

前端不另造活动页，MCP adapter 不另造 page registry，测试宿主也不造。所有人调同一套 Runtime 方法。

现状：这本账在 `browser/lib/session.js` 的 `BrowserSession`。App Server 侧（阶段 3）经 `browser/sidecar.js` 这只 Node sidecar 访问同一份账——MCP adapter、sidecar、直调测试宿主调的是同一套方法，谁也不许另建第二份 page registry。`browser/server.js` 是装配壳；`browser/lib/tools.js` 是 MCP 工具皮，不持状态；`browser/lib/transport.js` 只管 stdio JSON-RPC 分帧。直调（不经 MCP 协议）与经 MCP adapter 的结果由同一套自测对账（`browser/test/run-direct-tests.js`）。

## 4. 用户与 Agent 仲裁

页面同时给人也给模型操作，规矩六条：

1. 用户按住页面输入时，Agent 写动作排队。
2. Agent 即将点、输、导航时，前端亮出动作。
3. 登录、支付、发送、删除仍过审批。
4. 用户一动 DOM，generation 或 observation epoch 递增；旧 ref 明报 stale。
5. screenshot、DOM、Console、Network 都带 page id 与 generation。
6. 用户可一键暂停 Agent 浏览器动作，不停整个模型回合。

所有写动作带 owner。用户与模型看同一份截图时，指向同一 artifact，不许各截一张、各说各话。

## 5. 路线与现状

推荐路线：可见调试 MVP（Runtime 开 headed 窗，宿主面板先亮证据账）→ 核心 BrowserRuntime → 桌面内嵌 Browser Tab → Package 装配 Agent/Skill/Workflow。不能反过来先做 Package：挂载器没装上，先雕箱花无益。

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 0 | 冻结边界（本文） | 已落 |
| 1 | 从 MCP 中抽 BrowserSession，三层分家，直调测试 | 已落（`browser/lib/`） |
| 2 | Console/Network journal、screenshot artifact 进 App Server 事件、可见调试面板 | 已落(`browser_console`/`browser_network` 两本环形账;`item/completed` 附截图 artifact 引用;headed 可选;用户观察代仲裁。面板落终端工具回执——Desktop Tab 待阶段 4) |
| 3 | App Server Browser 协议（方法 + 带 seq 的事件 + 断线补账） | 已落（协议升 1.1，additive；`browser/sidecar.js` 起 Node sidecar 复用 `BrowserSession`，`src/app_server/browser_service.*` 只做转发；journal 批量有帽丢老明记，`browser/console\|network query` 凭 `sinceSeq` 补账；截图只发 artifact 引用；owner=agent 写动作过审批，取消贯通到 sidecar 动作队列。断线重连的边界：stdio app-server 断线即 EOF 退场、sidecar 随之收尸，页面状态不跨 app-server 重启存活——跨进程续场归阶段 4 的宿主） |
| 4 | 真正内嵌 Browser Tab（Electron/WebView2/CEF 选型另写 ADR） | 待做 |
| 5 | 官方 Browser Package（Agent/Skill/Workflow，不复制 Runtime） | 待做 |

MCP adapter 日后可作为可选 code-bearing 组件进 Package，只用于终端版无 Desktop、第三方 MCP 客户端、旧配置兼容与远程无界面场景；照 Package 信任门走。核心 BrowserRuntime 随 LubanCode 发布，不吃项目 Package 信任。

## 6. `examples/packages/browser-agent` 的身份

这只包是 content-only：一枚 Skill 教模型操作章法，一张练习页供本地演练。它不承载浏览器本体——页签、Cookie、登录态、下载由 BrowserRuntime（现经 `browser/` 的 MCP server 暴露）保管。卸掉这只包，浏览器照常能用；装上它，只是多了章法。往后官方 Browser Package 照 `docs/reference/packages.md` 的格式装配，只声明核心工具依赖，不复制 Runtime。
