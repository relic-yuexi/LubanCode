#include "gateway/status.hpp"

#include <algorithm>
#include <fstream>

#include "gateway/automation_store.hpp"
#include "gateway/reply_outbox.hpp"
#include "platform/paths.hpp"
#include "platform/wall_clock.hpp"
#include "trajectory/session_lock.hpp"

namespace lubancode::gateway {

namespace {

std::optional<GatewayLockRecord> ReadLockRecord(const std::filesystem::path& lock_file,
                                                std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(lock_file, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream stream(lock_file, std::ios::binary);
    if (!stream) {
        if (error != nullptr) *error = "锁文件在,但打不开";
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        if (error != nullptr) *error = "锁文件是空的";
        return std::nullopt;
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(text);
        std::string parse_error;
        auto record = GatewayLockRecord::FromJsonStrict(parsed, &parse_error);
        if (!record.has_value() && error != nullptr) *error = "锁账读不懂: " + parse_error;
        return record;
    } catch (const nlohmann::json::exception&) {
        if (error != nullptr) *error = "锁文件不是合法 JSON(可能写了一半)";
        return std::nullopt;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// V1 分栏:三本领域账的只读投影
// ---------------------------------------------------------------------------

GatewayStatusSections ProbeStatusSections(const GatewayProfilePaths& paths) {
    GatewayStatusSections sections;
    // work 栏:automation 账。
    {
        const AutomationProjection projection = ReadAutomationProjection(paths.automation_log);
        std::error_code ec;
        sections.work_ledger_present =
            std::filesystem::exists(paths.automation_log, ec) && !ec;
        sections.jobs_total = projection.jobs.size();
        const std::int64_t now_ms = platform::WallClockNowMs();
        for (const auto& [id, occurrence] : projection.occurrences) {
            switch (occurrence.state) {
                case AutomationOccurrence::State::Scheduled:
                    if (occurrence.slot_ms <= now_ms) {
                        ++sections.occurrences_due;
                    }
                    break;
                case AutomationOccurrence::State::Claimed:
                    ++sections.occurrences_in_flight;
                    break;
                case AutomationOccurrence::State::Settled:
                    if (occurrence.outcome == "succeeded") {
                        ++sections.occurrences_succeeded;
                    } else if (occurrence.outcome == "failed") {
                        ++sections.occurrences_failed;
                    } else {
                        ++sections.occurrences_needs_review;
                    }
                    break;
            }
        }
        // execution 栏:最近 5 枚(倒序;settled 与 in_flight 都算"跑过/在跑",
        // scheduled 不进)。
        struct Orderly {
            std::int64_t key;
            GatewayStatusSections::ExecutionEntry entry;
        };
        std::vector<Orderly> orderly;
        for (const auto& [id, occurrence] : projection.occurrences) {
            if (occurrence.state == AutomationOccurrence::State::Scheduled) continue;
            Orderly item;
            item.key = occurrence.state == AutomationOccurrence::State::Settled
                           ? occurrence.settled_at_ms
                           : occurrence.claimed_at_ms;
            item.entry.occurrence_id = occurrence.occurrence_id;
            item.entry.job_id = occurrence.job_id;
            item.entry.session_id = occurrence.session_id;
            item.entry.turn_id = occurrence.turn_id;
            item.entry.outcome = occurrence.outcome;
            orderly.push_back(std::move(item));
        }
        std::sort(orderly.begin(), orderly.end(),
                  [](const Orderly& a, const Orderly& b) { return a.key > b.key; });
        for (std::size_t i = 0; i < orderly.size() && i < 5; ++i) {
            sections.recent_executions.push_back(std::move(orderly[i].entry));
        }
    }
    // delivery 栏:outbox 账。
    {
        const OutboxProjection projection = ReadOutboxProjection(paths.outbox_log);
        std::error_code ec;
        sections.delivery_ledger_present =
            std::filesystem::exists(paths.outbox_log, ec) && !ec;
        for (const auto& [id, item] : projection.items) {
            if (item.state == "pending") {
                ++sections.delivery_pending;
                sections.pending_delivery_ids.push_back(id);
            } else if (item.state == "delivered") {
                ++sections.delivery_delivered;
            } else {
                ++sections.delivery_flagged;
            }
        }
    }
    return sections;
}

nlohmann::json SectionsToJson(const GatewayStatusSections& sections) {
    nlohmann::json json = nlohmann::json::object();
    nlohmann::json work = nlohmann::json::object();
    work["ledger_present"] = sections.work_ledger_present;
    work["jobs_total"] = sections.jobs_total;
    work["occurrences_due"] = sections.occurrences_due;
    work["occurrences_in_flight"] = sections.occurrences_in_flight;
    work["occurrences_succeeded"] = sections.occurrences_succeeded;
    work["occurrences_failed"] = sections.occurrences_failed;
    work["occurrences_needs_review"] = sections.occurrences_needs_review;
    json["work"] = std::move(work);
    nlohmann::json execution = nlohmann::json::array();
    for (const auto& entry : sections.recent_executions) {
        nlohmann::json item = nlohmann::json::object();
        item["occurrence_id"] = entry.occurrence_id;
        item["job_id"] = entry.job_id;
        item["session_id"] = entry.session_id.empty() ? nlohmann::json(nullptr)
                                                      : nlohmann::json(entry.session_id);
        item["turn_id"] = entry.turn_id.empty() ? nlohmann::json(nullptr)
                                                : nlohmann::json(entry.turn_id);
        item["outcome"] = entry.outcome.empty() ? nlohmann::json("in_flight")
                                                : nlohmann::json(entry.outcome);
        execution.push_back(std::move(item));
    }
    json["execution"] = std::move(execution);
    nlohmann::json delivery = nlohmann::json::object();
    delivery["ledger_present"] = sections.delivery_ledger_present;
    delivery["pending"] = sections.delivery_pending;
    delivery["delivered"] = sections.delivery_delivered;
    delivery["flagged"] = sections.delivery_flagged;
    json["delivery"] = std::move(delivery);
    return json;
}

std::vector<std::string> FormatSectionLines(const GatewayStatusSections& sections) {
    std::vector<std::string> lines;
    lines.push_back("[work] 任务账" + std::string(sections.work_ledger_present ? "" : "(空:尚无任务)"));
    lines.push_back("  任务总数: " + std::to_string(sections.jobs_total) +
                    ";occurrence: 待跑 " + std::to_string(sections.occurrences_due) +
                    ",在途 " + std::to_string(sections.occurrences_in_flight) +
                    ",成功 " + std::to_string(sections.occurrences_succeeded) +
                    ",失败 " + std::to_string(sections.occurrences_failed) +
                    ",待审 " + std::to_string(sections.occurrences_needs_review));
    lines.push_back("[execution] 最近执行");
    if (sections.recent_executions.empty()) {
        lines.push_back("  (尚无执行记录——进程在跑不等于任务跑过)");
    } else {
        for (const auto& entry : sections.recent_executions) {
            const std::string outcome =
                entry.outcome.empty() ? std::string("in_flight") : entry.outcome;
            std::string line = "  " + entry.occurrence_id + " -> " + outcome;
            if (!entry.session_id.empty()) {
                line += "(session " + entry.session_id + ",turn " + entry.turn_id + ")";
            }
            lines.push_back(std::move(line));
        }
    }
    lines.push_back("[delivery] 结果投递" +
                    std::string(sections.delivery_ledger_present ? "" : "(空:尚无投递)"));
    lines.push_back("  待投 " + std::to_string(sections.delivery_pending) +
                    ",已投 " + std::to_string(sections.delivery_delivered) +
                    ",异常 " + std::to_string(sections.delivery_flagged));
    return lines;
}

GatewayProbe ProbeGateway(const GatewayProfilePaths& paths) {
    GatewayProbe probe;
    if (paths.root.empty()) {
        probe.detail = "profile 名不合法";
        return probe;
    }

    // control 快照:文件在但读不懂 = 坏 control endpoint,降级诊断不崩。
    std::string control_error;
    probe.control = ReadControlSnapshot(paths.control_file, &control_error);
    probe.control_error = control_error;
    probe.control_unreadable = !control_error.empty();

    const GatewayBootHistory history(paths.boot_history);
    probe.unclean_streak = CountUncleanBootStreak(history.ReadAll());

    std::string lock_error;
    const auto holder = ReadLockRecord(paths.lock_file, &lock_error);
    if (!holder.has_value() && !lock_error.empty()) {
        probe.state = GatewayProbe::State::BrokenLock;
        probe.detail = "gateway.lock_stale: " + lock_error + "(锁文件: " +
                       platform::PathToUtf8(paths.lock_file) + ")";
        return probe;
    }
    if (!holder.has_value()) {
        if (probe.control.has_value() && probe.control->state != "stopped") {
            probe.state = GatewayProbe::State::StaleRemnant;
            probe.detail = "未运行:无锁,但控制快照残留 state=" + probe.control->state +
                           "(上次未及收口或硬杀)";
            return probe;
        }
        probe.state = GatewayProbe::State::NotRunning;
        probe.detail = "gateway.not_running: 没有运行中的 Gateway";
        return probe;
    }

    probe.holder = *holder;
    const trajectory::SessionLockOwner owner{holder->pid, holder->start_token, 0};
    if (trajectory::ProbeLockHolder(owner) == trajectory::LockHolderState::Alive) {
        probe.state = GatewayProbe::State::Running;
        probe.detail = "运行中(boot " + holder->boot_id + ",pid " + std::to_string(holder->pid) +
                       ")";
        if (probe.control_unreadable) {
            probe.detail += ";控制快照读不懂: " + probe.control_error +
                            "(gateway.control_unreachable)";
        } else if (!probe.control.has_value()) {
            probe.detail += ";控制快照缺失";
        } else if (probe.control->safe_mode) {
            probe.detail += ";SafeMode(业务面暂停)";
        }
        return probe;
    }
    probe.state = GatewayProbe::State::StaleLock;
    probe.detail = "未运行:锁是陈旧的(持有进程已死或 PID 复用),下次启动自动清";
    return probe;
}

nlohmann::json ProbeToJson(const GatewayProbe& probe) {
    nlohmann::json json = nlohmann::json::object();
    const char* state_name = "not_running";
    switch (probe.state) {
        case GatewayProbe::State::Running:
            state_name = "running";
            break;
        case GatewayProbe::State::StaleLock:
            state_name = "stale_lock";
            break;
        case GatewayProbe::State::StaleRemnant:
            state_name = "stale_remnant";
            break;
        case GatewayProbe::State::BrokenLock:
            state_name = "broken_lock";
            break;
        case GatewayProbe::State::NotRunning:
            break;
    }
    json["state"] = state_name;
    json["detail"] = probe.detail;
    json["unclean_boot_streak"] = probe.unclean_streak;
    if (probe.holder.pid != 0) {
        nlohmann::json holder = nlohmann::json::object();
        holder["pid"] = probe.holder.pid;
        holder["start_token"] = probe.holder.start_token;
        holder["boot_id"] = probe.holder.boot_id;
        holder["acquired_at_ms"] = probe.holder.acquired_at_ms;
        json["holder"] = holder;
    }
    if (probe.control.has_value()) {
        json["control"] = probe.control->ToJson();
    } else {
        json["control"] = nullptr;
        if (probe.control_unreadable) {
            json["control_error"] = probe.control_error;
            json["error_code"] = "gateway.control_unreachable";
        }
    }
    if (probe.state == GatewayProbe::State::Running && probe.control.has_value()) {
        json["health"] = probe.control->health;
        json["safe_mode"] = probe.control->safe_mode;
    }
    return json;
}

std::vector<std::string> FormatProbeLines(const GatewayProbe& probe) {
    std::vector<std::string> lines;
    lines.push_back(probe.detail);
    if (probe.state == GatewayProbe::State::Running) {
        if (probe.control.has_value()) {
            lines.push_back("  状态: " + probe.control->state + " / health: " +
                            probe.control->health);
            lines.push_back("  启动于: " + std::to_string(probe.control->started_at_ms) +
                            "ms(boot " + probe.control->boot_id + ")");
            if (probe.control->safe_mode) {
                lines.push_back("  SafeMode: 连续非干净关机达阈值,业务面暂停;"
                                "干净关机一次即退出");
            }
        }
    }
    if (probe.unclean_streak > 0) {
        lines.push_back("  连续非干净关机: " + std::to_string(probe.unclean_streak) + " 次");
    }
    return lines;
}

}  // namespace lubancode::gateway
