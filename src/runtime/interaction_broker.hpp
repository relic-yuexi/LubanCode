// InteractionBroker 合同(显示系统剥离单第一步:立合同,不改画面)。
//
// 审批与提问的中立通道:Runtime 发 ApprovalRequested/QuestionRequested、
// 登记 request_id,工作线程等 future;前端从任何线程拿 request_id 回答
//(ResolveApproval/AnswerQuestion)。连接线程/WebSocket 线程/Tauri relay
// 不跟着阻塞——这就是把 on_tool_confirm 的同步问话从 stdin 上解下来的
// 那颗螺钉。
//
// 约定(单子"三、审批与提问"全文照抄进类型注释):
//   1. 决定四态:accept(本次允许)/ accept_for_session(会话总允许,只写
//      本场权限账)/ decline(拒绝)/ cancel(悬空收口)。会话永久放行
//      不落盘;"顺手写进 settings.local.json"另发一枚明确命令,不藏在
//      审批回调里追问第二遍。
//   2. 回合取消、客户端断开、超时或 thread 关闭时,Broker 清掉悬空请求,
//      统一按 cancel 收口;迟到的回答对已失效 request_id 报
//      kStaleRequestId(不等、不存、不崩)。
//   3. ask_user 走同一 Broker,不另开 ReadLine 私门。
//
// 依赖铁律同 event.hpp:零实现依赖,不 include cli/app/frontend。请求/
// 回答/未来那族中立形状住 runtime/interaction.hpp(引擎侧也只认那头),
// 这里只剩请求号与 Broker 本体。

#pragma once

#include <memory>
#include <string>

#include "runtime/interaction.hpp"

namespace lubancode::runtime {

// 审批/提问共用一枚请求号:同一张 pending 表、同一套四态、同一套悬空
// 收口,不分两张账。
struct InteractionRequestId {
    std::string value;  // Runtime 独家分配,不重号;空串不是合法 id

    bool valid() const { return !value.empty(); }
};

// 同步门面。Ask* 在工作线程被调;返回的 future 由调用方在同一线程 Wait。
// 实现负责把 ApprovalRequested/QuestionRequested 事件发给 EventSink、
// 登记 pending 表;Resolve/Answer 从任意线程进来。
class InteractionBroker {
public:
    ~InteractionBroker() = default;

    // 发一枚审批请求(工具 needs_confirm 真要问用户时)。
    virtual std::shared_ptr<InteractionFuture> AskApproval(const ApprovalRequest& request) = 0;

    // 发一枚提问(ask_user)。
    virtual std::shared_ptr<InteractionFuture> AskQuestion(const QuestionRequest& request) = 0;

    // 前端回答(任意线程):request 已失效(答完/收口/不认识)返回 false,
    // 迟到回答不 resurrect。
    virtual bool ResolveApproval(const InteractionRequestId& id, const ApprovalResponse& response) = 0;
    virtual bool AnswerQuestion(const InteractionRequestId& id, const QuestionResponse& response) = 0;
};

// 稳定错误码:迟到/失效回答的判据(前端要拿它区分"答对了"与"答晚了")。
inline constexpr const char* kStaleRequestId = "stale_request_id";

// 枚举 <-> 稳定字符串。
std::string ToString(InteractionDecision decision);
bool ParseInteractionDecision(const std::string& s, InteractionDecision& out);

}  // namespace lubancode::runtime
