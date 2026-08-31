# app-server:无界面后台协议

[文档首页](../../README.md) · [命令参考](../../reference/commands.md) · [工具参考](../../reference/tools.md) · [测试手册](../../development/testing.md) · [架构说明](../../architecture/README.md)

`lubancode app-server` 是无界面后台入口:前端从 stdin 发 JSON-RPC 请求,后台在项目机上读文件、改代码、跑命令,再从 stdout 把正文、工具进度、审批、diff 与结果一笔笔传回来。SSH 只管架通道,活计全落在远端。

```text
本机 LubanCode 前端
    │ 消息 / 审批 / 打断 / 进度
    └──── SSH ────► 远端 lubancode app-server
                         ├─ 会话与模型请求
                         ├─ 项目文件和工具
                         └─ 日志、diff、usage 与终态事件
```

不把监听端口裸露到公网。stdio 走 SSH 承载;本机/局域网的 WebSocket 承载见下文《WS 承载》——回环免鉴权,非回环强制 token。

## 传输与进程纪律

- 承载两副面孔,按需起一种:`lubancode app-server` 走 stdio(stdin 逐行收 UTF-8 JSON,stdout 逐行吐 UTF-8 JSON,一行一条完整消息);`lubancode app-server --app-server-ws <端口|主机:端口>` 走 WebSocket(一条文本帧一条消息)。两副面孔同一套协议、同一个方法表。
- stdout 是协议专线。日志、诊断、崩溃前说明一概走 stderr;工具子进程输出装进事件字段,绝不漏到 stdout 搅坏分帧。
- **jsonrpc 字段已冻结**:出站不带 `jsonrpc:"2.0"`,入站不校验(带不带都认)。翻这个开关等于协议破坏,须 bump 协议版本。
- 先 `initialize`(回能力表)再 `initialized`(通知),然后业务放行;重复 initialize 报 `-32600`,握手前调业务报 `-32002`。握手状态机是**连接级**的:WS 重连后须重新 initialize(每条连接自己的握手)。
- EOF、断管、`shutdown`、`exit` 都收口:打断在跑回合、刷完能刷的终态、退出,不留孤儿进程。
- 入站单行上限 1MB(stdio)/ 单条消息上限 8MB(WS 分片拼装),超限回 parse error 后退线(WS 直接断线)。
- 事件队列有界(默认 4096)。撞满先合并可合并的 delta(同条目的增量并成一条,计 `coalesced`),再丢可丢事件并补一条 `queue/overflow` 通报(计 `dropped`);终态与审批类事件绝不丢。

## WS 承载(多前端外壳单阶段 A)

同一条 protocol/dispatcher 线的第二个传输层,给 Web/Android 外壳用:

```bash
lubancode app-server --app-server-ws 8765                # 127.0.0.1:8765,回环免鉴权
lubancode app-server --app-server-ws 0.0.0.0:8765        # 非回环:强制 token(见下)
lubancode app-server --app-server-ws 9001 --app-server-ws-token <token>
```

- **绑定**:裸端口只绑 `127.0.0.1`;`<主机>:<端口>` 显式给地址(点分 IPv4 或 `localhost`),非回环地址启动时没配 token 直接拒启。
- **token 门**:`--app-server-ws-token` 或环境变量 `LUBANCODE_APPSERVER_TOKEN` 配了就启用——HTTP 升级完成后**第一条文本帧**必须是 `{"method":"app_server/auth","params":{"token":...}}`,不过即断(连接关闭,不回协议错误)。token 恒时比较,不落任何日志。回环 + 没配 token = 免鉴权(本机首版口径)。
- **连接生命周期**:TCP 断开只收那条连接(进程活着等重连——与 stdio 的"EOF 自退"不同);对端发 `exit`/`shutdown` 才整场收线、进程退出。断线后:thread 账与浏览器会话(sidecar)不收尸,重连的壳凭 cursor(`browser/console/query`、`browser/network/query`、`trace/query`、`workflow/query` 的 `lastSeq`/`sinceSeq`)补账,老 threadId 继续用。
- **seq**:事件序号进程级单调(`ProcessIdAuthority` 唯一发号),每条连接看到的是自己的单调子序列;重连不回卷、永不复用,cursor 补账不撞号。
- **多连接**:会话一条一条串行服务(同拍一条活连接,后来者升级完排队等)——同拍多客户端是 §4.4 的活,不在本单阶段。阶段 D 起 accept 由专职线程做(不被在服务的会话堵死):artifact GET 即到即答,与会话并发;WS 会话仍串行。
- **实现**:服务端 WS(握手算料、帧编解码、回环 TCP)是仓内自带的极小实现(`src/app_server/ws_frames.*`、`ws_sockets.*`、`ws_transport.*`),零第三方依赖——不谈压缩扩展、不收 binary 帧、客户端帧必须掩码;不引依赖巨兽。
- 验证口径:单测 `tests/unit/app_server/test_app_server_ws.cpp`(握手算料 RFC 向量、帧编解码分型、token 门、回环真监听一幕幕:事件 seq、断线重连、exit 收线);冒烟 `scripts/tests/app_server_ws_smoke.sh`(独立客户端 `app_server_ws_client.js`:本地假 Anthropic 后端跑一 turn、断线重连、cursor 补账、token 门;浏览器面等价集要 playwright)。

## 参考前端(多前端外壳单阶段 D)

`examples/web-console/`:一只最小 localhost Web 页,**不是产品,是验收工具**——协议面缺什么,写它的时候全暴露。四件套:聊天流(thread/turn/item 事件)、页签账 + Console/Network/Downloads 面板(query 补账)、镜像流 + 输入注入(screencast 帧经 artifact 口子取字节;快照没有坐标,"点镜像"=点元素清单一行,`browser/action` 走用户路)、审批弹层(`permission/request` 反向请求)。

- 全程只走协议与承载面:WS 文本帧 + `GET /artifact/<名>`;不 import 内核头文件、不读内核盘上账、不碰 sidecar。
- 内核(`web_console_core.js`,纯逻辑零 DOM)与 Node 冒烟 `scripts/tests/app_server_web_console_smoke.js` 同一份——页上怎么走协议,冒烟就怎么验(端到端一幕:开页 → 看账 → 点镜像 → Agent 收 `browser.stale_ref`)。
- 跑法见 `examples/web-console/README.md`;全链路本地回环。

## 协议版本

`1.1`。任何报文形状变更必须 bump,前端拿 `initialize` 结果里的 `protocolVersion` 对表。

版本账:

| 版本 | 内容 |
| --- | --- |
| `1.0` | 骨架期全部方法与事件(thread/turn/item、审批、usage/context、workflow/trace 查询、goal/loop/plan)。 |
| `1.0` 内 additive | `item/completed` 可带可选 `images` 数组——MCP 富结果图片的元数据与 `artifact` 引用,不带 base64(2026-08 起)。 |
| `1.1` | 浏览器调试工作台阶段 3:additive 新增 `browser/*` 方法 18 枚与 `browser/*` 事件 13 族(见下两节)。老方法老事件形状一字未动。 |
| `1.1`(承载注) | WS 传输层(2026-08 起):同一条报文线的第二副面孔,报文形状零改动,不 bump 版本。连接级握手(`initialize`)每条连接各来一遍;首帧鉴权消息 `app_server/auth` 只在 WS 且配了 token 时出现,不算协议方法面。 |
| `1.1`(阶段 B 注) | 用户输入路由与暂停(2026-08 起,additive):新增 `browser/pause`/`browser/resume` 方法与 `browser/paused`/`browser/resumed` 事件;`browser/status` 的 result 增可选布尔 `paused`;`browser/action/completed` 增稳定错误码 `browser.paused`。**owner 仲裁升级**:`owner` 由内核按连接与鉴权裁定(见《owner 仲裁》),外壳报的只是意向;`owner` 缺省值从写死 `user` 改为连接的裁定身份。老报文形状零改动。 |
| `1.1`(阶段 C 注) | 镜像流(2026-08 起,additive):新增 `browser/screencast/start`/`browser/screencast/stop` 方法与 `browser/screencast/frame` 事件。只读、不问审批(与 `snapshot`/`screenshot` 同档);帧字节走同一条截图 artifact 链落盘,协议上只有引用与 `pageId`,绝不出现 base64。老报文形状零改动。 |
| `1.1`(阶段 D 注) | 参考前端(2026-09 起):WS 端口的只读 HTTP artifact 口子 `GET /artifact/<内容寻址名>`(与 WS 同端口、同 token 门)——事件里只有引用,字节走这条口子,base64 仍永不进协议。承载面(与 `app_server/auth` 同级),不是协议方法面,报文形状零改动,不 bump 版本。 |

## 方法面

### 握手与退场

| 方法 | 方向 | 说明 |
| --- | --- | --- |
| `initialize` | 请求 | 回 `protocolVersion`/`lubancodeVersion`/`platform`/`capabilities`。能力表如实分 `methods`(已接线)、`pending`(名字认识没接线)、`serverRequests`(服务端反向请求)、`itemTypes`、`turnStatuses`。 |
| `initialized` | 通知 | 握手收尾,业务放行。 |
| `shutdown` | 请求 | 回空 result 后收线。 |
| `exit` | 通知 | 立即收线,不回话。 |

### thread(会话)

| 方法 | 参数 | 结果 |
| --- | --- | --- |
| `thread/start` | `cwd?` | `{threadId, cwd}`;会话档真落盘(`~/.lubancode/sessions/`),meta 写真值(wire/model 来自配置四级合并)。 |
| `thread/list` | `scope?/state?/sort?/search?/cwd?/cursor?/limit?` | `{threads:[...], total}`;走 `runtime::SessionCommandService`,与终端 `/sessions` 同一碗饭。缺省全量 + active + updated。`startedAt` 续给(`createdAt` 同源),老前端不断。 |
| `thread/stop` | `threadId` | 停场;在跑回合按打断收口。 |
| `thread/archive` | `threadId` | 搬进 `archive/`;成功发 `thread/updated`(state=archived)。开着的 thread 拒 `active_thread`。 |
| `thread/unarchive` | `threadId` | 搬回根;成功发 `thread/updated`(state=active)。 |
| `thread/delete` | `threadId, confirm` | 没带 `confirm` 拒 `confirmation_required`;带了真删,发 `thread/deleted`。 |

搬删的错误码走 `error.data.reason`:SessionCommandService 的稳定串(`not_found`/`ambiguous`/`confirmation_required`/`path_outside_root`/`target_exists`/`io_error`)加协议侧的 `active_thread`。

### turn(回合)

| 方法 | 参数 | 说明 |
| --- | --- | --- |
| `turn/start` | `threadId, text, images?` | 立即回 `{threadId, turnId}`,整回合在工作线程跑。`images` 是数组,元素 `{mediaType, data, filename?, width?, height?}`(`data` 是不带 data URL 前缀的 base64)——字段名与 `api::ImageBlock` 对齐,图片原样入会话历史。同一 thread 同拍两轮拒 `-32004`。 |
| `turn/interrupt` | `threadId, turnId?` | 置打断旗;审批悬停立即醒;终态 `interrupted`。回合不在跑或点名别的回合报 `-32005`(迟到不追)。 |
| `turn/steer` | 留位 | 未接线,`initialize` 能力表里在 `pending`。 |

### workflow(run 账查询)

| 方法 | 参数 | 结果 |
| --- | --- | --- |
| `workflow/query` | `runId, lastSeq?` | `{runId, workflowId, state, lastSeq, nodes, ...}` 快照 + `lastSeq+1` 起的增量事件(`workflow/event`)。`lastSeq` 缺省 0 = 全量。读 `~/.lubancode/workflow-runs/<run-id>/`。 |

### browser(浏览器调试工作台,1.1 起)

C++ 这层是协议转发层:真 Runtime 在 Node sidecar(`browser/sidecar.js`,复用 `browser/lib/session.js` 的 BrowserSession——Playwright 生命周期、页签账、ref、journal、崩溃终态只有那一本账)。sidecar 懒起、进程复用、崩溃明报、收线收尸;`LUBAN_BROWSER_SIDECAR` 环境变量指到 `browser/sidecar.js`(没指则按可执行文件旁边与当前目录找)。术语(`pageId`/`generation`/`snapshotId`/`ref`/`seq`)与 `docs/reference/browser-runtime.md` 冻结版一致。

方法分两档。**同步**(读线程直答,sidecar 内存账):`browser/status`、`browser/page/list`、`browser/console/query`、`browser/network/query`、`browser/downloads/query`。**异步**(受理即回 `{actionId, accepted}`,终态走 `browser/action/completed` 事件——导航与动作可能要等审批、等页面,不堵读线程):其余全部。

| 方法 | 档 | 参数 | 说明 |
| --- | --- | --- | --- |
| `browser/start` | 异步 | `engine?`(chromium\|webkit)、`headed?`、`profile?`(persistent\|ephemeral)、`profileName?`、`viewport?{width,height}`、`journalCap?`、`timeoutMs?` | 起一场浏览器会话(浏览器本身仍是首个页面动作才懒启动)。已有一场活会话报 `browser.session_running`。 |
| `browser/stop` | 异步 | 无 | 收掉会话(context/browser/profile 锁)。 |
| `browser/status` | 同步 | 无 | 会话状态(engine/headless/profile/launched/crashed/pages/downloadsDir)+ `sidecarRunning` + `paused`(1.1 阶段 B 注起:内核的暂停旗,sidecar 不知道这事)。 |
| `browser/page/open` | 异步 | `url`、`newPage?`、`waitUntil?`、`timeoutMs?` | 只收 http/https/about:blank。回 `pageId`/`generation`/标题/HTTP 状态。 |
| `browser/page/list` | 同步 | 无 | 页签行数组 `{pageId, title, url, active, generation}`。 |
| `browser/page/select` | 异步 | `pageId` | 明切活动页(不靠"最后一页"猜)。 |
| `browser/page/close` | 异步 | `pageId` | 关页;旧 `pageId` 稳定报 `page_closed`,不复用。 |
| `browser/page/navigate` | 异步 | `pageId, url, waitUntil?, timeoutMs?` | 指名导航;`generation` +1,旧 ref 即 stale。 |
| `browser/page/back` / `forward` / `reload` | 异步 | `pageId` | 历史导航;没有历史时 `navigated:false` 如实说。 |
| `browser/snapshot` | 异步 | `pageId?, maxChars?, timeoutMs?` | 语义快照(可访问性树),带 `snapshotId` 与 ref 标记。 |
| `browser/screenshot` | 异步 | `pageId?, fullPage?, ref?, snapshotId?, timeoutMs?` | 截图。**只回 artifact 引用**(`result.image.artifact`,与 `item/completed` 的 images 同形),另发 `browser/screenshot/ready` 事件;字节落 `<HomeLubancodeDir>/browser-artifacts`(内容寻址)。 |
| `browser/action` | 异步 | `kind`(click\|type\|select\|wait)+ 各自的 `ref/text/value/label/forText/urlContains/ms` + `snapshotId?/timeoutMs?` | 统一动作口。click/type/select 须 `ref`;type 另须 `text`;wait 须条件其一。 |
| `browser/action/cancel` | 同步 | `actionId` | 取消在飞动作:审批段立即悬空收口;sidecar 段发 `cancelled` 通知(轮询型动作见旗即停,单发动作靠自身超时)。动作不在跑报 `-32005`(`browser.stale_action`)。 |
| `browser/console/query` | 同步 | `pageId, sinceSeq?, level?, limit?` | Console journal 补账:回 `rows/total/dropped/lastSeq`。`sinceSeq` 是"已见到的最大 seq",返回大于它的。 |
| `browser/network/query` | 同步 | `pageId, sinceSeq?, urlContains?, status?, failedOnly?, limit?` | Network journal 补账(元数据账,响应体不收)。 |
| `browser/downloads/query` | 同步 | 无 | 下载账(id/state/filename/mime/bytes/sha256/path)。 |
| `browser/pause` | 同步 | 无 | 拨暂停旗(1.1 阶段 B 注起)。暂停期间 `owner=agent` 的动作一律**受理不执行**,终态 `error.code=browser.paused`;用户动作照走;终态事件照发。回 `{paused:true}`,另发 `browser/paused` 事件(must_keep)。手闸只归用户连接。 |
| `browser/resume` | 同步 | 无 | 落暂停旗。回 `{paused:false}`,另发 `browser/resumed` 事件。 |
| `browser/screencast/start` | 异步 | `pageId?, fps?`(1-30,缺省 5)、`format?`(jpeg\|png,缺省 jpeg)、`quality?`(1-100)、`maxWidth?/maxHeight?` | 起镜像流(1.1 阶段 C 注起)。只有 chromium 引擎支持(靠 CDP `Page.startScreencast`),webkit 报 `browser.screencast_unsupported`。帧率帽在 sidecar 侧按墙钟节流,不是 CDP 的跳帧参数。只读,不问审批。 |
| `browser/screencast/stop` | 异步 | `pageId?` | 停镜像流。没在跑报 `browser.screencast_not_running`。页面关闭/崩溃时 sidecar 自动收尾,不必显式 stop。 |

**owner 仲裁**(1.1 阶段 B 注起,内核说了算):写动作(`page/*` 导航族、`page/select`、`page/close`、`action`)可带 `owner`("agent"|"user"),但 **`owner` 由内核按连接与鉴权裁定,外壳报什么不算数**——stdio 宿主与过门的 WS 连接(回环免鉴权或 token)裁定为操作者本人(`user`),内核内部的 agent 发放路(回合驱动的浏览器工具,与多客户端阶段的 agent 连接)裁定为 `agent`。规则:

- 缺省 `owner` = 连接的裁定身份(用户连接缺省 `user`;agent 侧缺省 `agent`——缺省也逃不过审批,不许靠"不说 owner"绕门)。
- 非用户连接报 `owner=user`:**明拒**(`error.data.reason=browser.owner_denied`)——Agent 假冒用户,门都没有。
- `owner=user`(用户路):不带 `threadId`(带了也被内核摘掉——用户不是 Agent)、不问审批、**不排队**——排队时先挑用户动作,Agent 动作让路(在途的那只让不了:一份浏览器状态一位主人,动作串行是底线);输入动作(click/type/select)执行成功后 sidecar 递 `userEpoch` 并发 `browser/user_epoch` 事件,Agent 拿旧 snapshot 的 ref 再动作即报 `browser.stale_ref`(仲裁第 4 条原样复用)。
- `owner=agent`:须带 `threadId`,先过 `permission/request` 审批(`acceptForSession` 按方法名记会话级放行账),decline/cancel/超时/打断按拒绝收口;暂停期间按 `browser.paused` 收口(不问审批)。

browser 错误走 `error.data.reason` 带稳定串(`browser.not_configured`/`browser.sidecar_spawn_failed`/`browser.sidecar_dead`/`browser.sidecar_timeout`/`browser.session_running`/`browser.permission_denied`/`browser.approval_cancelled`/`browser.artifact_unavailable`/`browser.thread_required`/`browser.stale_action`/`browser.owner_denied` 等;`browser.paused` 是暂停期间 agent 动作的**终态**错误码,走 `browser/action/completed` 的 `error.code`),sidecar 侧的浏览器码(`browser.stale_ref`、`browser.unknown_page`、`browser.page_closed`、`browser.timeout`、`browser.screencast_unsupported`、`browser.screencast_running`、`browser.screencast_not_running`、`browser.screencast_start_failed` 等)原样透传。

## 事件账

统一三层:会话装回合,回合装条目。每条事件的 `params` 带 `seq`——进程内单调序号(连接层统一盖),前端凭它排序、查漏。

| 事件 | 说明 |
| --- | --- |
| `thread/started` | `params`: `threadId, cwd`。 |
| `thread/updated` | 搬删后:`threadId, state`。 |
| `thread/deleted` | 删除后:`threadId`。 |
| `thread/stopped` | 停场后:`threadId`。 |
| `turn/started` | `threadId, turnId`。 |
| `item/started` | `params.item`: `{id, type, ...}`。工具条目带 `tool`/`toolUseId`/`input`;`write_file`/`edit_file` 额外带 `diff`(见下);`run_command` 的 type 是 `command`。 |
| `item/delta` | `itemId, delta`(正文/思考/输出的增量)。 |
| `item/completed` | `params.item`: 终态字段(`result`/`isError`;打断收口的带 `cancelled`)。工具富结果带图(截图一类)时另带 `images` 数组:每项 `{mime_type, width, height, bytes, sha256, artifact:{id, filename, path, mime_type, bytes, sha256, stored}}`——只递 artifact 引用不递 base64,前端点开的图与模型看的图指向同一 artifact。 |
| `turn/usage` | 每次到模型的请求收尾:`usage`(五项 camelCase,与 `turn/completed` 的 usage 同形)、`model`。 |
| `turn/context` | 上下文压力:`context.phase`(pre_request/after_hard_trim)、`projectedTokens`、`windowTokens` 等。前端画水位条吃这个。 |
| `turn/completed` | 唯一终态:`status`(success/error/cancelled/interrupted)、`usage`、`stepsUsed`;error 时带 `error` 文案。 |
| `queue/overflow` | 丢事件后的通报:`dropped`、`coalesced`。 |
| `workflow/event` | wf run 的 journal 事件:`runId`、`eventSeq`(run 内单调号,与快照的 `lastSeq` 对账)、`type`、`workflowId`、`nodeId?`、`data`。 |
| `browser/started` | 会话起:`sessionId, engine, headless, profile, profileName`。 |
| `browser/stopped` | 会话收:`sessionId`。终态,must_keep。 |
| `browser/crashed` | 崩溃终态:`reason`。旧 page id / ref / 截图观察全作废。must_keep。 |
| `browser/page/created` | `pageId`。 |
| `browser/page/updated` | 切页等:`pageId, url, generation`。 |
| `browser/page/closed` | `pageId, reason`(closed\|crashed)。 |
| `browser/navigation` | `pageId, url, generation`(主框架导航,generation 已递增)。 |
| `browser/console/event` | **批量**:`pageId, entries:[{seq, level, text, sourceUrl, line, column, ts, generation}], dropped, lastSeq`。Console 四级 + 未捕获异常,文本默认脱敏。 |
| `browser/network/event` | **批量**:`pageId, entries:[{seq, method, url, status, resourceType, durationMs, failed, error, ts, generation}], dropped, lastSeq`。元数据账。 |
| `browser/download/event` | 下载账单条:`id, state, suggested, filename?, path?, mime?, bytes?, sha256?, error?`。 |
| `browser/screenshot/ready` | `pageId, generation, url, fullPage, image`(image 与 `item/completed` 的 images 元素同形,只带 artifact 引用)。must_keep(无查询口)。 |
| `browser/action/started` | `actionId, method, owner, threadId?, input`。 |
| `browser/action/completed` | 动作终态:`actionId, method, owner, ok, result?\|error?{code,message}, cancelled?, durationMs`。must_keep。暂停期间 agent 动作的终态是 `ok=false, error.code=browser.paused`。 |
| `browser/user_epoch` | `pageId, userEpoch`——用户动了页面(手点/按键,或经协议注入的 `owner=user` 输入动作),观察代递增,旧 ref 按仲裁规矩报 stale。 |
| `browser/paused` / `browser/resumed` | 暂停旗拨动:`paused`。must_keep(1.1 阶段 B 注起)。 |
| `browser/screencast/frame` | 镜像流单帧(1.1 阶段 C 注起):`pageId, frameSeq, width, height, dropped, artifact`。**只发 artifact 引用**(与截图同形,不递 base64)。可丢——`dropped` 是这一帧之前、这一页因队满丢了几帧,报完清零。 |

**高频事件的有界规矩**(console/network):sidecar 源头批量(单批帽 200 条、40ms 一冲;在飞批数帽 64,撞帽丢最老整批并计数),App Server 出站队列再兜一层有界(撞满先合并同页批量——entries 拼接、dropped 求和——再丢可丢事件并补 `queue/overflow` 通报)。丢了不要紧:`browser/console/query` / `browser/network/query` 凭 `sinceSeq` 补账,`dropped` 明说丢过多少,不冒充全账。

**镜像流的有界规矩**(1.1 阶段 C 注起):帧率帽在 sidecar 侧按墙钟节流(`browser/screencast/start` 的 `fps`,缺省 5,夹 [1,30])——节流丢的帧不计 `dropped`(设计内,不是消费者跟不上)。帧到达后落 artifact 这一步可能比帧到达慢(磁盘、host 读得慢),App Server 侧另开一条有界队列(缺省容量 8)+ 专职工作线程消化;队满丢队首最老那帧,按其 `pageId` 记账,下一帧成功报出时把这页累计的 `dropped` 带上再清零——不阻塞 sidecar 的事件读线程,也不会无限攒内存。`browser/screencast/frame` 不是 must_keep:丢了不必补账,前端要的是"现在",不是补全过去的每一帧。

条目类型(`item.type`):`text`、`thinking`、`tool`、`command`、`file_change`(留位)、`question`(留位)、`agent`(留位)、`error`。

### diff 行表

`write_file`/`edit_file` 的 `item/started` 带 `diff` 字段——`runtime::DiffTable` 中立行表直转,零翻译:

```json
{
  "path": "src/a.cpp",
  "located": true,
  "replacedCount": 1,
  "oldExists": true,
  "addedLines": 2,
  "removedLines": 1,
  "rows": [
    {"kind": "context", "text": "int main() {", "oldNo": 1, "newNo": 1},
    {"kind": "del", "text": "  return 0;", "oldNo": 2, "newNo": 0},
    {"kind": "add", "text": "  return 1;", "oldNo": 0, "newNo": 2}
  ]
}
```

`kind` 三值:`context`/`del`/`add`;`oldNo`/`newNo` 1 起,没有的是 0。行表在工具执行前算(预览语义,与终端确认块同款);edit_file 定位失败(`located: false`)也照样给段内行表,前端不须特判。终端按 `kind` 添色,GUI 按 `kind` 上 DOM class——两家渲染,一份真值。

### 命令输出

`run_command` 的条目类型是 `command`;输出整段落 `item/completed` 的 `result`(工具层没有流式回调,增量留待工具层出流式钩子后再接)。

## 服务端反向请求(审批与提问)

需确认工具停住发 `permission/request`;`ask_user` 工具发 `user/ask`。两者都是服务端发给前端的双向请求,信封 `id` 为 0,配对靠 `params.requestId`(`req-<n>`)。

`permission/request` 的 params:`threadId`、`turnId`、`requestId`、`tool`、`input`(结构化 JSON)、`toolUseId?`、`reason?`。

前端响应:`{"id":0, "result":{"requestId":"...", "decision":"..."}}`,decision 四态:

| 决定 | 行为 |
| --- | --- |
| `accept` | 本次放行。 |
| `acceptForSession` | 放行并记入本 thread 会话级账,同工具后续免问(只写内存,不落盘)。 |
| `decline` | 拒绝;原因随 tool_result 送回模型。 |
| `cancel` | 视作用户撤回,按拒绝收口。 |

悬停期间事件泵照常活:前端可随时 `turn/interrupt`(悬停立即按取消收口,文案写"被 turn/interrupt 打断",不冒充用户拒绝)。悬停超时按"没人可答"悬空收口,同样不冒充。迟到/失效的答复报 `-32005`。

`user/ask` 的 params:`threadId`、`turnId`、`requestId`、`header`、`question`、`options:[{label, description}]`、`multiSelect`。响应 result 里 `requestId` + `answers`(字符串数组)。

## 错误码

标准段:`-32700` parse error、`-32600` invalid request(含重复 initialize)、`-32601` method not found、`-32602` invalid params、`-32603` internal error。

服务器段:`-32000` busy、`-32002` 未握手、`-32003` 已 shutdown、`-32004` 同 thread 同拍两轮、`-32005` 迟到/失效的反向请求答复。

错误响应的 `error.data` 可带稳定 `reason` 串(会话搬删等),前端凭它分支。

## SSH 承载

远端只须备好 SSH、LubanCode CLI 和既有 provider 凭据。前端 SSH 登录后从远端用户的 login shell 拉起:

```bash
ssh <host> lubancode app-server
```

验证口径:

- 单测 `tests/integration/app_server/test_app_server_smoke.cpp`:真进程 + stdio 管道替身(SSH 通道的进程形状),验 stdout 逐行可解析、握手一条线、坏 JSON 不炸、EOF 自退。
- 手测脚本 `scripts/tests/app_server_ssh_smoke.sh`:真 SSH 通道(`ssh localhost` 起手;无 sshd 的机器 `--local` 走本机管道)。验 login shell 拉起、stderr 隔离、断线后远端不留孤儿。
- 浏览器面:`tests/unit/app_server/test_app_server_browser.cpp`(假 sidecar 直驱协议层:方法面、参数表、审批、取消、journal 批量、截图 artifact、真进程 sidecar 的起/复用/崩/收尸);`scripts/tests/app_server_browser_smoke.sh` 起真 `lubancode app-server` + 真 sidecar + 真 Playwright 跑一幕(独立协议客户端 `app_server_browser_client.js`:开页、导航、收事件、cursor 补账、取消、审批、截图 artifact、断线重连的边界)。

```bash
# 本机管道(CI/开发机)
bash scripts/tests/app_server_ssh_smoke.sh --local
# 真 SSH(要求 localhost sshd 与免密)
bash scripts/tests/app_server_ssh_smoke.sh localhost
```

SSH 断开后 app-server 读到 stdin EOF 自退,远端不留后台子进程;再连后 `thread/list` 照样能列出既有会话档。

## 不做

- 不口头宣称兼容 Codex app-server。兼容要有 schema 版本、黄金报文与跨版本合同测试作证。
- 不内置 SSH 客户端、不代管密钥;SSH 发现、登录和隧道归调用方。
- 不把本机文件、凭据或工具混进远端会话。哪台机器跑 app-server,哪台机器出环境。
