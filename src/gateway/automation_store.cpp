// AutomationStore 实现(常驻总装 V1)。合同见头文件与 contracts.md §11。
#include "gateway/automation_store.hpp"

#include <fstream>
#include <utility>

#include "platform/paths.hpp"
#include "platform/sha256.hpp"

namespace lubancode::gateway {

namespace {

// 账行 type 值(纯追加制;读取侧对未知 type 跳过留诊断,不拒账)。
constexpr const char* kTypeJobCreated = "job.created";
constexpr const char* kTypeOccurrenceCreated = "occurrence.created";
constexpr const char* kTypeOccurrenceClaimed = "occurrence.claimed";
constexpr const char* kTypeOccurrenceBound = "occurrence.bound";
constexpr const char* kTypeOccurrenceSettled = "occurrence.settled";

std::string GetJsonString(const nlohmann::json& json, const char* key) {
    if (!json.is_object() || !json.contains(key) || !json[key].is_string()) {
        return std::string();
    }
    return json[key].get<std::string>();
}

std::int64_t GetJsonInt(const nlohmann::json& json, const char* key) {
    if (!json.is_object() || !json.contains(key) || !json[key].is_number_integer()) {
        return 0;
    }
    return json[key].get<std::int64_t>();
}

bool ValidOutcome(const std::string& outcome) {
    return outcome == "succeeded" || outcome == "failed" || outcome == "needs_review";
}

}  // namespace

AutomationProjection ReadAutomationProjection(const std::filesystem::path& log_file) {
    AutomationProjection projection;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(log_file, ec) || ec) {
        return projection;  // 没有账(或路径被占)= 空投影(只读面零副作用)
    }
    std::ifstream stream(log_file, std::ios::binary);
    std::string line_text;
    while (std::getline(stream, line_text)) {
        if (line_text.empty()) continue;
        nlohmann::json line;
        try {
            line = nlohmann::json::parse(line_text);
        } catch (const nlohmann::json::exception&) {
            ++projection.skipped_lines;
            continue;
        }
        const std::string type = GetJsonString(line, "type");
        if (type == kTypeJobCreated) {
            AutomationJob job;
            job.job_id = GetJsonString(line, "jobId");
            job.prompt = GetJsonString(line, "prompt");
            job.due_at_ms = GetJsonInt(line, "dueAtMs");
            job.revision = static_cast<std::uint64_t>(GetJsonInt(line, "revision"));
            job.created_at_ms = GetJsonInt(line, "createdAtMs");
            job.idempotency_key = GetJsonString(line, "idempotencyKey");
            if (job.job_id.empty() || job.prompt.empty() || job.revision == 0) {
                ++projection.skipped_lines;
                continue;
            }
            const std::uint64_t counter =
                static_cast<std::uint64_t>(GetJsonInt(line, "jobCounter"));
            if (counter > projection.job_counter) {
                projection.job_counter = counter;
            }
            const std::string key = GetJsonString(line, "idempotencyKey");
            if (!key.empty()) {
                projection.create_keys[key] = job.job_id;
            }
            projection.jobs[job.job_id] = std::move(job);
        } else if (type == kTypeOccurrenceCreated) {
            AutomationOccurrence occurrence;
            occurrence.occurrence_id = GetJsonString(line, "occurrenceId");
            occurrence.job_id = GetJsonString(line, "jobId");
            occurrence.slot_ms = GetJsonInt(line, "slotMs");
            if (occurrence.occurrence_id.empty() || occurrence.job_id.empty()) {
                ++projection.skipped_lines;
                continue;
            }
            projection.occurrences[occurrence.occurrence_id] = std::move(occurrence);
            const std::string key = GetJsonString(line, "idempotencyKey");
            if (!key.empty()) {
                projection.runnow_keys[key] = GetJsonString(line, "occurrenceId");
            }
        } else if (type == kTypeOccurrenceClaimed || type == kTypeOccurrenceBound) {
            const auto found = projection.occurrences.find(GetJsonString(line, "occurrenceId"));
            if (found == projection.occurrences.end()) {
                ++projection.skipped_lines;
                continue;
            }
            if (type == kTypeOccurrenceClaimed) {
                found->second.state = AutomationOccurrence::State::Claimed;
                found->second.owner_epoch = GetJsonString(line, "ownerEpoch");
                found->second.attempt = static_cast<std::uint64_t>(GetJsonInt(line, "attempt"));
                found->second.claimed_at_ms = GetJsonInt(line, "claimedAtMs");
            } else {
                found->second.session_id = GetJsonString(line, "sessionId");
                found->second.turn_id = GetJsonString(line, "turnId");
            }
        } else if (type == kTypeOccurrenceSettled) {
            const auto found = projection.occurrences.find(GetJsonString(line, "occurrenceId"));
            if (found == projection.occurrences.end()) {
                ++projection.skipped_lines;
                continue;
            }
            const std::string outcome = GetJsonString(line, "outcome");
            if (!ValidOutcome(outcome)) {
                ++projection.skipped_lines;
                continue;
            }
            found->second.state = AutomationOccurrence::State::Settled;
            found->second.outcome = outcome;
            found->second.detail = GetJsonString(line, "detail");
            found->second.settled_at_ms = GetJsonInt(line, "settledAtMs");
        } else {
            ++projection.skipped_lines;  // 未知 type:前向兼容跳过(账不拒)
        }
    }
    return projection;
}

std::string MakeOccurrenceId(const std::string& job_id, std::uint64_t revision,
                             std::int64_t slot_ms) {
    const std::string canonical = job_id + "\n" + std::to_string(revision) + "\n" +
                                  std::to_string(slot_ms);
    const std::string hash = platform::Sha256Hex(canonical);
    return "occ-" + hash.substr(0, 16);
}

AutomationStore::~AutomationStore() = default;

AutomationStore::AutomationStore(AutomationStore&&) noexcept = default;

AutomationStore& AutomationStore::operator=(AutomationStore&&) noexcept = default;

AutomationStore::OpenResult AutomationStore::Open(AutomationStore* out,
                                                  const std::filesystem::path& log_file) {
    OpenResult result;
    if (out == nullptr) {
        result.error = "automation.store_unavailable: out 为空";
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(log_file.parent_path(), ec);
    if (ec && !log_file.parent_path().empty()) {
        result.error = "automation.store_unavailable: 建目录失败 " +
                       platform::PathToUtf8(log_file.parent_path()) + ": " + ec.message();
        return result;
    }
    // 既有账重放(ReadAutomationProjection 同一份逻辑):坏行跳过留数,
    // 不崩(索引是派生物;已提交行不回写)。写者 lazy 开——首笔提交才
    // 真正打开账文件(占位/只读一类故障在首笔暴露,Open 不提前撞)。
    AutomationProjection projection = ReadAutomationProjection(log_file);
    out->jobs_ = std::move(projection.jobs);
    out->occurrences_ = std::move(projection.occurrences);
    out->create_keys_ = std::move(projection.create_keys);
    out->runnow_keys_ = std::move(projection.runnow_keys);
    out->job_counter_ = projection.job_counter;
    out->log_path_ = log_file;
    out->writer_.reset();
    out->broken_ = false;
    result.ok = true;
    result.skipped_lines = projection.skipped_lines;
    return result;
}

bool AutomationStore::AppendLinePowerLoss(const nlohmann::json& line) {
    if (broken_) {
        return false;
    }
    if (!writer_.has_value()) {
        auto writer = trajectory::JournalWriter::Open(log_path_,
                                                      trajectory::JournalWriter::OpenMode::Append);
        if (!writer.has_value()) {
            broken_ = true;  // 账开不了:与首笔写失败同款处置(停受理)
            return false;
        }
        writer_ = std::move(*writer);
    }
    if (!writer_->AppendLine(line.dump(), trajectory::Durability::PowerLoss)) {
        broken_ = true;  // §11.5:写失败即停受理/停执行,后续全拒
        return false;
    }
    return true;
}

AutomationStore::JobReceipt AutomationStore::CreateOnceJob(const std::string& job_id,
                                                            const std::string& prompt,
                                                            std::int64_t due_at_ms,
                                                            std::int64_t now_ms,
                                                            const std::string& idempotency_key) {
    JobReceipt receipt;
    // 幂等键查重:同键同载荷回原回执,同键异载荷 conflict。
    if (!idempotency_key.empty()) {
        const auto found = create_keys_.find(idempotency_key);
        if (found != create_keys_.end()) {
            const auto original = jobs_.find(found->second);
            if (original != jobs_.end() && original->second.prompt == prompt &&
                original->second.due_at_ms == due_at_ms) {
                receipt.duplicate = true;
                receipt.job_id = original->second.job_id;
                return receipt;
            }
            receipt.error_code = "automation.revision_conflict";
            return receipt;
        }
    }
    if (prompt.empty()) {
        receipt.error_code = "automation.schedule_invalid";
        return receipt;
    }
    std::string final_id = job_id;
    if (final_id.empty()) {
        do {
            ++job_counter_;
            final_id = "job-" + std::to_string(job_counter_);
        } while (jobs_.find(final_id) != jobs_.end());
    } else if (jobs_.find(final_id) != jobs_.end()) {
        receipt.error_code = "automation.revision_conflict";  // 同名任务已存在
        return receipt;
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeJobCreated;
    line["schemaVersion"] = 1;
    line["jobId"] = final_id;
    line["prompt"] = prompt;
    line["scheduleKind"] = "once";
    line["dueAtMs"] = due_at_ms;
    line["revision"] = 1;
    line["createdAtMs"] = now_ms;
    line["jobCounter"] = job_counter_;
    if (!idempotency_key.empty()) line["idempotencyKey"] = idempotency_key;
    // 首枚 occurrence 同笔落(slot = due_at_ms,计划内时间):once 的"这一
    // 拍"在建任务时就定死,时钟倒拨不再造同一拍(§11.1)。
    const std::string occurrence_id = MakeOccurrenceId(final_id, 1, due_at_ms);
    nlohmann::json occurrence_line = nlohmann::json::object();
    occurrence_line["type"] = kTypeOccurrenceCreated;
    occurrence_line["schemaVersion"] = 1;
    occurrence_line["jobId"] = final_id;
    occurrence_line["occurrenceId"] = occurrence_id;
    occurrence_line["slotMs"] = due_at_ms;
    occurrence_line["reason"] = "schedule";
    if (!AppendLinePowerLoss(line) || !AppendLinePowerLoss(occurrence_line)) {
        receipt.error_code = "automation.append_failed";
        return receipt;
    }
    AutomationJob job;
    job.job_id = final_id;
    job.prompt = prompt;
    job.due_at_ms = due_at_ms;
    job.revision = 1;
    job.created_at_ms = now_ms;
    job.idempotency_key = idempotency_key;
    jobs_[final_id] = std::move(job);
    AutomationOccurrence occurrence;
    occurrence.occurrence_id = occurrence_id;
    occurrence.job_id = final_id;
    occurrence.slot_ms = due_at_ms;
    occurrences_[occurrence_id] = occurrence;
    if (!idempotency_key.empty()) {
        create_keys_[idempotency_key] = final_id;
    }
    receipt.accepted = true;
    receipt.job_id = final_id;
    receipt.occurrence_id = occurrence_id;
    return receipt;
}

AutomationStore::JobReceipt AutomationStore::RequestRunNow(const std::string& job_id,
                                                            std::int64_t now_ms,
                                                            const std::string& idempotency_key) {
    JobReceipt receipt;
    const auto job = jobs_.find(job_id);
    if (job == jobs_.end()) {
        receipt.error_code = "automation.job_not_found";
        return receipt;
    }
    // 幂等:同键回原 occurrence(不另造一拍,§11.1 slot 用请求时刻)。
    if (!idempotency_key.empty()) {
        const auto found = runnow_keys_.find(idempotency_key);
        if (found != runnow_keys_.end()) {
            receipt.duplicate = true;
            receipt.job_id = job_id;
            receipt.occurrence_id = found->second;
            return receipt;
        }
    }
    const std::string occurrence_id = MakeOccurrenceId(job_id, job->second.revision, now_ms);
    if (occurrences_.find(occurrence_id) != occurrences_.end()) {
        // 同 slot 撞车(同毫秒两发):回已存在的,不造第二枚。
        receipt.duplicate = true;
        receipt.job_id = job_id;
        receipt.occurrence_id = occurrence_id;
        return receipt;
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeOccurrenceCreated;
    line["schemaVersion"] = 1;
    line["jobId"] = job_id;
    line["occurrenceId"] = occurrence_id;
    line["slotMs"] = now_ms;
    line["reason"] = "run_now";
    line["requestedAtMs"] = now_ms;
    if (!idempotency_key.empty()) line["idempotencyKey"] = idempotency_key;
    if (!AppendLinePowerLoss(line)) {
        receipt.error_code = "automation.append_failed";
        return receipt;
    }
    AutomationOccurrence occurrence;
    occurrence.occurrence_id = occurrence_id;
    occurrence.job_id = job_id;
    occurrence.slot_ms = now_ms;
    occurrences_[occurrence_id] = occurrence;
    if (!idempotency_key.empty()) {
        runnow_keys_[idempotency_key] = occurrence_id;
    }
    receipt.accepted = true;
    receipt.job_id = job_id;
    receipt.occurrence_id = occurrence_id;
    return receipt;
}

std::optional<AutomationOccurrence> AutomationStore::ClaimDue(const std::string& owner_epoch,
                                                               std::int64_t now_ms) {
    // due = scheduled 且 slot <= now。挑 slot 最老的(FIFO)。
    std::string picked_id;
    for (const auto& [id, occurrence] : occurrences_) {
        if (occurrence.state != AutomationOccurrence::State::Scheduled) continue;
        if (occurrence.slot_ms <= now_ms &&
            (picked_id.empty() || occurrence.slot_ms < occurrences_[picked_id].slot_ms)) {
            picked_id = id;
        }
    }
    if (picked_id.empty()) {
        return std::nullopt;
    }
    AutomationOccurrence& occurrence = occurrences_[picked_id];
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeOccurrenceClaimed;
    line["schemaVersion"] = 1;
    line["occurrenceId"] = picked_id;
    line["jobId"] = occurrence.job_id;
    line["slotMs"] = occurrence.slot_ms;
    line["ownerEpoch"] = owner_epoch;
    line["attempt"] = occurrence.attempt;
    line["claimedAtMs"] = now_ms;
    if (!AppendLinePowerLoss(line)) {
        return std::nullopt;  // broken:泵应停
    }
    occurrence.state = AutomationOccurrence::State::Claimed;
    occurrence.owner_epoch = owner_epoch;
    occurrence.claimed_at_ms = now_ms;
    return occurrence;
}

bool AutomationStore::BindOccurrence(const std::string& occurrence_id, const std::string& session_id,
                                     const std::string& turn_id, std::int64_t now_ms) {
    auto found = occurrences_.find(occurrence_id);
    if (found == occurrences_.end() || found->second.state != AutomationOccurrence::State::Claimed) {
        return false;
    }
    if (!found->second.session_id.empty()) {
        return found->second.session_id == session_id;  // 幂等重绑:同场即真
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeOccurrenceBound;
    line["schemaVersion"] = 1;
    line["occurrenceId"] = occurrence_id;
    line["jobId"] = found->second.job_id;
    line["sessionId"] = session_id;
    line["turnId"] = turn_id;
    line["boundAtMs"] = now_ms;
    if (!AppendLinePowerLoss(line)) {
        return false;
    }
    found->second.session_id = session_id;
    found->second.turn_id = turn_id;
    return true;
}

bool AutomationStore::SettleOccurrence(const std::string& occurrence_id, const std::string& outcome,
                                       const std::string& detail, std::int64_t now_ms) {
    if (!ValidOutcome(outcome)) {
        return false;
    }
    auto found = occurrences_.find(occurrence_id);
    if (found == occurrences_.end() || found->second.state == AutomationOccurrence::State::Settled) {
        return false;  // 幂等:首笔结算为准
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeOccurrenceSettled;
    line["schemaVersion"] = 1;
    line["occurrenceId"] = occurrence_id;
    line["jobId"] = found->second.job_id;
    line["outcome"] = outcome;
    line["detail"] = detail;
    line["sessionId"] = found->second.session_id;
    line["turnId"] = found->second.turn_id;
    line["settledAtMs"] = now_ms;
    if (!AppendLinePowerLoss(line)) {
        return false;
    }
    found->second.state = AutomationOccurrence::State::Settled;
    found->second.outcome = outcome;
    found->second.detail = detail;
    found->second.settled_at_ms = now_ms;
    return true;
}

std::vector<AutomationJob> AutomationStore::ListJobs() const {
    std::vector<AutomationJob> jobs;
    jobs.reserve(jobs_.size());
    for (const auto& [id, job] : jobs_) {
        jobs.push_back(job);
    }
    return jobs;
}

std::optional<AutomationJob> AutomationStore::FindJob(const std::string& job_id) const {
    const auto found = jobs_.find(job_id);
    if (found == jobs_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<AutomationOccurrence> AutomationStore::FindOccurrence(
    const std::string& occurrence_id) const {
    const auto found = occurrences_.find(occurrence_id);
    if (found == occurrences_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<AutomationOccurrence> AutomationStore::ListOccurrences() const {
    std::vector<AutomationOccurrence> occurrences;
    occurrences.reserve(occurrences_.size());
    for (const auto& [id, occurrence] : occurrences_) {
        occurrences.push_back(occurrence);
    }
    return occurrences;
}

std::size_t AutomationStore::DueCount(std::int64_t now_ms) const {
    std::size_t count = 0;
    for (const auto& [id, occurrence] : occurrences_) {
        if (occurrence.state == AutomationOccurrence::State::Scheduled && occurrence.slot_ms <= now_ms) {
            ++count;
        }
    }
    return count;
}

std::vector<AutomationOccurrence> AutomationStore::OpenOccurrences() const {
    std::vector<AutomationOccurrence> open;
    for (const auto& [id, occurrence] : occurrences_) {
        if (occurrence.state == AutomationOccurrence::State::Claimed) {
            open.push_back(occurrence);
        }
    }
    return open;
}

}  // namespace lubancode::gateway
