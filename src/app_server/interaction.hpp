// app-server 的交互悬起件(阶段 2:审批与打断):把 runtime::
// InteractionBroker 的合同接到 stdio 双向协议上。
//
// 一枚 PendingInteraction = 一条在飞的服务端反向请求:
//   - 工作线程(turn 驱动)经 AskApproval/AskQuestion 发请求:登记
//     request_id、把 permission/request 或 user/ask 出站消息推进 outbox
//     (must_keep,审批丢了客户端就不知道要答),然后原地等 future;
//   - 读线程收到前端对反向请求的响应(IncomingResponse),经
//     HandleIncomingResponse 把答复 resolve 到对应 promise;
//   - 悬空收口(打断/断线/超时/关 thread)由 CancelPending 统一按
//     cancel 收口:future 返回 nullopt,拒绝文案由调用方写明真因
//     (断线/超时/打断),不冒充"用户拒绝";
//   - 迟到的回答(答完/收口/不认识的 id)报 kStaleRequestId 错误码,
//     不等、不存、不崩。
//
// request_id 的形状:"turn-<N>-item-<M>-<k>"——回合内条目 id 天然单调,
// 同一条目上多枚交互(理论上有,防御)再添序号。TODO(seq 分配器):runtime
// P4 的统一 seq 分配器落地后,request_id 应改挂 thread 级单调序号,免得
// 两个 thread 的 id 撞形(现下 thread_id 也随请求登记,配对按 thread +
// request_id 两级查,撞不了;改挂 seq 是给前端排序对账用的,另一路工人
// 的活,这里只留口)。
//
// 会话级放行账(acceptForSession):记在本 ThreadRecord 的
// session_allowed_tools 里,同 thread 后续同工具名免问。只写本会话内存,
// 不落盘——"顺手写进 settings.local.json"要另发明确命令,不藏在审批
// 回调里追问第二遍(runtime 合同约定 1)。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "runtime/interaction_broker.hpp"  // ApprovalRequest/ApprovalResponse/InteractionFuture(合同在 runtime/interaction.hpp)

namespace lubancode::app_server {

// 一枚挂起的交互请求(审批或提问共用,四态共用,一张 pending 表)。
struct PendingInteraction {
    std::string request_id;
    std::string thread_id;
    std::string turn_id;
    // 出站方法名(permission/request 或 user/ask)。
    std::string method;
    // 审批对应的工具名(acceptForSession 记会话级放行账用;提问时空)。
    std::string tool_name;
    // 答复类型:审批走 ApprovalResponse,提问走 QuestionResponse。
    enum class Kind { Approval, Question };
    Kind kind = Kind::Approval;
};

// 前端对反向请求的响应解析结果。
struct InteractionResolution {
    bool ok = false;          // false = 报文形状不对(错误码见 error_code)
    std::string error_code;   // kStaleRequestId / kErrInvalidParams / 空
    std::string error_message;
    runtime::ApprovalResponse approval;
    runtime::QuestionResponse question;
};

// PendingFuture 是 runtime::InteractionFuture 的悬起实现(骨架拆解批四:
// agent 侧与 runtime 侧的同名接口已合成一份合同,这里只实现一份)。
//
// Wait 是"阻塞到有结果或悬空收口"的合同语义;限时与打断轮询的实现:
// promise 的 future 是真 std::future,Wait 外围分片轮询(poll_slice),
// 每 25ms 醒一次查打断旗与超时线——审批是人工节奏,这个粒度毫无压力,
// 且不造额外线程(审批悬停本来就该轻)。
class PendingFuture final : public runtime::InteractionFuture {
public:
    std::optional<runtime::ApprovalResponse> WaitApproval() override;
    std::optional<runtime::QuestionResponse> WaitQuestion() override;

    // 超时线(0 = 不限):Wait 里到了还没答复,按悬空收口返回 nullopt。
    void SetTimeout(std::chrono::milliseconds timeout) { timeout_ = timeout; }
    // 打断旗:置位即悬空收口(打断先于答复/超时的情形)。
    void WatchInterrupt(const std::atomic<bool>* flag) { interrupt_flag_ = flag; }

    // 悬起件内部用(共享 promise 的两头)。
    std::shared_ptr<std::promise<std::optional<runtime::ApprovalResponse>>> approval_promise;
    std::shared_ptr<std::promise<std::optional<runtime::QuestionResponse>>> question_promise;

private:
    // 分片等 promise:醒一次查旗查表,没到就再睡。true = promise 有了
    //(值在 out);false = 悬空收口(打断/超时)。
    template <typename T>
    bool PollFuture(std::future<std::optional<T>>& future, std::optional<T>& out);

    std::chrono::milliseconds timeout_{0};
    const std::atomic<bool>* interrupt_flag_ = nullptr;
};

// 就绪 future(会话级放行免问路径用):构造时带答案,Wait 立即返回。
class ReadyApprovalFuture final : public runtime::InteractionFuture {
public:
    explicit ReadyApprovalFuture(std::optional<runtime::ApprovalResponse> response)
        : response_(std::move(response)) {}
    std::optional<runtime::ApprovalResponse> WaitApproval() override { return response_; }
    // 审批路用不到提问:空实现返回悬空收口(合同允许,实现窄)。
    std::optional<runtime::QuestionResponse> WaitQuestion() override { return std::nullopt; }

private:
    std::optional<runtime::ApprovalResponse> response_;
};

// 一场 thread 的交互悬起件:pending 表 + 会话级放行账。
// 线程安全:读线程(登记/答复/收口)与工作线程(Wait)并发用。
// thread_id 定死在构造时(不可拷贝不可赋值:mutex 与 const 成员在里头);
// ThreadRecord 里经 unique_ptr 持有,thread_id 就绪后 Reset 一次。
class InteractionLedger {
public:
    explicit InteractionLedger(std::string thread_id)
        : thread_id_(std::move(thread_id)) {}

    // ---- 登记侧(工作线程,turn 驱动) ----

    // 发一枚审批请求:登记 request_id、emit 出站事件,返回可 Wait 的
    // future。emit 为出站口(server 装配:outbox 的 must_keep push)。
    // 返回的 future 由调用方在同一线程 Wait。
    std::shared_ptr<runtime::InteractionFuture> AskApproval(
        const runtime::ApprovalRequest& request, const std::string& turn_id,
        const std::function<void(std::string_view method, const nlohmann::json& params)>& emit);

    // 发一枚提问(user/ask)。返回的 future 只用得到 WaitQuestion,类型
    // 是 PendingFuture(带超时/打断的 SetTimeout/WatchInterrupt 口)。
    std::shared_ptr<PendingFuture> AskQuestion(
        const runtime::QuestionRequest& request, const std::string& turn_id,
        const std::function<void(std::string_view method, const nlohmann::json& params)>& emit);

    // ---- 答复侧(读线程,HandleResponse 进来) ----

    // 前端对反向请求的响应:id 配 pending 表,resolve promise。
    // 配不上(答完/收口/不认识)= 迟到,报 kStaleRequestId。
    // acceptForSession 的答复顺带落会话级放行账(工具名在 Ask 时登记)。
    InteractionResolution HandleIncomingResponse(const IncomingResponse& response);

    // ---- 悬空收口 ----

    // 清掉全部挂起请求(打断/断线/超时/关 thread):resolve 成 cancel
    // 语义(future 返回 nullopt,调用方写真因文案)。返回清掉的条数。
    std::size_t CancelPending();

    // ---- 会话级放行账(acceptForSession) ----

    // 同工具名是否已放行(免问)。
    bool IsSessionAllowed(const std::string& tool_name) const;
    // 记一笔会话级放行。
    void AllowForSession(const std::string& tool_name);

    // 挂起条数(诊断与测试)。
    std::size_t pending_count() const;
    const std::string& thread_id() const { return thread_id_; }

private:
    const std::string thread_id_;
    mutable std::mutex mutex_;
    // request_id -> 挂起交互(future 归工作线程;这里只放 promise 的
    // shared 端与身份,不 Wait)。
    struct Entry {
        PendingInteraction info;
        std::shared_ptr<std::promise<std::optional<runtime::ApprovalResponse>>> approval_promise;
        std::shared_ptr<std::promise<std::optional<runtime::QuestionResponse>>> question_promise;
    };
    std::map<std::string, Entry> pending_;
    // 会话级放行账(只写内存,不落盘)。
    std::map<std::string, bool> session_allowed_;
};

}  // namespace lubancode::app_server
