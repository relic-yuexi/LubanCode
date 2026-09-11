// v3 事件账 writer profile 实现。哈希/发号/落盘与 V3Writer 同一条纪律,
// 只去掉 message 行与上下文链——那是 agent 会话的账,不是事件账的。
#include "trajectory/v3/event_ledger.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "trajectory/canonical_json.hpp"
#include "trajectory/journal.hpp"

namespace lubancode::trajectory::v3 {

namespace {

// ISO-8601 UTC 毫秒(与 writer.cpp 同款算法;那边在匿名命名空间,不外借)。
std::string IsoTimestamp(std::int64_t wall_ms) {
    std::int64_t secs = wall_ms / 1000;
    int millis = static_cast<int>(wall_ms % 1000);
    if (millis < 0) {
        millis += 1000;
        secs -= 1;
    }
    std::int64_t days = secs / 86400;
    std::int64_t tod = secs % 86400;
    days += 719468;
    std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    std::int64_t doe = days - era * 146097;
    std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t y = yoe + era * 400;
    std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    std::int64_t mp = (5 * doy + 2) / 153;
    std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    std::int64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer),
                  "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03dZ",
                  static_cast<long long>(y), static_cast<long long>(m),
                  static_cast<long long>(d), static_cast<long long>(tod / 3600),
                  static_cast<long long>((tod / 60) % 60), static_cast<long long>(tod % 60),
                  millis);
    return buffer;
}

// 读全部行(检测尾行截断;与 writer.cpp 同规矩)。返回 nullopt = 读不开。
std::optional<std::vector<std::string>> ReadRawLines(const std::filesystem::path& path,
                                                     bool* truncated_tail) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < data.size()) {
        std::size_t end = data.find('\n', start);
        if (end == std::string::npos) {
            *truncated_tail = true;  // 尾段无换行 = 崩溃截断
            break;
        }
        if (end > start) {
            lines.emplace_back(data.substr(start, end - start));
        }
        start = end + 1;
    }
    return lines;
}

}  // namespace

struct V3EventLedger::Impl {
    std::mutex mutex;
    JournalWriter journal;
    std::string session_id;
    std::string run_id;
    std::uint64_t next_seq = 1;
    std::uint64_t last_seq_value = 0;
    std::string last_hash{std::string(kGenesisHash)};
    std::map<std::string, std::uint64_t> id_counters;  // prefix -> 已发最大号
    bool broken = false;
    V3EventLedgerOptions options;
    const V3Clock* clock = nullptr;
    V3Clock owned_clock;

    const V3Clock& Clock() const { return clock != nullptr ? *clock : owned_clock; }

    std::string NextIdLocked(const std::string& prefix) {
        std::uint64_t& counter = id_counters[prefix];
        ++counter;
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%s-%06llu", prefix.c_str(),
                      static_cast<unsigned long long>(counter));
        return buffer;
    }

    WriteReceipt CommitEventLocked(EventDraft draft, Durability durability) {
        WriteReceipt receipt;
        EventLine line;
        line.session_id = session_id;
        line.run_id = run_id;
        line.seq = next_seq;
        line.timestamp = IsoTimestamp(Clock().WallMs());
        line.event_id = NextIdLocked("evt");
        line.kind = draft.kind;
        line.status = draft.status;
        line.turn_id = std::move(draft.turn_id);
        line.parent_turn_id = std::move(draft.parent_turn_id);
        line.step_id = std::move(draft.step_id);
        line.request_id = std::move(draft.request_id);
        line.action_id = std::move(draft.action_id);
        line.compact_id = std::move(draft.compact_id);
        line.command_id = std::move(draft.command_id);
        line.hook_dispatch_id = std::move(draft.hook_dispatch_id);
        line.task_id = std::move(draft.task_id);
        line.title_generation_id = std::move(draft.title_generation_id);
        line.payload = std::move(draft.payload);
        line.effects = std::move(draft.effects);
        line.effect_refs = std::move(draft.effect_refs);
        // status 按 kind 固定映射兜底:调用方没给的,按 RequiredStatusForKind
        // 补齐;给了不一致的,校验会拒(schema3 层同一张表)。
        if (!line.status.has_value()) {
            line.status = RequiredStatusForKind(line.kind);
        }
        if (auto error = ValidateEventLine(line)) {
            receipt.error_code = error->code;
            receipt.error_message = error->message;
            return receipt;
        }
        return AppendLineLocked(line.ToJson(), line.event_id, durability);
    }

    WriteReceipt AppendLineLocked(nlohmann::json json, std::string id, Durability durability) {
        WriteReceipt receipt;
        json.erase("prevHash");
        json.erase("lineHash");
        auto canonical = CanonicalJsonDump(json);
        if (!canonical.has_value()) {
            receipt.error_code = "v3ledger.canonical_failed";
            receipt.error_message = canonical.error();
            return receipt;
        }
        const std::string line_hash = ComputeLineHash(last_hash, *canonical);
        json["prevHash"] = last_hash;
        json["lineHash"] = line_hash;
        auto final_line = CanonicalJsonDump(json);
        if (!final_line.has_value()) {
            receipt.error_code = "v3ledger.canonical_failed";
            receipt.error_message = final_line.error();
            return receipt;
        }
        if (options.inject_io_failure && options.inject_io_failure()) {
            broken = true;
            receipt.error_code = "v3ledger.injected";
            receipt.error_message = "注入的提交失败(测试)";
            return receipt;
        }
        if (!journal.AppendLine(*final_line, durability)) {
            broken = true;
            receipt.error_code = "v3ledger.io_failed";
            receipt.error_message = "追加落盘失败,句柄已断";
            return receipt;
        }
        receipt.status = WriteReceipt::Status::Committed;
        receipt.id = std::move(id);
        receipt.seq = next_seq;
        receipt.line_hash = line_hash;
        last_seq_value = next_seq;
        next_seq += 1;
        last_hash = std::move(line_hash);
        return receipt;
    }
};

V3EventLedger::V3EventLedger(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

V3EventLedger::V3EventLedger(V3EventLedger&&) noexcept = default;
V3EventLedger& V3EventLedger::operator=(V3EventLedger&&) noexcept = default;
V3EventLedger::~V3EventLedger() = default;

std::expected<V3EventLedger, std::string> V3EventLedger::Start(
    const std::filesystem::path& jsonl_path, std::string_view session_id,
    std::string_view run_id, EventDraft opening_event, V3EventLedgerOptions options,
    const V3Clock* clock) {
    auto journal = JournalWriter::Open(jsonl_path, JournalWriter::OpenMode::CreateNew);
    if (!journal.has_value()) {
        return std::unexpected("v3ledger.create_new_failed: " + journal.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->journal = std::move(*journal);
    impl->session_id = std::string(session_id);
    impl->run_id = std::string(run_id);
    impl->options = std::move(options);
    impl->clock = clock;
    V3EventLedger ledger(std::move(impl));
    // 开账事实首行按 PowerLoss 落稳:首行没落稳,这本账就不存在。开不成
    // 不在盘上留 0 字节正式账(P0-C 同款)。
    WriteReceipt receipt = ledger.Append(std::move(opening_event), Durability::PowerLoss);
    if (receipt.status != WriteReceipt::Status::Committed) {
        std::error_code ec;
        std::filesystem::remove(jsonl_path, ec);
        return std::unexpected("v3ledger.opening_event_failed: " + receipt.error_code + " " +
                               receipt.error_message);
    }
    return ledger;
}

std::expected<V3EventLedger, std::string> V3EventLedger::Continue(
    const std::filesystem::path& jsonl_path, V3EventLedgerOptions options,
    const V3Clock* clock) {
    V3EventLedgerReport report = VerifyV3EventLedgerFile(jsonl_path);
    if (!report.ok) {
        return std::unexpected(report.error_code + ": " + report.message);
    }
    auto journal = JournalWriter::Open(jsonl_path, JournalWriter::OpenMode::Append);
    if (!journal.has_value()) {
        return std::unexpected("v3ledger.append_open_failed: " + journal.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->journal = std::move(*journal);
    impl->session_id = report.session_id;
    impl->run_id = report.run_id;
    impl->next_seq = report.last_seq + 1;
    impl->last_seq_value = report.last_seq;
    impl->last_hash = report.last_line_hash;
    // 行身份计数恢复:扫卷内 eventId 前缀(evt-<n>),续卷不撞号。
    {
        bool truncated = false;
        auto lines = ReadRawLines(jsonl_path, &truncated);
        if (lines.has_value()) {
            for (const std::string& raw : *lines) {
                nlohmann::json line = nlohmann::json::parse(raw, nullptr, false);
                if (line.is_discarded() || !line.is_object()) continue;
                const auto it = line.find("eventId");
                if (it == line.end() || !it->is_string()) continue;
                const std::string id = it->get<std::string>();
                const std::size_t dash = id.rfind('-');
                if (dash == std::string::npos) continue;
                const std::string prefix = id.substr(0, dash);
                std::uint64_t value = 0;
                bool ok = true;
                for (std::size_t i = dash + 1; i < id.size(); ++i) {
                    if (id[i] < '0' || id[i] > '9') {
                        ok = false;
                        break;
                    }
                    value = value * 10 + static_cast<std::uint64_t>(id[i] - '0');
                }
                if (!ok) continue;
                std::uint64_t& counter = impl->id_counters[prefix];
                if (value > counter) counter = value;
            }
        }
    }
    impl->options = std::move(options);
    impl->clock = clock;
    return V3EventLedger(std::move(impl));
}

WriteReceipt V3EventLedger::Append(EventDraft draft, Durability durability) {
    if (impl_ == nullptr) {
        WriteReceipt receipt;
        receipt.error_code = "v3ledger.no_impl";
        receipt.error_message = "事件账未开";
        return receipt;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->broken) {
        WriteReceipt receipt;
        receipt.error_code = "v3ledger.broken";
        receipt.error_message = "事件账句柄已断,不再提交";
        return receipt;
    }
    return impl_->CommitEventLocked(std::move(draft), durability);
}

std::string V3EventLedger::NextId(std::string_view prefix) {
    if (impl_ == nullptr) return std::string();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextIdLocked(std::string(prefix));
}

const std::filesystem::path& V3EventLedger::path() const {
    static const std::filesystem::path kEmpty;
    return impl_ != nullptr ? impl_->journal.path() : kEmpty;
}

std::uint64_t V3EventLedger::next_seq() const {
    return impl_ != nullptr ? impl_->next_seq : 0;
}

std::uint64_t V3EventLedger::last_seq() const {
    return impl_ != nullptr ? impl_->last_seq_value : 0;
}

std::string V3EventLedger::last_line_hash() const {
    return impl_ != nullptr ? impl_->last_hash : std::string();
}

bool V3EventLedger::broken() const { return impl_ == nullptr || impl_->broken; }

const std::string& V3EventLedger::session_id() const {
    static const std::string kEmpty;
    return impl_ != nullptr ? impl_->session_id : kEmpty;
}

const std::string& V3EventLedger::run_id() const {
    static const std::string kEmpty;
    return impl_ != nullptr ? impl_->run_id : kEmpty;
}

V3EventLedgerReport VerifyV3EventLedgerFile(const std::filesystem::path& path) {
    V3EventLedgerReport report;
    bool truncated = false;
    auto lines = ReadRawLines(path, &truncated);
    if (!lines.has_value()) {
        report.error_code = "v3ledger.open_failed";
        report.message = "事件账读不开";
        return report;
    }
    if (truncated) {
        report.truncated_tail = true;
        report.error_code = "v3ledger.truncated_tail";
        report.message = "尾行缺换行(崩溃截断);删尾修复归读取侧,写入侧不偷偷裁";
        return report;
    }
    if (lines->empty()) {
        report.error_code = "v3ledger.empty";
        report.message = "事件账为空(开账事实事件缺位)";
        return report;
    }
    std::string prev_hash{std::string(kGenesisHash)};
    for (std::size_t i = 0; i < lines->size(); ++i) {
        nlohmann::json line_json = nlohmann::json::parse((*lines)[i], nullptr, false);
        if (line_json.is_discarded()) {
            report.error_code = "v3ledger.bad_json";
            report.message = "第 " + std::to_string(i + 1) + " 行不是合法 JSON";
            return report;
        }
        auto error = VerifyLine(line_json, prev_hash, static_cast<std::uint64_t>(i + 1));
        if (error.has_value()) {
            report.error_code = error->code;
            report.message = "第 " + std::to_string(i + 1) + " 行: " + error->message;
            return report;
        }
        // 事件账 profile:只认 event 行。message 行出现在这里 = 有人拿 agent
        // 写者写编排账(或拿事件账 profile 写会话),拒收不猜。
        if (line_json.at("type").get<std::string>() != "event") {
            report.error_code = "v3ledger.message_line_rejected";
            report.message =
                "第 " + std::to_string(i + 1) + " 行是 message 行;事件账 profile 只收 event 行";
            return report;
        }
        if (i == 0) {
            report.session_id = line_json.at("sessionId").get<std::string>();
            report.run_id = line_json.at("runId").get<std::string>();
        }
        prev_hash = line_json.at("lineHash").get<std::string>();
    }
    report.ok = true;
    report.lines = lines->size();
    report.last_seq = lines->size();
    report.last_line_hash = prev_hash;
    return report;
}

}  // namespace lubancode::trajectory::v3
