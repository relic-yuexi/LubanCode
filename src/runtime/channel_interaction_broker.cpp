// ChannelInteractionBroker 实现(合同见头文件)。
#include "runtime/channel_interaction_broker.hpp"

#include <chrono>
#include <future>
#include <random>
#include <sstream>

#include "platform/sha256.hpp"

namespace lubancode::runtime {

namespace {

// 分片轮询周期:审批是人工节奏,50ms 的反应间隔毫无压力(助理 Web 的
// AssistantApprovalBroker 同款粒度),不造额外线程。
constexpr auto kPollSlice = std::chrono::milliseconds(50);

std::string Sha256HexOfString(const std::string& text) {
    return platform::Sha256Hex(text);
}

}  // namespace

// ---------------------------------------------------------------------------
// ChannelApprovalFuture
// ---------------------------------------------------------------------------

std::optional<ApprovalResponse> ChannelApprovalFuture::WaitApproval() {
    if (promise == nullptr) {
        return std::nullopt;  // 形状不对(防御):按悬空收口
    }
    auto future = promise->get_future();
    const auto deadline =
        timeout_ > std::chrono::milliseconds(0)
            ? std::chrono::steady_clock::now() + timeout_
            : std::chrono::steady_clock::time_point::max();
    while (true) {
        if (interrupt_flag_ != nullptr && interrupt_flag_->load()) {
            return std::nullopt;  // 打断(turn 取消/关停):悬空收口
        }
        if (future.wait_for(kPollSlice) == std::future_status::ready) {
            return future.get();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;  // 超时:悬空收口(默认拒绝不默认放行)
        }
    }
}

// ---------------------------------------------------------------------------
// 按钮 data 编解码
// ---------------------------------------------------------------------------

std::string EncodeApprovalButtonData(const std::string& token, bool accept) {
    return "qai:" + token + ":" + (accept ? "1" : "0");
}

bool DecodeApprovalButtonData(const std::string& button_data, std::string* token, bool* accept) {
    // 形状 "qai:<token>:<1|0>":前缀不认/尾码不认 = 不是我们的按钮(菜单
    // 类互动不走这里),返回 false 不唤醒任何 future。
    const std::string prefix = "qai:";
    if (button_data.size() <= prefix.size() + 2 ||
        button_data.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    const std::size_t last = button_data.rfind(':');
    if (last == std::string::npos || last + 1 >= button_data.size()) {
        return false;
    }
    const std::string decision = button_data.substr(last + 1);
    if (decision != "1" && decision != "0") {
        return false;
    }
    const std::string candidate = button_data.substr(prefix.size(), last - prefix.size());
    if (candidate.empty() || candidate.find(':') != std::string::npos) {
        return false;
    }
    if (token != nullptr) {
        *token = candidate;
    }
    if (accept != nullptr) {
        *accept = decision == "1";
    }
    return true;
}

std::string ChannelApprovalTokenHash(const std::string& token) {
    return Sha256HexOfString(token);
}

// ---------------------------------------------------------------------------
// ChannelInteractionBroker
// ---------------------------------------------------------------------------

std::string ChannelInteractionBroker::NewToken() {
    // random_device 播种的 mt19937_64(与配对 code 同款威胁模型:别猜得中;
    // 防御的是"猜 token 代批",不是无限算力)。计数器掺和防同毫秒碰撞
    //(计数器在 mutex_ 内递增)。
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    std::ostringstream out;
    out << std::hex << engine() << engine() << "-" << token_counter_;
    return out.str();
}

std::shared_ptr<ChannelApprovalFuture> ChannelInteractionBroker::AskApproval(
    const ChannelApprovalContext& context,
    const std::function<void(const RequestedFact&)>& on_requested) {
    auto future = std::make_shared<ChannelApprovalFuture>();
    future->promise = std::make_shared<std::promise<std::optional<ApprovalResponse>>>();

    RequestedFact fact;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++token_counter_;
        fact.token = NewToken();
        fact.token_hash = ChannelApprovalTokenHash(fact.token);
        fact.context = context;
        Entry entry;
        entry.fact = fact;
        entry.promise = future->promise;
        pending_[fact.token] = std::move(entry);
        requested_log_.push_back(fact);
    }
    // 期限线:窗口由调用方直接递(毫秒),等待侧按 steady_clock 计——
    // 不与墙钟/调用方时钟混算,墙钟跳变不影响审批窗。
    future->SetTimeout(std::chrono::milliseconds(context.timeout_ms > 0 ? context.timeout_ms : 0));
    if (on_requested) {
        on_requested(fact);  // 泵在回调里发审批卡(入交互 outbox)
    }
    return future;
}

ChannelInteractionBroker::Resolution ChannelInteractionBroker::ResolveByToken(
    const std::string& token, bool accept, const std::string& operator_id,
    const std::string& interaction_id) {
    std::shared_ptr<std::promise<std::optional<ApprovalResponse>>> promise;
    ResolvedFact fact;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(token);
        if (it == pending_.end()) {
            const auto seen = recently_resolved_.find(token);
            if (seen != recently_resolved_.end()) {
                // 同 token 重复回调(用户连点/平台重投):幂等——只返回
                // 已处理,不再 resolve(promise 已收口,重复裁决无意义)。
                return Resolution::Duplicate;
            }
            return Resolution::Stale;  // 未知/过期(重启后/跨账号/篡改)
        }
        // 身份复核(§12.2:批准者须属于该请求的授权集合):配对 sender 是
        // 唯一授权人;他人代按 NotAuthorized,请求仍挂(本人还能按)。
        if (operator_id != it->second.fact.context.operator_id) {
            return Resolution::NotAuthorized;
        }
        promise = it->second.promise;
        fact.token_hash = it->second.fact.token_hash;
        fact.turn_key = it->second.fact.context.turn_key;
        fact.by = operator_id;
        fact.interaction_id = interaction_id;
        fact.outcome = accept ? Outcome::Approved : Outcome::Declined;
        fact.resolved_at_ms = NowMs();
        pending_.erase(it);
        resolved_log_.push_back(fact);
        recently_resolved_[token] = accept;
        if (recently_resolved_.size() > 256) {
            recently_resolved_.erase(recently_resolved_.begin());
        }
    }
    if (promise != nullptr) {
        ApprovalResponse response;
        response.decision = accept ? InteractionDecision::Accept : InteractionDecision::Decline;
        promise->set_value(response);
    }
    return Resolution::Applied;
}

void ChannelInteractionBroker::CancelByToken(const std::string& token, const std::string& reason) {
    std::shared_ptr<std::promise<std::optional<ApprovalResponse>>> promise;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(token);
        if (it == pending_.end()) {
            return;  // 未知 token:静默(防御,不记账)
        }
        promise = it->second.promise;
        ResolvedFact fact;
        fact.token_hash = it->second.fact.token_hash;
        fact.turn_key = it->second.fact.context.turn_key;
        fact.by = reason.empty() ? std::string("cancelled") : reason;
        fact.outcome = Outcome::Cancelled;
        fact.resolved_at_ms = NowMs();
        resolved_log_.push_back(std::move(fact));
        pending_.erase(it);
    }
    if (promise != nullptr) {
        promise->set_value(std::nullopt);  // 悬空收口
    }
}

bool ChannelInteractionBroker::IsPending(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.count(token) > 0;
}

void ChannelInteractionBroker::NoteTimeout(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pending_.find(token);
    if (it == pending_.end()) {
        return;
    }
    ResolvedFact fact;
    fact.token_hash = it->second.fact.token_hash;
    fact.turn_key = it->second.fact.context.turn_key;
    fact.by = "timeout";
    fact.outcome = Outcome::Timeout;
    fact.resolved_at_ms = NowMs();
    resolved_log_.push_back(std::move(fact));
    pending_.erase(it);
    recently_resolved_[token] = false;
    if (recently_resolved_.size() > 256) {
        recently_resolved_.erase(recently_resolved_.begin());
    }
}

void ChannelInteractionBroker::TakeFactsForTurn(const std::string& turn_key,
                                                std::vector<RequestedFact>* requested,
                                                std::vector<ResolvedFact>* resolved) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (requested != nullptr) {
        for (auto it = requested_log_.begin(); it != requested_log_.end();) {
            if (it->context.turn_key == turn_key) {
                requested->push_back(std::move(*it));
                it = requested_log_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (resolved != nullptr) {
        for (auto it = resolved_log_.begin(); it != resolved_log_.end();) {
            if (it->turn_key == turn_key) {
                resolved->push_back(std::move(*it));
                it = resolved_log_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

std::vector<ChannelInteractionBroker::ResolvedFact>
ChannelInteractionBroker::DrainResolved() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ResolvedFact> out = std::move(resolved_log_);
    resolved_log_.clear();
    return out;
}

std::vector<ChannelInteractionBroker::RequestedFact>
ChannelInteractionBroker::DrainRequested() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RequestedFact> out = std::move(requested_log_);
    requested_log_.clear();
    return out;
}

std::size_t ChannelInteractionBroker::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

}  // namespace lubancode::runtime
