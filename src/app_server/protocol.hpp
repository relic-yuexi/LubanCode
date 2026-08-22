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
inline constexpr std::string_view kProtocolVersion = "1.0";

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

// turn:一轮问答。steer 骨架期不接(SteeringQueue 另一张单在改),interrupt
// 阶段 2 接线。
inline constexpr std::string_view kMethodTurnStart = "turn/start";
inline constexpr std::string_view kMethodTurnSteer = "turn/steer";        // 留位:追加指令
inline constexpr std::string_view kMethodTurnInterrupt = "turn/interrupt"; // 精确打断(已接线)

// 只读信息面。骨架期不接。
inline constexpr std::string_view kMethodModelList = "model/list";   // 留位
inline constexpr std::string_view kMethodConfigRead = "config/read"; // 留位

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
