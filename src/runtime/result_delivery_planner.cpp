// result_delivery_planner.hpp 的实现。写账纪律:持 writer_mutex(会话
// 共享写者);配对链走 ReopenAligned 只补 selected+tool 消息,不执行、
// 不造 pending、不改 attempt(失败与恢复单 P1-A 恢复侧把手的合同)。
#include "runtime/result_delivery_planner.hpp"

#include <algorithm>
#include <utility>

#include "platform/log_sink.hpp"

namespace lubancode::runtime {
namespace {

using trajectory::v3::Durability;
using trajectory::v3::EventDraft;
using trajectory::v3::EventKindV3;
using trajectory::v3::WriteReceipt;
// Terminal 是 ToolActionSession 的嵌套枚举,不是命名空间成员——别名引入。
using Terminal = trajectory::v3::ToolActionSession::Terminal;

trajectory::v3::ToolActionSession ReopenFor(const CompletionNotice& notice) {
    Terminal terminal = Terminal::None;
    switch (notice.terminal_kind) {
        case 1: terminal = Terminal::Finished; break;
        case 2: terminal = Terminal::Failed; break;
        case 3: terminal = Terminal::Cancelled; break;
        case 4: terminal = Terminal::Rejected; break;
        case 5: terminal = Terminal::Unknown; break;
        default: terminal = Terminal::None; break;
    }
    return trajectory::v3::ToolActionSession::ReopenAligned(
        notice.turn_id, notice.step_id, notice.action_id,
        notice.attempt > 0 ? notice.attempt : 2, true, terminal);
}

// 配对正文:成功的业务预览;失败的失败报告(不冒充成功,也不吞结果)。
std::string PairingContent(const CompletionNotice& notice) {
    if (notice.failed) {
        return nlohmann::json::object({{"jobId", notice.job_id},
                                       {"status", "failed"},
                                       {"failure", notice.failure.empty() ? "job_failed" : notice.failure}})
            .dump();
    }
    return notice.preview;
}

}  // namespace

ResultDeliveryPlannerImpl::ResultDeliveryPlannerImpl(Hooks hooks) : hooks_(std::move(hooks)) {}

ResultDeliveryPlannerImpl::~ResultDeliveryPlannerImpl() = default;

void ResultDeliveryPlannerImpl::NotifyCompletion(CompletionNotice notice) {
    if (notice.job_id.empty() || notice.action_id.empty()) {
        return;  // 无身份的通知不人账(信封隔离在协调器侧拒)
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const CompletionNotice& existing : mailbox_) {
        if (existing.job_id == notice.job_id &&
            existing.result_version >= notice.result_version) {
            return;  // 重复/乱序信封:按 result_version 去重(只认最新)
        }
    }
    for (const PendingDelivery& delivery : in_flight_) {
        if (delivery.notice.job_id == notice.job_id && !delivery.settled) {
            return;  // 已在投递中,迟到通知不重复投
        }
    }
    if (notice.commit_ordinal == 0) {
        notice.commit_ordinal = next_ordinal_++;
    }
    mailbox_.push_back(std::move(notice));
}

std::vector<api::Message> ResultDeliveryPlannerImpl::SelectForRequestBoundary() {
    std::vector<api::Message> delivered;
    // 选取面(P2):native_deferred 欠账,按稳定提交次序(通知到达序,
    // 同序按 job_id——NotifyCompletion 已按到达追加)。分支过滤:通知的
    // branch 须落在本会话(单 §7 待投递结果不跨目标分支)。
    std::lock_guard<std::mutex> lock(mutex_);
    if (hooks_.writer == nullptr || hooks_.writer_mutex == nullptr) {
        return delivered;
    }
    const std::string session_branch = hooks_.writer->session_id();
    std::vector<std::size_t> picked;
    for (std::size_t i = 0; i < mailbox_.size(); ++i) {
        const CompletionNotice& notice = mailbox_[i];
        if (notice.mode != "native_deferred") {
            continue;  // job_handle:模型经 get/wait 自取(P4 才接 notify)
        }
        if (!notice.branch.empty() && session_branch.empty() == false &&
            notice.branch != session_branch) {
            continue;  // 不跨目标分支
        }
        picked.push_back(i);
    }
    if (picked.empty()) {
        return delivered;
    }
    // 锁内落账(writer 共享写者;配对链 + 接纳一手包)。
    std::unique_lock<std::recursive_mutex> writer_lock(*hooks_.writer_mutex);
    std::string current_turn = hooks_.current_turn_id ? hooks_.current_turn_id() : std::string();
    for (std::size_t index : picked) {
        CompletionNotice notice = mailbox_[index];
        if (notice.turn_id.empty()) {
            notice.turn_id = current_turn;
        }
        const std::string content = PairingContent(notice);
        const std::string delivery_id =
            "delivery-" + notice.job_id + "-v" + std::to_string(notice.result_version);
        // 配对链:selected + tool 消息 + 接纳(ReopenAligned 只补账)。
        auto session = ReopenFor(notice);
        const std::string selected_event = [&]() -> std::string {
            const auto selected = session.SelectResult(
                *hooks_.writer, {notice.result_ref}, {}, notice.failed ? "failed" : "done",
                notice.attempt, Durability::PowerLoss);
            if (selected.status != WriteReceipt::Status::Committed) {
                platform::LogSink::Instance().Error(
                    "delivery", "tool.result.selected(配对) 落账失败: " + selected.error_code);
                return std::string();
            }
            const auto message = session.AppendToolMessage(
                *hooks_.writer, content, selected.id, notice.failed, Durability::PowerLoss);
            if (message.status != WriteReceipt::Status::Committed) {
                platform::LogSink::Instance().Error(
                    "delivery", "配对 tool 消息落账失败: " + message.error_code);
                return std::string();
            }
            return selected.id;
        }();
        if (selected_event.empty()) {
            continue;  // 链没立起来:通知留在 mailbox,下次边界再试
        }
        PendingDelivery delivery;
        delivery.delivery_id = delivery_id;
        delivery.notice = notice;
        delivery.message_written = true;
        in_flight_.push_back(std::move(delivery));
        api::Message pairing;
        pairing.role = api::Role::User;
        pairing.content.push_back(
            api::ToolResultBlock{notice.provider_call_id, content, notice.failed});
        delivered.push_back(std::move(pairing));
        mailbox_[index] = CompletionNotice{};  // 已选中,出 mailbox
    }
    // 出列的清掉(选中失败的留在原位重试)。
    mailbox_.erase(std::remove_if(mailbox_.begin(), mailbox_.end(),
                                  [](const CompletionNotice& notice) { return notice.job_id.empty(); }),
                   mailbox_.end());
    return delivered;
}

void ResultDeliveryPlannerImpl::NoteRequestPrepared(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hooks_.writer == nullptr || hooks_.writer_mutex == nullptr || request_id.empty()) {
        return;
    }
    std::unique_lock<std::recursive_mutex> writer_lock(*hooks_.writer_mutex);
    for (PendingDelivery& delivery : in_flight_) {
        if (delivery.settled || delivery.prepared_written || !delivery.message_written) {
            continue;
        }
        EventDraft draft;
        draft.kind = EventKindV3::ToolDeliveryPrepared;
        draft.turn_id = delivery.notice.turn_id;
        draft.step_id = delivery.notice.step_id;
        draft.action_id = delivery.notice.action_id;
        draft.request_id = request_id;
        draft.payload = nlohmann::json::object({
            {"tool_call_id", delivery.notice.action_id},
            {"deliveryId", delivery.delivery_id},
            {"resultRef", delivery.notice.result_ref},
            {"resultVersion", delivery.notice.result_version},
        });
        const auto receipt = hooks_.writer->AppendEvent(std::move(draft), Durability::ProcessCrash);
        if (receipt.status != WriteReceipt::Status::Committed) {
            platform::LogSink::Instance().Error(
                "delivery", "tool.delivery.prepared 落账失败: " + receipt.error_code);
            continue;
        }
        delivery.prepared_written = true;
        delivery.request_id = request_id;
    }
}

void ResultDeliveryPlannerImpl::NoteResponseOutcome(const std::string& request_id,
                                                    bool acknowledged) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hooks_.writer == nullptr || hooks_.writer_mutex == nullptr || request_id.empty()) {
        return;
    }
    std::unique_lock<std::recursive_mutex> writer_lock(*hooks_.writer_mutex);
    for (PendingDelivery& delivery : in_flight_) {
        if (delivery.settled || delivery.request_id != request_id) {
            continue;
        }
        EventDraft draft;
        if (acknowledged) {
            std::optional<std::string> evidence;
            if (hooks_.response_evidence) {
                evidence = hooks_.response_evidence(request_id);
            }
            if (!evidence.has_value()) {
                // 没证据不宣称接纳(单 §5):回执丢失按 uncertain 落账。
                draft.kind = EventKindV3::ToolDeliveryUncertain;
                draft.payload = nlohmann::json::object(
                    {{"tool_call_id", delivery.notice.action_id},
                     {"deliveryId", delivery.delivery_id},
                     {"reason", "response_evidence_unavailable"}});
            } else {
                draft.kind = EventKindV3::ToolDeliveryAcknowledged;
                draft.payload = nlohmann::json::object(
                    {{"tool_call_id", delivery.notice.action_id},
                     {"deliveryId", delivery.delivery_id},
                     {"evidenceRef", *evidence}});
            }
        } else {
            draft.kind = EventKindV3::ToolDeliveryUncertain;
            draft.payload = nlohmann::json::object(
                {{"tool_call_id", delivery.notice.action_id},
                 {"deliveryId", delivery.delivery_id},
                 {"reason", "response_receipt_lost"}});
        }
        draft.turn_id = delivery.notice.turn_id;
        draft.step_id = delivery.notice.step_id;
        draft.action_id = delivery.notice.action_id;
        draft.request_id = request_id;
        const auto receipt = hooks_.writer->AppendEvent(std::move(draft), Durability::ProcessCrash);
        if (receipt.status != WriteReceipt::Status::Committed) {
            platform::LogSink::Instance().Error(
                "delivery", "投递收口事件落账失败: " + receipt.error_code);
            continue;  // 不置 settled:下次收口再补(至少一次)
        }
        delivery.settled = true;
    }
    // 已收口的出列(未收口的留着,迟到证据解除 uncertain 归读取侧)。
    in_flight_.erase(std::remove_if(in_flight_.begin(), in_flight_.end(),
                                    [](const PendingDelivery& delivery) { return delivery.settled; }),
                     in_flight_.end());
}

std::size_t ResultDeliveryPlannerImpl::RestoreFromLedger(const trajectory::v3::V3Ledger& ledger) {
    // 单 §6"原文已落仓、tool 消息未提交→补投递不重跑":终态已落、义务
    // 未配的 native 欠账重建进 mailbox——同一套配对路补消息,不重跑工作。
    const auto jobs = trajectory::v3::FoldJobExecutions(ledger);
    const auto actions = trajectory::v3::FoldToolActions(ledger);
    const auto obligations = trajectory::v3::ProjectProtocolObligations(ledger);
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t restored = 0;
    for (const auto& job : jobs) {
        if (job.mode != "native_deferred") {
            continue;
        }
        const std::string& action_id = job.origin_action_id;
        const trajectory::v3::ToolActionSnapshot* snapshot =
            trajectory::v3::FindActionSnapshot(actions, action_id);
        const trajectory::v3::ProtocolObligationView* obligation =
            trajectory::v3::FindProtocolObligation(obligations, action_id);
        if (snapshot == nullptr || obligation == nullptr) {
            continue;
        }
        // 已配齐(tool 消息在链上)的不投;无终态观测的不投(归恢复
        // disposition unknown_hold,不合成假终态)。
        if (obligation->paired) {
            continue;
        }
        if (job.state != "succeeded" && job.state != "failed" && job.state != "cancelled") {
            continue;
        }
        if (!job.observed_result_ref.has_value() || snapshot->persisted_event_refs.empty()) {
            continue;  // 原文没落仓:补不了投递(缺账如实留缺口)
        }
        CompletionNotice notice;
        notice.job_id = job.job_id;
        notice.action_id = action_id;
        notice.mode = "native_deferred";
        notice.provider_call_id = obligation->provider_call_id.value_or("");
        notice.turn_id = job.event_ids.empty() ? snapshot->turn_id : snapshot->turn_id;
        notice.step_id = snapshot->step_id;
        notice.result_ref = *job.observed_result_ref;
        notice.result_version = job.observed_result_version;
        notice.failed = job.state == "failed" || job.state == "cancelled";
        notice.failure = job.state == "cancelled" ? "job_cancelled" : "job_failed";
        if (!snapshot->attempts.empty()) {
            const auto& attempt_view = snapshot->attempts.back();
            notice.attempt = attempt_view.attempt;
            if (attempt_view.status == "done") {
                notice.terminal_kind = 1;
            } else if (attempt_view.status == "failed") {
                notice.terminal_kind = 2;
            } else if (attempt_view.status == "cancelled") {
                notice.terminal_kind = 3;
            } else if (attempt_view.status == "rejected") {
                notice.terminal_kind = 4;
            } else {
                notice.terminal_kind = 5;
            }
        }
        notice.commit_ordinal = next_ordinal_++;
        notice.branch = ledger.session_id;
        // 预览从账面取不到原文仓(结果仓 artifact 另追),配对正文用
        // resultRef 引用说明——补投递的正文完整性如实,不冒充原文。
        notice.preview = nlohmann::json::object({{"jobId", job.job_id},
                                                 {"status", job.state},
                                                 {"resultRef", notice.result_ref},
                                                 {"resultVersion", notice.result_version},
                                                 {"note", "restored_from_ledger"}})
                             .dump();
        // mailbox 去重(同 job 已在列不重复入)。
        const bool already = std::any_of(mailbox_.begin(), mailbox_.end(),
                                         [&notice](const CompletionNotice& existing) {
                                             return existing.job_id == notice.job_id;
                                         });
        if (already) {
            continue;
        }
        mailbox_.push_back(std::move(notice));
        ++restored;
    }
    return restored;
}

std::size_t ResultDeliveryPlannerImpl::pending_native_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& notice : mailbox_) {
        if (notice.mode == "native_deferred") {
            ++count;
        }
    }
    return count;
}

std::size_t ResultDeliveryPlannerImpl::mailbox_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mailbox_.size();
}

}  // namespace lubancode::runtime
