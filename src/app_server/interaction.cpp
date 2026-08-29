// interaction.hpp 的实现。
#include "app_server/interaction.hpp"

#include <chrono>
#include <future>
#include <utility>
#include <vector>

#include "runtime/id_authority.hpp"

namespace lubancode::app_server {

// ---------------------------------------------------------------------------
// PendingFuture
// ---------------------------------------------------------------------------

namespace {

// 分片轮询周期:审批是人工节奏,25ms 的反应间隔对协议毫无压力。
constexpr auto kPollSlice = std::chrono::milliseconds(25);

}  // namespace

// PollFuture 的显式实例化(模板成员在 .cpp 里用,两类都要有形)。
template <typename T>
bool PendingFuture::PollFuture(std::future<std::optional<T>>& future, std::optional<T>& out) {
    const auto deadline =
        timeout_ > std::chrono::milliseconds(0)
            ? std::chrono::steady_clock::now() + timeout_
            : std::chrono::steady_clock::time_point::max();
    while (true) {
        if (interrupt_flag_ != nullptr && interrupt_flag_->load()) {
            return false; // 打断:悬空收口
        }
        std::future_status status = future.wait_for(kPollSlice);
        if (status == std::future_status::ready) {
            out = future.get();
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false; // 超时:悬空收口
        }
    }
}

std::optional<runtime::ApprovalResponse> PendingFuture::WaitApproval() {
    if (approval_promise == nullptr) {
        return std::nullopt; // 形状不对(防御):按悬空收口
    }
    auto future = approval_promise->get_future();
    std::optional<runtime::ApprovalResponse> raw;
    if (!PollFuture(future, raw)) {
        return std::nullopt; // 打断/超时:悬空收口
    }
    return raw; // CancelPending 收的口(nullopt)原样透出
}

std::optional<runtime::QuestionResponse> PendingFuture::WaitQuestion() {
    if (question_promise == nullptr) {
        return std::nullopt;
    }
    auto future = question_promise->get_future();
    std::optional<runtime::QuestionResponse> answer;
    if (!PollFuture(future, answer)) {
        return std::nullopt;
    }
    return answer;
}

// ---------------------------------------------------------------------------
// InteractionLedger
// ---------------------------------------------------------------------------

namespace {

// 前端 result.decision 的四态字符串 -> InteractionDecision。认不得返回
// false(参数错,报 kErrInvalidParams)。协议面上的字符串
// (accept/acceptForSession/decline/cancel)与 runtime 内部字符串
// (accept/accept_for_session/decline/cancel)驼峰处不同,这里是唯一的
// 翻译点,别处不许散着抄。
bool ParseDecisionString(const std::string& s, runtime::InteractionDecision& out) {
    if (s == std::string(kDecisionAccept)) {
        out = runtime::InteractionDecision::Accept;
        return true;
    }
    if (s == std::string(kDecisionAcceptForSession)) {
        out = runtime::InteractionDecision::AcceptForSession;
        return true;
    }
    if (s == std::string(kDecisionDecline)) {
        out = runtime::InteractionDecision::Decline;
        return true;
    }
    if (s == std::string(kDecisionCancel)) {
        out = runtime::InteractionDecision::Cancel;
        return true;
    }
    return false;
}

// 审批反向请求的 params(协议底线第二节第 6 条:threadId/turnId/itemId、
// 工具名、参数摘要、风险说明)。input 摘要由前端自己决定怎么画,这里原样
// 给结构化 input,不预先翻文案(runtime 合同:结构化 input 是真值)。
nlohmann::json MakePermissionRequestParams(const std::string& thread_id, const std::string& turn_id,
                                           const std::string& request_id,
                                           const runtime::ApprovalRequest& request) {
    nlohmann::json params = nlohmann::json{
        {"threadId", thread_id},
        {"turnId", turn_id},
        {"requestId", request_id},
        {"tool", request.tool_name},
        {"input", request.input.is_null() ? nlohmann::json::object() : request.input},
    };
    // P4/P9 贯通:审批钉在哪枚条目上(tool_use_id),前端凭它把
    // permission/request 路由回条目;空 = 没有条目身份(旧调用方)。
    if (!request.tool_use_id.empty()) {
        params["toolUseId"] = request.tool_use_id;
    }
    if (!request.reason.empty()) {
        params["reason"] = request.reason;
    }
    return params;
}

// user/ask 反向请求的 params。
nlohmann::json MakeUserAskParams(const std::string& thread_id, const std::string& turn_id,
                                 const std::string& request_id, const runtime::QuestionRequest& request) {
    nlohmann::json options = nlohmann::json::array();
    for (const runtime::QuestionOption& option : request.options) {
        options.push_back(nlohmann::json{{"label", option.label}, {"description", option.description}});
    }
    nlohmann::json params = nlohmann::json{{"threadId", thread_id},
                                           {"turnId", turn_id},
                                           {"requestId", request_id},
                                           {"header", request.header},
                                           {"question", request.question},
                                           {"options", std::move(options)},
                                           {"multiSelect", request.multi_select}};
    return params;
}

}  // namespace

std::shared_ptr<runtime::InteractionFuture> InteractionLedger::AskApproval(
    const runtime::ApprovalRequest& request, const std::string& turn_id,
    const std::function<void(std::string_view method, const nlohmann::json& params)>& emit) {
    // request_id:P9 起统一走 runtime::ProcessIdAuthority(id_authority.hpp
    // 的 NextRequestId,前缀 req-),骨架期的回合内计数账拆掉。
    const std::string request_id = runtime::ProcessIdAuthority().NextRequestId();

    auto future = std::make_shared<PendingFuture>();
    future->approval_promise =
        std::make_shared<std::promise<std::optional<runtime::ApprovalResponse>>>();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry entry;
        entry.info = PendingInteraction{request_id, thread_id_, turn_id,
                                        std::string(kMethodPermissionRequest), request.tool_name,
                                        request.tool_use_id,
                                        PendingInteraction::Kind::Approval};
        entry.approval_promise = future->approval_promise;
        pending_[request_id] = std::move(entry);
    }
    if (emit) {
        emit(kMethodPermissionRequest, MakePermissionRequestParams(thread_id_, turn_id, request_id, request));
    }
    return future;
}

std::shared_ptr<PendingFuture> InteractionLedger::AskQuestion(
    const runtime::QuestionRequest& request, const std::string& turn_id,
    const std::function<void(std::string_view method, const nlohmann::json& params)>& emit) {
    // 提问与审批共用同一发号局(同一张 pending 表、同一套四态,单子原文);
    // 前缀同为 req-,前端不须按前缀分拣。
    const std::string request_id = runtime::ProcessIdAuthority().NextRequestId();

    auto future = std::make_shared<PendingFuture>();
    future->question_promise = std::make_shared<std::promise<std::optional<runtime::QuestionResponse>>>();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry entry;
        entry.info = PendingInteraction{request_id, thread_id_, turn_id, std::string(kMethodUserAsk),
                                        std::string(), std::string(),
                                        PendingInteraction::Kind::Question};
        entry.question_promise = future->question_promise;
        pending_[request_id] = std::move(entry);
    }
    if (emit) {
        emit(kMethodUserAsk, MakeUserAskParams(thread_id_, turn_id, request_id, request));
    }
    return future;
}

InteractionResolution InteractionLedger::HandleIncomingResponse(const IncomingResponse& response) {
    InteractionResolution result;
    // 响应信封不带字符串 id:pending 表按编号配对的路没有(响应 id 是前端
    // 给反向请求回的 request_id 字符串,藏在 result 里)。约定:反向请求
    // 的响应 id 就是 request_id 字符串;入站信封只认数字 id,所以这里
    // 约定 result.requestId 配对(见 schema.cpp 的 ParseIncoming:数字 id
    // 才算 Response;字符串 id 的报文折不成信封——前端按协议须把
    // requestId 放进 result,信封 id 给 0)。
    const nlohmann::json& body = response.result;
    if (!body.is_object() || !body.contains("requestId") || !body["requestId"].is_string()) {
        result.error_code = "invalid_params";
        result.error_message = "响应缺 requestId(反向请求的答复须带 requestId)";
        return result;
    }
    const std::string request_id = body["requestId"].get<std::string>();

    std::shared_ptr<std::promise<std::optional<runtime::ApprovalResponse>>> approval_promise;
    std::shared_ptr<std::promise<std::optional<runtime::QuestionResponse>>> question_promise;
    PendingInteraction::Kind kind = PendingInteraction::Kind::Approval;
    std::string tool_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(request_id);
        if (it == pending_.end()) {
            // 迟到/不认识:不等、不存、不崩。
            result.error_code = runtime::kStaleRequestId;
            result.error_message = "请求已收口或不存在: " + request_id;
            return result;
        }
        kind = it->second.info.kind;
        tool_name = it->second.info.tool_name;
        approval_promise = it->second.approval_promise;
        question_promise = it->second.question_promise;
        pending_.erase(it);
    }

    if (kind == PendingInteraction::Kind::Approval) {
        runtime::ApprovalResponse approval;
        if (response.is_error) {
            // 前端按错误响应回(cancel 的形状):按 cancel 收口。
            approval.decision = runtime::InteractionDecision::Cancel;
        } else if (body.contains("decision") && body["decision"].is_string()) {
            runtime::InteractionDecision decision = runtime::InteractionDecision::Decline;
            if (!ParseDecisionString(body["decision"].get<std::string>(), decision)) {
                result.error_code = "invalid_params";
                result.error_message = "认不得的决定四态: " + body["decision"].get<std::string>();
                // 报文坏了,但请求已从表里摘下——把 promise 收掉,免得工作
                // 线程空等(悬空收口语义)。
                if (approval_promise) {
                    approval_promise->set_value(std::nullopt);
                }
                return result;
            }
            approval.decision = decision;
            if (body.contains("reason") && body["reason"].is_string()) {
                approval.reason = body["reason"].get<std::string>();
            }
        } else {
            result.error_code = "invalid_params";
            result.error_message = "审批响应缺 decision 字段";
            if (approval_promise) {
                approval_promise->set_value(std::nullopt);
            }
            return result;
        }
        if (approval_promise) {
            approval_promise->set_value(approval);
        }
        // acceptForSession:当场落会话级放行账(后续同工具免问)。只写
        // 本会话内存,不落盘——"顺手写进 settings.local.json"要另发明确
        // 命令,不藏在审批回调里追问第二遍。
        if (approval.decision == runtime::InteractionDecision::AcceptForSession && !tool_name.empty()) {
            AllowForSession(tool_name);
        }
        result.ok = true;
        result.approval = approval;
        return result;
    }

    // 提问:answers 数组(空 answers + error 非空 = 取消/无法作答)。
    runtime::QuestionResponse question;
    if (response.is_error) {
        question.error = "前端按错误响应回(取消)";
    } else {
        if (body.contains("answers") && body["answers"].is_array()) {
            for (const auto& answer : body["answers"]) {
                if (answer.is_string()) {
                    question.answers.push_back(answer.get<std::string>());
                }
            }
        } else {
            result.error_code = "invalid_params";
            result.error_message = "提问响应缺 answers 数组";
            if (question_promise) {
                question_promise->set_value(std::nullopt);
            }
            return result;
        }
        if (body.contains("error") && body["error"].is_string()) {
            question.error = body["error"].get<std::string>();
        }
    }
    if (question_promise) {
        question_promise->set_value(question);
    }
    result.ok = true;
    result.question = question;
    return result;
}

std::size_t InteractionLedger::CancelPending() {
    std::vector<std::shared_ptr<std::promise<std::optional<runtime::ApprovalResponse>>>> approval_promises;
    std::vector<std::shared_ptr<std::promise<std::optional<runtime::QuestionResponse>>>> question_promises;
    std::size_t cleared = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cleared = pending_.size();
        for (auto& [id, entry] : pending_) {
            if (entry.approval_promise) {
                approval_promises.push_back(entry.approval_promise);
            }
            if (entry.question_promise) {
                question_promises.push_back(entry.question_promise);
            }
        }
        pending_.clear();
    }
    // 锁外收口(promise::set_value 不碰 ledger 状态,但保持"锁内不改
    // 状态、锁外发信号"的形状,免得将来回调化时把锁拖长)。
    for (auto& promise : approval_promises) {
        promise->set_value(std::nullopt);
    }
    for (auto& promise : question_promises) {
        promise->set_value(std::nullopt);
    }
    return cleared;
}

std::size_t InteractionLedger::CancelPendingForToolUse(const std::string& tool_use_id) {
    if (tool_use_id.empty()) {
        return 0;
    }
    std::vector<std::shared_ptr<std::promise<std::optional<runtime::ApprovalResponse>>>> approval_promises;
    std::vector<std::shared_ptr<std::promise<std::optional<runtime::QuestionResponse>>>> question_promises;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.info.tool_use_id == tool_use_id) {
                if (it->second.approval_promise) {
                    approval_promises.push_back(it->second.approval_promise);
                }
                if (it->second.question_promise) {
                    question_promises.push_back(it->second.question_promise);
                }
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& promise : approval_promises) {
        promise->set_value(std::nullopt);
    }
    for (auto& promise : question_promises) {
        promise->set_value(std::nullopt);
    }
    return approval_promises.size() + question_promises.size();
}

bool InteractionLedger::IsSessionAllowed(const std::string& tool_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_allowed_.count(tool_name) > 0;
}

void InteractionLedger::AllowForSession(const std::string& tool_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_allowed_[tool_name] = true;
}

std::size_t InteractionLedger::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

}  // namespace lubancode::app_server
