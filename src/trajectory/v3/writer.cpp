// v3 会话写者实现。
#include "trajectory/v3/writer.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "trajectory/canonical_json.hpp"
#include "trajectory/journal.hpp"

namespace lubancode::trajectory::v3 {

namespace {

// ISO-8601 UTC 毫秒:YYYY-MM-DDTHH:MM:SS.mmmZ。
std::string IsoTimestamp(std::int64_t wall_ms) {
    std::int64_t secs = wall_ms / 1000;
    int millis = static_cast<int>(wall_ms % 1000);
    if (millis < 0) {  // 防御:负毫秒向下借一秒
        millis += 1000;
        secs -= 1;
    }
    std::int64_t days = secs / 86400;
    std::int64_t tod = secs % 86400;
    // civil-from-days(Howard Hinnant 算法)。
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
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03dZ",
                  static_cast<long long>(y), static_cast<long long>(m),
                  static_cast<long long>(d), static_cast<long long>(tod / 3600),
                  static_cast<long long>((tod / 60) % 60), static_cast<long long>(tod % 60),
                  millis);
    return buffer;
}

std::optional<ChainNode> ChainNodeFromJson(const nlohmann::json& node) {
    if (!node.is_object() || !node.contains("messageRef") || !node["messageRef"].is_string() ||
        !node.contains("prevMessageRef")) {
        return std::nullopt;
    }
    ChainNode parsed;
    parsed.message_ref = node["messageRef"].get<std::string>();
    if (!node["prevMessageRef"].is_null()) {
        if (!node["prevMessageRef"].is_string()) {
            return std::nullopt;
        }
        parsed.prev_message_ref = node["prevMessageRef"].get<std::string>();
    }
    return parsed;
}

std::vector<nlohmann::json> ChainToJson(const std::vector<ChainNode>& chain) {
    std::vector<nlohmann::json> nodes;
    nodes.reserve(chain.size());
    for (const auto& node : chain) {
        nodes.push_back(node.ToJson());
    }
    return nodes;
}

}  // namespace

std::int64_t V3Clock::WallMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 前置:Impl::AppendLineLocked 落稳事件后统一重放视图(定义在重放节)。
std::string ApplyCommitEventToView(ContextView& view, const EventLine& line);

struct V3Writer::Impl {
    std::mutex mutex;
    JournalWriter journal;
    std::string session_id;
    std::string run_id;
    std::uint64_t next_seq = 1;
    std::string last_hash{std::string(kGenesisHash)};
    // msg/evt/turn/step/req/stream/compact/action/hookdispatch/task
    std::uint64_t id_counters[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ContextView context;
    std::unordered_set<std::string> message_ids;
    bool broken = false;
    V3WriterOptions options;
    const V3Clock* clock = nullptr;
    V3Clock owned_clock;  // 无注入时的默认钟

    const V3Clock& Clock() const { return clock != nullptr ? *clock : owned_clock; }

    std::string NextId(const char* prefix, int slot) {
        ++id_counters[slot];
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%s-%06llu", prefix,
                      static_cast<unsigned long long>(id_counters[slot]));
        return buffer;
    }

    // 锁内接纳:构造 appendedChain 并提交 context.input.applied。
    WriteReceipt AdmitLocked(const std::vector<std::string>& ids, Durability durability) {
        for (const auto& id : ids) {
            if (this->message_ids.count(id) == 0) {
                return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "",
                                    "v3writer.dangling_ref", "接纳引用未落稳: " + id};
            }
        }
        std::vector<ChainNode> appended;
        std::string prev =
            context.chain.empty() ? std::string() : context.chain.back().message_ref;
        nlohmann::json added = nlohmann::json::array();
        for (const auto& id : ids) {
            appended.push_back(ChainNode{
                id, prev.empty() ? std::nullopt : std::optional<std::string>(prev)});
            prev = id;
            added.push_back(id);
        }
        EventDraft draft;
        draft.kind = EventKindV3::ContextInputApplied;
        draft.payload = nlohmann::json::object(
            {{"contextId", context.context_id},
             {"beforeRevision", context.revision},
             {"afterRevision", context.revision + 1},
             {"appendedChain", ChainToJson(appended)},
             {"addedMessageRefs", added}});
        return CommitEvent(std::move(draft), durability);
    }

    // 单写者提交一枚 message 行:校验 -> 发号 -> 哈希 -> 落盘 -> 更新状态。
    // 调用方已持锁。失败不动状态(IoFailed 时置 broken)。
    WriteReceipt CommitMessage(MessageDraft draft, Durability durability) {
        WriteReceipt receipt;
        // 预留 id(§4.43):沿用调用方预留;已占用拒收,不留重复行身份。
        if (draft.message_id_override.has_value()) {
            if (message_ids.count(*draft.message_id_override) > 0) {
                receipt.error_code = "v3writer.duplicate_id";
                receipt.error_message = "预留 messageId 已被占用: " + *draft.message_id_override;
                return receipt;
            }
        }
        MessageLine line;
        line.session_id = session_id;
        line.run_id = run_id;
        line.seq = next_seq;
        line.timestamp = IsoTimestamp(Clock().WallMs());
        line.message_id = draft.message_id_override.has_value() ? *draft.message_id_override
                                                                : NextId("msg", 0);
        line.turn_id = std::move(draft.turn_id);
        line.parent_turn_id = std::move(draft.parent_turn_id);
        line.step_id = std::move(draft.step_id);
        line.request_id = std::move(draft.request_id);
        line.action_id = std::move(draft.action_id);
        line.compact_id = std::move(draft.compact_id);
        line.purpose = draft.purpose;
        line.origin = draft.origin;
        line.display = draft.display;
        line.message = std::move(draft.message);
        line.caused_by_event_ref = std::move(draft.caused_by_event_ref);
        line.source_message_ref = std::move(draft.source_message_ref);
        line.result_selection_ref = std::move(draft.result_selection_ref);
        line.source_tool_message_ref = std::move(draft.source_tool_message_ref);
        line.system_meta = std::move(draft.system_meta);
        line.completion_status = draft.completion_status;
        line.provider = std::move(draft.provider);
        line.wire = std::move(draft.wire);
        line.model = std::move(draft.model);
        line.response_model = std::move(draft.response_model);
        line.provider_config_ref = std::move(draft.provider_config_ref);
        line.model_profile_ref = std::move(draft.model_profile_ref);
        line.usage = std::move(draft.usage);
        if (auto error = ValidateMessageLine(line)) {
            receipt.error_code = error->code;
            receipt.error_message = error->message;
            return receipt;
        }
        return AppendLineLocked(line.ToJson(), line.message_id, durability);
    }

    WriteReceipt CommitEvent(EventDraft draft, Durability durability) {
        WriteReceipt receipt;
        EventLine line;
        line.session_id = session_id;
        line.run_id = run_id;
        line.seq = next_seq;
        line.timestamp = IsoTimestamp(Clock().WallMs());
        line.event_id = NextId("evt", 1);
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
        if (auto error = ValidateEventLine(line)) {
            receipt.error_code = error->code;
            receipt.error_message = error->message;
            return receipt;
        }
        return AppendLineLocked(line.ToJson(), line.event_id, durability);
    }

    // 落一行(哈希填好、canonical dump、追加、状态前移)。
    WriteReceipt AppendLineLocked(nlohmann::json json, std::string id, Durability durability) {
        WriteReceipt receipt;
        json.erase("prevHash");
        json.erase("lineHash");
        auto canonical = CanonicalJsonDump(json);
        if (!canonical.has_value()) {
            receipt.error_code = "v3writer.canonical_failed";
            receipt.error_message = canonical.error();
            return receipt;
        }
        const std::string line_hash = ComputeLineHash(last_hash, *canonical);
        json["prevHash"] = last_hash;
        json["lineHash"] = line_hash;
        auto final_line = CanonicalJsonDump(json);
        if (!final_line.has_value()) {
            receipt.error_code = "v3writer.canonical_failed";
            receipt.error_message = final_line.error();
            return receipt;
        }
        if (options.inject_io_failure && options.inject_io_failure()) {
            broken = true;
            receipt.error_code = "v3writer.injected";
            receipt.error_message = "注入的提交失败(测试)";
            return receipt;
        }
        if (!journal.AppendLine(*final_line, durability)) {
            broken = true;
            receipt.error_code = "v3writer.io_failed";
            receipt.error_message = "追加落盘失败,句柄已断";
            return receipt;
        }
        receipt.status = WriteReceipt::Status::Committed;
        receipt.id = std::move(id);
        receipt.seq = next_seq;
        receipt.line_hash = line_hash;
        if (json.at("type").get<std::string>() == "message") {
            message_ids.insert(json.at("messageId").get<std::string>());
        } else {
            // 事件落稳后统一重放内存视图(链/版本),便利 API 不各自手工维护。
            std::string ec, msg;
            if (auto event = EventLine::FromJsonStrict(json, &ec, &msg)) {
                ApplyCommitEventToView(context, *event);
            }
        }
        ++next_seq;
        last_hash = line_hash;
        return receipt;
    }
};

V3Writer::V3Writer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
V3Writer::V3Writer(V3Writer&&) noexcept = default;
V3Writer& V3Writer::operator=(V3Writer&&) noexcept = default;
V3Writer::~V3Writer() = default;

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

std::expected<V3Writer, std::string> V3Writer::Start(const std::filesystem::path& jsonl_path,
                                                     std::string_view session_id,
                                                     std::string_view run_id,
                                                     std::string_view system_content,
                                                     nlohmann::json system_extra,
                                                     V3WriterOptions options,
                                                     const V3Clock* clock) {
    auto journal = JournalWriter::Open(jsonl_path, JournalWriter::OpenMode::CreateNew);
    if (!journal.has_value()) {
        return std::unexpected(journal.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->journal = std::move(*journal);
    impl->session_id = std::string(session_id);
    impl->run_id = std::string(run_id);
    impl->options = std::move(options);
    impl->clock = clock;

    V3Writer writer(std::move(impl));
    std::lock_guard<std::mutex> lock(writer.impl_->mutex);
    // 行 1:首行 system(seq=1、turnId=null、自带 session/run/schema 身份,§1.2)。
    nlohmann::json system_meta = nlohmann::json::object(
        {{"cause", "initial"}, {"changeEventRef", nullptr}, {"systemChanged", false}});
    for (auto it = system_extra.begin(); it != system_extra.end(); ++it) {
        system_meta[it.key()] = it.value();
    }
    MessageDraft system_draft;
    system_draft.purpose = MessagePurpose::Conversation;
    system_draft.origin = MessageOrigin::SessionRuntime;
    system_draft.system_meta = system_meta;
    system_draft.message =
        nlohmann::json::object({{"role", "system"}, {"content", std::string(system_content)}});
    WriteReceipt receipt =
        writer.impl_->CommitMessage(std::move(system_draft), Durability::PowerLoss);
    if (receipt.status != WriteReceipt::Status::Committed) {
        return std::unexpected("v3writer.start_system_failed: " + receipt.error_code + " " +
                               receipt.error_message);
    }
    // 内存链先立(供 session.started 引用)。
    writer.impl_->context = ContextView{};
    writer.impl_->context.revision = 1;
    writer.impl_->context.chain.push_back(ChainNode{receipt.id, std::nullopt});
    writer.impl_->context.system_message_ref = receipt.id;
    // 行 2:session.started(revision 1 单节点链)。
    EventDraft started;
    started.kind = EventKindV3::SessionStarted;
    started.payload = nlohmann::json::object(
        {{"writerVersion", writer.impl_->options.writer_version},
         {"context",
          nlohmann::json::object({{"contextId", "main"},
                                  {"revision", 1},
                                  {"contextChain", ChainToJson(writer.impl_->context.chain)}})}});
    WriteReceipt started_receipt =
        writer.impl_->CommitEvent(std::move(started), Durability::PowerLoss);
    if (started_receipt.status != WriteReceipt::Status::Committed) {
        return std::unexpected("v3writer.start_event_failed: " + started_receipt.error_code +
                               " " + started_receipt.error_message);
    }
    return writer;
}

// ---------------------------------------------------------------------------
// 重放(Continue/VerifyV3File 共用)
// ---------------------------------------------------------------------------

// 重放一枚提交事件到视图(Impl 与 VerifyV3File 共用)。返回错误码或空串。
std::string ApplyCommitEventToView(ContextView& view, const EventLine& line) {
    using K = EventKindV3;
    auto set_chain_from = [&](const nlohmann::json& chain_json, std::uint64_t revision) {
        std::vector<ChainNode> chain;
        for (const auto& node : chain_json) {
            auto parsed = ChainNodeFromJson(node);
            if (!parsed.has_value()) {
                return std::string("v3writer.bad_chain_node");
            }
            chain.push_back(std::move(*parsed));
        }
        if (chain.empty()) {
            return std::string("v3writer.empty_chain");
        }
        view.chain = std::move(chain);
        view.revision = revision;
        view.system_message_ref = view.chain.front().message_ref;
        return std::string();
    };
    switch (line.kind) {
        case K::SessionStarted: {
            const auto& context = line.payload.at("context");
            view.context_id = context.at("contextId").get<std::string>();
            return set_chain_from(context.at("contextChain"),
                                  context.at("revision").get<std::uint64_t>());
        }
        case K::ContextSystemApplied:
            return set_chain_from(line.payload.at("contextChain"),
                                  line.payload.at("afterRevision").get<std::uint64_t>());
        case K::ContextToolPreviewsReduced: {
            // §4.38:降档提交携带完整新链(原 tool 节点换派生消息,后续重接);
            // 档位随之取事件里的新值,普通后续请求不自动回升。
            std::string error = set_chain_from(
                line.payload.at("contextChain"),
                line.payload.at("afterRevision").get<std::uint64_t>());
            if (!error.empty()) {
                return error;
            }
            if (line.payload.contains("newPreviewBudget") &&
                line.payload.at("newPreviewBudget").is_number_unsigned()) {
                view.preview_budget_bytes =
                    line.payload.at("newPreviewBudget").get<std::uint64_t>();
            }
            return std::string();
        }
        case K::CompactApplied: {
            std::string error = set_chain_from(
                line.payload.at("contextChain"),
                line.payload.at("newContextRevision").get<std::uint64_t>());
            if (!error.empty()) {
                return error;
            }
            if (line.compact_id.has_value()) {
                std::erase(view.open_compact_ids, *line.compact_id);
            }
            return std::string();
        }
        case K::ContextInputApplied: {
            for (const auto& node : line.payload.at("appendedChain")) {
                auto parsed = ChainNodeFromJson(node);
                if (!parsed.has_value()) {
                    return std::string("v3writer.bad_chain_node");
                }
                if (view.chain.empty() ||
                    view.chain.back().message_ref != parsed->prev_message_ref.value_or("")) {
                    return std::string("v3writer.append_mismatch");
                }
                view.chain.push_back(std::move(*parsed));
            }
            view.revision = line.payload.at("afterRevision").get<std::uint64_t>();
            return std::string();
        }
        case K::CompactRequested:
            if (line.compact_id.has_value()) {
                view.open_compact_ids.push_back(*line.compact_id);
            }
            return std::string();
        case K::CompactFailed:
        case K::CompactCancelled:
        case K::CompactRejected:
            if (line.compact_id.has_value()) {
                std::erase(view.open_compact_ids, *line.compact_id);
            }
            return std::string();
        default:
            return std::string();
    }
}

// 读全部行(检测尾行截断)。返回 nullopt = 读不开。
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
            *truncated_tail = true;  // 尾段无换行 = 崩溃截断,明报(§4.60)
            break;
        }
        if (end > start) {
            lines.emplace_back(data.substr(start, end - start));
        }
        start = end + 1;
    }
    return lines;
}

void RestoreIdCounters(std::uint64_t (&counters)[10], const std::vector<nlohmann::json>& lines) {
    struct Prefix {
        const char* prefix;
        int slot;
    };
    static const Prefix kPrefixes[] = {
        {"msg-", 0},          {"evt-", 1},        {"turn-", 2},        {"step-", 3},
        {"request-", 4},      {"stream-", 5},     {"compact-", 6},     {"action-", 7},
        {"hookdispatch-", 8}, {"task-", 9},       {"compact-turn-", 2}};
    auto bump = [&](const std::string& id) {
        for (const auto& p : kPrefixes) {
            if (id.rfind(p.prefix, 0) != 0) {
                continue;
            }
            std::uint64_t value = 0;
            bool ok = true;
            for (std::size_t i = std::string(p.prefix).size(); i < id.size(); ++i) {
                char c = id[i];
                if (c < '0' || c > '9') {
                    ok = false;
                    break;
                }
                value = value * 10 + static_cast<std::uint64_t>(c - '0');
            }
            if (ok && value > counters[p.slot]) {
                counters[p.slot] = value;
            }
            return;
        }
    };
    for (const auto& line : lines) {
        for (const char* key :
             {"messageId", "eventId", "turnId", "parentTurnId", "stepId", "requestId",
              "compactId", "actionId", "hookDispatchId", "taskId"}) {
            auto it = line.find(key);
            if (it != line.end() && it->is_string()) {
                bump(it->get<std::string>());
            }
        }
    }
}

V3VerifyReport VerifyV3File(const std::filesystem::path& path) {
    V3VerifyReport report;
    bool truncated = false;
    auto lines = ReadRawLines(path, &truncated);
    if (!lines.has_value()) {
        report.error_code = "v3writer.open_failed";
        report.message = "v3 卷读不开";
        return report;
    }
    if (truncated) {
        report.truncated_tail = true;
        report.error_code = "v3writer.truncated_tail";
        report.message = "尾行缺换行(崩溃截断);删尾修复归 §4.60,写入侧不偷偷裁";
        return report;
    }
    std::string prev_hash{std::string(kGenesisHash)};
    ContextView view;
    for (std::size_t i = 0; i < lines->size(); ++i) {
        nlohmann::json line_json = nlohmann::json::parse((*lines)[i], nullptr, false);
        if (line_json.is_discarded()) {
            report.error_code = "v3writer.bad_json";
            report.message = "第 " + std::to_string(i + 1) + " 行不是合法 JSON";
            return report;
        }
        auto error = VerifyLine(line_json, prev_hash, static_cast<std::uint64_t>(i + 1));
        if (error.has_value()) {
            report.error_code = error->code;
            report.message = "第 " + std::to_string(i + 1) + " 行: " + error->message;
            return report;
        }
        if (line_json.at("type").get<std::string>() == "event") {
            std::string ec, msg;
            auto event = EventLine::FromJsonStrict(line_json, &ec, &msg);
            if (event.has_value()) {
                std::string apply_error = ApplyCommitEventToView(view, *event);
                if (!apply_error.empty()) {
                    report.error_code = apply_error;
                    report.message = "第 " + std::to_string(i + 1) + " 行: 链重放失败";
                    return report;
                }
            }
        }
        prev_hash = line_json.at("lineHash").get<std::string>();
    }
    report.lines = lines->size();
    report.context = std::move(view);
    report.ok = !lines->empty();
    if (!report.ok) {
        report.error_code = "v3writer.empty";
        report.message = "空卷";
    }
    return report;
}

std::optional<Schema3Error> VerifyLine(const nlohmann::json& line, std::string_view prev_hash,
                                       std::uint64_t expect_seq) {
    std::string error_code, error_message;
    if (!line.is_object() || !line.contains("type") || !line.at("type").is_string()) {
        return Schema3Error{"v3writer.bad_type", "行缺 type"};
    }
    const std::string type = line.at("type").get<std::string>();
    if (type == "message") {
        auto parsed = MessageLine::FromJsonStrict(line, &error_code, &error_message);
        if (!parsed.has_value()) {
            return Schema3Error{error_code, error_message};
        }
        if (auto error = ValidateMessageLine(*parsed)) {
            return *error;
        }
    } else if (type == "event") {
        auto parsed = EventLine::FromJsonStrict(line, &error_code, &error_message);
        if (!parsed.has_value()) {
            return Schema3Error{error_code, error_message};
        }
        if (auto error = ValidateEventLine(*parsed)) {
            return *error;
        }
    } else {
        return Schema3Error{"v3writer.bad_type", "type 只取 message/event"};
    }
    if (!line.contains("seq") || !line.at("seq").is_number_unsigned() ||
        line.at("seq").get<std::uint64_t>() != expect_seq) {
        return Schema3Error{"v3writer.seq_gap", "seq 应为 " + std::to_string(expect_seq)};
    }
    if (!line.contains("prevHash") || !line.at("prevHash").is_string() ||
        line.at("prevHash").get<std::string>() != prev_hash) {
        return Schema3Error{"v3writer.hash_chain_broken", "prevHash 与上一行 lineHash 不衔接"};
    }
    nlohmann::json stripped = line;
    stripped.erase("prevHash");
    stripped.erase("lineHash");
    auto canonical = CanonicalJsonDump(stripped);
    if (!canonical.has_value()) {
        return Schema3Error{"v3writer.canonical_failed", canonical.error()};
    }
    const std::string recomputed = ComputeLineHash(prev_hash, *canonical);
    if (!line.contains("lineHash") || !line.at("lineHash").is_string() ||
        line.at("lineHash").get<std::string>() != recomputed) {
        return Schema3Error{"v3writer.hash_mismatch", "lineHash 重算对不上"};
    }
    return std::nullopt;
}

std::expected<V3Writer, std::string> V3Writer::Continue(const std::filesystem::path& jsonl_path,
                                                        V3WriterOptions options,
                                                        const V3Clock* clock) {
    V3VerifyReport report = VerifyV3File(jsonl_path);
    if (!report.ok) {
        return std::unexpected("v3writer.continue_not_clean: " + report.error_code + " " +
                               report.message);
    }
    auto journal = JournalWriter::Open(jsonl_path, JournalWriter::OpenMode::Append);
    if (!journal.has_value()) {
        return std::unexpected(journal.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->journal = std::move(*journal);
    impl->options = std::move(options);
    impl->clock = clock;
    impl->next_seq = report.lines + 1;
    impl->context = std::move(report.context);
    // 身份、尾 hash、id 计数器、messageId 集合从重放行恢复。
    bool truncated = false;
    auto raw_lines = ReadRawLines(jsonl_path, &truncated);
    std::vector<nlohmann::json> lines;
    std::string last_hash;
    for (const auto& raw : *raw_lines) {
        auto line = nlohmann::json::parse(raw, nullptr, false);
        if (line.is_discarded()) {
            continue;  // VerifyV3File 已保证不会走到这
        }
        lines.push_back(line);
        last_hash = line.at("lineHash").get<std::string>();
        if (line.at("type").get<std::string>() == "message") {
            impl->message_ids.insert(line.at("messageId").get<std::string>());
        }
    }
    impl->last_hash = last_hash;
    if (!lines.empty()) {
        impl->session_id = lines.front().at("sessionId").get<std::string>();
        impl->run_id = lines.front().at("runId").get<std::string>();
    }
    RestoreIdCounters(impl->id_counters, lines);
    return V3Writer(std::move(impl));
}

// ---------------------------------------------------------------------------
// 两类行与领域便利
// ---------------------------------------------------------------------------

WriteReceipt V3Writer::AppendMessage(MessageDraft draft, Durability durability) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->broken) {
        return WriteReceipt{WriteReceipt::Status::IoFailed, "", 0, "", "v3writer.broken",
                            "句柄已断,停止提交"};
    }
    return impl_->CommitMessage(std::move(draft), durability);
}

WriteReceipt V3Writer::AppendEvent(EventDraft draft, Durability durability) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->broken) {
        return WriteReceipt{WriteReceipt::Status::IoFailed, "", 0, "", "v3writer.broken",
                            "句柄已断,停止提交"};
    }
    return impl_->CommitEvent(std::move(draft), durability);
}

WriteReceipt V3Writer::AdmitMessages(std::vector<std::string> message_ids,
                                     Durability durability) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->broken) {
        return WriteReceipt{WriteReceipt::Status::IoFailed, "", 0, "", "v3writer.broken",
                            "句柄已断,停止提交"};
    }
    // 锁内构造追加链并提交;落稳后 CommitEvent 统一重放内存视图。
    return impl_->AdmitLocked(message_ids, durability);
}

V3Writer::ReduceToolPreviewsResult V3Writer::ReduceToolPreviews(
    std::uint64_t new_budget_bytes, std::string_view input_hash,
    std::uint64_t estimated_tokens_before, std::uint64_t estimated_tokens_after,
    const std::vector<PreviewReplacement>& replacements,
    const std::vector<std::string>& pairing_check_refs, Durability durability) {
    ReduceToolPreviewsResult result;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->broken) {
        result.reduced_event = WriteReceipt{WriteReceipt::Status::IoFailed, "", 0, "",
                                            "v3writer.broken", "句柄已断,停止提交"};
        return result;
    }
    if (replacements.empty()) {
        result.error = "v3reducer.empty_replacements: 至少替换一条 tool 消息";
        return result;
    }
    if (new_budget_bytes >= impl_->context.preview_budget_bytes) {
        result.error = "v3reducer.not_a_reduction: 新档位须低于当前档位(只降不升,§4.38)";
        return result;
    }
    // 源链上的原消息:从账上取身份(须为已在当前链上的 tool 消息),不凭
    // 调用方复述——身份对不上宁可拒收,不造错链。
    std::unordered_map<std::string, nlohmann::json> originals;
    {
        bool truncated = false;
        auto raw_lines = ReadRawLines(impl_->journal.path(), &truncated);
        if (!raw_lines.has_value() || truncated) {
            result.error = "v3reducer.read_failed: 原账读不齐";
            return result;
        }
        for (const auto& raw : *raw_lines) {
            auto line = nlohmann::json::parse(raw, nullptr, false);
            if (line.is_discarded() || line.at("type").get<std::string>() != "message") {
                continue;
            }
            std::string ec, msg;
            auto parsed = MessageLine::FromJsonStrict(line, &ec, &msg);
            if (parsed.has_value() &&
                parsed->message.value("role", "") == "tool") {
                originals.emplace(line.at("messageId").get<std::string>(), line);
            }
        }
    }
    std::unordered_set<std::string> in_chain;
    for (const auto& node : impl_->context.chain) {
        in_chain.insert(node.message_ref);
    }
    // 1. 派生消息先落稳(不接纳;接纳由降档提交事件换链完成)。
    std::vector<std::string> replacement_ids;
    for (const auto& replacement : replacements) {
        auto it = originals.find(replacement.original_message_id);
        if (it == originals.end()) {
            result.error = "v3reducer.original_not_tool: 原消息不在账上或不是 tool 消息: " +
                           replacement.original_message_id;
            return result;
        }
        if (in_chain.count(replacement.original_message_id) == 0) {
            result.error = "v3reducer.original_not_in_chain: 原消息不在当前上下文链上: " +
                           replacement.original_message_id;
            return result;
        }
        const auto& original = it->second;
        MessageDraft derived;
        // 保留原 turnId/stepId/actionId/tool_call_id 与结果选用引用(§4.38)。
        if (original.contains("turnId") && original.at("turnId").is_string()) {
            derived.turn_id = original.at("turnId").get<std::string>();
        }
        if (original.contains("stepId") && original.at("stepId").is_string()) {
            derived.step_id = original.at("stepId").get<std::string>();
        }
        if (original.contains("actionId") && original.at("actionId").is_string()) {
            derived.action_id = original.at("actionId").get<std::string>();
        }
        derived.purpose = MessagePurpose::Conversation;
        derived.origin = MessageOrigin::ContextRuntime;  // 派生展示版本,非新执行
        derived.message = nlohmann::json::object(
            {{"role", "tool"},
             {"tool_call_id", derived.action_id.value_or(std::string())},
             {"content", replacement.new_content}});
        derived.caused_by_event_ref = replacement.caused_by_event_ref;
        derived.source_tool_message_ref = replacement.original_message_id;
        if (original.contains("resultSelectionRef") &&
            original.at("resultSelectionRef").is_string()) {
            derived.result_selection_ref = original.at("resultSelectionRef").get<std::string>();
        }
        WriteReceipt receipt = impl_->CommitMessage(std::move(derived), durability);
        if (receipt.status != WriteReceipt::Status::Committed) {
            result.replacement_messages.push_back(receipt);
            result.error = "v3reducer.message_failed: " + receipt.error_code + " " +
                           receipt.error_message;
            return result;
        }
        result.replacement_messages.push_back(receipt);
        replacement_ids.push_back(receipt.id);
    }
    // 2. 新链:原消息节点换派生消息,后续节点重接(§4.38 示例链)。
    std::vector<ChainNode> new_chain;
    std::unordered_map<std::string, std::string> swap;
    for (std::size_t i = 0; i < replacements.size(); ++i) {
        swap[replacements[i].original_message_id] = replacement_ids[i];
    }
    std::optional<std::string> prev;
    for (const auto& node : impl_->context.chain) {
        std::string ref = node.message_ref;
        auto swapped = swap.find(ref);
        if (swapped != swap.end()) {
            ref = swapped->second;
        }
        new_chain.push_back(ChainNode{ref, prev});
        prev = ref;
    }
    nlohmann::json replacement_refs = nlohmann::json::array();
    for (const auto& id : replacement_ids) {
        replacement_refs.push_back(id);
    }
    nlohmann::json pairing = nlohmann::json::array();
    for (const auto& ref : pairing_check_refs) {
        pairing.push_back(ref);
    }
    // 3. 原子提交 context.tool_previews.reduced(完整新链,revision +1)。
    EventDraft draft;
    draft.kind = EventKindV3::ContextToolPreviewsReduced;
    draft.payload = nlohmann::json::object(
        {{"contextId", impl_->context.context_id},
         {"beforeRevision", impl_->context.revision},
         {"afterRevision", impl_->context.revision + 1},
         {"oldPreviewBudget", impl_->context.preview_budget_bytes},
         {"newPreviewBudget", new_budget_bytes},
         {"replacementRefs", replacement_refs},
         {"contextChain", ChainToJson(new_chain)},
         {"inputHash", std::string(input_hash)},
         {"estimatedTokensBefore", estimated_tokens_before},
         {"estimatedTokensAfter", estimated_tokens_after},
         {"pairingCheckRefs", pairing}});
    result.reduced_event = impl_->CommitEvent(std::move(draft), durability);
    result.ok = result.reduced_event.status == WriteReceipt::Status::Committed;
    if (!result.ok) {
        result.error = "v3reducer.commit_failed: " + result.reduced_event.error_code + " " +
                       result.reduced_event.error_message;
    }
    return result;
}

V3Writer::SwitchSystemResult V3Writer::SwitchSystem(std::string_view new_system_content,
                                                    nlohmann::json change_payload,
                                                    MessageOrigin origin, Durability durability) {
    // 调用方已持锁由各步内部自理;三步各自独立提交,失败即停。
    SwitchSystemResult result;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->broken) {
            result.change_event = WriteReceipt{WriteReceipt::Status::IoFailed, "", 0, "",
                                               "v3writer.broken", "句柄已断,停止提交"};
            return result;
        }
        // 1. system.change:缘由、旧 system 引用、目标设置版本(§4.3)。
        if (!change_payload.contains("cause") || !change_payload.contains("settingsVersion")) {
            result.change_event = WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "",
                                               "v3writer.missing_change_cause",
                                               "change_payload 须含 cause 与 settingsVersion"};
            return result;
        }
        nlohmann::json payload = change_payload;
        payload["oldSystemMessageRef"] = impl_->context.system_message_ref;
        if (!payload.contains("systemChanged")) {
            payload["systemChanged"] = true;
        }
        EventDraft change;
        change.kind = EventKindV3::SystemChange;
        change.payload = std::move(payload);
        result.change_event = impl_->CommitEvent(std::move(change), durability);
        if (result.change_event.status != WriteReceipt::Status::Committed) {
            return result;
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        // 2. 新 system 消息:完整拼装正文,回指变更事件(§4.3)。
        MessageDraft system_draft;
        system_draft.purpose = MessagePurpose::Conversation;
        system_draft.origin = origin;
        system_draft.system_meta = nlohmann::json::object(
            {{"cause", change_payload.value("cause", std::string("settings_changed"))},
             {"changeEventRef", result.change_event.id},
             {"settingsVersion", change_payload.value("settingsVersion", 0)},
             {"systemChanged", change_payload.value("systemChanged", true)}});
        system_draft.message = nlohmann::json::object(
            {{"role", "system"}, {"content", std::string(new_system_content)}});
        result.system_message = impl_->CommitMessage(std::move(system_draft), durability);
        if (result.system_message.status != WriteReceipt::Status::Committed) {
            return result;
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        // 3. context.system.applied:链根换新 system,后续节点重接(§4.30)。
        std::vector<ChainNode> new_chain;
        new_chain.push_back(
            ChainNode{result.system_message.id, std::nullopt});
        for (std::size_t i = 1; i < impl_->context.chain.size(); ++i) {
            new_chain.push_back(ChainNode{impl_->context.chain[i].message_ref,
                                          new_chain.back().message_ref});
        }
        const std::uint64_t before = impl_->context.revision;
        EventDraft apply;
        apply.kind = EventKindV3::ContextSystemApplied;
        apply.payload = nlohmann::json::object(
            {{"contextId", impl_->context.context_id},
             {"beforeRevision", before},
             {"afterRevision", before + 1},
             {"rootMessageRef", result.system_message.id},
             {"contextChain", ChainToJson(new_chain)}});
        result.apply_event = impl_->CommitEvent(std::move(apply), durability);
        // 落稳后 CommitEvent 统一重放视图:链根已换新 system、revision 前移。
    }
    return result;
}

WriteReceipt V3Writer::PrepareRequest(std::string_view request_id, std::string_view turn_id,
                                      std::string_view step_id, std::string_view purpose,
                                      std::string_view system_message_ref,
                                      const std::vector<std::string>& input_message_refs,
                                      nlohmann::json provider_snapshot,
                                      std::optional<std::string> compact_id,
                                      Durability durability) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->broken) {
        return WriteReceipt{WriteReceipt::Status::IoFailed, "", 0, "", "v3writer.broken",
                            "句柄已断,停止提交"};
    }
    // 引用先落稳才许发(§4.4):空数组、找不到的引用不能"从历史猜一份"。
    if (impl_->message_ids.count(std::string(system_message_ref)) == 0) {
        return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "", "v3writer.dangling_ref",
                            "systemMessageRef 未落稳: " + std::string(system_message_ref)};
    }
    nlohmann::json inputs = nlohmann::json::array();
    for (const auto& id : input_message_refs) {
        if (impl_->message_ids.count(id) == 0) {
            return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "",
                                "v3writer.dangling_ref", "inputMessageRefs 未落稳: " + id};
        }
        inputs.push_back(id);
    }
    EventDraft draft;
    draft.kind = EventKindV3::ModelRequestPrepared;
    draft.turn_id = std::string(turn_id);
    draft.step_id = std::string(step_id);
    draft.request_id = std::string(request_id);
    draft.compact_id = compact_id;
    draft.payload = nlohmann::json::object(
        {{"purpose", std::string(purpose)},
         {"contextId", impl_->context.context_id},
         {"contextRevision", impl_->context.revision},
         {"systemMessageRef", std::string(system_message_ref)},
         {"inputMessageRefs", inputs},
         {"readThroughSeq", impl_->next_seq - 1},
         {"readThroughHash", impl_->last_hash}});
    for (auto it = provider_snapshot.begin(); it != provider_snapshot.end(); ++it) {
        draft.payload[it.key()] = it.value();
    }
    return impl_->CommitEvent(std::move(draft), durability);
}

WriteReceipt V3Writer::BeginStreamResponse(std::string_view request_id,
                                           std::string_view stream_id, std::string_view turn_id,
                                           std::string_view step_id,
                                           std::string_view reserved_message_id,
                                           Durability durability) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->broken) {
        return WriteReceipt{WriteReceipt::Status::IoFailed, "", 0, "", "v3writer.broken",
                            "句柄已断,停止提交"};
    }
    EventDraft draft;
    draft.kind = EventKindV3::ModelResponseStarted;
    draft.turn_id = std::string(turn_id);
    draft.step_id = std::string(step_id);
    draft.request_id = std::string(request_id);
    draft.payload = nlohmann::json::object(
        {{"requestId", std::string(request_id)},
         {"streamId", std::string(stream_id)},
         {"messageId", std::string(reserved_message_id)}});
    return impl_->CommitEvent(std::move(draft), durability);
}

WriteReceipt V3Writer::AppendStreamDelta(std::string_view request_id, std::string_view stream_id,
                                         std::string_view message_id, std::uint64_t sequence,
                                         std::string_view delta_type, nlohmann::json content,
                                         Durability durability) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->broken) {
        return WriteReceipt{WriteReceipt::Status::IoFailed, "", 0, "", "v3writer.broken",
                            "句柄已断,停止提交"};
    }
    EventDraft draft;
    draft.kind = EventKindV3::ModelResponseDelta;
    draft.request_id = std::string(request_id);
    draft.payload = nlohmann::json::object(
        {{"requestId", std::string(request_id)},
         {"streamId", std::string(stream_id)},
         {"messageId", std::string(message_id)},
         {"sequence", sequence},
         {"deltaType", std::string(delta_type)},
         {"content", std::move(content)}});
    return impl_->CommitEvent(std::move(draft), durability);
}

WriteReceipt V3Writer::CompleteStreamResponse(
    std::string_view request_id, std::string_view stream_id, std::string_view turn_id,
    std::string_view step_id, std::string_view message_id, nlohmann::json message,
    std::string_view provider, std::string_view wire, std::string_view model,
    nlohmann::json response_model, nlohmann::json usage, std::string_view finish_reason,
    MessagePurpose purpose, std::optional<std::string> compact_id,
    std::optional<CompletionStatus> completion_status, Durability durability) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->broken) {
        return WriteReceipt{WriteReceipt::Status::IoFailed, "", 0, "", "v3writer.broken",
                            "句柄已断,停止提交"};
    }
    // 1. 终态事件:协议终态及片段范围已落稳(§4.43)。
    {
        EventDraft done;
        done.kind = EventKindV3::ModelResponseCompleted;
        done.status = OpStatus::Done;
        done.turn_id = std::string(turn_id);
        done.step_id = std::string(step_id);
        done.request_id = std::string(request_id);
        done.payload = nlohmann::json::object(
            {{"requestId", std::string(request_id)},
             {"streamId", std::string(stream_id)},
             {"messageId", std::string(message_id)},
             {"finishReason", std::string(finish_reason)}});
        WriteReceipt receipt = impl_->CommitEvent(std::move(done), durability);
        if (receipt.status != WriteReceipt::Status::Committed) {
            return receipt;
        }
    }
    // 2. 完整 assistant(预留 id 成行;来源与 usage 自带,§4.44/§4.12)。
    {
        MessageDraft assistant;
        assistant.message_id_override = std::string(message_id);  // 预留 id 成行
        assistant.turn_id = std::string(turn_id);
        assistant.step_id = std::string(step_id);
        assistant.request_id = std::string(request_id);
        assistant.compact_id = compact_id;
        assistant.purpose = purpose;
        assistant.origin = MessageOrigin::SessionRuntime;
        assistant.display = purpose == MessagePurpose::Conversation
                                ? std::optional<DisplayMode>(DisplayMode::Visible)
                                : std::optional<DisplayMode>(DisplayMode::Hidden);
        assistant.message = std::move(message);
        assistant.completion_status = completion_status;
        assistant.provider = std::string(provider);
        assistant.wire = std::string(wire);
        assistant.model = std::string(model);
        assistant.response_model = std::move(response_model);
        assistant.usage = std::move(usage);
        WriteReceipt receipt = impl_->CommitMessage(std::move(assistant), durability);
        if (receipt.status != WriteReceipt::Status::Committed) {
            return receipt;
        }
        // 3. 接纳进当前上下文(compact 内部回复不进 main 链,§4.8)。
        // 返回 message 回执;接纳失败时带回接纳错误。
        if (purpose == MessagePurpose::Conversation) {
            WriteReceipt admit = impl_->AdmitLocked({receipt.id}, durability);
            if (admit.status != WriteReceipt::Status::Committed) {
                return admit;
            }
        }
        return receipt;
    }
}

WriteReceipt V3Writer::InterruptStreamResponse(
    std::string_view request_id, std::string_view stream_id, std::string_view turn_id,
    std::string_view step_id, std::string_view message_id, nlohmann::json message,
    std::string_view provider, std::string_view wire, std::string_view model,
    std::uint64_t received_through, std::optional<nlohmann::json> usage, MessagePurpose purpose,
    std::optional<std::string> compact_id, Durability durability) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->broken) {
        return WriteReceipt{WriteReceipt::Status::IoFailed, "", 0, "", "v3writer.broken",
                            "句柄已断,停止提交"};
    }
    // 1. 定稿事件:固定接收水位(§4.63)。
    {
        EventDraft cancelled;
        cancelled.kind = EventKindV3::ModelResponseCancelled;
        cancelled.status = OpStatus::Cancelled;
        cancelled.turn_id = std::string(turn_id);
        cancelled.step_id = std::string(step_id);
        cancelled.request_id = std::string(request_id);
        cancelled.payload = nlohmann::json::object(
            {{"requestId", std::string(request_id)},
             {"streamId", std::string(stream_id)},
             {"messageId", std::string(message_id)},
             {"receivedThrough", received_through}});
        WriteReceipt receipt = impl_->CommitEvent(std::move(cancelled), durability);
        if (receipt.status != WriteReceipt::Status::Committed) {
            return receipt;
        }
    }
    // 2. 已收到内容组装成正式 interrupted assistant(§1.28/§4.63):
    // 正文保留,未收齐的调用/签名不伪造完整;usage 缺实报为 null 不补 0。
    {
        MessageDraft assistant;
        assistant.message_id_override = std::string(message_id);  // 预留 id 成行
        assistant.turn_id = std::string(turn_id);
        assistant.step_id = std::string(step_id);
        assistant.request_id = std::string(request_id);
        assistant.compact_id = compact_id;
        assistant.purpose = purpose;
        assistant.origin = MessageOrigin::SessionRuntime;
        assistant.display = purpose == MessagePurpose::Conversation
                                ? std::optional<DisplayMode>(DisplayMode::Visible)
                                : std::optional<DisplayMode>(DisplayMode::Hidden);
        assistant.message = std::move(message);
        assistant.completion_status = CompletionStatus::Interrupted;
        assistant.provider = std::string(provider);
        assistant.wire = std::string(wire);
        assistant.model = std::string(model);
        assistant.response_model = nlohmann::json(nullptr);
        assistant.usage = usage.has_value() ? std::move(*usage) : nlohmann::json(nullptr);
        WriteReceipt receipt = impl_->CommitMessage(std::move(assistant), durability);
        if (receipt.status != WriteReceipt::Status::Committed) {
            return receipt;
        }
        if (purpose == MessagePurpose::Conversation) {
            WriteReceipt admit = impl_->AdmitLocked({receipt.id}, durability);
            if (admit.status != WriteReceipt::Status::Committed) {
                return admit;
            }
        }
        return receipt;
    }
}

std::string V3Writer::NewMessageId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("msg", 0);
}
std::string V3Writer::NewEventId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("evt", 1);
}
std::string V3Writer::NewTurnId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("turn", 2);
}
std::string V3Writer::NewCompactTurnId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("compact-turn", 2);
}
std::string V3Writer::NewGoalEvalTurnId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("goaleval-turn", 2);
}
std::string V3Writer::NewStepId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("step", 3);
}
std::string V3Writer::NewRequestId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("request", 4);
}
std::string V3Writer::NewStreamId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("stream", 5);
}
std::string V3Writer::NewCompactId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("compact", 6);
}
std::string V3Writer::NewActionId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("action", 7);
}
std::string V3Writer::NewHookDispatchId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("hookdispatch", 8);
}
std::string V3Writer::NewTaskId() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->NextId("task", 9);
}

const std::filesystem::path& V3Writer::path() const { return impl_->journal.path(); }
std::uint64_t V3Writer::next_seq() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->next_seq;
}
std::string V3Writer::last_line_hash() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last_hash;
}
bool V3Writer::broken() const { return impl_->broken; }
const ContextView& V3Writer::context() const { return impl_->context; }
bool V3Writer::HasMessageId(std::string_view message_id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->message_ids.count(std::string(message_id)) > 0;
}
const std::string& V3Writer::session_id() const { return impl_->session_id; }
const std::string& V3Writer::run_id() const { return impl_->run_id; }

}  // namespace lubancode::trajectory::v3
