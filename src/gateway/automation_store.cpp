// AutomationStore 实现(常驻总装 V1 + V2 周期扩展)。合同见头文件与
// contracts.md §11/§13。
#include "gateway/automation_store.hpp"

#include <algorithm>
#include <fstream>
#include <utility>

#include "platform/paths.hpp"
#include "platform/sha256.hpp"

namespace lubancode::gateway {

namespace {

// 账行 type 值(纯追加制;读取侧对未知 type 跳过留诊断,不拒账)。
constexpr const char* kTypeJobCreated = "job.created";
constexpr const char* kTypeJobUpdated = "job.updated";
constexpr const char* kTypeJobPaused = "job.paused";
constexpr const char* kTypeJobResumed = "job.resumed";
constexpr const char* kTypeJobCancelled = "job.cancelled";
constexpr const char* kTypeJobScheduleAdvanced = "job.schedule_advanced";
constexpr const char* kTypeOccurrenceCreated = "occurrence.created";
constexpr const char* kTypeOccurrenceClaimed = "occurrence.claimed";
constexpr const char* kTypeOccurrenceBound = "occurrence.bound";
constexpr const char* kTypeOccurrenceSettled = "occurrence.settled";
constexpr const char* kTypeOccurrenceMerged = "occurrence.merged";
constexpr const char* kTypeOccurrenceRedispatched = "occurrence.redispatched";
constexpr const char* kTypeOccurrenceObserved = "occurrence.observed";
constexpr const char* kTypeOccurrenceCancelRequested = "occurrence.cancel_requested";
constexpr const char* kTypeLoopImported = "loop.imported";

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

bool GetJsonBool(const nlohmann::json& json, const char* key) {
    if (!json.is_object() || !json.contains(key) || !json[key].is_boolean()) {
        return false;
    }
    return json[key].get<bool>();
}

std::uint64_t GetJsonUint(const nlohmann::json& json, const char* key) {
    const std::int64_t value = GetJsonInt(json, key);
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

bool ValidOutcome(const std::string& outcome) {
    return outcome == "succeeded" || outcome == "failed" || outcome == "needs_review" ||
           outcome == "cancelled";
}

ScheduleKind ParseKindOr(const nlohmann::json& json, const char* key) {
    ScheduleKind kind = ScheduleKind::Once;
    const std::string text = GetJsonString(json, key);
    if (!text.empty()) {
        (void)ParseScheduleKind(text, kind);
    }
    return kind;
}

MisfirePolicy ParseMisfireOr(const nlohmann::json& json, const char* key) {
    MisfirePolicy policy = MisfirePolicy::Coalesce;
    const std::string text = GetJsonString(json, key);
    if (!text.empty()) {
        (void)ParseMisfirePolicy(text, policy);
    }
    return policy;
}

// job.created/updated 行 -> AutomationJob 的公共字段装载(旧键缺省 =
// V1 语义)。cursor_override 非 null 时覆盖游标(update 重排时间轴)。
void LoadJobLine(const nlohmann::json& line, AutomationJob* job,
                 const std::int64_t* cursor_override) {
    job->prompt = GetJsonString(line, "prompt");
    job->due_at_ms = GetJsonInt(line, "dueAtMs");
    job->created_at_ms = GetJsonInt(line, "createdAtMs");
    job->schedule_kind = ParseKindOr(line, "scheduleKind");
    job->interval_seconds = GetJsonInt(line, "intervalSeconds");
    job->anchor_ms = GetJsonInt(line, "anchorMs");
    job->cron_expr = GetJsonString(line, "cronExpr");
    const std::string timezone = GetJsonString(line, "timezone");
    if (!timezone.empty()) {
        job->timezone = timezone;
    }
    job->misfire = ParseMisfireOr(line, "misfirePolicy");
    job->deadline_ms = GetJsonInt(line, "deadlineMs");
    job->notify_on_change = GetJsonBool(line, "notifyOnChange");
    const std::string session_policy = GetJsonString(line, "sessionPolicy");
    if (!session_policy.empty()) {
        job->session_policy = session_policy;
    }
    job->imported_from = GetJsonString(line, "importedFrom");
    // 游标:显式 cursorMs 优先;缺省按形态推导(interval=锚点,cron=建账
    // 时刻,once=0)。
    const std::int64_t explicit_cursor = GetJsonInt(line, "cursorMs");
    if (cursor_override != nullptr) {
        job->schedule_cursor_ms = *cursor_override;
    } else if (explicit_cursor != 0) {
        job->schedule_cursor_ms = explicit_cursor;
    } else if (job->schedule_kind == ScheduleKind::Interval) {
        job->schedule_cursor_ms = job->anchor_ms;
    } else if (job->schedule_kind == ScheduleKind::Cron) {
        job->schedule_cursor_ms = job->created_at_ms;
    } else {
        job->schedule_cursor_ms = 0;
    }
}

ScheduleSpec SpecOfJob(const AutomationJob& job) {
    ScheduleSpec spec;
    spec.kind = job.schedule_kind;
    spec.due_at_ms = job.due_at_ms;
    spec.interval_seconds = job.interval_seconds;
    spec.anchor_ms = job.anchor_ms;
    spec.cron_expr = job.cron_expr;
    spec.timezone = job.timezone;
    spec.misfire = job.misfire;
    return spec;
}

std::string LoopImportSourceKey(const std::string& session_id, const std::string& task_id) {
    return "loop:" + session_id + ":" + task_id;
}

}  // namespace

std::string ToString(AutomationJobState state) {
    switch (state) {
        case AutomationJobState::Active:
            return "active";
        case AutomationJobState::Paused:
            return "paused";
        case AutomationJobState::Cancelled:
            return "cancelled";
    }
    return "active";
}

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
            job.revision = GetJsonUint(line, "revision");
            job.idempotency_key = GetJsonString(line, "idempotencyKey");
            LoadJobLine(line, &job, nullptr);
            if (job.job_id.empty() || job.prompt.empty() || job.revision == 0) {
                ++projection.skipped_lines;
                continue;
            }
            const std::uint64_t counter = GetJsonUint(line, "jobCounter");
            if (counter > projection.job_counter) {
                projection.job_counter = counter;
            }
            if (!job.idempotency_key.empty()) {
                projection.create_keys[job.idempotency_key] = job.job_id;
            }
            projection.jobs[job.job_id] = std::move(job);
        } else if (type == kTypeJobUpdated) {
            const auto found = projection.jobs.find(GetJsonString(line, "jobId"));
            if (found == projection.jobs.end()) {
                ++projection.skipped_lines;
                continue;
            }
            // patch 键在则改,不在保持(纯追加:老行的键老读法)。
            AutomationJob& job = found->second;
            if (line.contains("prompt") && line["prompt"].is_string()) {
                job.prompt = line["prompt"].get<std::string>();
            }
            if (line.contains("dueAtMs") && line["dueAtMs"].is_number_integer()) {
                job.due_at_ms = line["dueAtMs"].get<std::int64_t>();
            }
            if (line.contains("intervalSeconds") && line["intervalSeconds"].is_number_integer()) {
                job.interval_seconds = line["intervalSeconds"].get<std::int64_t>();
            }
            if (line.contains("anchorMs") && line["anchorMs"].is_number_integer()) {
                job.anchor_ms = line["anchorMs"].get<std::int64_t>();
            }
            if (line.contains("cronExpr") && line["cronExpr"].is_string()) {
                job.cron_expr = line["cronExpr"].get<std::string>();
            }
            if (line.contains("timezone") && line["timezone"].is_string()) {
                job.timezone = line["timezone"].get<std::string>();
            }
            if (line.contains("misfirePolicy") && line["misfirePolicy"].is_string()) {
                MisfirePolicy policy = MisfirePolicy::Coalesce;
                if (ParseMisfirePolicy(line["misfirePolicy"].get<std::string>(), policy)) {
                    job.misfire = policy;
                }
            }
            if (line.contains("deadlineMs") && line["deadlineMs"].is_number_integer()) {
                job.deadline_ms = line["deadlineMs"].get<std::int64_t>();
            }
            if (line.contains("notifyOnChange") && line["notifyOnChange"].is_boolean()) {
                job.notify_on_change = line["notifyOnChange"].get<bool>();
            }
            const std::string kind_text = GetJsonString(line, "scheduleKind");
            if (!kind_text.empty()) {
                ScheduleKind kind = job.schedule_kind;
                if (ParseScheduleKind(kind_text, kind)) {
                    job.schedule_kind = kind;
                }
            }
            job.revision = GetJsonUint(line, "toRevision");
            const std::int64_t cursor = GetJsonInt(line, "cursorMs");
            if (cursor != 0) {
                job.schedule_cursor_ms = cursor;  // 改排即重锚(游标不回拨见下)
            }
            const std::string idem = GetJsonString(line, "idempotencyKey");
            if (!idem.empty()) {
                // 领域操作幂等键不进 create_keys(那是创建键);投影册不另
                // 建 op_keys——重放只求状态,回执查重归活账(写者唯一)。
            }
        } else if (type == kTypeJobPaused || type == kTypeJobResumed ||
                   type == kTypeJobCancelled) {
            const auto found = projection.jobs.find(GetJsonString(line, "jobId"));
            if (found == projection.jobs.end()) {
                ++projection.skipped_lines;
                continue;
            }
            if (type == kTypeJobPaused) {
                found->second.state = AutomationJobState::Paused;
            } else if (type == kTypeJobResumed) {
                found->second.state = AutomationJobState::Active;
                const std::int64_t cursor = GetJsonInt(line, "cursorThroughMs");
                if (cursor > found->second.schedule_cursor_ms) {
                    found->second.schedule_cursor_ms = cursor;
                }
            } else {
                found->second.state = AutomationJobState::Cancelled;
            }
        } else if (type == kTypeJobScheduleAdvanced) {
            const auto found = projection.jobs.find(GetJsonString(line, "jobId"));
            if (found == projection.jobs.end()) {
                ++projection.skipped_lines;
                continue;
            }
            const std::int64_t through = GetJsonInt(line, "throughSlotMs");
            if (through > found->second.schedule_cursor_ms) {
                found->second.schedule_cursor_ms = through;  // 游标只前进
            }
        } else if (type == kTypeOccurrenceCreated) {
            AutomationOccurrence occurrence;
            occurrence.occurrence_id = GetJsonString(line, "occurrenceId");
            occurrence.job_id = GetJsonString(line, "jobId");
            occurrence.slot_ms = GetJsonInt(line, "slotMs");
            occurrence.reason = GetJsonString(line, "reason");
            occurrence.missed_count = static_cast<std::uint32_t>(GetJsonUint(line, "missedCount"));
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
            const auto found =
                projection.occurrences.find(GetJsonString(line, "occurrenceId"));
            if (found == projection.occurrences.end()) {
                ++projection.skipped_lines;
                continue;
            }
            if (type == kTypeOccurrenceClaimed) {
                found->second.state = AutomationOccurrence::State::Claimed;
                found->second.owner_epoch = GetJsonString(line, "ownerEpoch");
                found->second.attempt = GetJsonUint(line, "attempt");
                found->second.claimed_at_ms = GetJsonInt(line, "claimedAtMs");
            } else {
                found->second.session_id = GetJsonString(line, "sessionId");
                found->second.turn_id = GetJsonString(line, "turnId");
            }
        } else if (type == kTypeOccurrenceSettled) {
            const auto found =
                projection.occurrences.find(GetJsonString(line, "occurrenceId"));
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
        } else if (type == kTypeOccurrenceMerged) {
            const auto found =
                projection.occurrences.find(GetJsonString(line, "occurrenceId"));
            if (found == projection.occurrences.end()) {
                ++projection.skipped_lines;
                continue;
            }
            found->second.missed_count =
                static_cast<std::uint32_t>(GetJsonUint(line, "missedCount"));
        } else if (type == kTypeOccurrenceRedispatched) {
            const auto found =
                projection.occurrences.find(GetJsonString(line, "occurrenceId"));
            if (found == projection.occurrences.end()) {
                ++projection.skipped_lines;
                continue;
            }
            found->second.state = AutomationOccurrence::State::Scheduled;
            found->second.attempt = GetJsonUint(line, "attempt");
            found->second.owner_epoch.clear();
            found->second.claimed_at_ms = 0;
        } else if (type == kTypeOccurrenceObserved) {
            const auto found =
                projection.occurrences.find(GetJsonString(line, "occurrenceId"));
            if (found == projection.occurrences.end()) {
                ++projection.skipped_lines;
                continue;
            }
            found->second.observed_sha = GetJsonString(line, "resultSha256");
            found->second.observed_changed = GetJsonBool(line, "changed");
            found->second.observed_delivered = GetJsonBool(line, "delivered");
            if (GetJsonBool(line, "updateLastObserved")) {
                const auto job = projection.jobs.find(found->second.job_id);
                if (job != projection.jobs.end()) {
                    job->second.last_observed_sha = found->second.observed_sha;
                }
            }
        } else if (type == kTypeOccurrenceCancelRequested) {
            const auto found =
                projection.occurrences.find(GetJsonString(line, "occurrenceId"));
            if (found == projection.occurrences.end()) {
                ++projection.skipped_lines;
                continue;
            }
            found->second.cancel_requested = true;
        } else if (type == kTypeLoopImported) {
            LoopImportReceipt receipt;
            receipt.receipt_id = GetJsonString(line, "receiptId");
            receipt.job_id = GetJsonString(line, "jobId");
            receipt.source_session_id = GetJsonString(line, "sourceSessionId");
            receipt.source_task_id = GetJsonString(line, "sourceTaskId");
            receipt.prompt_sha256 = GetJsonString(line, "promptSha256");
            receipt.interval_seconds = GetJsonInt(line, "intervalSeconds");
            receipt.imported_at_ms = GetJsonInt(line, "importedAtMs");
            receipt.idempotency_key = GetJsonString(line, "idempotencyKey");
            if (receipt.receipt_id.empty() || receipt.job_id.empty() ||
                receipt.source_session_id.empty() || receipt.source_task_id.empty()) {
                ++projection.skipped_lines;
                continue;
            }
            // 来源标记并进 job 投影(导入后的 /loop 状态只读留档,凭 receipt
            // 反查)。
            const auto imported_job = projection.jobs.find(receipt.job_id);
            if (imported_job != projection.jobs.end() &&
                imported_job->second.imported_from.empty()) {
                imported_job->second.imported_from =
                    LoopImportSourceKey(receipt.source_session_id, receipt.source_task_id);
            }
            projection.loop_imports[LoopImportSourceKey(receipt.source_session_id,
                                                       receipt.source_task_id)] =
                std::move(receipt);
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
    out->loop_imports_ = std::move(projection.loop_imports);
    out->job_counter_ = projection.job_counter;
    out->op_keys_.clear();  // 领域操作幂等回执只在活账内(写者唯一,不跨开)
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

bool AutomationStore::CheckOpKey(const std::string& idempotency_key, const std::string& verb,
                                 const std::string& job_id, JobReceipt* receipt) {
    if (idempotency_key.empty()) {
        return true;  // 没带键:不做查重(每次都当新操作)
    }
    const auto found = op_keys_.find(idempotency_key);
    if (found == op_keys_.end()) {
        return true;
    }
    if (found->second.verb == verb && found->second.job_id == job_id) {
        receipt->duplicate = true;  // 同键同操作同任务:回原回执
        receipt->job_id = job_id;
        receipt->revision = found->second.revision;
        return false;
    }
    receipt->error_code = "automation.revision_conflict";  // 同键异操作/异任务
    return false;
}

void AutomationStore::RegisterOpKey(const std::string& idempotency_key, const std::string& verb,
                                    const std::string& job_id, std::uint64_t revision) {
    if (!idempotency_key.empty()) {
        op_keys_[idempotency_key] = OpKeyEntry{verb, job_id, revision};
    }
}

AutomationStore::JobReceipt AutomationStore::CreateJob(const JobSpec& spec,
                                                        std::int64_t now_ms,
                                                        const std::string& idempotency_key) {
    JobReceipt receipt;
    // 规格校验:明拒不猜(continuation 归 V3 渠道线;坏 cron/坏时区当场拒)。
    if (spec.prompt.empty()) {
        receipt.error_code = "automation.schedule_invalid";
        receipt.error_code += ": prompt 为空";
        return receipt;
    }
    if (spec.session_policy != "fresh") {
        receipt.error_code = "automation.schedule_invalid";
        receipt.error_code += ": sessionPolicy 只认 fresh(continuation 归 V3 渠道线)";
        return receipt;
    }
    ScheduleSpec schedule;
    schedule.kind = spec.kind;
    schedule.due_at_ms = spec.due_at_ms;
    schedule.interval_seconds = spec.interval_seconds;
    schedule.anchor_ms = now_ms;  // interval 锚点 = 创建时刻(时间轴起点)
    schedule.cron_expr = spec.cron_expr;
    schedule.timezone = spec.timezone;
    schedule.misfire = spec.misfire;
    const std::string schedule_error = ValidateScheduleSpec(schedule);
    if (!schedule_error.empty()) {
        receipt.error_code = schedule_error.substr(0, schedule_error.find(':'));
        return receipt;
    }
    // 幂等键查重:同键同载荷回原回执,同键异载荷 conflict。
    if (!idempotency_key.empty()) {
        const auto found = create_keys_.find(idempotency_key);
        if (found != create_keys_.end()) {
            const auto original = jobs_.find(found->second);
            if (original != jobs_.end() && original->second.prompt == spec.prompt &&
                original->second.schedule_kind == spec.kind &&
                original->second.due_at_ms == spec.due_at_ms &&
                original->second.interval_seconds == spec.interval_seconds &&
                original->second.cron_expr == spec.cron_expr &&
                original->second.timezone == spec.timezone &&
                original->second.misfire == spec.misfire &&
                original->second.deadline_ms == spec.deadline_ms &&
                original->second.notify_on_change == spec.notify_on_change) {
                receipt.duplicate = true;
                receipt.job_id = original->second.job_id;
                return receipt;
            }
            receipt.error_code = "automation.revision_conflict";
            return receipt;
        }
    }
    std::string final_id = spec.job_id;
    if (final_id.empty()) {
        do {
            ++job_counter_;
            final_id = "job-" + std::to_string(job_counter_);
        } while (jobs_.find(final_id) != jobs_.end());
    } else if (jobs_.find(final_id) != jobs_.end()) {
        receipt.error_code = "automation.revision_conflict";  // 同名任务已存在
        return receipt;
    }
    // 游标初值:interval = 锚点,cron = 建账时刻(slot 严格在其后)。
    const std::int64_t cursor =
        spec.kind == ScheduleKind::Interval
            ? now_ms
            : (spec.kind == ScheduleKind::Cron ? now_ms : 0);
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeJobCreated;
    line["schemaVersion"] = 2;
    line["jobId"] = final_id;
    line["prompt"] = spec.prompt;
    line["scheduleKind"] = ToString(spec.kind);
    line["dueAtMs"] = spec.due_at_ms;
    line["revision"] = 1;
    line["createdAtMs"] = now_ms;
    line["jobCounter"] = job_counter_;
    line["timezone"] = spec.timezone;
    line["misfirePolicy"] = ToString(spec.misfire);
    line["deadlineMs"] = spec.deadline_ms;
    line["notifyOnChange"] = spec.notify_on_change;
    line["sessionPolicy"] = spec.session_policy;
    if (spec.kind == ScheduleKind::Interval) {
        line["intervalSeconds"] = spec.interval_seconds;
        line["anchorMs"] = now_ms;
    } else if (spec.kind == ScheduleKind::Cron) {
        line["cronExpr"] = spec.cron_expr;
    }
    if (spec.kind != ScheduleKind::Once) {
        line["cursorMs"] = cursor;
    }
    if (!idempotency_key.empty()) line["idempotencyKey"] = idempotency_key;
    // once:首枚 occurrence 同笔落(slot = due_at_ms,计划内时间)——V1
    // 语义不变。interval/cron:不建 occurrence,归 SweepSchedule。
    std::string occurrence_id;
    nlohmann::json occurrence_line;
    if (spec.kind == ScheduleKind::Once) {
        occurrence_id = MakeOccurrenceId(final_id, 1, spec.due_at_ms);
        occurrence_line = nlohmann::json::object();
        occurrence_line["type"] = kTypeOccurrenceCreated;
        occurrence_line["schemaVersion"] = 1;
        occurrence_line["jobId"] = final_id;
        occurrence_line["occurrenceId"] = occurrence_id;
        occurrence_line["slotMs"] = spec.due_at_ms;
        occurrence_line["reason"] = "schedule";
        if (!idempotency_key.empty()) occurrence_line["idempotencyKey"] = idempotency_key;
    }
    if (!AppendLinePowerLoss(line) ||
        (spec.kind == ScheduleKind::Once && !AppendLinePowerLoss(occurrence_line))) {
        receipt.error_code = "automation.append_failed";
        return receipt;
    }
    AutomationJob job;
    job.job_id = final_id;
    job.prompt = spec.prompt;
    job.due_at_ms = spec.due_at_ms;
    job.revision = 1;
    job.created_at_ms = now_ms;
    job.idempotency_key = idempotency_key;
    job.schedule_kind = spec.kind;
    job.interval_seconds = spec.interval_seconds;
    job.anchor_ms = spec.kind == ScheduleKind::Interval ? now_ms : 0;
    job.cron_expr = spec.cron_expr;
    job.timezone = spec.timezone;
    job.misfire = spec.misfire;
    job.deadline_ms = spec.deadline_ms;
    job.notify_on_change = spec.notify_on_change;
    job.session_policy = spec.session_policy;
    job.schedule_cursor_ms = cursor;
    jobs_[final_id] = std::move(job);
    if (spec.kind == ScheduleKind::Once) {
        AutomationOccurrence occurrence;
        occurrence.occurrence_id = occurrence_id;
        occurrence.job_id = final_id;
        occurrence.slot_ms = spec.due_at_ms;
        occurrence.reason = "schedule";
        occurrences_[occurrence_id] = occurrence;
        receipt.occurrence_id = occurrence_id;
    }
    if (!idempotency_key.empty()) {
        create_keys_[idempotency_key] = final_id;
    }
    receipt.accepted = true;
    receipt.job_id = final_id;
    receipt.revision = 1;
    return receipt;
}

AutomationStore::JobReceipt AutomationStore::CreateOnceJob(const std::string& job_id,
                                                            const std::string& prompt,
                                                            std::int64_t due_at_ms,
                                                            std::int64_t now_ms,
                                                            const std::string& idempotency_key) {
    JobSpec spec;
    spec.job_id = job_id;
    spec.prompt = prompt;
    spec.kind = ScheduleKind::Once;
    spec.due_at_ms = due_at_ms;
    return CreateJob(spec, now_ms, idempotency_key);
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
    if (job->second.state == AutomationJobState::Cancelled) {
        receipt.error_code = "automation.job_terminal";  // 已取消:不再触发
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
    occurrence.reason = "run_now";
    occurrences_[occurrence_id] = occurrence;
    if (!idempotency_key.empty()) {
        runnow_keys_[idempotency_key] = occurrence_id;
    }
    receipt.accepted = true;
    receipt.job_id = job_id;
    receipt.occurrence_id = occurrence_id;
    return receipt;
}

AutomationStore::JobReceipt AutomationStore::ImportLoop(const std::string& source_session_id,
                                                         const std::string& source_task_id,
                                                         const std::string& prompt,
                                                         std::int64_t interval_seconds,
                                                         std::int64_t now_ms,
                                                         const std::string& idempotency_key) {
    JobReceipt receipt;
    if (source_session_id.empty() || source_task_id.empty()) {
        receipt.error_code = "automation.schedule_invalid";
        receipt.error_code += ": 导入须带来源 sessionId 与 taskId";
        return receipt;
    }
    // 来源幂等:同来源已导过 → 回原 receipt,不建第二个 job(不双跑)。
    const std::string source_key = LoopImportSourceKey(source_session_id, source_task_id);
    const auto existing = loop_imports_.find(source_key);
    if (existing != loop_imports_.end()) {
        receipt.duplicate = true;
        receipt.job_id = existing->second.job_id;
        receipt.receipt_id = existing->second.receipt_id;
        receipt.revision = 1;
        return receipt;
    }
    // 建账:interval job,prompt 固定副本(不逐拍现读原 /loop 状态)。
    JobSpec spec;
    spec.prompt = prompt;
    spec.kind = ScheduleKind::Interval;
    spec.interval_seconds = interval_seconds;
    const JobReceipt created = CreateJob(spec, now_ms, idempotency_key);
    if (!created.accepted) {
        receipt.error_code =
            created.error_code.empty() ? std::string("automation.import_conflict")
                                       : created.error_code;
        return receipt;
    }
    // receipt 行(建账行之后落;两行都在才算导入成——崩在中间:job 已建
    // 而 receipt 未落,重导时来源键查无 receipt 会再建一个。防双跑由幂等
    // 键(idempotencyKey)兜底:重导同键回原 job。裸重导(不带键)在极窄
    // 窗内可能出两个 job——receipt 未落稳属账 broken 一类,如实分账。)
    const std::string receipt_id =
        "imp-" + platform::Sha256Hex(source_session_id + "\n" + source_task_id).substr(0, 16);
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeLoopImported;
    line["schemaVersion"] = 1;
    line["receiptId"] = receipt_id;
    line["jobId"] = created.job_id;
    line["sourceSessionId"] = source_session_id;
    line["sourceTaskId"] = source_task_id;
    line["promptSha256"] = platform::Sha256Hex(prompt);
    line["intervalSeconds"] = interval_seconds;
    line["importedAtMs"] = now_ms;
    if (!idempotency_key.empty()) line["idempotencyKey"] = idempotency_key;
    if (!AppendLinePowerLoss(line)) {
        receipt.error_code = "automation.append_failed";
        receipt.job_id = created.job_id;
        return receipt;
    }
    // job 行补来源标记(纯追加行,重放侧并进 job 投影)。
    {
        const auto job = jobs_.find(created.job_id);
        if (job != jobs_.end()) {
            job->second.imported_from = source_key;
        }
    }
    LoopImportReceipt saved;
    saved.receipt_id = receipt_id;
    saved.job_id = created.job_id;
    saved.source_session_id = source_session_id;
    saved.source_task_id = source_task_id;
    saved.prompt_sha256 = platform::Sha256Hex(prompt);
    saved.interval_seconds = interval_seconds;
    saved.imported_at_ms = now_ms;
    saved.idempotency_key = idempotency_key;
    loop_imports_[source_key] = saved;
    receipt.accepted = true;
    receipt.job_id = created.job_id;
    receipt.occurrence_id = created.occurrence_id;
    receipt.receipt_id = receipt_id;
    receipt.revision = 1;
    return receipt;
}

std::optional<LoopImportReceipt> AutomationStore::FindLoopImport(
    const std::string& source_session_id, const std::string& source_task_id) const {
    const auto found = loop_imports_.find(LoopImportSourceKey(source_session_id, source_task_id));
    if (found == loop_imports_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<LoopImportReceipt> AutomationStore::ListLoopImports() const {
    std::vector<LoopImportReceipt> receipts;
    receipts.reserve(loop_imports_.size());
    for (const auto& [key, receipt] : loop_imports_) {
        receipts.push_back(receipt);
    }
    return receipts;
}

AutomationStore::JobReceipt AutomationStore::UpdateJob(const std::string& job_id,
                                                        std::uint64_t expected_revision,
                                                        const JobUpdatePatch& patch,
                                                        std::int64_t now_ms,
                                                        const std::string& idempotency_key) {
    JobReceipt receipt;
    const auto found = jobs_.find(job_id);
    if (found == jobs_.end()) {
        receipt.error_code = "automation.job_not_found";
        return receipt;
    }
    if (!CheckOpKey(idempotency_key, "update", job_id, &receipt)) {
        return receipt;
    }
    AutomationJob& job = found->second;
    if (job.state == AutomationJobState::Cancelled) {
        receipt.error_code = "automation.job_terminal";  // 已取消:不再改
        return receipt;
    }
    if (expected_revision == 0 || expected_revision != job.revision) {
        receipt.error_code = "automation.revision_conflict";  // CAS:必须显式且相等
        return receipt;
    }
    // 合成新规格再整体校验(明拒不猜)。
    AutomationJob next = job;
    bool re_anchor = false;
    if (patch.set_prompt) next.prompt = patch.prompt;
    if (patch.set_due_at) {
        next.due_at_ms = patch.due_at_ms;
        next.schedule_kind = ScheduleKind::Once;
    }
    if (patch.set_interval) {
        next.interval_seconds = patch.interval_seconds;
        next.schedule_kind = ScheduleKind::Interval;
        next.anchor_ms = now_ms;  // 改周期 = 时间轴重排(锚点重定)
        re_anchor = true;
    }
    if (patch.set_cron) {
        next.cron_expr = patch.cron_expr;
        next.schedule_kind = ScheduleKind::Cron;
        re_anchor = true;
    }
    if (patch.set_timezone) next.timezone = patch.timezone;
    if (patch.set_misfire) next.misfire = patch.misfire;
    if (patch.set_deadline) next.deadline_ms = patch.deadline_ms;
    if (patch.set_notify_on_change) next.notify_on_change = patch.notify_on_change;
    if (patch.set_prompt && next.prompt.empty()) {
        receipt.error_code = "automation.schedule_invalid";
        receipt.error_code += ": prompt 为空";
        return receipt;
    }
    const std::string schedule_error = ValidateScheduleSpec(SpecOfJob(next));
    if (!schedule_error.empty()) {
        receipt.error_code = schedule_error.substr(0, schedule_error.find(':'));
        return receipt;
    }
    const std::uint64_t new_revision = job.revision + 1;
    if (re_anchor) {
        next.schedule_cursor_ms = now_ms;  // 改排即重锚(旧 occurrence 固定旧 revision,不动)
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeJobUpdated;
    line["schemaVersion"] = 2;
    line["jobId"] = job_id;
    line["fromRevision"] = job.revision;
    line["toRevision"] = new_revision;
    line["updatedAtMs"] = now_ms;
    if (patch.set_prompt) line["prompt"] = next.prompt;
    if (patch.set_due_at) {
        line["scheduleKind"] = "once";
        line["dueAtMs"] = next.due_at_ms;
    }
    if (patch.set_interval) {
        line["scheduleKind"] = "interval";
        line["intervalSeconds"] = next.interval_seconds;
        line["anchorMs"] = next.anchor_ms;
    }
    if (patch.set_cron) {
        line["scheduleKind"] = "cron";
        line["cronExpr"] = next.cron_expr;
    }
    if (patch.set_timezone) line["timezone"] = next.timezone;
    if (patch.set_misfire) line["misfirePolicy"] = ToString(next.misfire);
    if (patch.set_deadline) line["deadlineMs"] = next.deadline_ms;
    if (patch.set_notify_on_change) line["notifyOnChange"] = next.notify_on_change;
    if (re_anchor) line["cursorMs"] = next.schedule_cursor_ms;
    if (!idempotency_key.empty()) line["idempotencyKey"] = idempotency_key;
    if (!AppendLinePowerLoss(line)) {
        receipt.error_code = "automation.append_failed";
        return receipt;
    }
    job = std::move(next);
    job.revision = new_revision;
    RegisterOpKey(idempotency_key, "update", job_id, new_revision);
    receipt.accepted = true;
    receipt.job_id = job_id;
    receipt.revision = new_revision;
    return receipt;
}

AutomationStore::JobReceipt AutomationStore::PauseJob(const std::string& job_id,
                                                       std::uint64_t expected_revision,
                                                       std::int64_t now_ms,
                                                       const std::string& idempotency_key) {
    JobReceipt receipt;
    const auto found = jobs_.find(job_id);
    if (found == jobs_.end()) {
        receipt.error_code = "automation.job_not_found";
        return receipt;
    }
    if (!CheckOpKey(idempotency_key, "pause", job_id, &receipt)) {
        return receipt;
    }
    AutomationJob& job = found->second;
    if (job.state == AutomationJobState::Cancelled) {
        receipt.error_code = "automation.job_terminal";
        return receipt;
    }
    if (job.state == AutomationJobState::Paused) {
        receipt.duplicate = true;  // 已暂停:幂等回执,不空耗 revision
        receipt.job_id = job_id;
        receipt.revision = job.revision;
        return receipt;
    }
    if (expected_revision == 0 || expected_revision != job.revision) {
        receipt.error_code = "automation.revision_conflict";
        return receipt;
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeJobPaused;
    line["schemaVersion"] = 2;
    line["jobId"] = job_id;
    line["revision"] = job.revision;
    line["pausedAtMs"] = now_ms;
    if (!idempotency_key.empty()) line["idempotencyKey"] = idempotency_key;
    if (!AppendLinePowerLoss(line)) {
        receipt.error_code = "automation.append_failed";
        return receipt;
    }
    job.state = AutomationJobState::Paused;
    RegisterOpKey(idempotency_key, "pause", job_id, job.revision);
    receipt.accepted = true;
    receipt.job_id = job_id;
    receipt.revision = job.revision;
    return receipt;
}

AutomationStore::JobReceipt AutomationStore::ResumeJob(const std::string& job_id,
                                                        std::uint64_t expected_revision,
                                                        std::int64_t now_ms,
                                                        const std::string& idempotency_key) {
    JobReceipt receipt;
    const auto found = jobs_.find(job_id);
    if (found == jobs_.end()) {
        receipt.error_code = "automation.job_not_found";
        return receipt;
    }
    if (!CheckOpKey(idempotency_key, "resume", job_id, &receipt)) {
        return receipt;
    }
    AutomationJob& job = found->second;
    if (job.state == AutomationJobState::Cancelled) {
        receipt.error_code = "automation.job_terminal";
        return receipt;
    }
    if (job.state == AutomationJobState::Active) {
        receipt.duplicate = true;  // 本来就在跑:幂等回执
        receipt.job_id = job_id;
        receipt.revision = job.revision;
        return receipt;
    }
    if (expected_revision == 0 || expected_revision != job.revision) {
        receipt.error_code = "automation.revision_conflict";
        return receipt;
    }
    // paused 窗口的拍不补跑(§七"paused/disabled 不补跑"):游标直进 now。
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeJobResumed;
    line["schemaVersion"] = 2;
    line["jobId"] = job_id;
    line["revision"] = job.revision;
    line["resumedAtMs"] = now_ms;
    line["cursorThroughMs"] = now_ms;
    if (!idempotency_key.empty()) line["idempotencyKey"] = idempotency_key;
    if (!AppendLinePowerLoss(line)) {
        receipt.error_code = "automation.append_failed";
        return receipt;
    }
    job.state = AutomationJobState::Active;
    if (now_ms > job.schedule_cursor_ms) {
        job.schedule_cursor_ms = now_ms;
    }
    RegisterOpKey(idempotency_key, "resume", job_id, job.revision);
    receipt.accepted = true;
    receipt.job_id = job_id;
    receipt.revision = job.revision;
    return receipt;
}

AutomationStore::JobReceipt AutomationStore::CancelJob(const std::string& job_id,
                                                        std::uint64_t expected_revision,
                                                        std::int64_t now_ms,
                                                        const std::string& idempotency_key) {
    JobReceipt receipt;
    const auto found = jobs_.find(job_id);
    if (found == jobs_.end()) {
        receipt.error_code = "automation.job_not_found";
        return receipt;
    }
    if (!CheckOpKey(idempotency_key, "cancel", job_id, &receipt)) {
        return receipt;
    }
    AutomationJob& job = found->second;
    if (job.state == AutomationJobState::Cancelled) {
        receipt.duplicate = true;  // 已取消:幂等回执,历史保留
        receipt.job_id = job_id;
        receipt.revision = job.revision;
        return receipt;
    }
    if (expected_revision == 0 || expected_revision != job.revision) {
        receipt.error_code = "automation.revision_conflict";
        return receipt;
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeJobCancelled;
    line["schemaVersion"] = 2;
    line["jobId"] = job_id;
    line["revision"] = job.revision;
    line["cancelledAtMs"] = now_ms;
    if (!idempotency_key.empty()) line["idempotencyKey"] = idempotency_key;
    if (!AppendLinePowerLoss(line)) {
        receipt.error_code = "automation.append_failed";
        return receipt;
    }
    job.state = AutomationJobState::Cancelled;
    // 先停未来派发:scheduled 的就地结算 cancelled(历史保留,不删账)。
    // 再处理在飞(claimed)的:标取消边界——执行收完按事实结算,恢复路
    // 对无绑定的取消件直接结算 cancelled。
    for (auto& [id, occurrence] : occurrences_) {
        if (occurrence.job_id != job_id) continue;
        if (occurrence.state == AutomationOccurrence::State::Scheduled) {
            (void)SettleOccurrence(id, "cancelled", "job_cancelled", now_ms);
        } else if (occurrence.state == AutomationOccurrence::State::Claimed) {
            nlohmann::json mark = nlohmann::json::object();
            mark["type"] = kTypeOccurrenceCancelRequested;
            mark["schemaVersion"] = 1;
            mark["occurrenceId"] = id;
            mark["jobId"] = job_id;
            mark["requestedAtMs"] = now_ms;
            if (!AppendLinePowerLoss(mark)) {
                receipt.error_code = "automation.append_failed";
                return receipt;
            }
            occurrence.cancel_requested = true;
        }
    }
    RegisterOpKey(idempotency_key, "cancel", job_id, job.revision);
    receipt.accepted = true;
    receipt.job_id = job_id;
    receipt.revision = job.revision;
    return receipt;
}

AutomationStore::SweepResult AutomationStore::SweepSchedule(std::int64_t now_ms) {
    SweepResult result;
    if (broken_) {
        result.ok = false;
        return result;
    }
    std::size_t open_count = OpenOccurrenceCount();
    for (auto& [job_id, job] : jobs_) {
        if (job.state != AutomationJobState::Active) continue;  // paused 不补跑
        if (job.schedule_kind == ScheduleKind::Once) continue;  // once 无周期生成
        if (job.deadline_ms > 0 && now_ms >= job.deadline_ms) continue;  // 过线不派
        // 同 job 不重叠:有 claimed 未结算的活儿 → 不生成、游标不动(下一
        // 拍等它收口后合并补)。
        bool has_claimed = false;
        bool has_scheduled = false;
        std::string scheduled_id;
        for (const auto& [id, occurrence] : occurrences_) {
            if (occurrence.job_id != job_id) continue;
            if (occurrence.state == AutomationOccurrence::State::Claimed) {
                has_claimed = true;
            } else if (occurrence.state == AutomationOccurrence::State::Scheduled) {
                has_scheduled = true;
                if (scheduled_id.empty() || occurrence.slot_ms <
                                               occurrences_[scheduled_id].slot_ms) {
                    scheduled_id = id;
                }
            }
        }
        if (has_claimed) continue;
        // 收集游标之后的到期拍(≤ now)。
        const ScheduleSpec spec = SpecOfJob(job);
        std::vector<std::int64_t> slots;
        std::int64_t cursor = job.schedule_cursor_ms;
        while (slots.size() < 100000) {  // sanity 帽:interval 1s x 久停也封顶
            const NextFire next = FirstSlotAfter(spec, cursor);
            if (!next.found || next.utc_ms > now_ms) break;
            slots.push_back(next.utc_ms);
            cursor = next.utc_ms;
        }
        if (slots.empty()) continue;
        const std::int64_t last_slot = slots.back();
        if (job.misfire == MisfirePolicy::Skip) {
            // skip:被后继拍顶掉的(迟到)拍不补不并,游标直进;最近一拍
            // 在宽限内仍算"当前拍"照跑——迟到判定 = now - slot > 宽限
            //(interval 取 min(周期, 60s),cron 固定 60s:泵轮询粒度量级)。
            // 已有 scheduled 待办占位则本轮不建(单待办语义,待办顶着跑)。
            const std::int64_t grace_ms =
                job.schedule_kind == ScheduleKind::Interval
                    ? std::min<std::int64_t>(spec.interval_seconds * 1000, 60000)
                    : 60000;
            const bool last_current = (now_ms - last_slot) <= grace_ms;
            const std::size_t dropped = last_current ? slots.size() - 1 : slots.size();
            if (last_current && !has_scheduled && open_count < max_open_occurrences_) {
                const std::string occurrence_id =
                    MakeOccurrenceId(job_id, job.revision, last_slot);
                if (occurrences_.find(occurrence_id) == occurrences_.end()) {
                    nlohmann::json created = nlohmann::json::object();
                    created["type"] = kTypeOccurrenceCreated;
                    created["schemaVersion"] = 2;
                    created["jobId"] = job_id;
                    created["occurrenceId"] = occurrence_id;
                    created["slotMs"] = last_slot;
                    created["reason"] = "schedule";
                    created["missedCount"] = dropped;  // 覆盖范围账
                    created["revision"] = job.revision;
                    if (!AppendLinePowerLoss(created)) {
                        result.ok = false;
                        return result;
                    }
                    AutomationOccurrence occurrence;
                    occurrence.occurrence_id = occurrence_id;
                    occurrence.job_id = job_id;
                    occurrence.slot_ms = last_slot;
                    occurrence.reason = "schedule";
                    occurrence.missed_count = static_cast<std::uint32_t>(dropped);
                    occurrences_[occurrence_id] = occurrence;
                    ++open_count;
                    ++result.generated;
                }
            }
            nlohmann::json advance = nlohmann::json::object();
            advance["type"] = kTypeJobScheduleAdvanced;
            advance["schemaVersion"] = 2;
            advance["jobId"] = job_id;
            advance["revision"] = job.revision;
            advance["throughSlotMs"] = last_slot;
            advance["atMs"] = now_ms;
            advance["policy"] = "skip";
            if (!AppendLinePowerLoss(advance)) {
                result.ok = false;
                return result;
            }
            job.schedule_cursor_ms = last_slot;
            result.skipped_slots += dropped;
            continue;
        }
        // coalesce(默认):合并补一拍。已有 scheduled 待办 → 并进它;没有
        // → 新建一枚(最老一拍,occurrenceId 按计划内 slot 定式)。
        if (has_scheduled) {
            AutomationOccurrence& pending = occurrences_[scheduled_id];
            nlohmann::json merged = nlohmann::json::object();
            merged["type"] = kTypeOccurrenceMerged;
            merged["schemaVersion"] = 1;
            merged["occurrenceId"] = scheduled_id;
            merged["jobId"] = job_id;
            merged["throughSlotMs"] = last_slot;
            merged["missedCount"] =
                static_cast<std::uint64_t>(pending.missed_count) + slots.size();
            merged["atMs"] = now_ms;
            if (!AppendLinePowerLoss(merged)) {
                result.ok = false;
                return result;
            }
            pending.missed_count += static_cast<std::uint32_t>(slots.size());
            result.merged += slots.size();
        } else {
            if (open_count >= max_open_occurrences_) {
                result.stalled = true;  // 队列帽满:生成停,游标不动,下轮重试
                continue;
            }
            const std::int64_t first_slot = slots.front();
            const std::string occurrence_id =
                MakeOccurrenceId(job_id, job.revision, first_slot);
            const auto existing = occurrences_.find(occurrence_id);
            const bool existing_scheduled =
                existing != occurrences_.end() &&
                existing->second.state == AutomationOccurrence::State::Scheduled;
            const bool existing_other =
                existing != occurrences_.end() && !existing_scheduled;
            if (existing_scheduled) {
                // 防御:同 id 已在等 claim(半笔 append 后重放等)→ 并进,
                // 不双建。
                nlohmann::json merged = nlohmann::json::object();
                merged["type"] = kTypeOccurrenceMerged;
                merged["schemaVersion"] = 1;
                merged["occurrenceId"] = occurrence_id;
                merged["jobId"] = job_id;
                merged["throughSlotMs"] = last_slot;
                merged["missedCount"] = static_cast<std::uint64_t>(
                                             existing->second.missed_count) +
                                         slots.size();
                merged["atMs"] = now_ms;
                if (!AppendLinePowerLoss(merged)) {
                    result.ok = false;
                    return result;
                }
                existing->second.missed_count +=
                    static_cast<std::uint32_t>(slots.size());
                result.merged += slots.size();
            } else if (existing_other) {
                // 已结算/在执行的同 id(理论到不了:游标只前进)——不覆盖
                // 既有事实,只推游标。
            } else {
                nlohmann::json created = nlohmann::json::object();
                created["type"] = kTypeOccurrenceCreated;
                created["schemaVersion"] = 2;
                created["jobId"] = job_id;
                created["occurrenceId"] = occurrence_id;
                created["slotMs"] = first_slot;
                created["reason"] = "schedule";
                created["missedCount"] = slots.size() - 1;  // 覆盖范围账
                created["revision"] = job.revision;
                if (!AppendLinePowerLoss(created)) {
                    result.ok = false;
                    return result;
                }
                AutomationOccurrence occurrence;
                occurrence.occurrence_id = occurrence_id;
                occurrence.job_id = job_id;
                occurrence.slot_ms = first_slot;
                occurrence.reason = "schedule";
                occurrence.missed_count = static_cast<std::uint32_t>(slots.size() - 1);
                occurrences_[occurrence_id] = occurrence;
                ++open_count;
                ++result.generated;
            }
        }
        nlohmann::json advance = nlohmann::json::object();
        advance["type"] = kTypeJobScheduleAdvanced;
        advance["schemaVersion"] = 2;
        advance["jobId"] = job_id;
        advance["revision"] = job.revision;
        advance["throughSlotMs"] = last_slot;
        advance["atMs"] = now_ms;
        advance["policy"] = "coalesce";
        if (!AppendLinePowerLoss(advance)) {
            result.ok = false;
            return result;
        }
        job.schedule_cursor_ms = last_slot;
    }
    return result;
}

std::optional<AutomationOccurrence> AutomationStore::ClaimDue(const std::string& owner_epoch,
                                                               std::int64_t now_ms) {
    if (broken_) {
        return std::nullopt;
    }
    // 先清不派发的:cancelled 任务的 scheduled 就地结算 cancelled(防御
    // ——CancelJob 已结算过,这里盖恢复窗);过 deadline 的结算 cancelled
    // (不判 failed、不再执行)。
    for (const auto& [id, occurrence] : occurrences_) {
        if (occurrence.state != AutomationOccurrence::State::Scheduled) continue;
        const auto job = jobs_.find(occurrence.job_id);
        if (job == jobs_.end()) continue;
        if (job->second.state == AutomationJobState::Cancelled) {
            (void)SettleOccurrence(id, "cancelled", "job_cancelled", now_ms);
            continue;
        }
        if (job->second.deadline_ms > 0 && now_ms >= job->second.deadline_ms) {
            (void)SettleOccurrence(id, "cancelled", "deadline_reached", now_ms);
        }
    }
    if (broken_) {
        return std::nullopt;
    }
    // due = scheduled 且 slot <= now 且任务 active(paused 的原地等)。
    // 挑 slot 最老的(FIFO)。
    std::string picked_id;
    for (const auto& [id, occurrence] : occurrences_) {
        if (occurrence.state != AutomationOccurrence::State::Scheduled) continue;
        if (occurrence.slot_ms > now_ms) continue;
        const auto job = jobs_.find(occurrence.job_id);
        if (job == jobs_.end() || job->second.state != AutomationJobState::Active) continue;
        if (job->second.deadline_ms > 0 && now_ms >= job->second.deadline_ms) continue;
        if (picked_id.empty() || occurrence.slot_ms < occurrences_[picked_id].slot_ms) {
            picked_id = id;
        }
    }
    if (picked_id.empty()) {
        return std::nullopt;
    }
    AutomationOccurrence& occurrence = occurrences_[picked_id];
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeOccurrenceClaimed;
    line["schemaVersion"] = 2;
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
    line["schemaVersion"] = 2;
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

bool AutomationStore::RedispatchOccurrence(const std::string& occurrence_id,
                                           const std::string& reason, std::int64_t now_ms) {
    auto found = occurrences_.find(occurrence_id);
    if (found == occurrences_.end()) {
        return false;
    }
    AutomationOccurrence& occurrence = found->second;
    // 只重派"claim 后无开轮事实"的:无绑定行 = 未开轮(绑定先于 V3
    // work.bound 与一切模型/工具动作)。有绑定的按 V3 账裁决,不重派。
    if (occurrence.state != AutomationOccurrence::State::Claimed ||
        !occurrence.session_id.empty() || !occurrence.turn_id.empty()) {
        return false;
    }
    if (occurrence.attempt + 1 > kMaxAttempts) {
        return false;  // attempt 帽到顶:调用方按 needs_review 收
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeOccurrenceRedispatched;
    line["schemaVersion"] = 1;
    line["occurrenceId"] = occurrence_id;
    line["jobId"] = occurrence.job_id;
    line["attempt"] = occurrence.attempt + 1;
    line["reason"] = reason;
    line["atMs"] = now_ms;
    if (!AppendLinePowerLoss(line)) {
        return false;
    }
    occurrence.state = AutomationOccurrence::State::Scheduled;
    occurrence.attempt += 1;
    occurrence.owner_epoch.clear();
    occurrence.claimed_at_ms = 0;
    return true;
}

bool AutomationStore::RecordObservation(const std::string& occurrence_id,
                                        const std::string& result_sha, bool changed,
                                        bool delivered, bool update_last_observed,
                                        std::int64_t now_ms) {
    auto found = occurrences_.find(occurrence_id);
    if (found == occurrences_.end()) {
        return false;
    }
    AutomationOccurrence& occurrence = found->second;
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeOccurrenceObserved;
    line["schemaVersion"] = 1;
    line["occurrenceId"] = occurrence_id;
    line["jobId"] = occurrence.job_id;
    line["resultSha256"] = result_sha;
    line["changed"] = changed;
    line["delivered"] = delivered;
    line["updateLastObserved"] = update_last_observed;
    line["observedAtMs"] = now_ms;
    if (!AppendLinePowerLoss(line)) {
        return false;
    }
    occurrence.observed_sha = result_sha;
    occurrence.observed_changed = changed;
    occurrence.observed_delivered = delivered;
    if (update_last_observed) {
        const auto job = jobs_.find(occurrence.job_id);
        if (job != jobs_.end()) {
            job->second.last_observed_sha = result_sha;
        }
    }
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

std::vector<AutomationOccurrence> AutomationStore::ListJobOccurrences(
    const std::string& job_id) const {
    std::vector<AutomationOccurrence> occurrences;
    for (const auto& [id, occurrence] : occurrences_) {
        if (occurrence.job_id == job_id) {
            occurrences.push_back(occurrence);
        }
    }
    std::sort(occurrences.begin(), occurrences.end(),
              [](const AutomationOccurrence& a, const AutomationOccurrence& b) {
                  return a.slot_ms < b.slot_ms;
              });
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

std::size_t AutomationStore::OpenOccurrenceCount() const {
    std::size_t count = 0;
    for (const auto& [id, occurrence] : occurrences_) {
        if (occurrence.state == AutomationOccurrence::State::Scheduled ||
            occurrence.state == AutomationOccurrence::State::Claimed) {
            ++count;
        }
    }
    return count;
}

}  // namespace lubancode::gateway
