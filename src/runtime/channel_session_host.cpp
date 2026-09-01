// ChannelSessionHost 实现(多渠道消息接入单阶段 3)。合同见头文件。
#include "runtime/channel_session_host.hpp"

namespace lubancode::runtime {

bool ChannelConfirmAllows(const channel::ToolRoutePolicy& tools, const std::string& tool_name) {
    // fail closed 的根基(§16.1):渠道会话没有远端审批渠道,须确认的
    // 工具只有"binding allowlist 明确允许"一条生路。allow 名单没列 =
    // 没有任何明确允许 = 拒绝;进了 deny 更是拒。不为"机器人好用"把
    // confirm 偷换成 auto。
    for (const std::string& allowed : tools.allow) {
        if (allowed == tool_name) {
            for (const std::string& denied : tools.deny) {
                if (denied == tool_name) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

std::string ChannelToolDenialText(const std::string& tool_name) {
    return "渠道会话没有审批渠道,工具 " + tool_name + " 未在 binding allowlist 明确允许,已拒绝执行。"
           "如需放行,在全局 config 的渠道 binding 里显式加进 tools.allow。";
}

ChannelSessionHost::ChannelSessionHost(Options options) : options_(options) {
    if (options_.max_active_channel_turns == 0) {
        options_.max_active_channel_turns = 1;  // 0 不当"无限"解:最保守一槽
    }
}

void ChannelSessionHost::SetEngineFactory(EngineFactory factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    factory_ = std::move(factory);
}

ChannelSessionHost::SubmitResult ChannelSessionHost::Submit(TurnIngress ingress) {
    SubmitResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (factory_ == nullptr) {
        result.status = SubmitResult::Status::NoFactory;
        return result;
    }
    PendingTurn pending;
    pending.seq = next_seq_++;
    pending.ingress = std::move(ingress);
    pending_.push_back(std::move(pending));
    return result;
}

ChannelSessionHost::BeginRefusal ChannelSessionHost::TryBeginTurn(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto busy_it = busy_.find(session_key);
    if (busy_it != busy_.end() && busy_it->second) {
        return BeginRefusal::SingleFlight;
    }
    if (active_turns_ >= options_.max_active_channel_turns) {
        return BeginRefusal::Capacity;
    }
    busy_[session_key] = true;
    ++active_turns_;
    return BeginRefusal::None;
}

void ChannelSessionHost::EndTurn(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto busy_it = busy_.find(session_key);
    if (busy_it == busy_.end() || !busy_it->second) {
        return;  // 幂等:没占过闸不硬收
    }
    busy_it->second = false;
    if (active_turns_ > 0) {
        --active_turns_;
    }
}

std::optional<ChannelSessionHost::TurnOutcome> ChannelSessionHost::PumpOne() {
    // 1) 挑活:提交序扫一条"session 空闲且未超限额"的待办。
    std::optional<PendingTurn> picked;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (factory_ == nullptr) {
            return std::nullopt;
        }
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            const auto busy_it = busy_.find(it->ingress.session_key);
            const bool session_running = busy_it != busy_.end() && busy_it->second;
            if (session_running || active_turns_ >= options_.max_active_channel_turns) {
                continue;
            }
            picked = std::move(*it);
            pending_.erase(it);
            busy_[picked->ingress.session_key] = true;
            ++active_turns_;
            break;
        }
    }
    if (!picked.has_value()) {
        return std::nullopt;
    }

    // 2) 取/建引擎(session_key 缓存:同会话复用同一场 history)。
    ChannelTurnEngine* engine = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = engines_.find(picked->ingress.session_key);
        if (found == engines_.end()) {
            auto created = factory_(picked->ingress.session_key);
            if (created == nullptr) {
                // 工厂拒建:就地收闸(持锁态,手动收,不调 EndTurn 防递归锁)。
                busy_[picked->ingress.session_key] = false;
                if (active_turns_ > 0) {
                    --active_turns_;
                }
                TurnOutcome outcome;
                outcome.session_key = picked->ingress.session_key;
                outcome.ingress_delivery_id = picked->ingress.ingress_delivery_id;
                outcome.error = "engine factory returned null";
                return outcome;
            }
            engine = created.get();
            engines_.emplace(picked->ingress.session_key, std::move(created));
        } else {
            engine = found->second.get();
        }
    }

    // 3) 跑轮(锁外:engine 同步跑,单飞闸已占)。引擎回 RunOutcome 值,
    // 失败摘要在 error 出参里(接口不是 expected——渠道轮的失败也收场,
    // 不往上抛异常形状)。
    TurnOutcome outcome;
    outcome.session_key = picked->ingress.session_key;
    outcome.ingress_delivery_id = picked->ingress.ingress_delivery_id;
    std::string reply_text;
    std::string error;
    const agent::RunOutcome run = engine->RunTurn(picked->ingress, &reply_text, &error);
    EndTurn(outcome.session_key);
    outcome.reply_text = std::move(reply_text);
    outcome.cancelled = run.cancelled;
    outcome.ok = error.empty();
    outcome.error = std::move(error);
    return outcome;
}

std::size_t ChannelSessionHost::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

std::size_t ChannelSessionHost::pending_count_for(const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const PendingTurn& pending : pending_) {
        if (pending.ingress.session_key == session_key) {
            ++count;
        }
    }
    return count;
}

std::size_t ChannelSessionHost::active_turn_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_turns_;
}

bool ChannelSessionHost::session_busy(const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto busy_it = busy_.find(session_key);
    return busy_it != busy_.end() && busy_it->second;
}

std::size_t ChannelSessionHost::engine_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return engines_.size();
}

}  // namespace lubancode::runtime
