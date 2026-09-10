// 轨迹 v3 收尾棒:clear 八步的 v3 折算(SessionManager::ClearV3Locked)。
// v3 场上 /clear = 关当前场开新场(§3.3.1 语义同源):
//   - 旧账 command.received + session.ended(reason=clear,nextSessionId 随行);
//   - 新卷首行 system + session.started(start_reason=clear,previous 指旧场),
//     新场 command.completed(qualifiedRequestedRef 指旧场 received);
//   - 旧场保留可 resume(LatestResumableSessionId),新场接活照写;
//   - 运行侧没收口的活动(turn/孩子)如实标 incomplete,不冒充 clean;
//   - 开关关的 v2 clear 原路一字不动(回归钉)。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;
using lubancode::runtime::TrajectoryTurnBridge;

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

std::filesystem::path FreshRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-v3-clear-switch-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const std::filesystem::path& root) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.238-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    return options;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantText(const std::string& text) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Request MakeRequest(const std::string& system, const std::vector<api::Message>& messages) {
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = system;
    request.messages = messages;
    return request;
}

agent::RequestPreparedContext PreparedContext() { return agent::RequestPreparedContext{}; }

api::Usage SampleUsage() {
    api::Usage usage;
    usage.input_tokens = 900;
    usage.output_tokens = 20;
    return usage;
}

std::vector<nlohmann::json> ReadLines(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    std::vector<nlohmann::json> rows;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        rows.push_back(nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false));
        REQUIRE_FALSE(rows.back().is_discarded());
    }
    return rows;
}

std::vector<std::string> KindsOf(const std::vector<nlohmann::json>& rows) {
    std::vector<std::string> kinds;
    for (const auto& row : rows) {
        const auto it = row.find("kind");
        kinds.push_back(it != row.end() && it->is_string() ? it->get<std::string>() : std::string());
    }
    return kinds;
}

std::filesystem::path V3StreamOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir() /
           platform::Utf8ToPath(platform::PathToUtf8(ledger.session_dir().filename()) + ".jsonl");
}

// 有活动 turn 的参与者:CancelActiveTurn 回非空 → v3 折算如实标 incomplete。
struct BusyParticipant : trajectory::ClearParticipant {
    std::string CancelActiveTurn() override { return "turn-open-1"; }
    std::vector<ChildClosure> CancelActiveChildren() override { return {}; }
    std::vector<std::string> CancelQueuedItems() override { return {}; }
    std::string ActiveRecordSelectionId() override { return {}; }
    void ResetInMemoryState() override { ++resets; }
    int resets = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// v3 clear:关当前场开新场
// ---------------------------------------------------------------------------

TEST_CASE("v3 clear: 旧场封账保留可 resume,新场开卷接跨场命令账") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("switch");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string old_id = ledger->session_id();
    const std::filesystem::path old_stream = V3StreamOf(*ledger);
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("清场前的一问"));
        const std::string request_id = bridge->OnRequestPrepared(
            MakeRequest("你是 LubanCode,读写跑都走工具。", {UserMessage("清场前的一问")}),
            PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("清场前的一答"), "end_turn",
                                          "resp-1"));
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }

    trajectory::NullClearParticipant participant;
    trajectory::ClearRequest request;
    const auto outcome = ledger->ClearSession(request, &participant);
    REQUIRE(outcome.error_code.empty());

    // ---- 换账证据:v3 无对应物的字段如实留空(不伪造 v2 形状)。
    CHECK(outcome.old_session_id == old_id);
    CHECK_FALSE(outcome.new_session_id.empty());
    CHECK(outcome.new_session_id != old_id);
    CHECK(outcome.new_session_prepared);
    CHECK_FALSE(outcome.requested_event_id.empty());            // 旧账 command.received
    CHECK(outcome.clear_requested_event_id.empty());            // v3 无 session.clear_requested
    CHECK_FALSE(outcome.old_session_ended_event_id.empty());    // session.ended
    CHECK(outcome.old_close_quality == "clean");
    CHECK(outcome.old_run_terminal_event_id.empty());           // v3 无 run terminal
    CHECK(outcome.old_run_terminal_kind.empty());
    CHECK_FALSE(outcome.old_session_json_finalized);            // v3 无 session.json
    CHECK(outcome.new_run_started_event_id.empty());            // v3 无 run.started
    CHECK_FALSE(outcome.new_command_completed_event_id.empty());
    CHECK(outcome.new_session_running);
    CHECK(outcome.active_switched);
    CHECK_FALSE(outcome.old_journal_sha256.empty());            // 封账行 hash

    // active 已切:账本指新场。
    CHECK(ledger->session_id() == outcome.new_session_id);

    // ---- 旧账:command.received + session.ended(reason=clear,nextSessionId)。
    const auto old_rows = ReadLines(old_stream);
    const auto old_kinds = KindsOf(old_rows);
    CHECK(std::find(old_kinds.begin(), old_kinds.end(), "command.received") != old_kinds.end());
    CHECK(std::find(old_kinds.begin(), old_kinds.end(), "session.ended") != old_kinds.end());
    for (const auto& row : old_rows) {
        if (row.value("kind", std::string()) == "command.received") {
            REQUIRE(row.contains("commandId"));
            CHECK(row.at("commandId") == request.command_id);
        }
        if (row.value("kind", std::string()) == "session.ended") {
            REQUIRE(row.contains("payload"));
            const auto& payload = row.at("payload");
            CHECK(payload.value("reason", std::string()) == "clear");
            CHECK(payload.value("closeQuality", std::string()) == "clean");
            CHECK(payload.value("nextSessionId", std::string()) == outcome.new_session_id);
        }
    }
    // 旧账验卷仍过(clear 补的两枚事件语义合法)。
    CHECK(lubancode::trajectory::v3::VerifyV3File(old_stream).ok);

    // ---- 新账:首行 system + session.started + 跨场 command.completed。
    const auto new_stream = V3StreamOf(*ledger);
    const auto new_rows = ReadLines(new_stream);
    REQUIRE_FALSE(new_rows.empty());
    CHECK(new_rows[0].value("type", std::string()) == "message");
    CHECK(new_rows[0].at("message").value("role", std::string()) == "system");
    const auto new_kinds = KindsOf(new_rows);
    CHECK(std::find(new_kinds.begin(), new_kinds.end(), "session.started") != new_kinds.end());
    CHECK(std::find(new_kinds.begin(), new_kinds.end(), "command.completed") != new_kinds.end());
    for (const auto& row : new_rows) {
        if (row.value("kind", std::string()) == "command.completed") {
            REQUIRE(row.contains("payload"));
            const auto& payload = row.at("payload");
            REQUIRE(payload.contains("qualifiedRequestedRef"));
            CHECK(payload.at("qualifiedRequestedRef").value("sessionId", std::string()) == old_id);
            CHECK(payload.at("qualifiedRequestedRef").value("eventId", std::string()) ==
                  outcome.requested_event_id);
        }
    }
    CHECK(lubancode::trajectory::v3::VerifyV3File(new_stream).ok);

    // ---- lifecycle:create_session 记了 start_reason=clear 的 intent。
    bool clear_intent_seen = false;
    const auto lifecycle_dir = ledger->session_dir().parent_path().parent_path() / "lifecycle";
    std::error_code ec;
    if (std::filesystem::exists(lifecycle_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(lifecycle_dir, ec)) {
            const auto intent_path = entry.path() / "intent.json";
            if (!std::filesystem::exists(intent_path, ec)) {
                continue;
            }
            std::ifstream file(intent_path, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
            const auto parsed = nlohmann::json::parse(content, nullptr, false);
            if (parsed.is_discarded()) {
                continue;
            }
            if (parsed.contains("parameters") && parsed.at("parameters").contains("start_reason") &&
                parsed.at("parameters").at("start_reason") == "clear") {
                clear_intent_seen = true;
            }
        }
    }
    CHECK(clear_intent_seen);

    // ---- 旧场保留可 resume:最近一场可恢复的正是刚封的旧场。
    CHECK(ledger->LatestResumableSessionId() == old_id);

    // ---- 新场接活照写:再走一轮,正文落新账。
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-2", "external_user");
        bridge->RecordInput(UserMessage("清场后的一问"));
        const std::string request_id = bridge->OnRequestPrepared(
            MakeRequest("你是 LubanCode,读写跑都走工具。", {UserMessage("清场后的一问")}),
            PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("清场后的一答"), "end_turn",
                                          "resp-2"));
        bridge->OnUsageRecorded(request_id, SampleUsage(), /*reported_by_provider=*/true,
                                 "resp-2", 0, true, false);
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }
    bool new_turn_seen = false;
    for (const auto& row : ReadLines(new_stream)) {
        if (row.value("type", std::string()) == "message" && row.contains("message") &&
            row.at("message").contains("content")) {
            const auto& content = row.at("message").at("content");
            const std::string text =
                content.is_string() ? content.get<std::string>() : content.dump();
            if (text.find("清场后的一问") != std::string::npos) {
                new_turn_seen = true;
            }
        }
    }
    CHECK(new_turn_seen);
    CHECK(lubancode::trajectory::v3::VerifyV3File(new_stream).ok);

    // 新场再封口也照走(session.ended on 新场)。
    CHECK(ledger->CloseSession("exit").error_code.empty());
}

TEST_CASE("v3 clear: 运行侧没收口的活动如实标 incomplete") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("incomplete");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    BusyParticipant participant;
    trajectory::ClearRequest request;
    const auto outcome = ledger->ClearSession(request, &participant);
    REQUIRE(outcome.error_code.empty());
    CHECK(outcome.old_close_quality == "incomplete");
    CHECK(participant.resets == 1);  // 第 8 步清内存确曾调用
    // v3 布局:sessions/<id>/<id>.jsonl(卷在同名目录里)。
    const auto sessions_root = V3StreamOf(*ledger).parent_path().parent_path();
    const auto old_stream = sessions_root / platform::Utf8ToPath(outcome.old_session_id) /
                            platform::Utf8ToPath(outcome.old_session_id + ".jsonl");
    for (const auto& row : ReadLines(old_stream)) {
        if (row.value("kind", std::string()) == "session.ended") {
            CHECK(row.at("payload").value("closeQuality", std::string()) == "incomplete");
        }
    }
}

// ---------------------------------------------------------------------------
// 开关关:v2 clear 八步原路(回归钉——v3 分支不扰动 v2)
// ---------------------------------------------------------------------------

TEST_CASE("开关关: v2 clear 八步照旧走 main.jsonl 换账") {
    const auto root = FreshRoot("v2-regression");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string old_id = ledger->session_id();
    const std::filesystem::path old_main = ledger->session_dir() / "main.jsonl";
    CHECK(std::filesystem::exists(old_main));  // v2 布局
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("v2 场的一问"));
        const std::string request_id =
            bridge->OnRequestPrepared(MakeRequest("SYSTEM-V2", {UserMessage("v2 场的一问")}),
                                      PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        bridge->OnRequestSent(request_id);
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("v2 场的一答"), "end_turn",
                                          "resp-1"));
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }
    trajectory::NullClearParticipant participant;
    trajectory::ClearRequest request;
    const auto outcome = ledger->ClearSession(request, &participant);
    REQUIRE(outcome.error_code.empty());
    // v2 证据字段齐活(run terminal/session.json 落定)。
    CHECK_FALSE(outcome.old_run_terminal_event_id.empty());
    CHECK(outcome.old_run_terminal_kind == "run.completed");
    CHECK(outcome.old_session_json_finalized);
    CHECK_FALSE(outcome.new_run_started_event_id.empty());
    CHECK(std::filesystem::exists(ledger->session_dir() / "main.jsonl"));
    CHECK(ledger->session_id() != old_id);
    // 旧场 session.json 落 closed(incomplete 前提没有,干净收口)。
    const auto old_manifest = trajectory::ReadSessionJson(
        ledger->session_dir().parent_path() / platform::Utf8ToPath(old_id));
    REQUIRE(old_manifest.has_value());
    CHECK(old_manifest->status == "closed");
}
