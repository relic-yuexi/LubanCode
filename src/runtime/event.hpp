// Runtime 事件合同(显示系统剥离单第一步:立合同,不改画面)。
//
// 这只头是"内核向画面说话"的唯一形状:SessionRuntime/TurnRuntime 只吐
// ServerEvent,不吐 ANSI、不吐终端宽、不吐"● Done"一类画面文案。现有
// TUI 先靠 adapter 消费,行为逐字不变;后来的 app-server/Web/Tauri 接
// 同一份结构,不复制 AgentLoop。
//
// 依赖铁律:本头(连同 command.hpp/event_sink.hpp/interaction_broker.hpp)
// 只 include 标准库与 nlohmann/json,不 include cli/*、app/*、frontend/*,
// 也不被反向依赖——事件层是最底层,谁都可以拿来用,它谁都不认。
//
// 约定(类型上表达不出来的,在这里定死):
//   1. seq 在一条 thread 内单调递增,由 Runtime 独家分配;前端凭它排序、
//      查漏、断线补账,不靠收到次序猜。1 起,0 不发。
//   2. 三层身份:thread_id(一场会话)> turn_id(一问一答)> item_id
//      (条目:工具/思考/正文块/命令/diff/todo/子代理都落成 item)。下层
//      事件必须带齐自己及以上所有层的 id。
//   3. 终态唯一:ItemCompleted/TurnCompleted 对同一 id 只发一次;终态四分
//      (succeeded/failed/declined/cancelled),不许拿 failed 冒充 declined。
//      非终态事件(delta)可发任意次。
//   4. 事件数据不带 ANSI,不按终端宽度截断,不预先翻成画面文案;错误用
//      稳定 code + details,可另带 fallback message 供人读。
//   5. 时间戳统一 Unix epoch 毫秒(int64);不上墙钟字符串。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::runtime {

// 事件里的身份与时间戳:每条事件至少带这些。终态唯一性、seq 单调性见
// 文件头约定。
struct EventEnvelope {
    std::string thread_id;  // 哪场会话
    std::uint64_t seq = 0;  // thread 内单调递增(Runtime 独家分配)
    std::int64_t timestamp_ms = 0;  // Unix epoch 毫秒

    // 序列化保真:往返(to_json/from_json)字段一致,见 test_runtime_contract.cpp。
    nlohmann::json to_json() const;
    static EventEnvelope from_json(const nlohmann::json& j);  // 坏形状抛异常
};

// 条目身份:一条 ServerEvent 只可能挂在其中一层。layer=Item 时三件齐;
// layer=Turn 时 item_id 为空;layer=Thread 时只带 thread_id(在 envelope)。
enum class EventLayer { Thread, Turn, Item };

// 条目种类:工具、思考、正文、命令、diff、todo、子代理——都落成 item,
// 前端按种类挑组件,不另开旁路事件。
enum class ItemKind { Tool, Thinking, Text, Command, Diff, Todo, Subagent, Goal };
// 条目种类:工具、思考、正文、命令、diff、todo、计划、子代理——都落成
// item,前端按种类挑组件,不另开旁路事件。Plan 是计划成品(PlanDocument)
// 的独立条目(只读研究硬闸单):不拿 Text/Todo 顶替——Web/Tauri 要凭它开
// 计划审阅器,delta 流式到完整 item/completed 才可审。
enum class ItemKind { Tool, Thinking, Text, Command, Diff, Todo, Plan, Subagent };

// 终态四分(文件头约定 3)。前置状态不叫终态,叫"进行中"。
enum class Outcome { Succeeded, Failed, Declined, Cancelled };

// 错误载荷:稳定 code 打头,前端凭 code 翻译;details 是结构化补充;
// fallback_message 是"没翻译表也能看"的人话兜底,三者各自可选填,
// 但 code 为空串的 Error 事件不许发(那就没翻译锚点了)。
struct ErrorPayload {
    std::string code;
    nlohmann::json details = nlohmann::json::object();
    std::string fallback_message;

    nlohmann::json to_json() const;
    static ErrorPayload from_json(const nlohmann::json& j);
};

// ---------------------------------------------------------------------------
// ServerEvent(单态结构,kind 领域字段按需填)
//
// 不用 std::variant 的原因:variant 每加一种事件全体消费方都得改 match,
// 协议期事件种数还会长;单态结构 + kind 枚举对序列化、对"未知 kind 容忍
// 跳过"的协议演进都更皮实。领域字段与 kind 不匹配时序列化照写(不崩),
// 消费方按 kind 取用——与"老版本读到新事件行当坏行跳过"的存档演进同思路。
// ---------------------------------------------------------------------------
enum class ServerEventKind {
    // thread 层
    ThreadStarted,
    ThreadUpdated,
    // thread 永久删除(SessionLifecycle 删成后发;payload 带 thread_id)。
    ThreadDeleted,
    // turn 层
    TurnStarted,
    TurnCompleted,
    // item 层:非终态可多次,终态一次
    ItemStarted,
    ItemDelta,
    ItemCompleted,
    // 交互(见 interaction_broker.hpp):request_id 悬在 pending 请求表里
    ApprovalRequested,
    QuestionRequested,
    InteractionResolved,
    // Plan 模式(只读研究硬闸单):模式切换与计划审阅,thread 层事件。
    //   CollaborationModeChanged:payload 带 mode/previous_mode/reason/revision。
    //   PlanReviewRequested:Plan turn 收口且见完整 PlanDocument 后由空闲层
    //     发,payload 带 plan_id/plan_revision/sha256/artifact_ref/available_
    //     decisions。不冒充普通 tool approval(单子:专用事件)。
    //   PlanReviewResolved:用户答完,payload 带 decision 与所选
    //     selected_permission_mode(批准时)。
    CollaborationModeChanged,
    PlanReviewRequested,
    PlanReviewResolved,
    // 记账与杂项
    UsageUpdated,
    ContextUpdated,
    Warning,
    Error,
};

struct ServerEvent {
    EventEnvelope envelope;
    ServerEventKind kind = ServerEventKind::ThreadStarted;

    // 下两层身份(envelope 里已有 thread_id)。
    std::string turn_id;
    std::string item_id;
    ItemKind item_kind = ItemKind::Tool;  // kind 是 Item* 时有效

    // ItemCompleted/TurnCompleted 的终态;InteractionResolved 的决定也复用
    // (Declined=拒绝、Cancelled=悬空收口)。
    std::optional<Outcome> outcome;

    // 领域载荷(按 kind 取用,见上):delta 文本、usage 数字、上下文水位、
    // 错误载荷、审批/提问的请求描述。不匹配 kind 的字段序列化时照带不崩,
    // 消费方别读。
    std::string text;              // ItemDelta 的正文/思考增量
    nlohmann::json payload = nlohmann::json::object();  // 其余结构领域数据

    nlohmann::json to_json() const;
    static ServerEvent from_json(const nlohmann::json& j);
};

// 枚举 <-> 稳定字符串(线上表示是字符串,不是数字——数字重排就是协议破坏)。
std::string ToString(ServerEventKind kind);
std::string ToString(ItemKind kind);
std::string ToString(EventLayer layer);
std::string ToString(Outcome outcome);
bool ParseServerEventKind(const std::string& s, ServerEventKind& out);
bool ParseItemKind(const std::string& s, ItemKind& out);
bool ParseOutcome(const std::string& s, Outcome& out);

// 事件落在哪一层(终态唯一性检查、路由过滤用)。
EventLayer LayerOf(const ServerEvent& event);

}  // namespace lubancode::runtime
