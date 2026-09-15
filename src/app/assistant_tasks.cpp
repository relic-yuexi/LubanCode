// assistant_tasks.hpp 的实现。
#include "app/assistant_tasks.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_server/dispatcher.hpp"
#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "gateway/process.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "runtime/headless_executor.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/session_lock.hpp"

namespace lubancode::app {

namespace {

std::int64_t WallClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 入参读字(缺键/类型不对给缺省;严格校验由各 handler 自管)。
std::string ReadJsonString(const nlohmann::json& params, const char* key) {
    if (params.is_object() && params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return std::string();
}

std::int64_t ReadJsonInt(const nlohmann::json& params, const char* key) {
    if (params.is_object() && params.contains(key) && params[key].is_number_integer()) {
        return params[key].get<std::int64_t>();
    }
    return 0;
}

// 结果正文帽:task/read 的 replyText 截到 64KB(长结果走发布文件)。
constexpr std::size_t kResultTextCapBytes = 64 * 1024;
// 输入预览帽(审批事件里给页面看的 input 摘要)。
constexpr std::size_t kInputPreviewCapBytes = 1024;
// 命令回执轮询:有界等待(泵可能正被一枚在飞执行占住——那是串行泵
// 的语义,如实等;等不到就报错,幂等键在,客户端重发不双建)。
constexpr std::int64_t kReceiptWaitMs = 15 * 1000;

// 有界读文本(帽到 cap;读不到回 nullopt)。
std::optional<std::string> ReadTextBounded(const std::filesystem::path& path, std::size_t cap) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (text.size() > cap) {
        text.resize(cap);
        text += "\n…(超过 64KB,已截断;完整结果见发布文件)";
    }
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// AssistantEventHub
// ---------------------------------------------------------------------------

AssistantEventHub::AssistantEventHub(std::string boot_id, std::size_t capacity)
    : boot_id_(std::move(boot_id)), capacity_(capacity == 0 ? 1 : capacity) {}

void AssistantEventHub::set_sink(
    std::function<void(const std::string& method, const nlohmann::json& params)> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

std::uint64_t AssistantEventHub::Push(std::string method, nlohmann::json params) {
    std::function<void(const std::string&, const nlohmann::json&)> sink;
    std::uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seq = next_seq_++;
        // params 带上游标本账的 seq:活连接收到即可推进游标,断线重连
        // 的补账(events/read)从游标后续,不重发。
        if (params.is_object()) {
            params["seq"] = seq;
        }
        entries_.push_back(Entry{seq, method, params});
        while (entries_.size() > capacity_) {
            entries_.pop_front();
        }
        sink = sink_;
    }
    if (sink) {
        // 锁外推:出口自己线程安全(server 的 EmitHostEvent 快照活连接),
        // 慢连接不吊泵线程在事件账的锁上。
        sink(method, params);
    }
    return seq;
}

std::uint64_t AssistantEventHub::current_seq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_seq_ - 1;
}

AssistantEventHub::ReadResult AssistantEventHub::Read(const std::string& boot_id,
                                                      std::uint64_t last_seq,
                                                      std::size_t max_events) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ReadResult result;
    result.boot_id = boot_id_;
    result.current_seq = next_seq_ - 1;
    result.oldest_seq = entries_.empty() ? 0 : entries_.front().seq;
    const bool boot_mismatch = boot_id != boot_id_;
    // 缺口判定:last_seq+1 已被帽挤出(或压根没见过这个 boot 的账)——
    // 增量续不上了,快照重读。
    const bool gap_evicted =
        !entries_.empty() && last_seq + 1 < entries_.front().seq;
    result.reset = boot_mismatch || gap_evicted;
    if (result.reset) {
        // 回最近 max_events 枚(全量重读的起点材料)。
        const std::size_t from = entries_.size() > max_events ? entries_.size() - max_events : 0;
        for (std::size_t i = from; i < entries_.size(); ++i) {
            result.events.push_back(entries_[i]);
        }
        return result;
    }
    for (const Entry& entry : entries_) {
        if (entry.seq > last_seq) {
            result.events.push_back(entry);
            if (result.events.size() >= max_events) {
                break;
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// AssistantApprovalBroker
// ---------------------------------------------------------------------------

AssistantApprovalBroker::AssistantApprovalBroker(AssistantEventHub* hub,
                                                 std::int64_t default_timeout_ms)
    : hub_(hub), default_timeout_ms_(default_timeout_ms > 0 ? default_timeout_ms : 120 * 1000) {}

std::pair<bool, std::string> AssistantApprovalBroker::Ask(
    const std::string& job_id, const std::string& occurrence_id, const std::string& tool_use_id,
    const std::string& tool_name, const nlohmann::json& input) {
    // 归属上下文:显式给了用显式的;委托路径(泵的确认回调)经
    // context_provider 取当前在飞 occurrence。
    std::string effective_job = job_id;
    std::string effective_occurrence = occurrence_id;
    if (effective_job.empty() && context_provider_) {
        const auto context = context_provider_();
        effective_job = context.first;
        effective_occurrence = context.second;
    }
    Pending pending;
    pending.job_id = effective_job;
    pending.occurrence_id = effective_occurrence;
    pending.tool_name = tool_name;
    pending.input = input;
    pending.created_at_ms = WallClockMs();
    pending.timeout_ms = default_timeout_ms_;
    pending.deadline_wall_ms = pending.created_at_ms + pending.timeout_ms;
    pending.answer = std::make_shared<std::promise<std::optional<bool>>>();
    pending.cancelled = std::make_shared<std::atomic<bool>>(false);
    auto future = pending.answer->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.request_id = "appro-" + std::to_string(next_request_++);
        pending_[pending.request_id] = pending;
    }
    // 事件进账(重连补账面能找回)+ 推活连接(must_keep 由 EventMustKeep
    // 表钉)。input 预览截断——审批输入可能带大正文。
    nlohmann::json request_params;
    request_params["requestId"] = pending.request_id;
    request_params["jobId"] = effective_job;
    request_params["occurrenceId"] = effective_occurrence;
    request_params["toolUseId"] = tool_use_id;
    request_params["toolName"] = tool_name;
    std::string preview = pending.input.is_null() ? std::string("{}") : pending.input.dump();
    if (preview.size() > kInputPreviewCapBytes) {
        preview.resize(kInputPreviewCapBytes);
        preview += "…(截断)";
    }
    request_params["inputPreview"] = preview;
    request_params["timeoutMs"] = pending.timeout_ms;
    request_params["createdAtMs"] = pending.created_at_ms;
    hub_->Push("assistant/approval/request", std::move(request_params));

    // 分片等 promise:50ms 醒一次查 deadline(审批是人工节奏,粒度无压;
    // 不造额外线程)。有答复按答复;超时按拒绝收口(默认拒绝不默认
    // 放行);CancelAll 的收口旗按"助理收口"拒绝。
    std::optional<bool> answer;
    while (true) {
        if (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready) {
            answer = future.get();
            break;
        }
        if (pending.cancelled->load()) {
            break;  // 收口:answer 保持 nullopt
        }
        if (WallClockMs() >= pending.deadline_wall_ms) {
            break;  // 超时:answer 保持 nullopt
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(pending.request_id);
    }
    const char* outcome = nullptr;
    bool allowed = false;
    std::string denial;
    if (answer.has_value() && *answer) {
        allowed = true;
        outcome = "accepted";
    } else if (answer.has_value()) {
        outcome = "declined";
        denial = "用户拒绝执行该工具";
    } else if (pending.cancelled->load()) {
        outcome = "cancelled";
        denial = "助理收口,审批悬空收口,未执行该工具";
    } else {
        outcome = "timeout_declined";
        denial = "审批超时(" + std::to_string(pending.timeout_ms / 1000) +
                 " 秒没人答复),按拒绝收口,未执行该工具";
    }
    nlohmann::json resolved_params;
    resolved_params["requestId"] = pending.request_id;
    resolved_params["outcome"] = outcome;
    hub_->Push("assistant/approval/resolved", std::move(resolved_params));
    return {allowed, denial};
}

AssistantApprovalBroker::RespondOutcome AssistantApprovalBroker::Respond(const std::string& request_id,
                                                                         bool accept) {
    std::shared_ptr<std::promise<std::optional<bool>>> answer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = pending_.find(request_id);
        if (found == pending_.end()) {
            return RespondOutcome{false, "stale_request_id"};
        }
        answer = found->second.answer;
        pending_.erase(found);
    }
    answer->set_value(accept);
    return RespondOutcome{true, std::string()};
}

std::vector<nlohmann::json> AssistantApprovalBroker::ListPending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> out;
    out.reserve(pending_.size());
    for (const auto& [id, pending] : pending_) {
        nlohmann::json item;
        item["requestId"] = id;
        item["jobId"] = pending.job_id;
        item["occurrenceId"] = pending.occurrence_id;
        item["toolName"] = pending.tool_name;
        std::string preview = pending.input.is_null() ? std::string("{}") : pending.input.dump();
        if (preview.size() > kInputPreviewCapBytes) {
            preview.resize(kInputPreviewCapBytes);
            preview += "…(截断)";
        }
        item["inputPreview"] = preview;
        item["timeoutMs"] = pending.timeout_ms;
        item["createdAtMs"] = pending.created_at_ms;
        item["expiresAtMs"] = pending.deadline_wall_ms;
        out.push_back(std::move(item));
    }
    return out;
}

void AssistantApprovalBroker::CancelAll(const std::string& reason) {
    std::vector<std::shared_ptr<std::atomic<bool>>> flags;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        flags.reserve(pending_.size());
        for (auto& [id, pending] : pending_) {
            flags.push_back(pending.cancelled);
        }
        // 表不清空:Ask 侧的轮询会因 cancelled 旗醒来,自己摘表、发
        // resolved 事件(outcome=cancelled),账面只走一条路。
    }
    for (const auto& flag : flags) {
        flag->store(true);
    }
    (void)reason;
}

std::size_t AssistantApprovalBroker::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

// ---------------------------------------------------------------------------
// AssistantAutomationFace
// ---------------------------------------------------------------------------

AssistantAutomationFace::AssistantAutomationFace(gateway::GatewayProfilePaths paths,
                                                 AssistantEventHub* hub,
                                                 AssistantApprovalBroker* broker)
    : paths_(std::move(paths)), hub_(hub), broker_(broker) {}

bool AssistantAutomationFace::WaitForCreateReceipt(const std::string& idempotency_key,
                                                    std::string* out_job_id,
                                                    std::string* out_occurrence_id,
                                                    std::uint64_t* out_revision) {
    const std::int64_t deadline = WallClockMs() + kReceiptWaitMs;
    while (WallClockMs() < deadline) {
        const gateway::AutomationProjection projection =
            gateway::ReadAutomationProjection(paths_.automation_log);
        const auto key = projection.create_keys.find(idempotency_key);
        if (key != projection.create_keys.end()) {
            const auto job = projection.jobs.find(key->second);
            if (job == projection.jobs.end()) {
                continue;  // 账行竞态(键在、job 行还没落全):再等一拍
            }
            *out_job_id = key->second;
            *out_revision = job->second.revision;
            for (const auto& [occurrence_id, occurrence] : projection.occurrences) {
                if (occurrence.job_id == key->second) {
                    *out_occurrence_id = occurrence_id;
                    break;
                }
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool AssistantAutomationFace::WaitForRunNowReceipt(const std::string& idempotency_key,
                                                   std::string* out_occurrence_id) {
    const std::int64_t deadline = WallClockMs() + kReceiptWaitMs;
    while (WallClockMs() < deadline) {
        const gateway::AutomationProjection projection =
            gateway::ReadAutomationProjection(paths_.automation_log);
        const auto key = projection.runnow_keys.find(idempotency_key);
        if (key != projection.runnow_keys.end()) {
            *out_occurrence_id = key->second;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

nlohmann::json AssistantAutomationFace::HandleTaskCreate(const nlohmann::json& params,
                                                          int& out_error_code,
                                                          std::string& out_error_message) {
    if (!params.is_object()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "task/create: params 须是对象";
        return nlohmann::json();
    }
    const std::string prompt = ReadJsonString(params, "prompt");
    const std::string client_operation_id = ReadJsonString(params, "clientOperationId");
    if (prompt.empty() || prompt.size() > 16 * 1024) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "task/create: prompt 必填(1 ~ 16KB)";
        return nlohmann::json();
    }
    if (client_operation_id.empty() || client_operation_id.size() > 256) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "task/create: clientOperationId 必填(幂等键,重发同键)";
        return nlohmann::json();
    }
    const std::int64_t due_at_ms = ReadJsonInt(params, "dueAtMs");
    // 幂等先查账:同键已受理 → 回原受理(duplicate),不写第二枚命令。
    // automation 的幂等合同(§11.5):同键回原回执,不比载荷。
    {
        const gateway::AutomationProjection projection =
            gateway::ReadAutomationProjection(paths_.automation_log);
        const auto key = projection.create_keys.find(client_operation_id);
        if (key != projection.create_keys.end()) {
            nlohmann::json result;
            result["duplicate"] = true;
            result["jobId"] = key->second;
            const auto job = projection.jobs.find(key->second);
            if (job != projection.jobs.end()) {
                result["revision"] = job->second.revision;
                result["state"] = gateway::ToString(job->second.state);
                result["dueAtMs"] = job->second.due_at_ms;
            }
            for (const auto& [occurrence_id, occurrence] : projection.occurrences) {
                if (occurrence.job_id == key->second) {
                    result["occurrenceId"] = occurrence_id;
                    break;
                }
            }
            return result;
        }
    }
    gateway::GatewayJobAddCommand command;
    command.prompt = prompt;
    command.idempotency_key = client_operation_id;
    command.due_at_ms = due_at_ms > 0 ? due_at_ms : 0;  // 0 = 立即(V1 语义)
    command.requested_at_ms = WallClockMs();
    const std::string error =
        gateway::WriteJobAddCommand(paths_.control_dir, command);
    if (!error.empty()) {
        out_error_code = app_server::kErrInternalError;
        out_error_message = "task/create: 命令落不了盘——" + error;
        return nlohmann::json();
    }
    std::string job_id;
    std::string occurrence_id;
    std::uint64_t revision = 0;
    if (!WaitForCreateReceipt(client_operation_id, &job_id, &occurrence_id, &revision)) {
        out_error_code = app_server::kErrInternalError;
        out_error_message =
            "task/create: 15 秒内没等到受理回执(泵忙或没起来)。幂等键在,重发同键不会双建。";
        return nlohmann::json();
    }
    nlohmann::json result;
    result["duplicate"] = false;
    result["jobId"] = job_id;
    result["occurrenceId"] = occurrence_id;
    result["revision"] = revision;
    result["dueAtMs"] = due_at_ms > 0 ? due_at_ms : 0;
    return result;
}

nlohmann::json AssistantAutomationFace::HandleTaskRunNow(const nlohmann::json& params,
                                                          int& out_error_code,
                                                          std::string& out_error_message) {
    if (!params.is_object()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "task/run-now: params 须是对象";
        return nlohmann::json();
    }
    const std::string job_id = ReadJsonString(params, "jobId");
    const std::string client_operation_id = ReadJsonString(params, "clientOperationId");
    if (job_id.empty() || client_operation_id.empty()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "task/run-now: jobId 与 clientOperationId 都要有";
        return nlohmann::json();
    }
    {
        const gateway::AutomationProjection projection =
            gateway::ReadAutomationProjection(paths_.automation_log);
        const auto job = projection.jobs.find(job_id);
        if (job == projection.jobs.end()) {
            out_error_code = app_server::kErrInvalidParams;
            out_error_message = "task/run-now: 没这个任务 " + job_id;
            return nlohmann::json();
        }
        const auto key = projection.runnow_keys.find(client_operation_id);
        if (key != projection.runnow_keys.end()) {
            nlohmann::json result;
            result["duplicate"] = true;
            result["jobId"] = job_id;
            result["occurrenceId"] = key->second;
            return result;
        }
    }
    gateway::GatewayJobRunNowCommand command;
    command.job_id = job_id;
    command.idempotency_key = client_operation_id;
    command.requested_at_ms = WallClockMs();
    const std::string error =
        gateway::WriteJobRunNowCommand(paths_.control_dir, command);
    if (!error.empty()) {
        out_error_code = app_server::kErrInternalError;
        out_error_message = "task/run-now: 命令落不了盘——" + error;
        return nlohmann::json();
    }
    std::string occurrence_id;
    if (!WaitForRunNowReceipt(client_operation_id, &occurrence_id)) {
        out_error_code = app_server::kErrInternalError;
        out_error_message =
            "task/run-now: 15 秒内没等到受理回执(泵忙或没起来)。幂等键在,重发同键不会双跑。";
        return nlohmann::json();
    }
    nlohmann::json result;
    result["duplicate"] = false;
    result["jobId"] = job_id;
    result["occurrenceId"] = occurrence_id;
    return result;
}

nlohmann::json AssistantAutomationFace::HandleTaskList(const nlohmann::json& params,
                                                        int& out_error_code,
                                                        std::string& out_error_message) {
    (void)params;
    const gateway::AutomationProjection projection =
        gateway::ReadAutomationProjection(paths_.automation_log);
    const gateway::OutboxProjection outbox = gateway::ReadOutboxProjection(paths_.outbox_log);
    nlohmann::json tasks = nlohmann::json::array();
    for (const auto& [job_id, job] : projection.jobs) {
        nlohmann::json item;
        item["jobId"] = job_id;
        std::string summary = job.prompt;
        if (summary.size() > 400) {
            summary.resize(400);
            summary += "…";
        }
        item["prompt"] = summary;
        item["state"] = gateway::ToString(job.state);
        item["scheduleKind"] =
            job.schedule_kind == gateway::ScheduleKind::Once
                ? "once"
                : (job.schedule_kind == gateway::ScheduleKind::Interval ? "interval" : "cron");
        item["dueAtMs"] = job.due_at_ms;
        item["revision"] = job.revision;
        item["createdAtMs"] = job.created_at_ms;
        // 最近一枚 occurrence(按 settled/claimed/scheduled 时间倒序挑)。
        const gateway::AutomationOccurrence* latest = nullptr;
        for (const auto& [occurrence_id, occurrence] : projection.occurrences) {
            if (occurrence.job_id != job_id) {
                continue;
            }
            if (latest == nullptr || occurrence.slot_ms >= latest->slot_ms) {
                latest = &occurrence;
            }
        }
        if (latest != nullptr) {
            nlohmann::json latest_json;
            // 找回 occurrence_id(上面只留了指针,重扫一次拿 id)。
            for (const auto& [occurrence_id, occurrence] : projection.occurrences) {
                if (&occurrence == latest) {
                    latest_json["occurrenceId"] = occurrence_id;
                    break;
                }
            }
            latest_json["state"] = latest->state == gateway::AutomationOccurrence::State::Scheduled
                                       ? "scheduled"
                                       : (latest->state == gateway::AutomationOccurrence::State::Claimed
                                              ? "claimed"
                                              : "settled");
            latest_json["outcome"] = latest->outcome;
            latest_json["detail"] = latest->detail;
            latest_json["settledAtMs"] = latest->settled_at_ms;
            const auto result = FindOccurrenceResult(*latest, outbox);
            if (result.has_value()) {
                latest_json["result"] = *result;
            }
            item["latest"] = std::move(latest_json);
        }
        tasks.push_back(std::move(item));
    }
    nlohmann::json result;
    result["tasks"] = std::move(tasks);
    return result;
}

std::optional<nlohmann::json> AssistantAutomationFace::FindOccurrenceResult(
    const gateway::AutomationOccurrence& occurrence, const gateway::OutboxProjection& outbox) {
    if (occurrence.session_id.empty() || occurrence.turn_id.empty()) {
        return std::nullopt;
    }
    for (const auto& [delivery_id, item] : outbox.items) {
        if (item.session_id != occurrence.session_id || item.turn_id != occurrence.turn_id) {
            continue;
        }
        nlohmann::json result;
        result["deliveryId"] = delivery_id;
        result["deliveryState"] = item.state;
        result["publishedPath"] = item.published_path;
        result["selectionId"] = item.selection_id;
        result["enqueuedAtMs"] = item.enqueued_at_ms;
        // 正文:优先发布文件(delivered 后必有),回落 replies 原件
        //(入箱即冻结)。账行不带正文——投影重放后 reply_text 是空的,
        // 这里按合同从文件读回。
        std::optional<std::string> text;
        if (!item.published_path.empty()) {
            // published_path 相对 profile 根(delivery/out/<id>.txt)。
            text = ReadTextBounded(paths_.profile_dir / item.published_path, kResultTextCapBytes);
        }
        if (!text.has_value()) {
            text = ReadTextBounded(paths_.replies_dir / (item.selection_id + ".txt"),
                                   kResultTextCapBytes);
        }
        if (text.has_value()) {
            result["replyText"] = *text;
        }
        return result;
    }
    return std::nullopt;
}

nlohmann::json AssistantAutomationFace::HandleTaskRead(const nlohmann::json& params,
                                                        int& out_error_code,
                                                        std::string& out_error_message) {
    const std::string job_id = ReadJsonString(params, "jobId");
    if (job_id.empty()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "task/read: jobId 必填";
        return nlohmann::json();
    }
    const gateway::AutomationProjection projection =
        gateway::ReadAutomationProjection(paths_.automation_log);
    const auto job = projection.jobs.find(job_id);
    if (job == projection.jobs.end()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "task/read: 没这个任务 " + job_id;
        return nlohmann::json();
    }
    const gateway::OutboxProjection outbox = gateway::ReadOutboxProjection(paths_.outbox_log);
    nlohmann::json job_json;
    job_json["jobId"] = job_id;
    job_json["prompt"] = job->second.prompt;
    job_json["state"] = gateway::ToString(job->second.state);
    job_json["scheduleKind"] =
        job->second.schedule_kind == gateway::ScheduleKind::Once
            ? "once"
            : (job->second.schedule_kind == gateway::ScheduleKind::Interval ? "interval" : "cron");
    job_json["dueAtMs"] = job->second.due_at_ms;
    job_json["revision"] = job->second.revision;
    job_json["createdAtMs"] = job->second.created_at_ms;
    job_json["idempotencyKey"] = job->second.idempotency_key;
    nlohmann::json occurrences = nlohmann::json::array();
    for (const auto& [occurrence_id, occurrence] : projection.occurrences) {
        if (occurrence.job_id != job_id) {
            continue;
        }
        nlohmann::json item;
        item["occurrenceId"] = occurrence_id;
        item["state"] = occurrence.state == gateway::AutomationOccurrence::State::Scheduled
                            ? "scheduled"
                            : (occurrence.state == gateway::AutomationOccurrence::State::Claimed
                                   ? "claimed"
                                   : "settled");
        item["outcome"] = occurrence.outcome;
        item["detail"] = occurrence.detail;
        item["reason"] = occurrence.reason;
        item["slotMs"] = occurrence.slot_ms;
        item["attempt"] = occurrence.attempt;
        item["claimedAtMs"] = occurrence.claimed_at_ms;
        item["settledAtMs"] = occurrence.settled_at_ms;
        item["sessionId"] = occurrence.session_id;
        item["turnId"] = occurrence.turn_id;
        const auto result = FindOccurrenceResult(occurrence, outbox);
        if (result.has_value()) {
            item["result"] = *result;
        }
        occurrences.push_back(std::move(item));
    }
    nlohmann::json result;
    result["job"] = std::move(job_json);
    result["occurrences"] = std::move(occurrences);
    return result;
}

nlohmann::json AssistantAutomationFace::HandleTaskCancel(const nlohmann::json& params,
                                                          int& out_error_code,
                                                          std::string& out_error_message) {
    if (!params.is_object()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "task/cancel: params 须是对象";
        return nlohmann::json();
    }
    const std::string job_id = ReadJsonString(params, "jobId");
    const std::int64_t expected_revision = ReadJsonInt(params, "expectedRevision");
    const std::string client_operation_id = ReadJsonString(params, "clientOperationId");
    if (job_id.empty() || expected_revision <= 0 || client_operation_id.empty()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message =
            "task/cancel: jobId、expectedRevision(CAS,0 拒)、clientOperationId 都要有";
        return nlohmann::json();
    }
    const gateway::AutomationProjection projection =
        gateway::ReadAutomationProjection(paths_.automation_log);
    const auto job = projection.jobs.find(job_id);
    if (job == projection.jobs.end()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "task/cancel: 没这个任务 " + job_id;
        return nlohmann::json();
    }
    // 已取消的重复取消:回当前态(幂等,不写第二枚命令)。
    if (job->second.state == gateway::AutomationJobState::Cancelled) {
        nlohmann::json result;
        result["jobId"] = job_id;
        result["state"] = "cancelled";
        result["revision"] = job->second.revision;
        result["duplicate"] = true;
        return result;
    }
    gateway::GatewayJobStateCommand command;
    command.verb = "cancel";
    command.job_id = job_id;
    command.expected_revision = static_cast<std::uint64_t>(expected_revision);
    command.idempotency_key = client_operation_id;
    command.requested_at_ms = WallClockMs();
    const std::string error =
        gateway::WriteJobStateCommand(paths_.control_dir, command);
    if (!error.empty()) {
        out_error_code = app_server::kErrInternalError;
        out_error_message = "task/cancel: 命令落不了盘——" + error;
        return nlohmann::json();
    }
    // 轮询到 state==cancelled(泵消费命令即落)。CAS 拒绝的形状 = revision
    // 不符——轮询超时后如实报,带当前 revision 供客户端重试。
    const std::int64_t deadline = WallClockMs() + kReceiptWaitMs;
    while (WallClockMs() < deadline) {
        const gateway::AutomationProjection now =
            gateway::ReadAutomationProjection(paths_.automation_log);
        const auto current = now.jobs.find(job_id);
        if (current != now.jobs.end() &&
            current->second.state == gateway::AutomationJobState::Cancelled) {
            nlohmann::json result;
            result["jobId"] = job_id;
            result["state"] = "cancelled";
            result["revision"] = current->second.revision;
            result["duplicate"] = false;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    out_error_code = app_server::kErrInternalError;
    out_error_message = "task/cancel: 15 秒内没等到取消生效(泵忙,或 expectedRevision 不符被拒)。"
                        "核对 task/read 的 revision 后重试。";
    return nlohmann::json();
}

nlohmann::json AssistantAutomationFace::HandleApprovalList(const nlohmann::json& params,
                                                            int& out_error_code,
                                                            std::string& out_error_message) {
    (void)params;
    nlohmann::json result;
    result["pending"] = broker_->ListPending();
    return result;
}

nlohmann::json AssistantAutomationFace::HandleApprovalRespond(const nlohmann::json& params,
                                                              int& out_error_code,
                                                              std::string& out_error_message) {
    if (!params.is_object()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "approval/respond: params 须是对象";
        return nlohmann::json();
    }
    const std::string request_id = ReadJsonString(params, "requestId");
    const std::string decision = ReadJsonString(params, "decision");
    if (request_id.empty() || (decision != "accept" && decision != "decline")) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "approval/respond: requestId 必填,decision 须是 accept 或 decline";
        return nlohmann::json();
    }
    const auto outcome = broker_->Respond(request_id, decision == "accept");
    if (!outcome.resolved) {
        // 迟到/收口/不认识:回执如实,不冒充已答。
        nlohmann::json result;
        result["resolved"] = false;
        result["reason"] = "stale_request_id";
        return result;
    }
    nlohmann::json result;
    result["resolved"] = true;
    return result;
}

nlohmann::json AssistantAutomationFace::HandleEventsRead(const nlohmann::json& params,
                                                          int& out_error_code,
                                                          std::string& out_error_message) {
    const std::string boot_id = ReadJsonString(params, "bootId");
    const std::int64_t last_seq = ReadJsonInt(params, "lastSeq");
    if (boot_id.empty()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "assistant/events/read: bootId 必填(WS 游标绑定 boot ID)";
        return nlohmann::json();
    }
    const auto read = hub_->Read(boot_id, last_seq < 0 ? 0 : static_cast<std::uint64_t>(last_seq));
    nlohmann::json result;
    result["bootId"] = read.boot_id;
    result["currentSeq"] = read.current_seq;
    result["oldestSeq"] = read.oldest_seq;
    result["reset"] = read.reset;
    nlohmann::json events = nlohmann::json::array();
    for (const AssistantEventHub::Entry& entry : read.events) {
        nlohmann::json item;
        item["seq"] = entry.seq;
        item["method"] = entry.method;
        item["params"] = entry.params;
        events.push_back(std::move(item));
    }
    result["events"] = std::move(events);
    return result;
}

// ---------------------------------------------------------------------------
// 方法注册
// ---------------------------------------------------------------------------

void RegisterAssistantTaskMethods(app_server::Dispatcher& dispatcher,
                                  const std::shared_ptr<AssistantAutomationFace>& face) {
    // 任务面方法注册:handler 内联 available 门(锁被同 profile gateway
    // 占/账开不了时方法在、回稳定错误 assistant.automation_unavailable,
    // 页面如实显示,不冒充)。approval/* 与 assistant/events/read 不受门
    //(审批悬着的可发现性、事件补账在任务面不可用时也如实回)。
    const auto register_face_method = [&dispatcher,
                                       &face](const char* method, bool require_available,
                                              nlohmann::json (AssistantAutomationFace::*handler)(
                                                  const nlohmann::json&, std::string&,
                                                  std::string&)) {
        dispatcher.RegisterMethod(
            method, [face, method, require_available,
                     handler](const app_server::IncomingRequest& request,
                              app_server::DispatchContext&) -> std::optional<nlohmann::json> {
                if (require_available && !face->available()) {
                    return app_server::MakeError(
                        request.id, app_server::kErrInternalError,
                        std::string(method) + ": 任务面不可用——" + face->unavailable_reason(),
                        nlohmann::json{{"code", "assistant.automation_unavailable"}});
                }
                int error_code = 0;
                std::string error_message;
                nlohmann::json result = (*face.*handler)(request.params, error_code, error_message);
                if (error_code != 0) {
                    return app_server::MakeError(request.id, error_code, error_message);
                }
                return app_server::MakeResult(request.id, std::move(result));
            });
    };

    register_face_method("task/create", true, &AssistantAutomationFace::HandleTaskCreate);
    register_face_method("task/run-now", true, &AssistantAutomationFace::HandleTaskRunNow);
    register_face_method("task/list", true, &AssistantAutomationFace::HandleTaskList);
    register_face_method("task/read", true, &AssistantAutomationFace::HandleTaskRead);
    register_face_method("task/cancel", true, &AssistantAutomationFace::HandleTaskCancel);
    register_face_method("approval/list", false, &AssistantAutomationFace::HandleApprovalList);
    register_face_method("approval/respond", false,
                         &AssistantAutomationFace::HandleApprovalRespond);
    register_face_method("assistant/events/read", false,
                         &AssistantAutomationFace::HandleEventsRead);
}

// ---------------------------------------------------------------------------
// AssistantAutomationRuntime
// ---------------------------------------------------------------------------

AssistantAutomationRuntime::AssistantAutomationRuntime(
    Options options, AssistantEventHub* hub, std::shared_ptr<AssistantApprovalBroker> broker)
    : options_(std::move(options)), hub_(hub), broker_(std::move(broker)) {}

AssistantAutomationRuntime::OpenOutcome AssistantAutomationRuntime::Open(
    api::Backend& backend, tools::ToolRegistry& registry, Options options, AssistantEventHub* hub,
    const std::shared_ptr<AssistantApprovalBroker>& broker, bool start_thread) {
    OpenOutcome outcome;
    // 1) gateway 同 profile 锁(单写者互斥):同 profile 的 gateway run 或
    //    另一只持锁实例在跑 → 任务面禁用(聊天线照常),不抢账。
    gateway::GatewayLockRecord self;
    self.pid = platform::CurrentProcessId();
    self.start_token = trajectory::CurrentProcessStartToken();
    self.boot_id = hub->boot_id();
    self.owner_epoch = hub->boot_id();  // 一 boot 一 epoch(与 GatewayProcess 同式)
    self.acquired_at_ms = WallClockMs();
    gateway::GatewayLock lock;
    const auto acquire =
        gateway::GatewayLock::TryAcquire(options.paths.lock_file, self, &lock);
    if (acquire.status == gateway::GatewayLock::AcquireResult::Status::RefusedAliveHolder) {
        outcome.unavailable_reason =
            "同 profile 的 Gateway 实例正在运行(pid " + std::to_string(acquire.holder.pid) +
            "),automation 账单写者不让渡;要用任务面,先停那只实例,或换个 --profile";
        return outcome;
    }
    if (acquire.status != gateway::GatewayLock::AcquireResult::Status::Acquired) {
        outcome.unavailable_reason =
            "gateway profile 锁取不到(" +
            (acquire.detail.empty() ? std::string("未知原因") : acquire.detail) + ")";
        return outcome;
    }
    // 2) 开泵(backend/registry 借用,归本对象活的更久——调用方栈上持有)。
    auto runtime = std::unique_ptr<AssistantAutomationRuntime>(
        new AssistantAutomationRuntime(std::move(options), hub, broker));
    runtime->lock_ = std::move(lock);
    runtime::GatewayAutomationPump::Options pump_options;
    pump_options.paths = runtime->options_.paths;
    pump_options.workspaces_root = runtime->options_.workspaces_root;
    pump_options.workspace_identity = runtime->options_.workspace_identity;
    pump_options.cwd_utf8 = runtime->options_.cwd_utf8;
    pump_options.lubancode_version = runtime->options_.lubancode_version;
    pump_options.wire_name = runtime->options_.wire_name;
    pump_options.model = runtime->options_.model;
    pump_options.max_steps_per_turn = runtime->options_.max_steps_per_turn;
    pump_options.max_wall_secs = runtime->options_.max_wall_secs;
    pump_options.max_total_tokens = runtime->options_.max_total_tokens;
    // 审批口:needs_confirm 工具问页面(超时默认拒绝);工具面不设渠道
    // 上限(allow=nullopt,受 Agent 工具表与审批闸管)。归属上下文经
    // broker 的 provider 取(泵线程查 store 的 claimed——单飞泵同时至多
    // 一枚在飞,回调发生在执行窗内,查到的就是它)。
    AssistantApprovalBroker* broker_ptr = runtime->broker_.get();
    AssistantAutomationRuntime* raw = runtime.get();
    broker_ptr->set_context_provider([raw]() -> std::pair<std::string, std::string> {
        const gateway::AutomationStore* store = raw->pump_.store();
        if (store == nullptr) {
            return {};
        }
        for (const auto& occurrence : store->ListOccurrences()) {
            if (occurrence.state == gateway::AutomationOccurrence::State::Claimed) {
                return {occurrence.job_id, occurrence.occurrence_id};
            }
        }
        return {};
    });
    pump_options.on_tool_confirm =
        [broker_ptr](const std::string& tool_use_id, const std::string& name,
                     const nlohmann::json& input)
        -> runtime::HeadlessExecutor::Options::ToolConfirmDecision {
        const auto allowed =
            broker_ptr->Ask(std::string(), std::string(), tool_use_id, name, input);
        runtime::HeadlessExecutor::Options::ToolConfirmDecision decision;
        decision.allowed = allowed.first;
        decision.denial_text = allowed.second;
        return decision;
    };
    pump_options.model_provider = runtime->options_.model_provider;
    const auto open = runtime::GatewayAutomationPump::Open(&runtime->pump_, backend, registry,
                                                           std::move(pump_options));
    if (!open.ok) {
        outcome.unavailable_reason = "自动任务泵开不了——" + open.error;
        return outcome;
    }
    runtime->pump_.set_owner_epoch(hub->boot_id());
    if (start_thread) {
        runtime->thread_ = std::thread([raw] {
            while (!raw->stop_requested_.load()) {
                if (!raw->TickAndPublish(WallClockMs())) {
                    std::fprintf(stderr,
                                 "[assistant] 自动任务泵 broken(领域账写不进),任务面停摆;"
                                 "进程保留供诊断\n");
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
    }
    outcome.runtime = std::move(runtime);
    return outcome;
}

AssistantAutomationRuntime::~AssistantAutomationRuntime() {
    Stop();
}

bool AssistantAutomationRuntime::TickAndPublish(std::int64_t now_ms) {
    const bool ok = pump_.TickOnce(now_ms);
    DiffAndPublish();
    return ok;
}

void AssistantAutomationRuntime::DiffAndPublish() {
    // store 内存投影的 diff(只归泵线程碰,零锁)。任务/occurrence 状态
    // 变迁 → assistant/task/event 事件进账(重连补账面)。
    const gateway::AutomationStore* store = pump_.store();
    if (store == nullptr) {
        return;
    }
    const std::vector<gateway::AutomationJob> jobs = store->ListJobs();
    const std::vector<gateway::AutomationOccurrence> occurrences = store->ListOccurrences();
    std::map<std::string, std::pair<std::string, std::uint64_t>> jobs_now;
    for (const auto& job : jobs) {
        jobs_now[job.job_id] = {gateway::ToString(job.state), job.revision};
        const auto seen = last_jobs_.find(job.job_id);
        if (seen == last_jobs_.end()) {
            nlohmann::json params;
            params["kind"] = "job.created";
            params["jobId"] = job.job_id;
            params["state"] = gateway::ToString(job.state);
            params["revision"] = job.revision;
            params["atMs"] = WallClockMs();
            hub_->Push("assistant/task/event", std::move(params));
        } else if (seen->second.first != jobs_now[job.job_id].first ||
                   seen->second.second != job.revision) {
            nlohmann::json params;
            params["kind"] = "job.updated";
            params["jobId"] = job.job_id;
            params["state"] = gateway::ToString(job.state);
            params["revision"] = job.revision;
            params["atMs"] = WallClockMs();
            hub_->Push("assistant/task/event", std::move(params));
        }
    }
    std::map<std::string, std::pair<std::string, std::string>> occurrences_now;
    for (const auto& occurrence : occurrences) {
        const char* state = occurrence.state == gateway::AutomationOccurrence::State::Scheduled
                                ? "scheduled"
                                : (occurrence.state == gateway::AutomationOccurrence::State::Claimed
                                       ? "claimed"
                                       : "settled");
        occurrences_now[occurrence.occurrence_id] = {state, occurrence.outcome};
        const auto seen = last_occurrences_.find(occurrence.occurrence_id);
        if (seen != last_occurrences_.end() && seen->second.first == std::string(state) &&
            seen->second.second == occurrence.outcome) {
            continue;
        }
        nlohmann::json params;
        params["kind"] =
            seen == last_occurrences_.end() ? "occurrence.created" : "occurrence.changed";
        params["jobId"] = occurrence.job_id;
        params["occurrenceId"] = occurrence.occurrence_id;
        params["state"] = state;
        params["outcome"] = occurrence.outcome;
        params["detail"] = occurrence.detail;
        params["sessionId"] = occurrence.session_id;
        params["turnId"] = occurrence.turn_id;
        params["atMs"] = WallClockMs();
        hub_->Push("assistant/task/event", std::move(params));
    }
    last_jobs_ = std::move(jobs_now);
    last_occurrences_ = std::move(occurrences_now);
}

void AssistantAutomationRuntime::Stop() {
    if (stop_requested_.exchange(true)) {
        return;
    }
    // 次序:先叫醒悬在审批里的泵线程(CancelAll 置旗,Ask 按收口拒醒),
    // 再 join(否则 120s 审批超时会拖住收口);泵收口;悬答清账;放锁。
    broker_->CancelAll("assistant_shutdown");
    if (thread_.joinable()) {
        thread_.join();
    }
    pump_.StopAccepting();
    (void)pump_.Close(0);
    lock_.Release();
}

}  // namespace lubancode::app
