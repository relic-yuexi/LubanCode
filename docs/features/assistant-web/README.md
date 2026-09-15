# 常驻助理 Web 主界面(W0/W1/W2)

[参考手册](../../README.md) · 源单 [todos/常驻助理Web主界面_CPP本地服务与Gateway总装.todo](../../../todos/常驻助理Web主界面_CPP本地服务与Gateway总装.todo)

`lubancode assistant` 的本地宿主:一只前台进程同时装本地 Web 服务(随包
网页 + 认证)、AppServer 控制入口(1.3 协议)与自动任务底座(W2 起:
GatewayAutomationPump 进程内直驱)。本页冻结 W0 合同(断线合同、任务
归属与路由、能力声明)、W1 的面(承载/认证/聊天)与 W2 的面(单次任务/
结果/审批/重连补账)。W3+ 的渠道设置、周期任务与系统托管不在本页冒充。

## 一、启动与停止(W1 已落)

```powershell
lubancode assistant                     # 系统分配端口,监听就绪才开浏览器
lubancode assistant --no-open           # 只打印 URL(可复制)
lubancode assistant --port 8765 --profile default
```

- 只绑 `127.0.0.1`;非回环绑定在启动门拒绝(远程访问是后续批次的设计,
  不靠改 host 参数无声开启)。
- 不指定端口由系统分配;指定端口被占准确报错(点名端口、说明不偷换),
  退出码非零。
- 监听与接口就绪后才开浏览器;打开失败不拦启动,URL 已打印可复制。
- 重复启动同 profile:读 `<状态根>/assistant/<profile>/assistant.lock`
  → 持有者探活 → `/healthz` 的 bootId 对锁账验明正身 → `/control/open`
  (吃锁文件里的控制口凭据)拿新打开页 URL → 开其页面后退码 2。探测
  失败或锁不明:退码 3,不杀原进程、不复用可疑 URL。
- 第一阶段进程随终端退出即停止,启动公告如实说明;Ctrl+C 走宽限收口
  (停收新活 → 打断收口 → 落账 → 释放监听与锁)。

## 二、断线合同(W0 冻结)

`ServerOptions::work_lifetime` 二值,能力声明区分,不全局改旧语义:

| 模式 | 值 | 合同 |
| --- | --- | --- |
| 独立 app-server(缺省) | `SessionBound` | stdio 的 EOF/断线/exit 合同原样:连接收线即打断在跑回合。`initialize` 不带 mode 字段。 |
| 助理模式 | `Detached` | 连接收线只撤订阅(事件出口摘掉),已受理回合照跑到终态、落账。`initialize` 的 capabilities 带 `mode:"assistant"`、`workLifetime:"detached"` 与助理方法名。 |

- **停止任务** = `turn/interrupt`(协作取消 + 硬时限);**停止助理** =
  `shutdown`/exit(整进程收口);关标签、刷新、WS 断线不是其中任何一
  个——只撤订阅。
- 断线期间回合的事件丢弃(没人听的事件不留);重连/刷新走领域快照补
  账:`thread/list`、`thread/read`(历史)、`operation/read`(终态核
  对)、`trace/query`(事件账)。页面不是账本。
- 落点:`src/app_server/server.cpp` 的 `ServeWsSessionBorrowed`(收线分
  岔)、`tests/unit/app_server/test_app_server_detached.cpp`(两模式对照
  钉死)。

## 三、任务归属与执行路由(W0 冻结)

每个 task/turn 只由一个执行器持有;同一输入不双跑:

| 输入来源 | 唯一执行器 | 防双跑 |
| --- | --- | --- |
| 页面立即聊天 | AppServer `turn/start`(一场 thread 同拍一轮,`kErrTurnAlreadyRunning` 明拒) | `clientOperationId` 受理幂等(1.3):同键同载荷回原受理,不建第二轮 |
| 后台/定时任务(W2+) | Gateway 调度(AutomationStore → GatewayAutomationPump) | AutomationStore 的受理账与 revision(单写者) |

聊天线与调度线不共享输入入口:页面输入只经 WS 协议,定时输入只经
AutomationStore。两线复用同一 SessionService 执行事实与 workspaces 账。

**装配缝定案(W2,2026-09-15)**:共用启动服务已由 #82 提取为
`app/gateway_launch`(GatewayLaunchPlan/RunGatewayWithPlan,gateway run
与 `lubancode im` 同用)。它是一只阻塞的整进程启动器——自带 profile 锁、
信号处理与停机合同,不能当工作泵内嵌进 AssistantHost。**W2 定案:助理
进程内直接驱动 GatewayAutomationPump(单进程方案)**:

- `GatewayAutomationPump` 本身是 TickOnce 同步泵(普通对象),助理宿主
  起一条泵线程每 250ms 一 tick(`src/app/assistant_tasks.cpp` 的
  `AssistantAutomationRuntime`),不经 gateway_launch,也不另起 Gateway
  进程——单用户本机场景,进程分工没有收益,反而多一份账互斥。
- **单写者互斥**:助理启动时取 gateway 同 profile 的 `GatewayLock`
  (锁账字段按 GatewayProcess 同式:boot_id=助理 bootId,owner_epoch 同
  值)。同 profile 的 `gateway run` 在跑 → 劏理拿不到锁 → **任务面禁用**
  (task/* 回稳定错误 `assistant.automation_unavailable`,
  `assistant/status` 的 `automation.available=false` 带原因),聊天线照常。
  反向同理:助理持锁期间 `gateway run` 同 profile 会按 already_running
  退 2。两把锁(assistant.lock/gateway.lock)互不替代:前者管重复启动,
  后者管 automation 账单写者。
- **渠道装配不挂**:W2 无渠道(CompositeGatewayPump 不用),W3 渠道
  设置只写配置不连渠道,渠道线的泵挂归后续批次——如实,不冒装。
- **任务写侧走 gateway 控制命令文件**(`WriteJobAddCommand`/
  `WriteJobRunNowCommand`/`WriteJobStateCommand`,与 CLI 同一份合同,
  泵的 TickOnce 消费);**读侧走只读投影文件**(`ReadAutomationProjection`/
  `ReadOutboxProjection`,与泵的单写者不抢账)。方法面不碰泵内存,
  零锁线程安全。
- 收口次序:审批悬着的按拒绝醒(CancelAll)→ 泵线程 join → 泵
  StopAccepting/Close → 放 gateway 锁。泵内 backend/模型是启动快照,
  `config/model/set` 不追改在飞泵(§三冻结合同)。

## 四、助理扩展方法面(additive,只在助理模式挂)

| 方法 | 语义 | 备注 |
| --- | --- | --- |
| `assistant/status` | 实例快照:bootId/profile/port/cwd/version/uptime/workLifetime + automation:{available,reason?} | 页头与诊断;任务面不可用如实报 |
| `config/status` | 生效配置投影:configured/provider/model/wire/baseUrl/apiKeyConfigured/apiKeySource | **零密钥**:来源只报 inline 或 env:NAME,永不回值 |
| `config/model/set` | 定点保存模型配置(Add/Replace + SetActive),活配置换血:新 thread 吃新账,已开 thread 材料不动(冻结合同) | 保存与连接检查分开;回执不回显密钥 |
| `config/test` | 端点 TCP 连通检查(有界 8s) | 如实:只验端点连通,不冒充"鉴权通过" |
| `task/create` | 建单次任务:{prompt, dueAtMs?, clientOperationId} | **幂等**:同键回原受理(duplicate=true),不写第二枚命令;due 0/缺省=立即;回执等泵消费(≤15s,泵忙如实报错,键在重发不双建) |
| `task/run-now` | 手动触发:{jobId, clientOperationId} | 幂等同上(runnow_keys) |
| `task/list` | 任务摘要列表:prompt 摘要/state/scheduleKind/dueAtMs/revision + 最近 occurrence(outcome/detail)+ 结果状态 | 只读投影;不含正文 |
| `task/read` | 任务全档 + occurrences(含 result:deliveryId/deliveryState/publishedPath/replyText≤64KB) | 结果正文优先发布文件、回落 replies 原件;账行不带正文,按 V1 合同从盘读回 |
| `task/cancel` | 取消:{jobId, expectedRevision(CAS), clientOperationId} | 已取消的重复取消回当前态(duplicate);CAS 拒如实报 |
| `approval/list` | 悬着的审批投影(重连/刷新后可发现) | 不受任务面可用性门 |
| `approval/respond` | 答复:{requestId, decision: accept\|decline} | 迟到/收口/不认识回 `{resolved:false, reason:"stale_request_id"}`,不冒充已答 |
| `assistant/events/read` | 事件账补账:{bootId, lastSeq} → {bootId, currentSeq, oldestSeq, reset, events} | 见 §四之二 |

独立 `app-server` 的方法面与能力表不带这些名字(capabilities.methods
按模式区分)。

## 四之二、任务/审批事件与重连补账(W2)

- **事件账**(`AssistantEventHub`):进程级有界环形账(容量 512,挤出最
  老),seq 单调、绑定 bootId。事件先进账再推活连接(`Server::
  EmitHostEvent`,与回合事件同一出口);**断线期间不丢**(留有界账)——
  这是与聊天线"没人听的事件不留"(W0)的分岔:任务/审批是补账面。
  实时推送的 params 带账面 `seq`,页面见过即推进游标。
- 事件三枚:`assistant/task/event`(kind:job.created/job.updated/
  occurrence.created/occurrence.changed,带 jobId/occurrenceId/state/
  outcome/detail/sessionId/turnId)、`assistant/approval/request`(带
  归属与 inputPreview≤1KB;**must_keep**——丢了页面不知道要答)、
  `assistant/approval/resolved`(outcome:accepted/declined/
  timeout_declined/cancelled)。
- **补账合同**:重连后 `assistant/events/read {bootId, lastSeq}` →
  bootId 不符(服务端重启)或 lastSeq+1 已被帽挤出 → `reset=true` 回最
  近一批,页面清本地投影快照重读;否则回 seq>lastSeq 的增量。不重复
  不丢以账面 seq 为准。
- **审批闸**:任务执行里 needs_confirm 工具经 `HeadlessExecutor` 的
  注入口(W2 加的 `Options::on_tool_confirm`,空=既有无人值守
  ChannelConfirmAllows fail-closed,行为零变化)推到页面。
  **超时政策默认拒绝不默认放行**(缺省 120s,`LUBANCODE_ASSISTANT_
  APPROVAL_TIMEOUT_MS` 可调);超时/收口的拒绝文案如实写("审批超时,
  按拒绝收口"),不冒充用户拒绝。审批归属(jobId/occurrenceId)按泵内
  当前 claimed 的在飞 occurrence 取(单飞泵同时至多一枚)。

## 五、本地 HTTP 认证与资源(W1,§七合同)

承载层 `src/app_server/local_web_server.cpp`:复用 ws_transport 底层
(ws_sockets/ws_frames/升级后的 Session),新增受限静态资源与认证门。

- **同源门**:Host 必须 `127.0.0.1:<port>` / `localhost:<port>`(防 DNS
  rebinding);Origin 若在场必须同源(防跨站)。`/healthz` 除外(只回
  身份:service/bootId/profile/version/pid,零秘密)。
- **静态资源**:随包固定 manifest 白名单(`/`、`/index.html`、
  `/assistant.css`、`/assistant_core.js`、`/assistant_app.js`);表外一
  律 404(白名单即墙:穿越/编码绕过/目录列表无从谈起)。资源缺失启动
  明错拒启,不起空壳服务。应答带 CSP(本地受控资源、显式列 ws:// 回环)、
  `X-Content-Type-Options: nosniff`、`Cache-Control: no-store`。
- **bootstrap 流程**:启动 URL 为 `http://127.0.0.1:<port>/#b=<一次性
  凭据>`。fragment 不发往服务端;页面 JS `POST /auth/exchange` 换
  `HttpOnly; SameSite=Strict; Path=/` 会话 cookie(24h),交换成功立即
  清 fragment。凭据一次性(用过即焚)、2 分钟时效、恒时比较;重放一律
  403,不区分话面。无凭据/会话过期:静态 401 + 本地配对提示页,不降级
  免认证。
- **控制口**:`POST /control/open` 吃锁文件里的 control_secret(同用户
  本地进程才读得到锁文件),回新 bootstrap URL——凭据不进 URL 查询串。
- **有界**:头部 16KB、POST body 8KB、单枚资源 4MB、单枚 artifact 64MB
  (与 WS 承载同尺)。
- **WS 升级**:过同源门 + 会话 cookie 门才应 101;Session 复用
  WsTransport(帧编解码/自动应 ping/close 收线一套账)。
- **单活跃控制连接 + 显式接管**(§六):第二条控制连接收
  `assistant/connection/occupied` 通报后即收线(页面显示"被占用,可接
  管");`/ws?takeover=1` 显式接管——只换控制连接(旧连接被关),不停
  后台任务(Detached 合同)。

## 六、页面(web/assistant,随包)

原生 HTML/JS/CSS,无外部 CDN、无构建要求。聊天(历史列表/消息流/输入/
停止任务)、任务(创建/列表/详情/结果/取消)、结果(执行与投递状态分
栏:成功+delivered / 待投递 / 投递异常+needs_review)、待审批(悬着+
最近已决,批准/拒绝)、设置(模型配置首配流程 + 运行信息 + 停止助理)。
复用 examples/web-console 的协议客户端经验(assistant_core.js:通道/事
件账/bootstrap 交换),断线自动重连 + 领域快照补账 + 事件账补账
(§四之二)。消息按不可信文本渲染(textContent,不 innerHTML);凭据
不进 localStorage/URL(fragment 交换完即清)。

## 七、验收与未验边界(如实)

已验(ctest,详见源单记账):

- `unit.app_server.test_local_web_server` —— 同源门/静态白名单/交换防
  重放/控制口/WS 门/启动门(缺资源、非回环、端口冲突)。
- `unit.app_server.test_app_server_detached` —— 断线合同两模式对照
  (SessionBound 断线即打断;Detached 断线后 final、模型恰好一次)、
  能力声明区分。
- `unit.app.test_assistant_cli_options` —— 子命令解析。
- `unit.app.test_assistant_task_face`(W2)—— 方法面全册:任务全链
  (create→执行→settled→结果正文与发布文件)、幂等双提交(任务恰一、
  模型恰一)、审批超时默认拒绝(工具零执行+事件轨迹)、审批批准放行
  (工具执行)、stale 答复、取消 CAS+幂等、任务面不可用稳定错误、
  事件账 seq/bootId/缺口、任务事件变迁。
- `e2e.assistant.web`(有 node 的腿)—— 真 exe + 假模型九幕:启动 URL、
  认证门、聊天链路(首配 → 对话 → 幂等重发零重跑)、刷新恢复、断线合同
  (含占用/接管)、**任务全链(建任务→断线→完成→重连补账→查结果)**、
  **幂等双提交(任务恰一/模型恰一)**、**审批(超时拒绝不执行工具/
  批准后文件落地/stale)**、重复启动/端口冲突/shutdown/锁释放重启。

未验边界:

- **真实浏览器 UI 交互**(390px/键盘导航/长消息渲染)未进 CI——协议层
  全验,UI 面待真实浏览器验收批次(可循 browser.mcp.selftest 路)。
- **真实模型**未碰——e2e 全部假后端;真模型首轮人工验收归发布批次。
- 端口冲突案只验"被占"分岔;防火墙/杀软拦监听的现场形状未验。
- **任务面与 gateway 并存的互斥**只验锁语义的单侧(单测钉锁取不到→
  不可用);"真 gateway run + 真 assistant 同机同 profile"的端到端互斥
  演练未做(锁是同一把 GatewayLock,合同上互斥;如实留账)。
- **审批悬停在断线期间的重现**:补账面(approval/list + 事件账)合同
  在、单测钉过;真实浏览器上"断线回来看到悬着审批"的交互未验。
- thread/start 回执不带 connection 快照(首配可改配置,冻结快照会误导);
  当前连接真值走 `config/status`。
- 泵内模型配置是启动快照:首配模型后 `config/model/set` 不追改任务泵
  (聊天新场吃新账);重启助理后任务泵吃新配置。如实。
