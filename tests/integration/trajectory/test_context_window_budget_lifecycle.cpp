// 上下文预算单 P1(§四"写账-恢复-应用一条线"/§五 清场/§七 生命周期回归)。
//
// 复现口径(§四第一条,以测试代码模拟,不做真机操作):在有明确模型身份
// 的会话里设预算 → 封场 → 分别走 /resume(ResumeInteractive)与
// --continue(Open(resume_at_launch));断言旧场事件、恢复值(control 折
// 叠)与恢复裁决(ResolveRestoredContextWindow)全链不丢。修复前:事件
// 无写入口、折叠无身份、恢复无应用——本册每一断言都钉住其中一环。
//
// 盖住:
//   1. schema:session.context_window.applied 注册、statusless、载荷合同。
//   2. v3 写路:RecordContextWindowChanged 落真值+身份+来源;读面投影
//      FindLastContextWindowApplied 折最后一枚。
//   3. v3 /resume:outcome.control 折出预算与身份,仲裁裁决 apply。
//   4. v3 --continue:LaunchResumeControlState 带回同一份控制态。
//   5. v3 /clear:ClearSession 换场后快照落新场,旧场字节不动。
//   6. v2 旧档:control.context_window.changed(字符串值)可折;无字段
//      旧档回落(control 无预算,仲裁 no_record)。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"
#include "agent/loop.hpp"  // RequestPreparedContext
#include "api/types.hpp"
#include "cli/context_tracker.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/schema3.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
namespace fs = std::filesystem;
using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;

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

fs::path FreshRoot(const char* tag) {
    const auto dir =
        fs::temp_directory_path() / ("lubancode-cw-budget-" + std::string(tag));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const fs::path& root) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "repo");
    options.launch_cwd = "D:/tmp/repo";
    options.lubancode_version = "0.26.279-test";
    options.v3_system_content = "你是 LubanCode。";
    return options;
}

fs::path V3StreamOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir() /
           platform::Utf8ToPath(platform::PathToUtf8(ledger.session_dir().filename()) + ".jsonl");
}

std::vector<nlohmann::json> ReadLines(const fs::path& stream) {
    std::vector<nlohmann::json> rows;
    std::ifstream file(stream, std::ios::binary);
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

std::vector<const nlohmann::json*> RowsOfKind(const std::vector<nlohmann::json>& rows,
                                              const char* kind) {
    std::vector<const nlohmann::json*> out;
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) == kind) {
            out.push_back(&row);
        }
    }
    return out;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

// 铺一轮真实对话(与 test_v3_t11_title 同款):resume 源场要有内容可续。
void WriteOneTurn(TrajectorySessionLedger& ledger) {
    auto bridge = ledger.NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("帮我修上下文预算"));
    agent::RequestPreparedContext ctx;
    ctx.purpose = accounting::RequestPurpose::MainTurn;
    api::Request request;
    request.model = "kimi";
    request.system = "SYSTEM-X";
    request.messages.push_back(UserMessage("帮我修上下文预算"));
    const std::string request_id = bridge->OnRequestPrepared(request, ctx);
    REQUIRE_FALSE(request_id.empty());
    REQUIRE(bridge->OnRequestSent(request_id));
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"看了配置。"});
    REQUIRE(bridge->OnOutputCompleted(request_id, assistant, "end_turn", "resp-1"));
    bridge->EndTurn(true, false, "");
}

// schema 合同层的构造件(纯 JSON,不动盘)。
nlohmann::json EventJson(const char* kind, nlohmann::json payload) {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "event";
    json["schemaVersion"] = 3;
    json["sessionId"] = "20260919-090000-PC0001";
    json["runId"] = "run-000001";
    json["seq"] = 5;
    json["timestamp"] = "2026-09-19T01:00:00.000Z";
    json["eventId"] = "evt-000004";
    json["kind"] = kind;
    json["payload"] = std::move(payload);
    json["prevHash"] = std::string(lubancode::trajectory::v3::kGenesisHash);
    json["lineHash"] = std::string(lubancode::trajectory::v3::kGenesisHash);
    return json;
}

std::optional<lubancode::trajectory::v3::Schema3Error> CheckEvent(const char* kind,
                                                                 nlohmann::json payload) {
    nlohmann::json json = EventJson(kind, std::move(payload));
    std::string ec, msg;
    auto parsed = lubancode::trajectory::v3::EventLine::FromJsonStrict(json, &ec, &msg);
    if (!parsed.has_value()) {
        return lubancode::trajectory::v3::Schema3Error{ec, msg};
    }
    return lubancode::trajectory::v3::ValidateEventLine(*parsed);
}

bool HasCode(const std::optional<lubancode::trajectory::v3::Schema3Error>& error, const char* code) {
    return error.has_value() && error->code == code;
}

}  // namespace

// ---------------------------------------------------------------------------
// schema 合同
// ---------------------------------------------------------------------------

TEST_CASE("预算 schema: session.context_window.applied 注册、statusless、载荷合同") {
    using lubancode::trajectory::v3::EventKindV3;
    CHECK(lubancode::trajectory::v3::EventKindV3FromName("session.context_window.applied") ==
          EventKindV3::SessionContextWindowApplied);
    CHECK(lubancode::trajectory::v3::EventKindV3Name(EventKindV3::SessionContextWindowApplied) ==
          "session.context_window.applied");
    // 控制状态事实,不带 status(与 session.title.applied 同族)。
    CHECK_FALSE(
        lubancode::trajectory::v3::RequiredStatusForKind(EventKindV3::SessionContextWindowApplied)
            .has_value());
    // 合法:contextWindow 正整数,身份/来源可选。
    CHECK_FALSE(CheckEvent("session.context_window.applied",
                           nlohmann::json{{"contextWindow", 256000}})
                    .has_value());
    CHECK_FALSE(CheckEvent("session.context_window.applied",
                           nlohmann::json{{"contextWindow", 256000},
                                          {"provider", "moonshot"},
                                          {"model", "kimi"},
                                          {"source", "manual"}})
                    .has_value());
    // contextWindow 缺/零/类型不对:拒。
    CHECK(HasCode(CheckEvent("session.context_window.applied", nlohmann::json{}),
                  "schema3.bad_type"));
    CHECK(HasCode(CheckEvent("session.context_window.applied",
                             nlohmann::json{{"contextWindow", 0}}),
                  "schema3.bad_type"));
    CHECK(HasCode(CheckEvent("session.context_window.applied",
                             nlohmann::json{{"contextWindow", "256k"}}),
                  "schema3.bad_type"));
    // source 枚举之外:拒。
    CHECK(HasCode(CheckEvent("session.context_window.applied",
                             nlohmann::json{{"contextWindow", 256000}, {"source", "wizard"}}),
                  "schema3.bad_type"));
}

// ---------------------------------------------------------------------------
// v3 写路 + 读面投影
// ---------------------------------------------------------------------------

TEST_CASE("v3 写路: RecordContextWindowChanged 落真值+身份+来源,读面折最后一枚") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("write");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const fs::path stream = V3StreamOf(*ledger);

    // 零值守卫:写账入口拒收,如实回 false(调用方报"仅本次生效")。
    CHECK_FALSE(ledger->RecordContextWindowChanged(0, 0, "moonshot", "kimi", "manual"));
    // 开场快照:identity 齐全。
    CHECK(ledger->RecordContextWindowChanged(1048576, 0, "moonshot", "kimi", "initial"));
    // 手动改小(用户 /context):末枚是真值。
    CHECK(ledger->RecordContextWindowChanged(256000, 1048576, "moonshot", "kimi", "manual"));

    const auto rows = ReadLines(stream);
    const auto applied = RowsOfKind(rows, "session.context_window.applied");
    REQUIRE(applied.size() == 2);
    CHECK(applied[0]->at("payload").at("contextWindow") == 1048576);
    CHECK(applied[0]->at("payload").value("source", std::string()) == "initial");
    CHECK(applied[1]->at("payload").at("contextWindow") == 256000);
    CHECK(applied[1]->at("payload").value("oldContextWindow", std::uint64_t{0}) == 1048576);
    CHECK(applied[1]->at("payload").value("provider", std::string()) == "moonshot");
    CHECK(applied[1]->at("payload").value("model", std::string()) == "kimi");
    CHECK(applied[1]->at("payload").value("source", std::string()) == "manual");

    // 读面:FindLastContextWindowApplied 折最后一枚(末枚胜)。
    const auto ledger_back = lubancode::trajectory::v3::ReadV3Ledger(stream);
    REQUIRE(ledger_back.has_value());
    const auto fact = lubancode::trajectory::v3::FindLastContextWindowApplied(*ledger_back);
    REQUIRE(fact.has_value());
    CHECK(fact->context_window == 256000);
    CHECK(fact->provider == "moonshot");
    CHECK(fact->model == "kimi");
    CHECK(fact->source == "manual");
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

// ---------------------------------------------------------------------------
// /resume 与 --continue:写账-恢复-应用一条线(§四第一条的模拟复现)
// ---------------------------------------------------------------------------

TEST_CASE("v3 /resume: 折出预算与身份,仲裁裁决 apply,值不丢") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("resume");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string source_id = ledger->session_id();
    WriteOneTurn(*ledger);
    // 有明确模型身份的会话设预算(修复前:事件没有写入口,这条线断在
    // 第一环)。
    REQUIRE(ledger->RecordContextWindowChanged(256000, 1048576, "moonshot", "kimi", "manual"));

    const auto summary = ledger->ResumeInteractive(source_id);
    REQUIRE(summary.outcome.error_code.empty());
    // 恢复值:control 折叠带出预算与身份(修复前:v3 源 resume 不折
    // context_window,这条线断在第二环)。
    REQUIRE(summary.outcome.control.context_window.has_value());
    CHECK(*summary.outcome.control.context_window == 256000);
    CHECK(summary.outcome.control.context_window_provider == "moonshot");
    CHECK(summary.outcome.control.context_window_model == "kimi");
    CHECK(summary.outcome.control.context_window_source == "manual");

    // 应用:恢复裁决(修复前:恢复值从不进 tracker,这条线断在第三环)。
    cli::ContextTracker tracker(1048576);
    tracker.SetWindowBudget(1048576, cli::ContextWindowSource::Config);
    cli::RestoredWindowInput input;
    input.session_window_present = summary.outcome.control.context_window.has_value();
    input.session_window_tokens = 256000;
    input.session_provider = summary.outcome.control.context_window_provider;
    input.session_model = summary.outcome.control.context_window_model;
    input.now_provider = "moonshot";
    input.now_model = "kimi";
    const auto decision = cli::ResolveRestoredContextWindow(input);
    REQUIRE(decision.apply);
    CHECK(decision.tokens == 256000);
    tracker.SetWindowBudget(decision.tokens, cli::ContextWindowSource::Resumed, "moonshot", "kimi");
    CHECK(tracker.window_tokens() == 256000);
    CHECK(tracker.window_source() == cli::ContextWindowSource::Resumed);
    CHECK_FALSE(tracker.window_manually_set());  // 恢复不是手动覆盖

    // 本次明确覆盖压过会话账:同一份折叠,manual_override 下不套用。
    input.manual_override = true;
    const auto kept = cli::ResolveRestoredContextWindow(input);
    CHECK_FALSE(kept.apply);
    CHECK(std::string(kept.note) == "resume_window.manual_kept");
}

TEST_CASE("v3 --continue: 进程重启后 LaunchResumeControlState 仍带回预算") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("continue");
    std::string source_id;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        source_id = ledger->session_id();
        WriteOneTurn(*ledger);
        REQUIRE(ledger->RecordContextWindowChanged(400000, 200000, "moonshot", "kimi", "manual"));
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
    }
    // 进程重启:--continue 启动路续接源场,控制态带预算。
    auto options = LedgerOptions(root);
    options.resume_at_launch = true;
    auto ledger2 = TrajectorySessionLedger::Open(options);
    REQUIRE(ledger2.has_value());
    CHECK(ledger2->resumed_at_launch());
    CHECK(ledger2->session_id() == source_id);  // v3 同 ID 续接语义不动
    const auto control = ledger2->LaunchResumeControlState();
    REQUIRE(control.has_value());
    REQUIRE(control->context_window.has_value());
    CHECK(*control->context_window == 400000);
    CHECK(control->context_window_provider == "moonshot");
    CHECK(control->context_window_model == "kimi");
}

// ---------------------------------------------------------------------------
// /clear:换场后快照落新场,旧场字节不动
// ---------------------------------------------------------------------------

TEST_CASE("v3 /clear: ClearSession 换场,初始快照落新场,旧场不动") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("clear");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string old_id = ledger->session_id();
    const fs::path old_stream = V3StreamOf(*ledger);
    WriteOneTurn(*ledger);
    REQUIRE(ledger->RecordContextWindowChanged(256000, 0, "moonshot", "kimi", "manual"));
    const auto old_rows = ReadLines(old_stream);

    trajectory::ClearRequest request;
    request.reason = "user_clear";
    request.user_initiated = true;
    const auto outcome = ledger->ClearSession(request, nullptr);
    REQUIRE(outcome.error_code.empty());
    CHECK(ledger->session_id() != old_id);

    // 新场初始快照:窗口保留(§五:/clear 不改预算),source=initial。
    CHECK(ledger->RecordContextWindowChanged(256000, 0, "moonshot", "kimi", "initial"));
    const auto new_fact = lubancode::trajectory::v3::FindLastContextWindowApplied(
        *lubancode::trajectory::v3::ReadV3Ledger(V3StreamOf(*ledger)));
    REQUIRE(new_fact.has_value());
    CHECK(new_fact->context_window == 256000);
    CHECK(new_fact->source == "initial");
    // 旧场字节不动(封口只读)。
    CHECK(ReadLines(old_stream).size() == old_rows.size());
}

// ---------------------------------------------------------------------------
// v2 旧档:字符串值可折;无字段旧档回落
// ---------------------------------------------------------------------------

TEST_CASE("v2 旧档: control.context_window.changed 折叠,无字段回落 no_record") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("v2");
    std::string source_id;
    fs::path main_stream;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        source_id = ledger->session_id();
        WriteOneTurn(*ledger);
        main_stream = ledger->session_dir() / "main.jsonl";
        REQUIRE(fs::exists(main_stream));
        // v2 写路:十进制字符串真值 + 身份。
        REQUIRE(ledger->RecordContextWindowChanged(256000, 1048576, "moonshot", "kimi", "manual"));
        // 无字段的旧档对照:只写一轮对话、不写预算事件的另一场在下方
        // (v2 fold 从事件流取数,无事件即无字段)。
    }
    {
        const auto fold = trajectory::FoldStreamReplay(main_stream);
        REQUIRE(fold.ok());
        REQUIRE(fold.state.control.context_window.has_value());
        CHECK(*fold.state.control.context_window == 256000);
        CHECK(fold.state.control.context_window_provider == "moonshot");
        CHECK(fold.state.control.context_window_model == "kimi");
    }
    // 无字段旧档:resume 折叠后 control 无预算,仲裁回落 no_record。
    const auto root2 = FreshRoot("v2-legacy");
    std::string legacy_id;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root2));
        REQUIRE(ledger.has_value());
        legacy_id = ledger->session_id();
        WriteOneTurn(*ledger);
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
    }
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root2));
        REQUIRE(ledger.has_value());
        const auto summary = ledger->ResumeInteractive(legacy_id);
        REQUIRE(summary.outcome.error_code.empty());
        CHECK_FALSE(summary.outcome.control.context_window.has_value());  // 旧档无字段
        cli::RestoredWindowInput input;
        input.now_provider = "moonshot";
        input.now_model = "kimi";
        const auto decision = cli::ResolveRestoredContextWindow(input);
        CHECK_FALSE(decision.apply);
        CHECK(std::string(decision.note) == "resume_window.no_record");  // 回落并说明来源
    }
}
