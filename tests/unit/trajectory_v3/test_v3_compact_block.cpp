// T12-A(V3-GAP-07 P0,SessionV3 旧设计清理单 B1):compact 投影失败的
// 会话级执行阻断。applied 已落稳而内存换账(ProjectV3ContextHistory/
// ReplaceHistory)失败时,调用方置 BlockV3Execution——此后本场所有主会话
// 轮桥的请求最终准入一律拒绝(V3RequestPrepared 返回空串,loop 本步明败
// 不发模型:CLI/AppServer/Goal/Loop 殊途同门);收尾口(在飞请求的
// output 三态、turn 收口)不受影响,按真实状态收尾。幂等保首因;非 v3
// 场 no-op;换场(books 重建)即天然解除。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
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
                     ("lubancode-v3-compact-block-" + std::string(tag));
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
    options.lubancode_version = "0.26.251-test";
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

// 一轮的开场:开桥、开 turn、记输入(不动请求——各案自己控制)。
struct OpenTurn {
    std::unique_ptr<TrajectoryTurnBridge> bridge;
    explicit OpenTurn(TrajectorySessionLedger& ledger) {
        bridge = ledger.NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("压一压"));
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 阻断门:置位后请求准入全拒
// ---------------------------------------------------------------------------

TEST_CASE("v3 阻断: BlockV3Execution 后所有轮桥的请求准入拒绝") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("gate");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    CHECK_FALSE(ledger->V3ExecutionBlocked());

    const std::string system = "你是 LubanCode,读写跑都走工具。";
    {
        OpenTurn turn(*ledger);
        // 阻断前:请求照常落 prepared(非空 = loop 可发模型)。
        const std::string before = turn.bridge->OnRequestPrepared(
            MakeRequest(system, {UserMessage("压一压")}), PreparedContext());
        REQUIRE_FALSE(before.empty());

        // T12-A:compact applied 后投影失败,调用方置阻断。
        ledger->BlockV3Execution("compact.swap.ledger_unreadable: 注入");
        REQUIRE(ledger->V3ExecutionBlocked());

        // 同一只桥:新请求被拒(空串 = "prepared 记不住不发模型"语义)。
        CHECK(turn.bridge
                  ->OnRequestPrepared(MakeRequest(system, {UserMessage("再来一问")}),
                                      PreparedContext())
                  .empty());
        // 在飞请求按真实状态收尾:output 三态不受阻断影响。
        CHECK(turn.bridge->OnOutputCompleted(before, AssistantText("半截也照实落账"),
                                             "end_turn", "resp-1"));
        turn.bridge->OnUsageRecorded(before, SampleUsage(), /*reported_by_provider=*/true,
                                     "resp-1");
        turn.bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, {});
    }
    // 新桥(下一轮,CLI/AppServer/Goal/Loop 各自开桥都走这道门):同拒。
    auto next = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(next != nullptr);
    next->BeginTurn("turn-2", "external_user");
    next->RecordInput(UserMessage("下一轮也进不来"));
    CHECK(next
              ->OnRequestPrepared(MakeRequest(system, {UserMessage("下一轮也进不来")}),
                                  PreparedContext())
              .empty());
    next->EndTurn(/*ok=*/false, /*cancelled=*/false, "execution_blocked");
}

TEST_CASE("v3 阻断: 幂等保首因,诊断汇可查") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("idempotent");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    ledger->BlockV3Execution("compact.swap.ledger_unreadable: 第一次");
    ledger->BlockV3Execution("compact.swap.内存替换失败: 第二次");
    CHECK(ledger->V3ExecutionBlocked());
    // 首因只记一笔:重复置位不添诊断、不覆盖原因(/doctor 从这读)。
    const auto errors = ledger->recent_io_errors();
    int blocked_notes = 0;
    for (const std::string& note : errors) {
        if (note.rfind("compact.execution_blocked:", 0) == 0) {
            ++blocked_notes;
            CHECK(note.find("第一次") != std::string::npos);
        }
    }
    CHECK(blocked_notes == 1);
}

TEST_CASE("v2 场: BlockV3Execution no-op,请求照发(v2 无此门)") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("v2-noop");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    REQUIRE(ledger->v3_main_writer() == nullptr);
    ledger->BlockV3Execution("compact.swap.不该生效");
    CHECK_FALSE(ledger->V3ExecutionBlocked());
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("v2 老路不受影响"));
    CHECK_FALSE(bridge
                    ->OnRequestPrepared(MakeRequest("v2 system", {UserMessage("v2 老路不受影响")}),
                                        PreparedContext())
                    .empty());
    bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, {});
}

TEST_CASE("v3 阻断解除: clear 换场后新 books 不携带旧阻断") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("clear-unblock");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string system = "你是 LubanCode,读写跑都走工具。";
    {
        OpenTurn turn(*ledger);
        REQUIRE_FALSE(turn.bridge
                          ->OnRequestPrepared(MakeRequest(system, {UserMessage("压一压")}),
                                              PreparedContext())
                          .empty());
        ledger->BlockV3Execution("compact.swap.ledger_unreadable: 注入");
        CHECK(ledger->V3ExecutionBlocked());
        turn.bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, {});
    }
    // clear 换场:关旧场开新场,books 重建——恢复路的"核验重建即解除"。
    trajectory::NullClearParticipant participant;
    trajectory::ClearRequest clear_request;
    const auto outcome = ledger->ClearSession(clear_request, &participant);
    REQUIRE(outcome.error_code.empty());
    CHECK_FALSE(ledger->V3ExecutionBlocked());
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("新场照常开工"));
    CHECK_FALSE(bridge
                    ->OnRequestPrepared(MakeRequest(system, {UserMessage("新场照常开工")}),
                                        PreparedContext())
                    .empty());
    bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, {});
}
