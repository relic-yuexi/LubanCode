// storage_migrator.hpp 的实现(P0-5)。
//
// 结构:
//   1. 基础件(读文件/hash/原子写/时钟);
//   2. 旧档行模型与解析(旧 JSONL 只在此处读,生产零依赖);
//   3. 单场导入器(旧消息/事件 -> typed trajectory 事件流);
//   4. 旧 Memory 迁移(schema 1/2/3 主题 -> 新 workspace memory,schema 3);
//   5. 回执读写(intent/progress/result,合同 §五);
//   6. plan/run/status 三口。
//
// 红线:迁移器只读旧档写新账;旧源一字不动(--delete-source 除外,且逐件
// 复验)。目标写坏时旧源照旧,续跑删除半截目标重建。
#include "workspace/storage_migrator.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "hooks/hash.hpp"
#include "memory/frontmatter.hpp"
#include "memory/project_memory.hpp"
#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/text_encoding.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/safety.hpp"
#include "trajectory/session_lock.hpp"
#include "trajectory/session_manager.hpp"
#include "tools/tool_content.hpp"
#include "workspace/identity.hpp"
#include "workspace/manifest.hpp"
#include "workspace/storage_contracts.hpp"

namespace lubancode::workspace::migrator {

namespace {

namespace fs = std::filesystem;
using platform::PathToUtf8;
using platform::Utf8ToPath;

// ---------------------------------------------------------------------------
// 0. 故障注入的中断信号
// ---------------------------------------------------------------------------

// Fault(point) 回 true 时抛出;RunStorageMigration 捕获后按
// migration.interrupted 收口(磁盘停在越过的耐久点之后,与真崩溃同形)。
struct MigrationInterrupted : std::runtime_error {
    explicit MigrationInterrupted(const std::string& at_point)
        : std::runtime_error("migration.interrupted@" + at_point), point(at_point) {}
    std::string point;
};

// ---------------------------------------------------------------------------
// 1. 基础件
// ---------------------------------------------------------------------------

std::int64_t NowWallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<std::string> ReadFileBytes(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string Sha256Text(std::string_view bytes) { return hooks::Sha256Hex(bytes); }

bool AtomicWriteText(const fs::path& path, const std::string& content) {
    return platform::AtomicWriteFile(path, content).has_value();
}

std::optional<nlohmann::json> ParseJsonFile(const fs::path& path) {
    const auto bytes = ReadFileBytes(path);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    nlohmann::json json = nlohmann::json::parse(*bytes, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        return std::nullopt;
    }
    return json;
}

// 旧档时间串 "yyyy-mm-dd HH:MM:SS"(本地时)-> epoch 毫秒。解析不动给 0
//(导入场 wall_time 只是回放辅助,0 不拦任何校验)。
std::int64_t LegacyLocalTimestampToMs(const std::string& text) {
    if (text.size() < 19) {
        return 0;
    }
    std::tm tm{};
    tm.tm_year = std::atoi(text.substr(0, 4).c_str()) - 1900;
    tm.tm_mon = std::atoi(text.substr(5, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(text.substr(8, 2).c_str());
    tm.tm_hour = std::atoi(text.substr(11, 2).c_str());
    tm.tm_min = std::atoi(text.substr(14, 2).c_str());
    tm.tm_sec = std::atoi(text.substr(17, 2).c_str());
    tm.tm_isdst = -1;
    const std::time_t epoch = std::mktime(&tm);
    if (epoch == static_cast<std::time_t>(-1)) {
        return 0;
    }
    return static_cast<std::int64_t>(epoch) * 1000;
}

// 迁移钟:wall 由导入器按旧档 ts 逐事件推(单调不倒退);monotonic 只管
// 单调递增。注入 Recorder 的时钟 seam,旧档时间序因此保真。
class MigrationClock : public trajectory::RecorderClock {
public:
    void AdvanceTo(std::int64_t wall_ms) {
        if (wall_ms > wall_) {
            wall_ = wall_ms;
        }
    }

    std::int64_t WallMs() const override { return wall_ > 0 ? wall_ : NowWallMs(); }
    std::int64_t MonotonicNs() const override {
        mono_ += 1000;
        return mono_;
    }

private:
    std::int64_t wall_ = 0;
    mutable std::int64_t mono_ = 0;
};

// ---------------------------------------------------------------------------
// 2. 旧档行模型(旧 JSONL 只在这里被解释)
// ---------------------------------------------------------------------------

struct LegacyLine {
    bool is_event = false;
    nlohmann::json json;
};

struct LegacySessionDoc {
    std::string wire;
    std::string model;
    std::string cwd;
    std::string started_at;
    std::vector<LegacyLine> lines;
    std::size_t skipped_lines = 0;
};

// 旧 meta 首行 + 逐行分类。首行不是合法 meta 给 nullopt(压根不是本工具
// 的旧档)。坏行跳过不废整场(与旧 ParseSessionFile 同一取舍)。
std::optional<LegacySessionDoc> ParseLegacySessionDoc(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    if (!std::getline(stream, line)) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const nlohmann::json meta = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!meta.is_object() || !meta.contains("version") || !meta["version"].is_number_integer()) {
        return std::nullopt;
    }
    LegacySessionDoc doc;
    doc.wire = meta.value("wire", std::string());
    doc.model = meta.value("model", std::string());
    doc.cwd = meta.value("cwd", std::string());
    doc.started_at = meta.value("started_at", std::string());
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const nlohmann::json json = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (!json.is_object()) {
            doc.skipped_lines += 1;
            continue;
        }
        LegacyLine entry;
        entry.is_event = json.contains("type") && json["type"].is_string();
        entry.json = json;
        doc.lines.push_back(std::move(entry));
    }
    return doc;
}

// ---------------------------------------------------------------------------
// 3. 单场导入器
// ---------------------------------------------------------------------------

// 旧会话目录旁挂文件的搬运口(mcp-artifacts/images/context blob)。
struct ArtifactSink {
    fs::path old_session_dir;       // sessions/<id>/(可能不存在)
    fs::path new_session_dir;       // workspaces/<key>/sessions/<id>/
    std::vector<std::string>* missing;

    // 字节齐:搬进新场 artifacts/sha256/<sha>.<ext>(内容寻址,幂等)。
    // 返回新相对路径;字节缺/hash 不合给空串并记缺口(不冒充可取)。
    std::string CopyArtifact(const std::string& relative_path, const std::string& sha256) {
        if (relative_path.empty()) {
            return std::string();
        }
        std::error_code ec;
        const fs::path source = old_session_dir / Utf8ToPath(relative_path);
        if (!fs::is_regular_file(source, ec) || ec) {
            missing->push_back("artifact 字节缺失: " + relative_path);
            return std::string();
        }
        const auto bytes = ReadFileBytes(source);
        if (!bytes.has_value()) {
            missing->push_back("artifact 读不出: " + relative_path);
            return std::string();
        }
        const std::string actual = Sha256Text(*bytes);
        if (!sha256.empty() && actual != sha256) {
            missing->push_back("artifact hash 不合(不搬): " + relative_path);
            return std::string();
        }
        const std::string extension = source.extension().string();
        const std::string name = actual + extension;
        const fs::path target = new_session_dir / "artifacts" / "sha256" / Utf8ToPath(name);
        if (!fs::exists(target, ec) || ec) {
            if (!platform::AtomicWriteFile(target, *bytes).has_value()) {
                missing->push_back("artifact 写不进: " + PathToUtf8(target));
                return std::string();
            }
        }
        return "artifacts/sha256/" + name;
    }
};

struct SessionImportOutcome {
    std::string terminal_event_hash;
    bool legacy_partial = false;
    std::vector<std::string> missing;
    std::uint64_t event_count = 0;
    std::uint64_t imported_messages = 0;
    std::uint64_t imported_tool_results = 0;
};

class SessionImporter {
public:
    SessionImporter(trajectory::TrajectoryRecorder* recorder, MigrationClock* clock,
                    const LegacySessionDoc* doc, ArtifactSink artifacts,
                    std::function<bool(const std::string&)> fault)
        : recorder_(recorder), clock_(clock), doc_(doc), artifacts_(std::move(artifacts)),
          fault_(std::move(fault)) {}

    // 走完整本旧档,落 run.started → … → run terminal → session.ended。
    // 抛 MigrationInterrupted(故障注入);写失败回 false(error 带稳定码)。
    bool Run(SessionImportOutcome* outcome, std::string* error) {
        wall_ = LegacyLocalTimestampToMs(doc_->started_at);
        clock_->AdvanceTo(wall_);
        if (!WriteRunStarted(error)) {
            return false;
        }
        first_event_hash_ = last_event_hash_;
        FoldTraceLedger();
        for (const LegacyLine& line : doc_->lines) {
            if (!ImportLine(line, error)) {
                return false;
            }
        }
        CloseOpenTurn();
        if (!FinishRunAndSession(outcome, error)) {
            return false;
        }
        outcome->legacy_partial = legacy_partial_;
        outcome->missing = missing_;
        outcome->imported_messages = imported_messages_;
        outcome->imported_tool_results = imported_results_;
        return true;
    }

private:
    // ---- 事件写口(薄壳:receipt 检查 + 故障点 + hash 记账) ----
    bool Put(trajectory::EventKind kind, trajectory::EventScope scope, trajectory::EventLinks links,
             nlohmann::json payload, trajectory::Durability durability, const char* where,
             std::string* error) {
        trajectory::RecordRequest request;
        request.kind = kind;
        request.scope = std::move(scope);
        request.links = std::move(links);
        request.payload = std::move(payload);
        const trajectory::RecordReceipt receipt = recorder_->Record(std::move(request), durability);
        if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
            *error = std::string(contracts::kErrMigrationTargetWriteFailed) + ": " + where + ": " +
                     receipt.error_code;
            return false;
        }
        last_event_id_ = receipt.event_id;
        last_event_hash_ = receipt.event_hash;
        outcome_events_ = receipt.seq;
        if (fault_ != nullptr && fault_("event_committed")) {
            throw MigrationInterrupted("event_committed");
        }
        return true;
    }

    bool WriteRunStarted(std::string* error) {
        trajectory::EventScope scope = recorder_->base_scope();
        nlohmann::json payload{
            {"run_kind", trajectory::RunKindName(trajectory::RunKind::MainSession)},
            {"start_reason", std::string(contracts::kStartReasonLegacyImport)},
            {"writer_version", "storage-migrator-v1"},
            {"min_reader_version", static_cast<std::uint64_t>(1)}};
        return Put(trajectory::EventKind::RunStarted, std::move(scope), {}, std::move(payload),
                   trajectory::Durability::PowerLoss, "run.started", error);
    }

    // ---- tool_trace_v1 行折叠(生产 ToolExecutionLedger 复用:纯函数,
    // 工具追踪事件本身仍是现行生产事件,只有旧档持久化是 legacy) ----
    void FoldTraceLedger() {
        for (const LegacyLine& line : doc_->lines) {
            if (!line.is_event || line.json.value("type", std::string()) != "tool_trace_v1") {
                continue;
            }
            if (const auto event = agent::ParseToolTraceEvent(line.json.dump())) {
                trace_ledger_.Fold(*event);
            }
        }
    }

    std::optional<agent::ToolOutcome> TraceOutcomeOf(const std::string& tool_use_id) const {
        const auto records = trace_ledger_.FindByToolUse(tool_use_id);
        if (records.empty()) {
            return std::nullopt;
        }
        return records.front()->outcome;
    }

    bool TraceNeverStarted(const std::string& tool_use_id) const {
        const auto outcome = TraceOutcomeOf(tool_use_id);
        return outcome.has_value() && agent::OutcomeNeverStarted(*outcome);
    }

    // ---- 行分派 ----
    bool ImportLine(const LegacyLine& line, std::string* error) {
        if (!line.is_event) {
            return ImportMessage(line.json, error);
        }
        const std::string type = line.json.value("type", std::string());
        AdvanceClock(line.json.value("ts", std::string()));
        if (type == "compact" || type == "compact_v2") {
            return ImportCompact(line.json, error);
        }
        if (type == "title") {
            const std::string title = line.json.value("title", std::string());
            if (title.empty()) {
                skipped_lines_ += 1;
                return true;
            }
            trajectory::EventScope scope = recorder_->base_scope();
            scope.actor = trajectory::Actor::User;
            scope.origin = trajectory::Origin::ExternalUser;
            nlohmann::json payload{{"title", title}};
            if (!last_title_.empty()) {
                payload["old_title"] = last_title_;
            }
            last_title_ = title;
            return Put(trajectory::EventKind::ControlTitleChanged, std::move(scope), {},
                       std::move(payload), trajectory::Durability::ProcessCrash,
                       "control.title.changed", error);
        }
        if (type == "cwd") {
            const std::string cwd = line.json.value("cwd", std::string());
            if (cwd.empty()) {
                skipped_lines_ += 1;
                return true;
            }
            trajectory::EventScope scope = recorder_->base_scope();
            return Put(trajectory::EventKind::ControlCwdChanged, std::move(scope), {},
                       nlohmann::json{{"cwd", cwd}}, trajectory::Durability::ProcessCrash,
                       "control.cwd.changed", error);
        }
        if (type == "queue") {
            if (!line.json.contains("items") || !line.json["items"].is_array()) {
                skipped_lines_ += 1;
                return true;
            }
            nlohmann::json items = nlohmann::json::array();
            for (const auto& item : line.json["items"]) {
                if (!item.is_object()) {
                    continue;
                }
                items.push_back(nlohmann::json{
                    {"item_id", std::to_string(item.value("id", std::uint64_t{0}))},
                    {"target", item.value("target", std::string("main"))},
                    {"text", platform::SanitizeExternalText(item.value("text", std::string()))},
                    {"attempts", item.value("attempts", 0)}});
            }
            trajectory::EventScope scope = recorder_->base_scope();
            scope.actor = trajectory::Actor::User;
            scope.origin = trajectory::Origin::ExternalUser;
            return Put(trajectory::EventKind::ControlQueueSnapshot, std::move(scope), {},
                       nlohmann::json{{"items", std::move(items)}, {"reason", "legacy_import"}},
                       trajectory::Durability::ProcessCrash, "control.queue.snapshot", error);
        }
        if (type == "mode_v1") {
            const std::string mode = line.json.value("mode", std::string());
            if (mode.empty()) {
                skipped_lines_ += 1;
                return true;
            }
            trajectory::EventScope scope = recorder_->base_scope();
            return Put(trajectory::EventKind::ControlModeChanged, std::move(scope), {},
                       nlohmann::json{{"mode", mode}, {"reason", line.json.value("reason", "resume")}},
                       trajectory::Durability::ProcessCrash, "control.mode.changed", error);
        }
        if (type == "tool_trace_v1") {
            return true;  // 已折叠进 ledger,终态结论融进工具链
        }
        if (type.rfind("goal_", 0) == 0) {
            goal_events_ += 1;
            legacy_partial_ = true;
            return true;
        }
        if (type == "plan_v1" || type == "plan_review_v1") {
            plan_events_ += 1;
            legacy_partial_ = true;
            return true;
        }
        if (type == "think_history_v1") {
            think_events_ += 1;
            legacy_partial_ = true;
            return true;
        }
        skipped_lines_ += 1;  // 认不得的事件行:跳过(旧档通用约定)
        return true;
    }

    void AdvanceClock(const std::string& ts) {
        const std::int64_t parsed = LegacyLocalTimestampToMs(ts);
        wall_ = parsed > wall_ ? parsed : wall_ + 1;
        clock_->AdvanceTo(wall_);
    }

    // ---- 消息行 ----
    bool ImportMessage(const nlohmann::json& json, std::string* error) {
        const std::string role = json.value("role", std::string());
        if (!json.contains("content") || !json["content"].is_array() ||
            (role != "user" && role != "assistant")) {
            skipped_lines_ += 1;
            return true;
        }
        AdvanceClock(json.value("ts", std::string()));
        const nlohmann::json& blocks = json["content"];
        // 投影锚(compact.applied 的 old_state_hash 用,角色+内容拼串)。
        effective_history_key_ += role + "|" + blocks.dump() + "\n";
        const bool has_tool_result = AnyBlock(blocks, "tool_result");
        if (role == "user" && has_tool_result) {
            return ImportToolResultMessage(blocks, error);
        }
        if (role == "user") {
            return ImportUserMessage(blocks, error);
        }
        return ImportAssistantMessage(blocks, error);
    }

    static bool AnyBlock(const nlohmann::json& blocks, const char* type) {
        for (const auto& block : blocks) {
            if (block.is_object() && block.value("type", std::string()) == type) {
                return true;
            }
        }
        return false;
    }

    bool ImportUserMessage(const nlohmann::json& blocks, std::string* error) {
        CloseOpenTurn();
        if (!OpenTurn("external_user", error)) {
            return false;
        }
        nlohmann::json content = nlohmann::json::array();
        std::size_t non_text = 0;
        for (const auto& block : blocks) {
            if (!block.is_object()) {
                non_text += 1;
                continue;
            }
            const std::string type = block.value("type", std::string());
            if (type == "text") {
                content.push_back(nlohmann::json{
                    {"type", "text"},
                    {"text", platform::SanitizeExternalText(block.value("text", std::string()))}});
            } else if (type == "image") {
                content.push_back(nlohmann::json{
                    {"type", "image"},
                    {"filename", platform::SanitizeExternalText(block.value("filename", std::string()))}});
            } else {
                non_text += 1;
            }
        }
        if (non_text > 0) {
            missing_.push_back("user 消息含 " + std::to_string(non_text) +
                               " 块不可还原类型(旧档块形状无对应投影)");
            legacy_partial_ = true;
        }
        trajectory::EventScope scope = recorder_->base_scope();
        scope.turn_id = turn_id_;
        scope.actor = trajectory::Actor::User;
        scope.origin = trajectory::Origin::ExternalUser;
        nlohmann::json payload{{"input_id", NextId("input")},
                               {"content", std::move(content)},
                               {"channel", "legacy_import"},
                               {"sender", nlohmann::json{{"kind", "local_user"}}}};
        if (!Put(trajectory::EventKind::InputReceived, std::move(scope), {}, std::move(payload),
                 trajectory::Durability::ProcessCrash, "input.received", error)) {
            return false;
        }
        last_input_event_id_ = last_event_id_;
        imported_messages_ += 1;
        return true;
    }

    bool ImportAssistantMessage(const nlohmann::json& blocks, std::string* error) {
        if (!turn_open_) {
            // 旧档首条就是 assistant(异常但存在):开一只宿主回合接住。
            if (!OpenTurn("legacy_import", error)) {
                return false;
            }
            legacy_partial_ = true;
            missing_.push_back("assistant 消息先于任何用户输入(宿主回合兜底)");
        }
        const std::string request_id = NextId("req");
        trajectory::EventScope scope = recorder_->base_scope();
        scope.turn_id = turn_id_;
        scope.request_id = request_id;
        nlohmann::json prepared{{"model", doc_->model.empty() ? "unknown_legacy" : doc_->model},
                                {"provider", "legacy"},
                                {"wire", doc_->wire.empty() ? "unknown" : doc_->wire},
                                {"purpose", "main_turn"}};
        nlohmann::json message_refs = nlohmann::json::array();
        if (!last_input_event_id_.empty()) {
            message_refs.push_back(last_input_event_id_);
        }
        prepared["message_refs"] = std::move(message_refs);
        if (!Put(trajectory::EventKind::ModelRequestPrepared, scope, {}, std::move(prepared),
                 trajectory::Durability::ProcessCrash, "model.request.prepared", error)) {
            return false;
        }
        const std::string prepared_event_id = last_event_id_;
        if (!Put(trajectory::EventKind::ModelRequestSent, scope, {},
                 nlohmann::json{{"prepared_event_id", prepared_event_id}},
                 trajectory::Durability::ProcessCrash, "model.request.sent", error)) {
            return false;
        }
        nlohmann::json output_blocks = nlohmann::json::array();
        std::vector<std::string> declared_calls;
        for (const auto& block : blocks) {
            if (!block.is_object()) {
                continue;
            }
            const std::string type = block.value("type", std::string());
            if (type == "text") {
                output_blocks.push_back(nlohmann::json{
                    {"type", "text"},
                    {"text", platform::SanitizeExternalText(block.value("text", std::string()))}});
            } else if (type == "thinking") {
                output_blocks.push_back(nlohmann::json{
                    {"type", "thinking"},
                    {"text", platform::SanitizeExternalText(block.value("text", std::string()))}});
            } else if (type == "tool_use") {
                const std::string call_id =
                    platform::SanitizeExternalText(block.value("id", std::string()));
                const std::string name =
                    platform::SanitizeExternalText(block.value("name", std::string()));
                nlohmann::json arguments =
                    block.contains("input") && block["input"].is_object() ? block["input"]
                                                                          : nlohmann::json::object();
                tools::SanitizeJsonTextInPlace(arguments);
                output_blocks.push_back(nlohmann::json{{"type", "tool_call"},
                                                       {"call_id", call_id},
                                                       {"provider_call_id", call_id},
                                                       {"name", name},
                                                       {"arguments", std::move(arguments)}});
                declared_calls.push_back(call_id);
            } else if (type == "model_image") {
                // 模型输出图:字节搬进新场 artifacts,引用块改内容寻址路径。
                const std::string sha = block.value("sha256", std::string());
                const std::string moved =
                    artifacts_.CopyArtifact(block.value("path", std::string()), sha);
                nlohmann::json projected{{"type", "image_ref"},
                                         {"mime_type", block.value("mime_type", std::string())},
                                         {"sha256", sha},
                                         {"stored", !moved.empty()}};
                if (block.contains("width") && block["width"].is_number_unsigned()) {
                    projected["width"] = block["width"];
                }
                if (block.contains("height") && block["height"].is_number_unsigned()) {
                    projected["height"] = block["height"];
                }
                if (block.contains("bytes") && block["bytes"].is_number_unsigned()) {
                    projected["bytes"] = block["bytes"];
                }
                if (!moved.empty()) {
                    projected["path"] = moved;
                } else {
                    projected["path"] = block.value("path", std::string());
                    legacy_partial_ = true;
                }
                output_blocks.push_back(std::move(projected));
            } else if (type == "server_tool_use" || type == "tool_search_tool_result") {
                legacy_partial_ = true;
                missing_.push_back("assistant 消息含 " + type + " 块,无对应迁移投影,跳过");
            }
        }
        nlohmann::json payload{{"output_id", NextId("output")},
                               {"blocks", std::move(output_blocks)},
                               {"stop_reason", "end_turn"}};
        trajectory::EventScope output_scope = recorder_->base_scope();
        output_scope.turn_id = turn_id_;
        output_scope.request_id = request_id;
        output_scope.actor = trajectory::Actor::Model;
        output_scope.origin = trajectory::Origin::ProviderModel;
        if (!Put(trajectory::EventKind::ModelOutputCompleted, std::move(output_scope), {},
                 std::move(payload), trajectory::Durability::ProcessCrash,
                 "model.output.completed", error)) {
            return false;
        }
        // 声明的 call 登记(§6.1:call 由模型输出定义)。
        for (const std::string& call_id : declared_calls) {
            CallBook& book = calls_[call_id];
            book.request_id = request_id;
            book.tool_name = ToolNameOf(blocks, call_id);
            book.arguments = ToolArgumentsOf(blocks, call_id);
            seen_calls_.insert(call_id);
        }
        imported_messages_ += 1;
        return true;
    }

    std::string ToolNameOf(const nlohmann::json& blocks, const std::string& call_id) const {
        for (const auto& block : blocks) {
            if (block.is_object() && block.value("type", std::string()) == "tool_use" &&
                block.value("id", std::string()) == call_id) {
                return block.value("name", std::string("unknown_legacy"));
            }
        }
        return "unknown_legacy";
    }

    nlohmann::json ToolArgumentsOf(const nlohmann::json& blocks, const std::string& call_id) const {
        for (const auto& block : blocks) {
            if (block.is_object() && block.value("type", std::string()) == "tool_use" &&
                block.value("id", std::string()) == call_id) {
                return block.contains("input") && block["input"].is_object() ? block["input"]
                                                                             : nlohmann::json::object();
            }
        }
        return nlohmann::json::object();
    }

    // 结果消息:混排的非结果文本先落 input.received(保内容、保次序),
    // 再逐枚结果块走 tool.result.committed。
    bool ImportToolResultMessage(const nlohmann::json& blocks, std::string* error) {
        if (!turn_open_) {
            if (!OpenTurn("legacy_import", error)) {
                return false;
            }
            legacy_partial_ = true;
            missing_.push_back("tool_result 先于任何回合(宿主回合兜底)");
        }
        nlohmann::json lead_in = nlohmann::json::array();
        for (const auto& block : blocks) {
            if (block.is_object() && block.value("type", std::string()) == "text") {
                lead_in.push_back(nlohmann::json{
                    {"type", "text"},
                    {"text", platform::SanitizeExternalText(block.value("text", std::string()))}});
            }
        }
        if (!lead_in.empty()) {
            trajectory::EventScope scope = recorder_->base_scope();
            scope.turn_id = turn_id_;
            scope.actor = trajectory::Actor::User;
            scope.origin = trajectory::Origin::ExternalUser;
            nlohmann::json payload{{"input_id", NextId("input")},
                                   {"content", std::move(lead_in)},
                                   {"channel", "legacy_import"},
                                   {"sender", nlohmann::json{{"kind", "local_user"}}}};
            if (!Put(trajectory::EventKind::InputReceived, std::move(scope), {}, std::move(payload),
                     trajectory::Durability::ProcessCrash, "input.received(mixed)", error)) {
                return false;
            }
        }
        return ImportToolResultBlocks(blocks, error);
    }

    bool ImportToolResultBlocks(const nlohmann::json& blocks, std::string* error) {
        for (const auto& block : blocks) {
            if (!block.is_object() || block.value("type", std::string()) != "tool_result") {
                continue;
            }
            const std::string call_id =
                platform::SanitizeExternalText(block.value("tool_use_id", std::string()));
            auto it = calls_.find(call_id);
            if (it == calls_.end()) {
                legacy_partial_ = true;
                if (seen_calls_.count(call_id) != 0) {
                    missing_.push_back("迟到的 tool_result(所属回合已收口,丢弃): " + call_id);
                } else {
                    missing_.push_back("孤儿 tool_result(无对应 tool_use,丢弃): " + call_id);
                }
                continue;
            }
            if (!EnsureCallChain(it->second, call_id, error)) {
                return false;
            }
            nlohmann::json content = nlohmann::json::array();
            const std::string text = block.value("content", std::string());
            if (!text.empty()) {
                content.push_back(
                    nlohmann::json{{"type", "text"}, {"text", platform::SanitizeExternalText(text)}});
            }
            if (block.contains("blocks") && block["blocks"].is_array()) {
                for (const auto& rich : block["blocks"]) {
                    content.push_back(ProjectRichBlock(rich));
                }
            }
            nlohmann::json payload{{"call_id", call_id},
                                   {"content", std::move(content)},
                                   {"is_error", block.value("is_error", false)}};
            if (block.contains("structured_content") && block["structured_content"].is_object()) {
                nlohmann::json structured = block["structured_content"];
                tools::SanitizeJsonTextInPlace(structured);
                payload["structured_content"] = std::move(structured);
            }
            if (!it->second.terminal_event_id.empty()) {
                payload["derived_from_event"] = it->second.terminal_event_id;
            }
            trajectory::EventScope scope = recorder_->base_scope();
            scope.turn_id = turn_id_;
            scope.request_id = it->second.request_id;
            scope.call_id = call_id;
            scope.actor = trajectory::Actor::Tool;
            scope.origin = trajectory::Origin::BuiltinTool;
            if (!Put(trajectory::EventKind::ToolResultCommitted, std::move(scope), {},
                     std::move(payload), trajectory::Durability::ProcessCrash,
                     "tool.result.committed", error)) {
                return false;
            }
            it->second.result_committed = true;
            imported_results_ += 1;
        }
        return true;
    }

    // 富块 -> 无损投影(对齐 trajectory_session 的 RichBlockToProjection;
    // artifact 引用按新场内容寻址路径改写,字节随 CopyArtifact 搬)。
    nlohmann::json ProjectRichBlock(const nlohmann::json& rich) {
        const std::string type = rich.value("type", std::string());
        if (type == "text") {
            return nlohmann::json{{"type", "text"},
                                  {"text", platform::SanitizeExternalText(
                                               rich.value("text", std::string()))}};
        }
        if (type == "image" || type == "audio") {
            const bool image = type == "image";
            const nlohmann::json artifact = rich.contains("artifact") && rich["artifact"].is_object()
                                                ? rich["artifact"]
                                                : nlohmann::json::object();
            const std::string sha = artifact.value("sha256", rich.value("sha256", std::string()));
            const bool stored_before = artifact.value("stored", false);
            const std::string moved =
                stored_before ? artifacts_.CopyArtifact(artifact.value("path", std::string()), sha)
                              : std::string();
            nlohmann::json projected{{"type", image ? "image_ref" : "audio_ref"},
                                     {"mime_type", rich.value("mime_type", std::string())},
                                     {"bytes", rich.value("bytes", std::uint64_t{0})},
                                     {"sha256", sha},
                                     {"stored", !moved.empty()}};
            if (image && rich.contains("width") && rich["width"].is_number_unsigned()) {
                projected["width"] = rich["width"];
            }
            if (image && rich.contains("height") && rich["height"].is_number_unsigned()) {
                projected["height"] = rich["height"];
            }
            if (!moved.empty()) {
                projected["artifact_id"] = artifact.value("id", std::string());
                projected["path"] = moved;
            } else if (stored_before) {
                // 旧账说存过、现在搬不动:如实列缺口,不冒充可取。
                legacy_partial_ = true;
                missing_.push_back(std::string(image ? "图片" : "音频") + " artifact 字节缺失: " +
                                   artifact.value("path", std::string()));
            }
            return projected;
        }
        if (type == "resource_link") {
            nlohmann::json projected{
                {"type", "resource_link"},
                {"uri", platform::SanitizeExternalText(rich.value("uri", std::string()))}};
            if (rich.contains("name") && rich["name"].is_string()) {
                projected["name"] = rich["name"];
            }
            if (rich.contains("mime_type") && rich["mime_type"].is_string()) {
                projected["mime_type"] = rich["mime_type"];
            }
            if (rich.contains("size") && rich["size"].is_number_integer() &&
                rich["size"].get<std::int64_t>() >= 0) {
                projected["size"] = rich["size"];
            }
            return projected;
        }
        if (type == "resource_text") {
            return nlohmann::json{
                {"type", "embedded_text"},
                {"uri", platform::SanitizeExternalText(rich.value("uri", std::string()))},
                {"text", platform::SanitizeExternalText(rich.value("text", std::string()))},
                {"truncated", rich.value("truncated", false)}};
        }
        if (type == "resource_blob") {
            return nlohmann::json{{"type", "blob_ref"},
                                  {"uri", platform::SanitizeExternalText(
                                              rich.value("uri", std::string()))},
                                  {"mime_type", rich.value("mime_type", std::string())},
                                  {"bytes", rich.value("bytes", std::uint64_t{0})},
                                  {"sha256", rich.value("sha256", std::string())},
                                  {"stored", false}};
        }
        return nlohmann::json{{"type", "unknown"},
                              {"original_type", type},
                              {"summary", std::string("旧档富块无迁移投影")}};
    }

    // ---- 工具调用链(planned → effective → [started] → terminal) ----
    struct CallBook {
        std::string request_id;
        std::string tool_name = "unknown_legacy";
        nlohmann::json arguments = nlohmann::json::object();
        bool terminal = false;
        std::string terminal_event_id;
        bool result_committed = false;
    };

    bool EnsureCallChain(CallBook& book, const std::string& call_id, std::string* error) {
        if (book.terminal) {
            return true;
        }
        const std::optional<agent::ToolOutcome> trace_outcome = TraceOutcomeOf(call_id);
        const bool never_started =
            trace_outcome.has_value() && agent::OutcomeNeverStarted(*trace_outcome);
        trajectory::EventScope scope = recorder_->base_scope();
        scope.turn_id = turn_id_;
        scope.request_id = book.request_id;
        scope.call_id = call_id;
        // planned(模型声明)。
        {
            trajectory::EventScope planned_scope = scope;
            planned_scope.actor = trajectory::Actor::Model;
            planned_scope.origin = trajectory::Origin::ProviderModel;
            if (!Put(trajectory::EventKind::ToolExecutionPlanned, std::move(planned_scope), {},
                     nlohmann::json{{"call_id", call_id}, {"tool_name", book.tool_name}},
                     trajectory::Durability::ProcessCrash, "tool.execution.planned", error)) {
                return false;
            }
        }
        // effective:trace 账有就用真值,没账按 legacy 兜底声明并记缺口。
        std::string source_kind = "builtin";
        std::string effect_class = agent::ToString(agent::EffectClass::InProcessUnknown);
        if (const auto records = trace_ledger_.FindByToolUse(call_id); !records.empty()) {
            const agent::ToolExecutionRecord* record = records.front();
            source_kind = agent::ToString(record->source_kind);
            effect_class = agent::ToString(record->effect_class);
        } else {
            legacy_partial_ = true;
            missing_.push_back("工具 " + call_id + " 无 trace 账,source/effect 按保守值导入");
        }
        nlohmann::json arguments = book.arguments;
        tools::SanitizeJsonTextInPlace(arguments);
        {
            trajectory::EventScope effective_scope = scope;
            effective_scope.actor = trajectory::Actor::Tool;
            effective_scope.origin = trajectory::Origin::BuiltinTool;
            if (!Put(trajectory::EventKind::ToolInputEffective, std::move(effective_scope), {},
                     nlohmann::json{{"call_id", call_id},
                                    {"tool_name", book.tool_name},
                                    {"source_kind", source_kind},
                                    {"effect_class", effect_class},
                                    {"effective_arguments", arguments},
                                    {"effective_arguments_sha256", Sha256Text(arguments.dump())},
                                    {"rewritten_by", nlohmann::json::array()}},
                     trajectory::Durability::ProcessCrash, "tool.input.effective", error)) {
                return false;
            }
        }
        if (never_started) {
            // 闸前被收掉:cancelled 是唯一不须 started 的终态(§6.2 约束 16)。
            trajectory::EventScope terminal_scope = scope;
            terminal_scope.actor = trajectory::Actor::Tool;
            terminal_scope.origin = trajectory::Origin::BuiltinTool;
            if (!Put(trajectory::EventKind::ToolExecutionCancelled, std::move(terminal_scope), {},
                     nlohmann::json{{"reason", agent::ToString(*trace_outcome)}},
                     trajectory::Durability::PowerLoss, "tool.execution.cancelled", error)) {
                return false;
            }
            book.terminal = true;
            book.terminal_event_id = last_event_id_;
            return true;
        }
        // started(副作用边界)。
        {
            trajectory::EventScope started_scope = scope;
            started_scope.actor = trajectory::Actor::Tool;
            started_scope.origin = trajectory::Origin::BuiltinTool;
            if (!Put(trajectory::EventKind::ToolExecutionStarted, std::move(started_scope), {},
                     nlohmann::json{{"call_id", call_id}, {"attempt", std::uint64_t{1}}},
                     trajectory::Durability::PowerLoss, "tool.execution.started", error)) {
                return false;
            }
        }
        // terminal:trace 四档结论优先;没账按"结果已交付"记 finished。
        trajectory::EventKind terminal_kind = trajectory::EventKind::ToolExecutionFinished;
        nlohmann::json payload{{"outcome", "succeeded"}, {"duration_ms", std::int64_t{0}}};
        if (trace_outcome.has_value() && *trace_outcome == agent::ToolOutcome::UnknownAfterStart) {
            terminal_kind = trajectory::EventKind::ToolExecutionUnknown;
            payload = nlohmann::json{{"reason", "unknown_after_start"},
                                     {"duration_ms", std::int64_t{0}}};
            legacy_partial_ = true;
        } else if (!trace_outcome.has_value()) {
            legacy_partial_ = true;
            missing_.push_back("工具 " + call_id + " 执行细账(duration/exit code)不可还原");
        }
        {
            trajectory::EventScope terminal_scope = scope;
            terminal_scope.actor = trajectory::Actor::Tool;
            terminal_scope.origin = trajectory::Origin::BuiltinTool;
            if (!Put(terminal_kind, std::move(terminal_scope), {}, std::move(payload),
                     trajectory::Durability::PowerLoss, "tool.terminal", error)) {
                return false;
            }
        }
        book.terminal = true;
        book.terminal_event_id = last_event_id_;
        return true;
    }

    // 回合收口:悬空 call(声明了没等到结果)按迁移语义补账——trace 判
    // unknown 的终态 + 明示"[迁移导入]"的错误结果(与旧 RepairToolPairs
    // 的"结果缺失"补洞同语义,不冒充真结果)。
    void CloseOpenTurn() {
        if (!turn_open_) {
            return;
        }
        for (auto& [call_id, book] : calls_) {
            if (book.terminal) {
                continue;
            }
            std::string error;
            if (!EnsureCallChain(book, call_id, &error)) {
                missing_.push_back("悬空工具 " + call_id + " 终态补写失败: " + error);
                continue;
            }
            if (!WriteSynthesizedResult(book, call_id, &error)) {
                missing_.push_back("悬空工具 " + call_id + " 补结果失败: " + error);
                continue;
            }
            legacy_partial_ = true;
            missing_.push_back("tool_use 无结果送达: " + call_id + "(按 [迁移导入:结果缺失] 补洞)");
        }
        calls_.clear();
        std::string error;
        trajectory::EventScope scope = recorder_->base_scope();
        scope.turn_id = turn_id_;
        if (!Put(trajectory::EventKind::TurnCompleted, std::move(scope), {},
                 nlohmann::json{{"outcome", "succeeded"}}, trajectory::Durability::ProcessCrash,
                 "turn.completed", &error)) {
            missing_.push_back("turn 收口失败: " + error);
        }
        turn_open_ = false;
    }

    bool WriteSynthesizedResult(CallBook& book, const std::string& call_id, std::string* error) {
        trajectory::EventScope scope = recorder_->base_scope();
        scope.turn_id = turn_id_;
        scope.request_id = book.request_id;
        scope.call_id = call_id;
        scope.actor = trajectory::Actor::Tool;
        scope.origin = trajectory::Origin::BuiltinTool;
        nlohmann::json payload{
            {"call_id", call_id},
            {"content", nlohmann::json::array(
                            {nlohmann::json{{"type", "text"}, {"text", "[迁移导入:结果缺失]"}}})},
            {"is_error", true}};
        if (!book.terminal_event_id.empty()) {
            payload["derived_from_event"] = book.terminal_event_id;
        }
        if (!Put(trajectory::EventKind::ToolResultCommitted, std::move(scope), {}, std::move(payload),
                 trajectory::Durability::ProcessCrash, "tool.result.committed(synthesized)", error)) {
            return false;
        }
        book.result_committed = true;
        return true;
    }

    bool OpenTurn(const char* trigger, std::string* error) {
        turn_id_ = NextId("turn");
        turn_open_ = true;
        last_input_event_id_.clear();
        trajectory::EventScope scope = recorder_->base_scope();
        scope.turn_id = turn_id_;
        scope.actor = trajectory::Actor::User;
        scope.origin = trajectory::Origin::ExternalUser;
        if (!Put(trajectory::EventKind::TurnStarted, std::move(scope), {},
                 nlohmann::json{{"trigger", trigger}}, trajectory::Durability::ProcessCrash,
                 "turn.started", error)) {
            return false;
        }
        return true;
    }

    bool ImportCompact(const nlohmann::json& json, std::string* error) {
        // v1/v2 同型:archive + kept_from(v2 另带 epoch/manifest/metrics)。
        if (!json.contains("archive") || !json["archive"].is_object() ||
            !json.contains("kept_from") || !json["kept_from"].is_number_unsigned()) {
            skipped_lines_ += 1;
            return true;
        }
        const std::size_t kept_from = json["kept_from"].get<std::size_t>();
        const std::uint64_t epoch =
            json.contains("epoch") && json["epoch"].is_number_integer()
                ? json["epoch"].get<std::uint64_t>()
                : static_cast<std::uint64_t>(compact_count_ + 1);
        compact_count_ += 1;
        nlohmann::json payload{{"old_state_hash", Sha256Text(effective_history_key_)},
                               {"new_state_hash", HashCompactArchive(json["archive"], kept_from)},
                               {"source_event_span",
                                nlohmann::json::array({std::uint64_t{1}, outcome_events_})},
                               {"epoch", epoch}};
        if (json.contains("metrics") && json["metrics"].is_object()) {
            if (json["metrics"].contains("pre_tokens") &&
                json["metrics"]["pre_tokens"].is_number_unsigned()) {
                payload["pre_tokens"] = json["metrics"]["pre_tokens"];
            }
            if (json["metrics"].contains("post_tokens") &&
                json["metrics"]["post_tokens"].is_number_unsigned()) {
                payload["post_tokens"] = json["metrics"]["post_tokens"];
            }
        }
        trajectory::EventScope scope = recorder_->base_scope();
        if (!Put(trajectory::EventKind::CompactApplied, std::move(scope), {}, std::move(payload),
                 trajectory::Durability::ProcessCrash, "compact.applied", error)) {
            return false;
        }
        // 新账规矩:compact 只记边界,不重写 Journal(与生产 RecordCompactApplied
        // 同语义);旧档 kept_from 的裁剪回放语义如实列缺口。
        legacy_partial_ = true;
        missing_.push_back("compact 已记 compact.applied;旧档 kept_from 裁剪语义在新账 replay "
                           "中不重放(新格式 compact 不重写 Journal)");
        return true;
    }

    std::string HashCompactArchive(const nlohmann::json& archive, std::size_t kept_from) {
        std::string key = effective_history_key_;
        key += "|compact:" + std::to_string(kept_from) + "|";
        key += archive.dump();
        return Sha256Text(key);
    }

    bool FinishRunAndSession(SessionImportOutcome* outcome, std::string* error) {
        if (goal_events_ > 0) {
            missing_.push_back("goal_v1 事件 " + std::to_string(goal_events_) +
                               " 条无对应新格式事件种类,未迁");
        }
        if (plan_events_ > 0) {
            missing_.push_back("plan_v1/plan_review_v1 事件 " + std::to_string(plan_events_) +
                               " 条无对应新格式事件种类,未迁");
        }
        if (think_events_ > 0) {
            missing_.push_back("think_history_v1 事件 " + std::to_string(think_events_) +
                               " 条无对应新格式事件种类,未迁");
        }
        if (skipped_lines_ > 0) {
            missing_.push_back("解析不动/认不得的行 " + std::to_string(skipped_lines_) + " 条,跳过");
            legacy_partial_ = true;
        }
        missing_.push_back("usage 账不可迁(旧档消息行不带 token 计数)");
        missing_.push_back("子代理细账不可得(旧主账只有最终回话)");
        legacy_partial_ = true;

        const std::string terminal_reason = "legacy_import";
        {
            trajectory::EventScope scope = recorder_->base_scope();
            nlohmann::json payload = trajectory::MakeTerminalSealPayload(
                first_event_hash_, outcome_events_, trajectory::kEnvelopeSchemaVersion,
                "storage-migrator-v1");
            payload["reason"] = terminal_reason;
            if (!Put(trajectory::EventKind::RunCompleted, std::move(scope), {}, std::move(payload),
                     trajectory::Durability::PowerLoss, "run.completed", error)) {
                return false;
            }
        }
        {
            nlohmann::json payload = trajectory::MakeTerminalSealPayload(
                first_event_hash_, outcome_events_, trajectory::kEnvelopeSchemaVersion,
                "storage-migrator-v1");
            payload["reason"] = terminal_reason;
            payload["close_quality"] = "legacy_import";
            trajectory::EventScope scope = recorder_->base_scope();
            if (!Put(trajectory::EventKind::SessionEnded, std::move(scope), {}, std::move(payload),
                     trajectory::Durability::PowerLoss, "session.ended", error)) {
                return false;
            }
        }
        outcome->terminal_event_hash = last_event_hash_;
        outcome->event_count = outcome_events_;
        const auto closed = recorder_->Close();
        if (!closed.has_value()) {
            *error = std::string(contracts::kErrMigrationTargetWriteFailed) + ": recorder.close: " +
                     closed.error();
            return false;
        }
        if (fault_ != nullptr && fault_("session_closed")) {
            throw MigrationInterrupted("session_closed");
        }
        return true;
    }

    std::string NextId(const char* prefix) {
        return std::string(prefix) + "-" + std::to_string(++id_counter_);
    }

    trajectory::TrajectoryRecorder* recorder_;
    MigrationClock* clock_;
    const LegacySessionDoc* doc_;
    ArtifactSink artifacts_;
    std::function<bool(const std::string&)> fault_;

    std::string turn_id_;
    bool turn_open_ = false;
    std::string last_input_event_id_;
    std::string last_event_id_;
    std::string last_event_hash_;
    std::string first_event_hash_;
    std::map<std::string, CallBook> calls_;
    std::set<std::string> seen_calls_;
    agent::ToolExecutionLedger trace_ledger_;
    std::vector<std::string> missing_;
    bool legacy_partial_ = false;
    std::uint64_t id_counter_ = 0;
    std::uint64_t outcome_events_ = 0;
    std::uint64_t imported_messages_ = 0;
    std::uint64_t imported_results_ = 0;
    std::uint64_t compact_count_ = 0;
    std::size_t goal_events_ = 0;
    std::size_t plan_events_ = 0;
    std::size_t think_events_ = 0;
    std::size_t skipped_lines_ = 0;
    std::string last_title_;
    std::int64_t wall_ = 0;
    std::string effective_history_key_;
};

}  // namespace

// ---------------------------------------------------------------------------
// 旧 project_key 复算(§7.3;算法照 P0-1 收编前的原样封存于此)
// ---------------------------------------------------------------------------

namespace {

std::string LowerAsciiLegacy(std::string value) {
    for (char& c : value) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x80) {
            c = static_cast<char>(std::tolower(byte));
        }
    }
    return value;
}

std::string LegacySafeName(std::string_view value) {
    constexpr std::size_t kMaxBytes = 48;
    std::string out;
    out.reserve(value.size() < kMaxBytes ? value.size() : kMaxBytes);
    bool dash = false;
    for (const unsigned char byte : value) {
        if (out.size() >= kMaxBytes) {
            break;
        }
        if (byte >= 0x80 || std::isalnum(byte) != 0 || byte == '_' || byte == '-') {
            out.push_back(static_cast<char>(byte));
            dash = false;
        } else if (!dash && !out.empty()) {
            out.push_back('-');
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? "project" : out;
}

std::string LegacyHexHash(std::string_view value) {
    // FNV-1a 64(与旧 memory 侧 StableHash 同参数)。
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

fs::path LegacyAbsoluteNormal(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    fs::path canonical = fs::weakly_canonical(absolute, ec);
    return (ec ? absolute : canonical).lexically_normal();
}

std::string TrimCopy(const std::string& value) {
    const auto not_space = [](char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    const auto begin = std::find_if(value.begin(), value.end(), not_space);
    const auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    return begin < end ? std::string(begin, end) : std::string();
}

// generic(正斜杠)UTF-8 文本:旧 key 算法的 identity_path 用(PathToUtf8
// 是 native 分隔符,不能混)。
std::string GenericUtf8(const fs::path& path) {
    const std::u8string u8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

fs::path LegacyResolveGitCommonDir(const fs::path& directory) {
    const fs::path dot_git = directory / ".git";
    std::error_code ec;
    if (fs::is_directory(dot_git, ec) && !ec) {
        return LegacyAbsoluteNormal(dot_git);
    }
    if (!fs::is_regular_file(dot_git, ec) || ec) {
        return {};
    }
    const auto marker = ReadFileBytes(dot_git);
    if (!marker.has_value() || !marker->starts_with("gitdir:")) {
        return {};
    }
    fs::path git_dir = Utf8ToPath(TrimCopy(marker->substr(7)));
    if (git_dir.is_relative()) {
        git_dir = directory / git_dir;
    }
    git_dir = LegacyAbsoluteNormal(git_dir);
    const auto common_text = ReadFileBytes(git_dir / "commondir");
    if (!common_text.has_value()) {
        return git_dir;
    }
    fs::path common_dir = Utf8ToPath(TrimCopy(*common_text));
    if (common_dir.is_relative()) {
        common_dir = git_dir / common_dir;
    }
    return LegacyAbsoluteNormal(common_dir);
}

}  // namespace

std::string ComputeLegacyProjectKey(const fs::path& project_root) {
    fs::path current = LegacyAbsoluteNormal(project_root);
    std::error_code ec;
    if (!fs::is_directory(current, ec) || ec) {
        current = current.parent_path();
    }
    fs::path common_root;
    fs::path local_config_root;
    bool git = false;
    for (;; current = current.parent_path()) {
        if (const auto common = LegacyResolveGitCommonDir(current); !common.empty()) {
            common_root = common;
            git = true;
            break;
        }
        if (local_config_root.empty()) {
            std::error_code config_ec;
            if (fs::is_regular_file(current / ".lubancode" / "config.json", config_ec) && !config_ec) {
                local_config_root = current;
            }
        }
        const fs::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
    }
    if (common_root.empty()) {
        common_root = local_config_root.empty() ? LegacyAbsoluteNormal(project_root) : local_config_root;
    }
    const fs::path identity_root = common_root;
    std::string display_name =
        PathToUtf8((git ? identity_root.parent_path() : identity_root).filename());
    if (display_name.empty()) {
        display_name = "project";
    }
    std::string identity_path = GenericUtf8(identity_root);
#ifdef _WIN32
    identity_path = LowerAsciiLegacy(std::move(identity_path));
#endif
    const std::string seed = (git ? "git:" : "path:") + identity_path;
    return LegacySafeName(display_name) + "-" + LegacyHexHash(seed);
}

// ---------------------------------------------------------------------------
// 4. 旧 Memory 迁移
// ---------------------------------------------------------------------------

namespace {

struct LegacyTopic {
    memory::MemoryEntry entry;
    nlohmann::json fingerprints = nlohmann::json::object();
    std::string body;
    std::string error;
    bool parsed = false;
};

// 双格式读:schema 3 走生产 frontmatter::Parse;schema 1/2 的 HTML 注释
// 严格 JSON 由这里最小复刻(字段口径对齐生产 ParseStoredEntry,只服务
// 旧根输入)。
LegacyTopic ParseLegacyTopic(const std::string& text) {
    LegacyTopic topic;
    if (text.starts_with("---\n") || text.starts_with("---\r\n")) {
        const auto parsed = memory::frontmatter::Parse(text);
        if (!parsed.has_value()) {
            topic.error = parsed.error();
            return topic;
        }
        topic.entry = std::move(parsed->entry);
        topic.fingerprints = std::move(parsed->fingerprints);
        topic.body = std::move(parsed->body);
        topic.parsed = true;
        return topic;
    }
    constexpr std::string_view kMetaOpen = memory::frontmatter::kLegacyMetaOpen;
    constexpr std::string_view kMetaClose = memory::frontmatter::kLegacyMetaClose;
    if (!text.starts_with(kMetaOpen)) {
        topic.error = "缺 lubancode-memory 元数据或 front matter";
        return topic;
    }
    const std::size_t end = text.find(kMetaClose, kMetaOpen.size());
    if (end == std::string::npos) {
        topic.error = "记忆元数据没有闭合";
        return topic;
    }
    const nlohmann::json meta =
        nlohmann::json::parse(text.substr(kMetaOpen.size(), end - kMetaOpen.size()), nullptr,
                              /*allow_exceptions=*/false);
    if (meta.is_discarded() || !meta.is_object()) {
        topic.error = "记忆元数据不是合法 JSON";
        return topic;
    }
    memory::MemoryEntry& entry = topic.entry;
    const int schema = meta.value("schema", 0);
    if (schema != 1 && schema != 2) {
        topic.error = "记忆元数据 schema 不受支持";
        return topic;
    }
    entry.schema = schema;
    entry.id = meta.value("id", std::string());
    entry.title = meta.value("title", std::string());
    entry.summary = meta.value("summary", std::string());
    entry.status = meta.value("status", std::string("active"));
    entry.updated_at = meta.value("updated_at", std::string());
    entry.created_at = meta.value("created_at", std::string());
    if (entry.created_at.empty()) {
        entry.created_at = entry.updated_at;
    }
    auto kind = memory::ParseMemoryKind(meta.value("kind", std::string()));
    if (!kind.has_value()) {
        topic.error = kind.error();
        return topic;
    }
    entry.kind = *kind;
    if (meta.contains("keywords") && meta["keywords"].is_array()) {
        for (const auto& item : meta["keywords"]) {
            if (item.is_string()) entry.keywords.push_back(item.get<std::string>());
        }
    }
    if (meta.contains("paths") && meta["paths"].is_array()) {
        for (const auto& item : meta["paths"]) {
            if (item.is_string()) entry.paths.push_back(item.get<std::string>());
        }
    }
    if (meta.contains("source_sessions") && meta["source_sessions"].is_array()) {
        for (const auto& item : meta["source_sessions"]) {
            if (item.is_string()) entry.source_sessions.push_back(item.get<std::string>());
        }
    }
    if (meta.contains("scope") && meta["scope"].is_object()) {
        entry.scope.level = meta["scope"].value("level", std::string("project"));
        entry.scope.kind = meta["scope"].value("kind", std::string("project"));
        entry.scope.value = meta["scope"].value("value", std::string());
    } else {
        entry.scope.level = "project";
        entry.scope.kind = "project";
    }
    if (meta.contains("evidence") && meta["evidence"].is_array()) {
        for (const auto& item : meta["evidence"]) {
            if (!item.is_object()) continue;
            memory::MemoryEvidence proof;
            proof.path = item.value("path", std::string());
            proof.symbol = item.value("symbol", std::string());
            if (!proof.path.empty()) entry.evidence.push_back(std::move(proof));
        }
    }
    entry.confidence = meta.value("confidence", std::string());
    if (entry.confidence.empty()) {
        entry.confidence = entry.kind == memory::MemoryKind::Fact ? "verified" : "user-stated";
    }
    entry.last_verified_at = meta.value("last_verified_at", std::string());
    if (meta.contains("expires_at") && meta["expires_at"].is_string()) {
        entry.expires_at = meta["expires_at"].get<std::string>();
    }
    if (meta.contains("fingerprints") && meta["fingerprints"].is_object()) {
        topic.fingerprints = meta["fingerprints"];
    }
    const std::string kind_name = memory::MemoryKindName(entry.kind);
    entry.name = entry.id.starts_with(kind_name + ".") && entry.id.size() > kind_name.size() + 1
                     ? entry.id.substr(kind_name.size() + 1)
                     : entry.id;
    topic.body = memory::frontmatter::StripTitleHeading(memory::frontmatter::StripTopicMetadata(text));
    topic.parsed = true;
    return topic;
}

bool LegacyIsValidId(const std::string& id) {
    if (id.size() < 8 || id.size() > 128) {
        return false;
    }
    if (!id.starts_with("fact.") && !id.starts_with("preference.") && !id.starts_with("feedback.")) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        c == '.' || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// 旧裸 session id -> 全限定引用(合同 §六)。迁移场 session id 沿用旧 id,
// 故 session_id 段即旧值;run/event 无账可指,按 P0-3 兜底记 none。
std::string QualifyLegacySessionRef(const std::string& workspace_key, const std::string& session_id) {
    return "workspace_key=" + workspace_key + "/session_id=" + session_id +
           "/run_id=none/event_id=none";
}

}  // namespace

// ---------------------------------------------------------------------------
// 5. 回执读写
// ---------------------------------------------------------------------------

namespace receipts {

fs::path OperationsRoot(const fs::path& home_lubancode) {
    return home_lubancode / "migrations" / "storage-v2";
}

std::optional<nlohmann::json> ReadIntent(const fs::path& operation_dir) {
    return ParseJsonFile(operation_dir / "intent.json");
}
std::optional<nlohmann::json> ReadProgress(const fs::path& operation_dir) {
    return ParseJsonFile(operation_dir / "progress.json");
}
std::optional<nlohmann::json> ReadResult(const fs::path& operation_dir) {
    return ParseJsonFile(operation_dir / "result.json");
}

}  // namespace receipts

// ---------------------------------------------------------------------------
// 6. plan / run / status
// ---------------------------------------------------------------------------

namespace {

struct MigratorContext {
    MigratorOptions options;
    fs::path home;
    fs::path workspaces_root;
    fs::path migrations_root;
    std::int64_t now_ms = 0;
    std::map<std::string, workspace::WorkspaceIdentity> identity_cache;

    // 已 committed 的 source SHA -> (operation_id, target_session_id, workspace_key)。
    std::map<std::string, std::tuple<std::string, std::string, std::string>> committed_sources;

    void LoadCommittedSources() {
        committed_sources.clear();
        std::error_code ec;
        if (!fs::is_directory(migrations_root, ec) || ec) {
            return;
        }
        for (const auto& dir : ListOperationDirs()) {
            const auto result = receipts::ReadResult(dir);
            if (!result.has_value() || !result->is_object() ||
                !result->contains("items") || !(*result)["items"].is_array()) {
                continue;
            }
            const std::string operation_id = result->value("operation_id", std::string());
            for (const auto& item : (*result)["items"]) {
                if (!item.is_object()) {
                    continue;
                }
                const std::string outcome = item.value("outcome", std::string());
                if (outcome != "imported" && outcome != "already_imported") {
                    continue;
                }
                const std::string sha = item.value("source_sha256", std::string());
                if (sha.empty()) {
                    continue;
                }
                committed_sources.emplace(
                    sha, std::make_tuple(operation_id, item.value("target_session_id", std::string()),
                                         item.value("target_workspace_key", std::string())));
            }
        }
    }

    // 回执目录按名字倒序(新→旧)。
    std::vector<fs::path> ListOperationDirs() const {
        std::vector<fs::path> dirs;
        std::error_code ec;
        if (!fs::is_directory(migrations_root, ec) || ec) {
            return dirs;
        }
        for (const auto& entry : fs::directory_iterator(migrations_root, ec)) {
            if (ec) break;
            std::error_code dir_ec;
            if (entry.is_directory(dir_ec) && !dir_ec) {
                dirs.push_back(entry.path());
            }
        }
        std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
            return PathToUtf8(a.filename()) > PathToUtf8(b.filename());
        });
        return dirs;
    }

    bool HasCommitted(const std::string& sha) const { return committed_sources.count(sha) != 0; }

    // meta.cwd -> 身份(逐 cwd 缓存;ResolveWorkspaceIdentity 是唯一裁决)。
    const workspace::WorkspaceIdentity* ResolveCached(const std::string& cwd_text) {
        const std::string key = cwd_text.empty() ? std::string("(empty)") : cwd_text;
        const auto cached = identity_cache.find(key);
        if (cached != identity_cache.end()) {
            return &cached->second;
        }
        const auto resolved = workspace::ResolveWorkspaceIdentity(Utf8ToPath(cwd_text), home);
        if (!resolved.has_value()) {
            return nullptr;
        }
        return &identity_cache.emplace(key, std::move(*resolved)).first->second;
    }

    void Fault(const std::string& point) const {
        if (options.fault != nullptr && options.fault(point)) {
            throw MigrationInterrupted(point);
        }
    }

    void Note(const std::string& text) const {
        if (options.progress_note != nullptr) {
            options.progress_note(text);
        }
    }
};

MigratorContext MakeContext(const MigratorOptions& options, std::string* error) {
    MigratorContext context;
    context.options = options;
    context.home = LegacyAbsoluteNormal(options.home_lubancode);
    if (context.home.empty()) {
        *error = std::string(contracts::kErrWorkspaceNotFound) + ": home_lubancode 未给";
        return context;
    }
    context.workspaces_root = options.workspaces_root.empty()
                                  ? context.home / "workspaces"
                                  : LegacyAbsoluteNormal(options.workspaces_root);
    context.migrations_root = receipts::OperationsRoot(context.home);
    context.now_ms = options.now_ms > 0 ? options.now_ms : NowWallMs();
    return context;
}

// 旧会话源扫描:sessions/*.jsonl + sessions/archive/*.jsonl。
std::vector<fs::path> ScanLegacySessionFiles(const fs::path& home) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const fs::path& folder : {home / "sessions", home / "sessions" / "archive"}) {
        if (!fs::is_directory(folder, ec) || ec) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(folder, ec)) {
            if (ec) break;
            std::error_code file_ec;
            if (entry.is_regular_file(file_ec) && !file_ec &&
                entry.path().extension() == ".jsonl") {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

struct OldProjectDir {
    std::string key;
    fs::path dir;     // projects/<key>
    fs::path memory;  // projects/<key>/memory
    bool has_memory = false;
};

std::vector<OldProjectDir> ScanLegacyProjectDirs(const fs::path& home) {
    std::vector<OldProjectDir> projects;
    const fs::path root = home / "projects";
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) {
        return projects;
    }
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        std::error_code dir_ec;
        if (!entry.is_directory(dir_ec) || dir_ec) {
            continue;
        }
        OldProjectDir project;
        project.key = PathToUtf8(entry.path().filename());
        project.dir = entry.path();
        project.memory = project.dir / "memory";
        std::error_code mem_ec;
        project.has_memory = fs::is_directory(project.memory, mem_ec) && !mem_ec;
        projects.push_back(std::move(project));
    }
    std::sort(projects.begin(), projects.end(),
              [](const OldProjectDir& a, const OldProjectDir& b) { return a.key < b.key; });
    return projects;
}

struct ProjectMapping {
    std::string workspace_key;
    std::string source;
};

// 旧 key -> 目标 workspace:project.json 的 project_root 优先,其次显式
// --project-root 映射。算不出给空串(unmappable)。
ProjectMapping ResolveProjectMapping(const MigratorContext& context, const OldProjectDir& project) {
    ProjectMapping mapping;
    std::string root_text;
    if (const auto json = ParseJsonFile(project.dir / "project.json");
        json.has_value() && json->is_object()) {
        root_text = json->value("project_root", std::string());
        if (!root_text.empty()) {
            mapping.source = "project.json";
        }
    }
    if (root_text.empty()) {
        const auto it = context.options.extra_project_roots.find(project.key);
        if (it != context.options.extra_project_roots.end()) {
            root_text = it->second;
            mapping.source = "project-root 选项";
        }
    }
    if (root_text.empty()) {
        return mapping;
    }
    const auto identity = workspace::ResolveWorkspaceIdentity(Utf8ToPath(root_text), context.home);
    if (!identity.has_value()) {
        return mapping;
    }
    mapping.workspace_key = identity->workspace_key;
    return mapping;
}

std::string RelativeToHome(const fs::path& home, const fs::path& path) {
    std::error_code ec;
    const fs::path relative = fs::relative(path, home, ec);
    if (ec || relative.empty()) {
        return PathToUtf8(path);
    }
    // 统一正斜杠:回执/账面文本跨平台同形,archive 段的判定也靠它。
    return relative.generic_string();
}

std::string NewOperationId(std::int64_t now_ms) {
    const std::time_t epoch = static_cast<std::time_t>(now_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &epoch);
#else
    gmtime_r(&epoch, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    static const char kAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string random;
    for (int i = 0; i < 6; ++i) {
        random.push_back(kAlphabet[std::rand() % (sizeof(kAlphabet) - 1)]);
    }
    return "mig-" + std::string(stamp) + "-" + random;
}

bool WriteProgress(const fs::path& operation_dir, const nlohmann::json& progress) {
    return AtomicWriteText(operation_dir / "progress.json", progress.dump(2) + "\n");
}

// ---------------------------------------------------------------------------
// 单文件导入(创建目标场 → 事件流 → 状态机 → 复验 → lifecycle)。
// ---------------------------------------------------------------------------

MigrationResultItem ImportOneSession(MigratorContext& context, const MigrationSourceFile& source,
                                     bool archived) {
    MigrationResultItem item;
    item.source_sha256 = source.sha256;
    item.source_path = source.path;
    item.subagent_detail = std::string(contracts::kSubagentDetailUnavailableLegacy);

    const fs::path source_path = context.home / Utf8ToPath(source.path);
    const auto bytes = ReadFileBytes(source_path);
    if (!bytes.has_value()) {
        item.outcome = "skipped_unreadable";
        item.error_code = std::string(contracts::kErrMigrationSourceUnreadable);
        return item;
    }
    if (Sha256Text(*bytes) != source.sha256) {
        item.outcome = "failed";
        item.error_code = std::string(contracts::kErrMigrationSourceShaMismatch);
        return item;
    }
    const auto doc = ParseLegacySessionDoc(*bytes);
    if (!doc.has_value()) {
        item.outcome = "skipped_unreadable";
        item.error_code = std::string(contracts::kErrMigrationSourceUnreadable);
        return item;
    }

    const workspace::WorkspaceIdentity* identity = context.ResolveCached(doc->cwd);
    if (identity == nullptr) {
        item.outcome = "failed";
        item.error_code = std::string(contracts::kErrIdentityNoBoundary);
        item.missing.push_back("meta.cwd 裁决不出 workspace: " + doc->cwd);
        return item;
    }
    item.target_workspace_key = identity->workspace_key;

    // 建/认 workspace(首仓原子写 manifest;只走唯一裁决的身份)。
    const auto directory = trajectory::TrajectoryDirectory::CreateWorkspace(
        context.workspaces_root, *identity, context.now_ms);
    if (!directory.has_value()) {
        item.outcome = "failed";
        item.error_code = std::string(contracts::kErrWorkspaceOpenFailed);
        item.missing.push_back(directory.error());
        return item;
    }

    // 旧 id 原样带入(§2D):文件名去 .jsonl。
    const std::string session_id = PathToUtf8(source_path.stem());
    item.target_session_id = session_id;  // 合同 §五:逐项记目标 session id
    const fs::path session_dir = directory->workspace_dir() / "sessions" / Utf8ToPath(session_id);
    std::error_code ec;
    if (fs::exists(session_dir, ec) || ec) {
        // 半截目标(上次中断且无 committed 回执):删了重建。
        std::error_code remove_ec;
        fs::remove_all(session_dir, remove_ec);
        std::error_code check_ec;
        if (remove_ec || fs::exists(session_dir, check_ec)) {
            item.outcome = "failed";
            item.error_code = std::string(contracts::kErrMigrationTargetWriteFailed);
            item.missing.push_back("半截目标目录删不掉: " + PathToUtf8(session_dir));
            return item;
        }
    }

    trajectory::SessionManifest manifest;
    manifest.schema_version = contracts::kSessionManifestSchemaVersion;
    manifest.workspace_key = identity->workspace_key;
    manifest.session_id = session_id;
    manifest.launch_cwd = doc->cwd;
    manifest.main_run_id = "run-mig-" + session_id;
    manifest.start_reason = std::string(contracts::kStartReasonLegacyImport);
    manifest.status = trajectory::SessionStatusName(trajectory::SessionStatus::Preparing);
    manifest.created_at_ms = context.now_ms;
    manifest.lubancode_version = context.options.lubancode_version;
    manifest.subagent_detail = std::string(contracts::kSubagentDetailUnavailableLegacy);
    manifest.training_policy = std::string(contracts::kLegacyTrainingPolicy);

    const auto created =
        trajectory::TrajectoryDirectory::CreateSession(context.workspaces_root,
                                                       identity->workspace_key, manifest);
    if (!created.has_value()) {
        item.outcome = "failed";
        item.error_code = std::string(contracts::kErrMigrationTargetWriteFailed);
        item.missing.push_back(created.error());
        return item;
    }
    context.Fault("session_created");

    trajectory::SessionLockOwner lock_owner;
    lock_owner.pid = platform::CurrentProcessId();
    lock_owner.process_start_token = trajectory::CurrentProcessStartToken();
    lock_owner.acquired_at_ms = context.now_ms;
    auto lock = trajectory::SessionLock::Acquire(created->session_dir(), lock_owner);
    if (!lock.has_value()) {
        item.outcome = "failed";
        item.error_code = std::string(contracts::kErrSessionLocked);
        item.missing.push_back(lock.error());
        return item;
    }

    MigrationClock clock;
    trajectory::EventScope scope;
    scope.workspace_key = identity->workspace_key;
    scope.session_id = session_id;
    scope.run_id = manifest.main_run_id;
    scope.run_kind = trajectory::RunKind::MainSession;
    scope.actor = trajectory::Actor::Host;
    scope.origin = trajectory::Origin::RecoveryRuntime;
    scope.training_policy = trajectory::TrainingPolicy::Exclude;
    // 信封 schema 要求 visibility 非空(§2.7);旧档导入场与生产主会话同取
    // HostOnly(迁移场不进模型可见面,训练策略另由 exclude 钉死)。
    scope.visibility = {trajectory::Visibility::HostOnly};
    auto recorder = trajectory::TrajectoryRecorder::Start(
        created->main_stream_path(), created->artifacts_root(), std::move(scope),
        trajectory::RecorderOptions{}, &clock);
    if (!recorder.has_value()) {
        item.outcome = "failed";
        item.error_code = std::string(contracts::kErrMigrationTargetWriteFailed);
        item.missing.push_back(recorder.error());
        return item;
    }

    ArtifactSink artifacts;
    artifacts.old_session_dir = source_path.parent_path() / Utf8ToPath(session_id);
    artifacts.new_session_dir = created->session_dir();
    artifacts.missing = &item.missing;

    SessionImporter importer(&*recorder, &clock, &*doc, artifacts, context.options.fault);
    SessionImportOutcome import_outcome;
    std::string error;
    if (!importer.Run(&import_outcome, &error)) {
        item.outcome = "failed";
        item.error_code = error;
        return item;
    }

    // 状态机:preparing -> running -> closing -> closed(archived 源加一跳)。
    if (auto current = trajectory::ReadSessionJson(created->session_dir());
        current.has_value()) {
        const std::vector<trajectory::SessionStatus> steps = {
            trajectory::SessionStatus::Running, trajectory::SessionStatus::Closing,
            trajectory::SessionStatus::Closed};
        for (const trajectory::SessionStatus step : steps) {
            const auto transition =
                trajectory::TransitionSessionStatus(created->session_dir(), &*current, step);
            if (!transition.has_value()) {
                item.outcome = "failed";
                item.error_code = std::string(contracts::kErrMigrationTargetWriteFailed);
                item.missing.push_back("session.json 状态迁移失败: " + transition.error());
                return item;
            }
        }
        if (archived) {
            const auto archived_transition = trajectory::TransitionSessionStatus(
                created->session_dir(), &*current, trajectory::SessionStatus::Archived);
            if (!archived_transition.has_value()) {
                item.missing.push_back("归档态写入失败(以 closed 收场): " +
                                       archived_transition.error());
                item.legacy_partial = true;
            }
        }
    }

    // 复验:verify + exact replay 都过才算 imported(§7.1 第 6 条)。
    const auto verify = trajectory::VerifySessionDir(created->session_dir());
    const auto replay = trajectory::FoldStreamReplay(created->main_stream_path());
    if (!verify.ok || !replay.ok()) {
        item.outcome = "failed";
        item.error_code = std::string(contracts::kErrMigrationTargetWriteFailed);
        item.missing.push_back("目标场复验不过: verify=" + verify.error_code +
                               " replay=" + replay.error_code);
        return item;
    }

    // workspace lifecycle:create_session 一笔(P0-2 交接的写法)。
    trajectory::WorkspaceLifecycle lifecycle(directory->workspace_dir());
    trajectory::LifecycleIntent intent;
    intent.operation_id = "lc-" + session_id + "-mig";
    intent.operation = trajectory::LifecycleOperationName(
        trajectory::LifecycleOperation::CreateSession);
    intent.workspace_key = identity->workspace_key;
    intent.session_id = session_id;
    intent.requested_at_ms = context.now_ms;
    intent.parameters["start_reason"] = manifest.start_reason;
    intent.parameters["source_sha256"] = source.sha256;
    const auto intent_dir = lifecycle.WriteIntent(intent);
    if (intent_dir.has_value()) {
        trajectory::LifecycleResult result;
        result.operation_id = intent.operation_id;
        result.status = "completed";
        result.completed_at_ms = context.now_ms;
        result.outcome["legacy_import"] = true;
        (void)lifecycle.WriteResult(result);
    }

    item.outcome = "imported";
    item.terminal_event_hash = import_outcome.terminal_event_hash;
    item.legacy_partial = import_outcome.legacy_partial;
    for (const std::string& gap : import_outcome.missing) {
        item.missing.push_back(gap);
    }
    return item;
}

}  // namespace

std::expected<MigrationPlanReport, std::string> PlanStorageMigration(const MigratorOptions& options) {
    std::string error;
    MigratorContext context = MakeContext(options, &error);
    if (!error.empty()) {
        return std::unexpected(error);
    }
    context.LoadCommittedSources();

    MigrationPlanReport report;
    report.operation_id = NewOperationId(context.now_ms);
    const fs::path operation_dir = context.migrations_root / Utf8ToPath(report.operation_id);
    {
        std::error_code ec;
        if (fs::exists(operation_dir / "intent.json", ec) || ec) {
            return std::unexpected(std::string(contracts::kErrMigrationIntentExists) +
                                   ": operation_id 已有 intent: " + report.operation_id);
        }
    }

    nlohmann::json intent;
    intent["schema"] = std::string(contracts::kMigrationSchemaName);
    intent["version"] = contracts::kMigrationSchemaVersion;
    intent["operation_id"] = report.operation_id;
    intent["created_at_ms"] = context.now_ms;
    intent["source"] = nlohmann::json{
        {"kind", "sessions"},
        {"roots", nlohmann::json::array({PathToUtf8(context.home / "sessions")})},
        {"files", nlohmann::json::array()}};
    intent["planned"] = nlohmann::json::array();

    for (const fs::path& file : ScanLegacySessionFiles(context.home)) {
        MigrationSourceFile source;
        source.path = RelativeToHome(context.home, file);
        const auto bytes = ReadFileBytes(file);
        if (!bytes.has_value()) {
            report.errors.push_back("读不出: " + source.path);
            continue;
        }
        source.bytes = bytes->size();
        source.sha256 = Sha256Text(*bytes);
        const auto doc = ParseLegacySessionDoc(*bytes);
        if (!doc.has_value()) {
            report.errors.push_back("不是旧会话档(首行非 meta): " + source.path);
            continue;
        }
        source.meta_cwd = doc->cwd;
        const workspace::WorkspaceIdentity* identity = context.ResolveCached(doc->cwd);
        if (identity == nullptr) {
            report.errors.push_back("meta.cwd 裁决不出 workspace(跳过该场): " + source.path);
            continue;
        }
        MigrationPlanSession planned;
        planned.source = source;
        planned.archived = PathToUtf8(file.parent_path().filename()) == "archive";
        planned.already_imported = context.HasCommitted(source.sha256);
        planned.workspace_key = identity->workspace_key;
        planned.identity_kind = identity->identity_kind;
        report.sessions.push_back(std::move(planned));
        intent["source"]["files"].push_back(nlohmann::json{{"path", source.path},
                                                           {"bytes", source.bytes},
                                                           {"sha256", source.sha256},
                                                           {"meta_cwd", source.meta_cwd}});
        intent["planned"].push_back(
            nlohmann::json{{"source_sha256", source.sha256},
                           {"workspace_key", report.sessions.back().workspace_key}});
    }
    report.imported_before = context.committed_sources.size();

    // memory 侧:列旧项目库与映射(不动盘)。
    for (const OldProjectDir& project : ScanLegacyProjectDirs(context.home)) {
        MigrationMemoryProject entry;
        entry.old_project_key = project.key;
        entry.source_dir = RelativeToHome(context.home, project.memory);
        if (!project.has_memory) {
            entry.outcome = "skipped";
            entry.note = "旧项目目录没有 memory/(只剩残留登记)";
            report.memory_projects.push_back(std::move(entry));
            continue;
        }
        const ProjectMapping mapping = ResolveProjectMapping(context, project);
        entry.workspace_key = mapping.workspace_key;
        entry.mapping_source = mapping.source;
        if (mapping.workspace_key.empty()) {
            entry.outcome = "unmappable";
            entry.note = "算不出目标 workspace(缺 project.json;用 --project-root <路径> 显式指认)";
        }
        report.memory_projects.push_back(std::move(entry));
    }

    std::error_code ec;
    fs::create_directories(operation_dir, ec);
    if (ec) {
        return std::unexpected("migration.target_write_failed: 回执目录建不起: " +
                               PathToUtf8(operation_dir));
    }
    if (!AtomicWriteText(operation_dir / "intent.json", intent.dump(2) + "\n")) {
        return std::unexpected("migration.target_write_failed: intent.json 写不进: " +
                               PathToUtf8(operation_dir / "intent.json"));
    }
    nlohmann::json progress;
    progress["schema"] = std::string(contracts::kMigrationSchemaName);
    progress["version"] = contracts::kMigrationSchemaVersion;
    progress["operation_id"] = report.operation_id;
    progress["phase"] = "planned";
    progress["done"] = 0;
    progress["total"] = report.sessions.size();
    progress["updated_at_ms"] = context.now_ms;
    progress["last_source_sha256"] = "";
    progress["last_outcome"] = "";
    if (!WriteProgress(operation_dir, progress)) {
        return std::unexpected("migration.target_write_failed: progress.json 写不进");
    }
    return report;
}

std::expected<MigrationRunReport, std::string> RunStorageMigration(const MigratorOptions& options,
                                                                   const std::string& operation_id) {
    std::string error;
    MigratorContext context = MakeContext(options, &error);
    if (!error.empty()) {
        return std::unexpected(error);
    }
    context.LoadCommittedSources();

    MigrationRunReport report;
    try {
        // 续跑指认:非空直取;空 = 最近一只未 committed 的 operation。
        std::string chosen = operation_id;
        if (chosen.empty()) {
            for (const fs::path& dir : context.ListOperationDirs()) {
                if (!receipts::ReadResult(dir).has_value()) {
                    chosen = PathToUtf8(dir.filename());
                    report.resumed_operation = chosen;
                    break;
                }
            }
        }
        if (chosen.empty()) {
            // 没有可续的:现 plan 一只再跑。
            auto plan = PlanStorageMigration(options);
            if (!plan.has_value()) {
                return std::unexpected(plan.error());
            }
            chosen = plan->operation_id;
            report.resumed_operation = chosen;
        }

        const fs::path operation_dir = context.migrations_root / Utf8ToPath(chosen);
        const auto intent = receipts::ReadIntent(operation_dir);
        if (!intent.has_value() || !intent->is_object()) {
            return std::unexpected(std::string(contracts::kErrMigrationIntentExists) +
                                   ": 找不到 intent.json: " + PathToUtf8(operation_dir));
        }
        if (receipts::ReadResult(operation_dir).has_value()) {
            return std::unexpected(std::string(contracts::kErrMigrationResultExists) +
                                   ": 该 operation 已 committed: " + chosen);
        }

        // intent 冻结的源清单(盘中 sha 变了逐件报 source_sha_mismatch,不中断整批)。
        struct PlannedFile {
            MigrationSourceFile source;
            bool archived = false;
        };
        std::vector<PlannedFile> planned_files;
        if (intent->contains("source") && (*intent)["source"].contains("files") &&
            (*intent)["source"]["files"].is_array()) {
            for (const auto& file : (*intent)["source"]["files"]) {
                if (!file.is_object()) {
                    continue;
                }
                PlannedFile planned;
                planned.source.path = file.value("path", std::string());
                planned.source.bytes = file.value("bytes", std::uintmax_t{0});
                planned.source.sha256 = file.value("sha256", std::string());
                planned.source.meta_cwd = file.value("meta_cwd", std::string());
                planned.archived =
                    planned.source.path.find("sessions/archive/") != std::string::npos;
                if (!planned.source.path.empty() && !planned.source.sha256.empty()) {
                    planned_files.push_back(std::move(planned));
                }
            }
        }

        const std::int64_t started_at = context.now_ms;
        std::size_t done = 0;
        for (const PlannedFile& planned : planned_files) {
            // 幂等:同 SHA 已 committed -> already_imported(不再造一份)。
            const auto committed = context.committed_sources.find(planned.source.sha256);
            if (committed != context.committed_sources.end()) {
                MigrationResultItem item;
                item.source_sha256 = planned.source.sha256;
                item.source_path = planned.source.path;
                item.outcome = "already_imported";
                item.target_session_id = std::get<1>(committed->second);
                item.target_workspace_key = std::get<2>(committed->second);
                item.subagent_detail = std::string(contracts::kSubagentDetailUnavailableLegacy);
                report.counts["already_imported"] += 1;
                report.items.push_back(std::move(item));
                done += 1;
                continue;
            }
            MigrationResultItem item = ImportOneSession(context, planned.source, planned.archived);
            if (item.outcome == "imported") {
                context.committed_sources[item.source_sha256] =
                    std::make_tuple(chosen, item.target_session_id, item.target_workspace_key);
                report.counts["imported"] += 1;
                context.Note("imported " + item.source_path + " -> " + item.target_workspace_key +
                             "/" + item.target_session_id);
            } else if (item.outcome == "skipped_unreadable") {
                report.counts["skipped_unreadable"] += 1;
                context.Note("skipped " + item.source_path + ": " + item.error_code);
            } else {
                report.counts["failed"] += 1;
                context.Note("failed " + item.source_path + ": " + item.error_code);
            }
            report.items.push_back(std::move(item));
            done += 1;
            nlohmann::json progress;
            progress["schema"] = std::string(contracts::kMigrationSchemaName);
            progress["version"] = contracts::kMigrationSchemaVersion;
            progress["operation_id"] = chosen;
            progress["phase"] = "importing";
            progress["done"] = done;
            progress["total"] = planned_files.size();
            progress["updated_at_ms"] = NowWallMs();
            progress["last_source_sha256"] = report.items.back().source_sha256;
            progress["last_outcome"] = report.items.back().outcome;
            if (!WriteProgress(operation_dir, progress)) {
                return std::unexpected(
                    std::string(contracts::kErrMigrationTargetWriteFailed) +
                    ": progress.json 写不进(可续跑)");
            }
            context.Fault("file_imported");
        }

        // ---- memory 侧(会话先落地,source_sessions 才升得了四段引用) ----
        for (const OldProjectDir& project : ScanLegacyProjectDirs(context.home)) {
            if (!project.has_memory) {
                continue;
            }
            MigrationMemoryProject entry;
            entry.old_project_key = project.key;
            entry.source_dir = RelativeToHome(context.home, project.memory);
            const ProjectMapping mapping = ResolveProjectMapping(context, project);
            entry.workspace_key = mapping.workspace_key;
            entry.mapping_source = mapping.source;
            if (mapping.workspace_key.empty()) {
                entry.outcome = "unmappable";
                entry.note = "算不出目标 workspace(--project-root 指认后重跑)";
                report.memory_projects.push_back(std::move(entry));
                continue;
            }
            const fs::path workspace_dir = context.workspaces_root / Utf8ToPath(mapping.workspace_key);
            const fs::path target_memory = workspace_dir / "memory";
            std::error_code mem_ec;
            fs::create_directories(target_memory, mem_ec);
            if (mem_ec) {
                entry.outcome = "failed";
                entry.note = "目标 memory 目录建不起: " + mem_ec.message();
                report.memory_projects.push_back(std::move(entry));
                continue;
            }
            // 首迁登记 migrated_from(合同 §二;已登记不覆盖)。
            const auto manifest = workspace::ReadWorkspaceManifest(workspace_dir);
            if (manifest.status == workspace::ManifestRead::Status::Ok &&
                !manifest.manifest.migrated_from.has_value()) {
                workspace::WorkspaceManifest updated = manifest.manifest;
                updated.migrated_from =
                    nlohmann::json{{"old_project_key", project.key}, {"at_ms", context.now_ms}};
                (void)workspace::WriteWorkspaceManifestAtomic(workspace_dir, updated);
            }

            std::size_t imported = 0;
            std::size_t skipped = 0;
            std::size_t failed = 0;
            for (const char* folder : {"facts", "preferences", "feedback"}) {
                const fs::path source_folder = project.memory / folder;
                std::error_code folder_ec;
                if (!fs::is_directory(source_folder, folder_ec) || folder_ec) {
                    continue;
                }
                for (const auto& topic_file : fs::directory_iterator(source_folder, folder_ec)) {
                    if (folder_ec) break;
                    std::error_code file_ec;
                    if (!topic_file.is_regular_file(file_ec) || file_ec ||
                        topic_file.path().extension() != ".md") {
                        continue;
                    }
                    MigrationMemoryTopic topic;
                    topic.source_file = RelativeToHome(context.home, topic_file.path());
                    const auto text = ReadFileBytes(topic_file.path());
                    if (!text.has_value()) {
                        topic.outcome = "failed";
                        topic.note = "读不出";
                        failed += 1;
                        entry.topics.push_back(std::move(topic));
                        continue;
                    }
                    topic.source_sha256 = Sha256Text(*text);
                    LegacyTopic parsed = ParseLegacyTopic(*text);
                    if (!parsed.parsed) {
                        topic.outcome = "skipped";
                        topic.note = "主题解析不动: " + parsed.error;
                        skipped += 1;
                        entry.topics.push_back(std::move(topic));
                        continue;
                    }
                    if (!LegacyIsValidId(parsed.entry.id)) {
                        topic.outcome = "skipped";
                        topic.note = "主题 id 不合法: " + parsed.entry.id;
                        skipped += 1;
                        entry.topics.push_back(std::move(topic));
                        continue;
                    }
                    topic.id = parsed.entry.id;
                    // 裸 session id 升四段引用(合同 §六)。
                    bool upgraded_refs = false;
                    for (std::string& session_ref : parsed.entry.source_sessions) {
                        if (session_ref.find("workspace_key=") == std::string::npos) {
                            session_ref =
                                QualifyLegacySessionRef(mapping.workspace_key, session_ref);
                            upgraded_refs = true;
                        }
                    }
                    // 统一写成 schema 3(生产新写一律 3;index/catalog 可重建)。
                    const std::string kind_name = memory::MemoryKindName(parsed.entry.kind);
                    parsed.entry.schema = 3;
                    parsed.entry.name =
                        parsed.entry.id.starts_with(kind_name + ".") &&
                                parsed.entry.id.size() > kind_name.size() + 1
                            ? parsed.entry.id.substr(kind_name.size() + 1)
                            : parsed.entry.id;
                    parsed.entry.file = std::string(folder) + "/" + parsed.entry.name + ".md";
                    const fs::path target_file = target_memory / Utf8ToPath(parsed.entry.file);
                    {
                        // 主题按 kind 分目录(facts/preferences/feedback),
                        // 目标侧首迁时目录还不存在,写盘前先落目录。
                        std::error_code topic_dir_ec;
                        fs::create_directories(target_file.parent_path(), topic_dir_ec);
                        if (topic_dir_ec) {
                            topic.outcome = "failed";
                            topic.note = "目标主题目录建不起: " + topic_dir_ec.message();
                            failed += 1;
                            entry.topics.push_back(std::move(topic));
                            continue;
                        }
                    }
                    const std::string composed = memory::frontmatter::BuildTopicText(
                        parsed.entry, parsed.fingerprints, parsed.body);
                    topic.target_file = RelativeToHome(workspace_dir, target_file);
                    std::error_code exists_ec;
                    if (fs::exists(target_file, exists_ec) && !exists_ec) {
                        const auto existing = ReadFileBytes(target_file);
                        if (existing.has_value() && *existing == composed) {
                            topic.outcome = "already_imported";
                            topic.target_sha256 = Sha256Text(composed);
                            entry.topics.push_back(std::move(topic));
                            continue;
                        }
                        // 同名不同文:不覆盖(目标侧可能有迁移后新写入)。
                        topic.outcome = "skipped";
                        topic.note = "目标已有同 id 不同内容的主题,不覆盖";
                        skipped += 1;
                        entry.topics.push_back(std::move(topic));
                        continue;
                    }
                    if (!AtomicWriteText(target_file, composed)) {
                        topic.outcome = "failed";
                        topic.note = "目标主题写不进";
                        failed += 1;
                        entry.topics.push_back(std::move(topic));
                        continue;
                    }
                    topic.outcome = "imported";
                    topic.target_sha256 = Sha256Text(composed);
                    topic.note = upgraded_refs ? "source_sessions 已升全限定引用" : "";
                    imported += 1;
                    entry.topics.push_back(std::move(topic));
                    context.Fault("memory_topic");
                }
            }
            // 候选箱:projects/<key>/memory-candidates/ -> <workspace>/memory/memory-candidates/。
            const fs::path candidates_source = project.dir / "memory-candidates";
            std::error_code cand_ec;
            if (fs::is_directory(candidates_source, cand_ec) && !cand_ec) {
                const fs::path candidates_target = target_memory / "memory-candidates";
                fs::create_directories(candidates_target, cand_ec);
                for (const auto& candidate : fs::directory_iterator(candidates_source, cand_ec)) {
                    if (cand_ec) break;
                    std::error_code cfile_ec;
                    if (!candidate.is_regular_file(cfile_ec) || cfile_ec ||
                        candidate.path().extension() != ".json") {
                        continue;
                    }
                    const auto bytes = ReadFileBytes(candidate.path());
                    if (!bytes.has_value()) {
                        continue;
                    }
                    if (AtomicWriteText(candidates_target / candidate.path().filename(), *bytes)) {
                        entry.candidates.push_back(PathToUtf8(candidate.path().filename()));
                    }
                }
            }
            if (imported > 0) {
                const auto rebuilt =
                    memory::RebuildMemoryIndex(target_memory, /*user_layer=*/false);
                if (!rebuilt.has_value()) {
                    entry.note += " index 重建失败: " + rebuilt.error();
                }
            }
            entry.outcome =
                failed > 0 ? "partial" : (imported > 0 || skipped > 0 ? "imported" : "skipped");
            if (entry.outcome != "skipped" || !entry.topics.empty() || !entry.candidates.empty()) {
                report.memory_projects.push_back(std::move(entry));
            }
        }

        // ---- result.json(原子写;只有它算 committed) ----
        nlohmann::json result;
        result["schema"] = std::string(contracts::kMigrationSchemaName);
        result["version"] = contracts::kMigrationSchemaVersion;
        result["operation_id"] = chosen;
        result["started_at_ms"] = started_at;
        result["finished_at_ms"] = NowWallMs();
        nlohmann::json items = nlohmann::json::array();
        for (const MigrationResultItem& item : report.items) {
            nlohmann::json entry{{"source_sha256", item.source_sha256},
                                 {"source_path", item.source_path},
                                 {"outcome", item.outcome},
                                 {"target_session_id", item.target_session_id},
                                 {"target_workspace_key", item.target_workspace_key},
                                 {"terminal_event_hash", item.terminal_event_hash},
                                 {"legacy_partial", item.legacy_partial},
                                 {"subagent_detail", item.subagent_detail},
                                 {"missing", item.missing}};
            if (!item.error_code.empty()) {
                entry["error_code"] = item.error_code;
            }
            items.push_back(std::move(entry));
        }
        result["items"] = std::move(items);
        nlohmann::json counts;
        for (const char* key : {"imported", "already_imported", "skipped_unreadable", "failed"}) {
            const auto it = report.counts.find(key);
            counts[key] = it != report.counts.end() ? it->second : 0;
        }
        result["counts"] = counts;
        result["memory_projects"] = nlohmann::json::array();
        for (const MigrationMemoryProject& project : report.memory_projects) {
            nlohmann::json entry{{"old_project_key", project.old_project_key},
                                 {"source_dir", project.source_dir},
                                 {"workspace_key", project.workspace_key},
                                 {"outcome", project.outcome}};
            nlohmann::json topics = nlohmann::json::array();
            for (const MigrationMemoryTopic& topic : project.topics) {
                topics.push_back(nlohmann::json{{"id", topic.id},
                                                {"source_file", topic.source_file},
                                                {"source_sha256", topic.source_sha256},
                                                {"target_file", topic.target_file},
                                                {"target_sha256", topic.target_sha256},
                                                {"outcome", topic.outcome},
                                                {"note", topic.note}});
            }
            entry["topics"] = std::move(topics);
            entry["candidates"] = project.candidates;
            if (!project.note.empty()) {
                entry["note"] = project.note;
            }
            result["memory_projects"].push_back(std::move(entry));
        }
        result["training"] = std::string(contracts::kLegacyTrainingPolicy);
        result["source_deleted"] = false;

        // ---- 删源(单独确认;只删已 committed 且复验通过的) ----
        if (options.delete_source) {
            if (!options.confirm_delete) {
                return std::unexpected(std::string(contracts::kErrMigrationDeleteUnverified) +
                                       ": --delete-source 须配 --yes 二次确认");
            }
            bool all_verified = true;
            std::vector<fs::path> to_delete;
            for (const MigrationResultItem& item : report.items) {
                if (item.outcome != "imported" && item.outcome != "already_imported") {
                    all_verified = false;
                    break;
                }
                const fs::path source_path = context.home / Utf8ToPath(item.source_path);
                const auto bytes = ReadFileBytes(source_path);
                if (!bytes.has_value() || Sha256Text(*bytes) != item.source_sha256) {
                    all_verified = false;
                    break;
                }
                if (item.outcome == "imported") {
                    const fs::path session_dir = context.workspaces_root /
                                                 Utf8ToPath(item.target_workspace_key) / "sessions" /
                                                 Utf8ToPath(item.target_session_id);
                    const auto verify = trajectory::VerifySessionDir(session_dir);
                    const auto replay = trajectory::FoldStreamReplay(session_dir / "main.jsonl");
                    if (!verify.ok || !replay.ok()) {
                        all_verified = false;
                        break;
                    }
                }
                to_delete.push_back(source_path);
            }
            if (!all_verified) {
                return std::unexpected(std::string(contracts::kErrMigrationDeleteUnverified) +
                                       ": 存在未核验源档,整批不删源");
            }
            for (const fs::path& source_path : to_delete) {
                std::error_code remove_ec;
                if (fs::remove(source_path, remove_ec) && !remove_ec) {
                    report.deleted_sources.push_back(RelativeToHome(context.home, source_path));
                }
                context.Fault("source_deleted");
            }
            result["source_deleted"] = !report.deleted_sources.empty();
            report.source_deleted = !report.deleted_sources.empty();
        }

        if (!AtomicWriteText(operation_dir / "result.json", result.dump(2) + "\n")) {
            return std::unexpected(std::string(contracts::kErrMigrationTargetWriteFailed) +
                                   ": result.json 写不进(可续跑)");
        }
        context.Fault("result_committed");
        report.operation_id = chosen;
        return report;
    } catch (const MigrationInterrupted& interrupted) {
        report.operation_id = operation_id.empty() ? report.resumed_operation : operation_id;
        report.error_code = std::string(contracts::kErrMigrationInterrupted);
        report.error_text = interrupted.point;
        return report;
    }
}

MigrationStatusReport QueryStorageMigrationStatus(const MigratorOptions& options) {
    MigrationStatusReport status;
    std::string error;
    MigratorContext context = MakeContext(options, &error);
    if (!error.empty()) {
        return status;
    }
    context.LoadCommittedSources();

    for (const fs::path& dir : context.ListOperationDirs()) {
        MigrationOperationStatus operation;
        operation.operation_id = PathToUtf8(dir.filename());
        if (receipts::ReadResult(dir).has_value()) {
            operation.phase = "committed";
            status.committed_operations += 1;
            // committed 也回填 done/total(progress 是最后落账的进度),账面
            // 不因收工而丢总数。
            if (const auto progress = receipts::ReadProgress(dir); progress.has_value()) {
                operation.done = progress->value("done", std::size_t{0});
                operation.total = progress->value("total", std::size_t{0});
                operation.last_source_sha256 = progress->value("last_source_sha256", std::string());
                operation.last_outcome = progress->value("last_outcome", std::string());
                operation.updated_at_ms = progress->value("updated_at_ms", std::int64_t{0});
            }
        } else if (const auto progress = receipts::ReadProgress(dir); progress.has_value()) {
            operation.phase = progress->value("phase", "unknown");
            operation.done = progress->value("done", std::size_t{0});
            operation.total = progress->value("total", std::size_t{0});
            operation.last_source_sha256 = progress->value("last_source_sha256", std::string());
            operation.last_outcome = progress->value("last_outcome", std::string());
            operation.updated_at_ms = progress->value("updated_at_ms", std::int64_t{0});
        } else if (receipts::ReadIntent(dir).has_value()) {
            operation.phase = "planned";
        } else {
            operation.phase = "unknown";
        }
        status.operations.push_back(std::move(operation));
    }

    // 未迁清单:旧档逐件对 committed SHA(§7.3:不能因旧 project 眼下没打开
    // 就说全机迁完)。
    for (const fs::path& file : ScanLegacySessionFiles(context.home)) {
        const auto bytes = ReadFileBytes(file);
        if (!bytes.has_value()) {
            status.pending_session_files += 1;
            continue;
        }
        if (!context.HasCommitted(Sha256Text(*bytes))) {
            status.pending_session_files += 1;
        }
    }
    for (const OldProjectDir& project : ScanLegacyProjectDirs(context.home)) {
        if (!project.has_memory) {
            continue;
        }
        const ProjectMapping mapping = ResolveProjectMapping(context, project);
        if (mapping.workspace_key.empty()) {
            status.unmappable_projects.push_back(project.key);
            continue;
        }
        // 目标 workspace 登记过 migrated_from 即算迁过(合同 §二)。
        const fs::path workspace_dir = context.workspaces_root / Utf8ToPath(mapping.workspace_key);
        const auto manifest = workspace::ReadWorkspaceManifest(workspace_dir);
        bool migrated = false;
        if (manifest.status == workspace::ManifestRead::Status::Ok &&
            manifest.manifest.migrated_from.has_value()) {
            migrated = manifest.manifest.migrated_from->value("old_project_key", std::string()) ==
                       project.key;
        }
        if (!migrated) {
            status.pending_memory_projects += 1;
        }
    }
    return status;
}

}  // namespace lubancode::workspace::migrator
