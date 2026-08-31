// app-server 协议常量:错误码、方法名、事件名、条目类型、回合终态、
// 审批决定。单子《无界面后台协议与 SSH 远程项目》协议底线 v1 的骨架期
// 落点——只立名字与号码,不接执行。
//
// 错误码沿用 JSON-RPC 2.0 标准段(-32700/-32600/-32601/-32602/-32603),
// 服务器自定义段用 -32000..-32099。骨架期定死的这一张表就是"稳定错误码":
// 坏 JSON、未知方法、错参数、未握手、队列满,各自的号码写进单测,冻结
// schema 前不许漂移。
#pragma once

#include <string_view>

namespace lubancode::app_server {

// ---------------------------------------------------------------------------
// 传输与信封
// ---------------------------------------------------------------------------

// 协议版本(initialize 结果里回给前端)。schema 冻结前从 "1.0" 起跳;
// 往后任何报文形状变更必须 bump,前端拿它对表。
//
// 版本账:
//   1.0 —— 骨架期全部方法与事件(thread/turn/item、审批、usage/context)。
//   1.1 —— 浏览器调试工作台阶段 3:additive 新增 browser/* 方法 18 枚与
//          browser/* 事件 13 族(老方法老事件形状一字未动;详见本文件
//          browser 两节与 docs/features/app-server/README.md)。
//   1.1(阶段 B 注,additive)—— 用户输入路由与暂停:新增 browser/pause|
//          browser/resume 两枚方法与 browser/paused|browser/resumed 两族
//          事件;browser/status 的 result 增可选布尔 paused;owner 改由
//          内核按连接裁定(外壳报的 owner 只是意向,内核说了算——伪造
//          owner:"user" 的非用户连接被 browser.owner_denied 明拒);owner
//          缺省不再是写死的 "user",而是连接的裁定身份。老报文形状零改动。
//   1.1(阶段 C 注,additive)—— 镜像流:新增 browser/screencast/start|
//          browser/screencast/stop 两枚方法与 browser/screencast/frame
//          事件。screencast 只读(不改页面状态)、不问审批,与 snapshot/
//          screenshot 同档;字节走同一条截图 artifact 链落盘,协议上只见
//          引用与 pageId,绝不出现 base64。frame 事件带 dropped(内核侧
//          落盘赶不上帧速时,丢最老帧计的账,下一帧报完清零)。老报文
//          形状零改动。
inline constexpr std::string_view kProtocolVersion = "1.1";

// jsonrpc:"2.0" 字段去留已冻结(阶段 3,schema 定案):
//   - 出站:不带。方法名/params/id 的形状自足,少一个字段少一分冗余;
//   - 入站:一律不校验(带不带都认)。
// 改这个开关 = 协议破坏,须 bump kProtocolVersion 并出兼容单,不许悄悄翻。
inline constexpr bool kEmitJsonRpcField = false;
inline constexpr std::string_view kJsonRpcVersion = "2.0";

// 事件显式 seq 字段(阶段 3 冻结):每条事件(params 里)带 "seq"——
// runtime::ProcessIdAuthority 的 NextSeq 发的进程内单调号,由连接层的
// 出站口统一盖(一处盖,不许散着抄)。前端凭它排序、查漏;与事件的
// 到达次序解耦。响应(result/error)按 id 配对,不带 seq。
inline constexpr const char* kSeqField = "seq";

// ---------------------------------------------------------------------------
// 错误码(稳定,冻结前不许改号)
// ---------------------------------------------------------------------------

// 标准 JSON-RPC 段。
inline constexpr int kErrParseError = -32700;     // 整行不是合法 JSON
inline constexpr int kErrInvalidRequest = -32600; // 不是合法请求/通知,或重复 initialize
inline constexpr int kErrMethodNotFound = -32601; // 方法名不认识
inline constexpr int kErrInvalidParams = -32602;  // 参数缺字段/类型不对
inline constexpr int kErrInternalError = -32603;  // handler 自己炸了

// 服务器自定义段。
inline constexpr int kErrServerBusy = -32000;       // 请求入口有界队列满,稍后重试
inline constexpr int kErrNotInitialized = -32002;   // 还没握手就调业务法子
inline constexpr int kErrShutdownRequested = -32003; // 已 shutdown,不再接业务
inline constexpr int kErrTurnAlreadyRunning = -32004; // 同一 thread 同拍开两轮:协议明拒

// ---------------------------------------------------------------------------
// 方法名(请求 + 通知)
// ---------------------------------------------------------------------------

// 握手与退场。
inline constexpr std::string_view kMethodInitialize = "initialize";
inline constexpr std::string_view kMethodInitialized = "initialized"; // 通知
inline constexpr std::string_view kMethodShutdown = "shutdown";
inline constexpr std::string_view kMethodExit = "exit"; // 通知

// thread:会话的创建、列举、停场、搬删。resume/read 留位,名字先留。
inline constexpr std::string_view kMethodThreadStart = "thread/start";
inline constexpr std::string_view kMethodThreadStop = "thread/stop";
inline constexpr std::string_view kMethodThreadList = "thread/list";
inline constexpr std::string_view kMethodThreadArchive = "thread/archive";
inline constexpr std::string_view kMethodThreadUnarchive = "thread/unarchive";
inline constexpr std::string_view kMethodThreadDelete = "thread/delete";
inline constexpr std::string_view kMethodThreadResume = "thread/resume"; // 留位:存档恢复单
inline constexpr std::string_view kMethodThreadRead = "thread/read";    // 留位:只读详情

// workflow:run 账的只读查询(阶段 4:wf 线的事件出口,快照 + 增量)。
inline constexpr std::string_view kMethodWorkflowList = "workflow/list";   // 留位
inline constexpr std::string_view kMethodWorkflowQuery = "workflow/query"; // run 快照 + 增量事件

// 逐枚追踪单:工具追踪账的查询口(断线补账 / 冷回放)。lastSeq 取
// "已见到的最大 seq",返回大于它的事件;缺省 0 = 全量。事件从 session
// 存档的 tool_trace_v1 行折叠——进程重启后仍可查(单子第 5 期:
// "app-server 断线按 seq 补事件,必要时从 session trace 冷回放")。
inline constexpr std::string_view kMethodTraceQuery = "trace/query";

// turn:一轮问答。steer 骨架期不接(SteeringQueue 另一张单在改),interrupt
// 阶段 2 接线。
inline constexpr std::string_view kMethodTurnStart = "turn/start";
inline constexpr std::string_view kMethodTurnSteer = "turn/steer";        // 留位:追加指令
inline constexpr std::string_view kMethodTurnInterrupt = "turn/interrupt"; // 精确打断(已接线)

// 只读信息面。骨架期不接。
inline constexpr std::string_view kMethodModelList = "model/list";   // 留位
inline constexpr std::string_view kMethodConfigRead = "config/read"; // 留位

// ---------------------------------------------------------------------------
// /goal 持久目标 + /loop 会话定时循环 + Plan 审阅(goal 单合流批:typed
// 命令面挂上 server)。方法名与 ClientCommandKind 的稳定串对齐(goal.*
// / loop.* / plan.*),payload 形状见 command.hpp 的注释——前端发的是
// typed 命令的参数,不是 slash 字符串。
// ---------------------------------------------------------------------------

inline constexpr std::string_view kMethodGoalCreate = "goal/create";
inline constexpr std::string_view kMethodGoalGet = "goal/get";
inline constexpr std::string_view kMethodGoalEdit = "goal/edit";
inline constexpr std::string_view kMethodGoalPause = "goal/pause";
inline constexpr std::string_view kMethodGoalResume = "goal/resume";
inline constexpr std::string_view kMethodGoalClear = "goal/clear";

inline constexpr std::string_view kMethodLoopCreate = "loop/create";
inline constexpr std::string_view kMethodLoopList = "loop/list";
inline constexpr std::string_view kMethodLoopRead = "loop/read";
inline constexpr std::string_view kMethodLoopPause = "loop/pause";
inline constexpr std::string_view kMethodLoopResume = "loop/resume";
inline constexpr std::string_view kMethodLoopCancel = "loop/cancel";
inline constexpr std::string_view kMethodLoopRunNow = "loop/run";

inline constexpr std::string_view kMethodPlanSetMode = "plan/set_mode";
inline constexpr std::string_view kMethodPlanReview = "plan/review";
inline constexpr std::string_view kMethodPlanReopen = "plan/reopen";

// ---------------------------------------------------------------------------
// browser(浏览器调试工作台 阶段 3:前端 <-> Runtime 的方法面)
//
// C++ 侧是协议转发层:真 Runtime 在 Node sidecar(browser/sidecar.js)里,
// 复用 browser/lib/session.js 的 BrowserSession——Playwright 生命周期、
// 页签账、ref、journal、崩溃终态只有那一本账。这里的 handler 只做参数
// 校验、审批、取消、事件转发与 artifact 落盘。
//
// 方法分两档:
//   同步(读线程直答,sidecar 内存账,快):status / page/list /
//     console/query / network/query / downloads/query;
//   异步(立即回 actionId,终态走 browser/action/completed 事件):
//     start / stop / page 一族 / snapshot / screenshot / action——导航与
//     动作可能要等审批、等页面,不许堵读线程。
//
// owner 仲裁:写动作带 owner("agent"|"user")。阶段 B 起 owner 由内核按
// 连接裁定(DispatchContext::principal):stdio 宿主与过门的 WS 连接是
// 操作者本人的手("user");内核内部(回合驱动的浏览器工具)与将来的
// agent 连接是 "agent"。外壳报的 owner 只是意向——非用户连接假冒
// owner:"user" 一律 browser.owner_denied 明拒(§六:Agent 假冒不来)。
// owner=agent 须带 threadId,过 permission/request 审批(acceptForSession
// 按方法名记账);owner=user 不带 threadId、不问审批、执行后递 userEpoch
// (Agent 拿旧观察自然 stale——阶段 2 的机制原样复用)。
// ---------------------------------------------------------------------------

inline constexpr std::string_view kMethodBrowserStart = "browser/start";
inline constexpr std::string_view kMethodBrowserStop = "browser/stop";
inline constexpr std::string_view kMethodBrowserStatus = "browser/status";
inline constexpr std::string_view kMethodBrowserPageOpen = "browser/page/open";
inline constexpr std::string_view kMethodBrowserPageList = "browser/page/list";
inline constexpr std::string_view kMethodBrowserPageSelect = "browser/page/select";
inline constexpr std::string_view kMethodBrowserPageClose = "browser/page/close";
inline constexpr std::string_view kMethodBrowserPageNavigate = "browser/page/navigate";
inline constexpr std::string_view kMethodBrowserPageBack = "browser/page/back";
inline constexpr std::string_view kMethodBrowserPageForward = "browser/page/forward";
inline constexpr std::string_view kMethodBrowserPageReload = "browser/page/reload";
inline constexpr std::string_view kMethodBrowserSnapshot = "browser/snapshot";
inline constexpr std::string_view kMethodBrowserScreenshot = "browser/screenshot";
inline constexpr std::string_view kMethodBrowserAction = "browser/action";
inline constexpr std::string_view kMethodBrowserActionCancel = "browser/action/cancel";
// 一键暂停(阶段 B):暂停期间 owner=agent 的动作一律受理不执行、终态
// error.code=browser.paused;用户动作照走;终态事件照发。只有用户连接
// 能按(操作者的手闸,Agent 自己不许碰)。
inline constexpr std::string_view kMethodBrowserPause = "browser/pause";
inline constexpr std::string_view kMethodBrowserResume = "browser/resume";
inline constexpr std::string_view kMethodBrowserConsoleQuery = "browser/console/query";
inline constexpr std::string_view kMethodBrowserNetworkQuery = "browser/network/query";
inline constexpr std::string_view kMethodBrowserDownloadsQuery = "browser/downloads/query";
// 镜像流(阶段 C):起停 Playwright/CDP screencast。只读,不问审批
//(与 snapshot/screenshot 同档);pageId 缺省 = 活动页。
inline constexpr std::string_view kMethodBrowserScreencastStart = "browser/screencast/start";
inline constexpr std::string_view kMethodBrowserScreencastStop = "browser/screencast/stop";

// ---------------------------------------------------------------------------
// browser 事件族(阶段 3)。params 一律带 seq(连接层统一盖)。高频的
// console/network 走批量事件(entries 数组 + dropped 明账),丢了可用
// browser/console/query / browser/network/query 凭 sinceSeq 补账。
// screenshot 只发 artifact 引用,绝不塞 base64。
// ---------------------------------------------------------------------------

inline constexpr std::string_view kEventBrowserStarted = "browser/started";
inline constexpr std::string_view kEventBrowserStopped = "browser/stopped";
inline constexpr std::string_view kEventBrowserCrashed = "browser/crashed";
inline constexpr std::string_view kEventBrowserPageCreated = "browser/page/created";
inline constexpr std::string_view kEventBrowserPageUpdated = "browser/page/updated";
inline constexpr std::string_view kEventBrowserPageClosed = "browser/page/closed";
inline constexpr std::string_view kEventBrowserNavigation = "browser/navigation";
inline constexpr std::string_view kEventBrowserConsoleEvent = "browser/console/event";
inline constexpr std::string_view kEventBrowserNetworkEvent = "browser/network/event";
inline constexpr std::string_view kEventBrowserDownloadEvent = "browser/download/event";
inline constexpr std::string_view kEventBrowserScreenshotReady = "browser/screenshot/ready";
inline constexpr std::string_view kEventBrowserActionStarted = "browser/action/started";
inline constexpr std::string_view kEventBrowserActionCompleted = "browser/action/completed";
inline constexpr std::string_view kEventBrowserUserEpoch = "browser/user_epoch";
// 暂停/恢复通报(阶段 B):params {paused:bool}。must_keep——丢了前端
// 的暂停灯就与内核拧着。
inline constexpr std::string_view kEventBrowserPaused = "browser/paused";
inline constexpr std::string_view kEventBrowserResumed = "browser/resumed";
// 镜像流帧(阶段 C):params {pageId, frameSeq, width, height, dropped,
// artifact}——字节落同一条截图 artifact 链,协议上只有引用。可丢(慢
// 消费者场景下队满丢最老帧);dropped 是这一帧之前、这一页丢了几帧,
// 报完清零(与 journal 批量的 dropped 同口径)。
inline constexpr std::string_view kEventBrowserScreencastFrame = "browser/screencast/frame";

// ---------------------------------------------------------------------------
// 服务端反向请求(骨架期只留名字与形状,不接线)
//
// 审批与 ask_user 是服务端发给前端的双向请求:请求带 threadId/turnId/
// itemId、工具名、参数摘要、风险说明与可选决定;前端回 accept /
// acceptForSession / decline / cancel(单子协议底线第二节第 6 条)。
// 审批反向请求的执行链走 Broker(另一条线),本批不实现,只把协议位
// 占住,免得冻结 schema 时再改方法名。
// ---------------------------------------------------------------------------

inline constexpr std::string_view kMethodPermissionRequest = "permission/request"; // 工具审批
inline constexpr std::string_view kMethodUserAsk = "user/ask";                     // ask_user

// 反向请求响应里的稳定错误码(runtime::kStaleRequestId 的协议面)。
inline constexpr int kErrStaleRequestId = -32005; // 迟到/失效的 requestId

// 审批决定(前端响应 permission/request 的 result.decision)。
inline constexpr std::string_view kDecisionAccept = "accept";
inline constexpr std::string_view kDecisionAcceptForSession = "acceptForSession";
inline constexpr std::string_view kDecisionDecline = "decline";
inline constexpr std::string_view kDecisionCancel = "cancel";

// ---------------------------------------------------------------------------
// 事件账(thread/* turn/* item/* 三层,单子协议底线第三节)
// ---------------------------------------------------------------------------

// thread 层。
inline constexpr std::string_view kEventThreadStarted = "thread/started";
inline constexpr std::string_view kEventThreadStopped = "thread/stopped";

// turn 层。completed 是唯一终态:一回合至多一条,由回合状态机保证。
inline constexpr std::string_view kEventTurnStarted = "turn/started";
inline constexpr std::string_view kEventTurnCompleted = "turn/completed";

// item 层。started/delta/completed 是条目生命周期;item 承正文、思考、
// 工具、命令、文件改动、提问、子代理与错误。
inline constexpr std::string_view kEventItemStarted = "item/started";
inline constexpr std::string_view kEventItemDelta = "item/delta";
inline constexpr std::string_view kEventItemCompleted = "item/completed";

// 条目类型(item/started 的 params.item.type)。
inline constexpr std::string_view kItemTypeText = "text";           // 正文
inline constexpr std::string_view kItemTypeThinking = "thinking";   // 思考
inline constexpr std::string_view kItemTypeTool = "tool";           // 工具调用
inline constexpr std::string_view kItemTypeCommand = "command";     // 前台命令
inline constexpr std::string_view kItemTypeFileChange = "file_change"; // 文件改动(留位:diff 线)
inline constexpr std::string_view kItemTypeQuestion = "question";   // ask_user(留位:审批线)
inline constexpr std::string_view kItemTypeAgent = "agent";         // 子代理(留位)
inline constexpr std::string_view kItemTypeError = "error";         // 错误条目

// 回合终态(turn/completed 的 params.status)。interrupted 是 turn/interrupt
// 的终态(阶段 2 接线);rejected 留位给"审批拒到回合收不了场"的分型。
inline constexpr std::string_view kTurnStatusSuccess = "success";
inline constexpr std::string_view kTurnStatusError = "error";
inline constexpr std::string_view kTurnStatusCancelled = "cancelled";
inline constexpr std::string_view kTurnStatusInterrupted = "interrupted"; // turn/interrupt 的终态
inline constexpr std::string_view kTurnStatusRejected = "rejected";       // 留位:审批拒绝分型

// ---------------------------------------------------------------------------
// 溢出通报
// ---------------------------------------------------------------------------

// 有界事件队列溢出时的明确通报(不许悄悄丢):params 带 dropped(这次
// 丢掉的条目数)与 coalesced(靠合并省下的条目数,阶段 3 起 delta 合并
// 真发生,不再恒 0)。终态与审批不许走这条路丢。
inline constexpr std::string_view kEventQueueOverflow = "queue/overflow";

// ---------------------------------------------------------------------------
// usage / context 事件(阶段 3:进度账)
// ---------------------------------------------------------------------------

// 每次到模型的请求收尾时发一次:params 带 usage 五项(camelCase,与
// turn/completed 的 usage 同形)与 model。一回合可能多次(工具往返)。
inline constexpr std::string_view kEventTurnUsage = "turn/usage";

// 上下文压力通报(PreRequest 评估 / hard trim 之后):params 带 phase
// ("pre_request"/"after_hard_trim")、projectedTokens、windowTokens、
// projectedOverflow、hardTrimmedTurns、hardDroppedMessages、
// hardTruncatedResults。前端画上下文水位条吃这个。
inline constexpr std::string_view kEventTurnContext = "turn/context";

}  // namespace lubancode::app_server
