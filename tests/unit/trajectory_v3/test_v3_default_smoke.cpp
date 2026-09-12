// T16(V3-ADD-02,B1 批,SessionV3 旧设计清理单):默认-v3 冒烟。
// 翻默认(v0.26.250)后产品语义 = 未设 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS
// 时新会话写 v3;ctest 全局钉 0 保 v2 老册,守门册只钉过开关函数本身。
// 本册钉"完全不设变量"时的真实入口链:
//   - TrajectorySessionLedger::Open(CLI/AppServer 建场共用路)开的是 v3 场;
//   - 一轮 turn 的账面形状(user/assistant 入链、prepared 落账);
//   - compact 管理入口的分派判据(v3_main_writer 非空)在默认态成立;
//   - CloseSession 封口(session.ended);
//   - SessionManager::LaunchSession(建场的更底层)同默认开 v3。
// 子代理账跟随父场(接线点 1 只在建场读开关),不另设变量即可复验。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "api/types.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"  // NewSessionV3WriteEnabled(前提断言)
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
using namespace lubancode;
using namespace lubancode::trajectory;
using lubancode::runtime::TrajectorySessionLedger;
using lubancode::runtime::TrajectoryTurnBridge;

namespace {

// 摘掉格式变量(ctest 的 ENVIRONMENT 注入、CI 父环境的意外设置都清掉),
// 析构还原为未设——钉"完全不设"的产品默认态。
struct EnvUnset {
    explicit EnvUnset(const char* name) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    ~EnvUnset() {
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
                     ("lubancode-v3-default-smoke-" + std::string(tag));
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

}  // namespace

// ---------------------------------------------------------------------------
// 默认态:未设变量,开账走 v3
// ---------------------------------------------------------------------------

TEST_CASE("默认-v3 冒烟: 未设变量时 ledger 开场即 v3,一轮 turn 账面齐全") {
    EnvUnset unset("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    CHECK(lubancode::trajectory::v3::NewSessionV3WriteEnabled());  // 前提钉死

    const auto root = FreshRoot("open-turn-close");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());

    // 建场形状:v3 主账在,旧两件(v2 main.jsonl / session.json)不在。
    const std::string session_id = ledger->session_id();
    const std::filesystem::path session_dir = ledger->session_dir();
    const std::filesystem::path stream =
        session_dir / platform::Utf8ToPath(session_id + ".jsonl");
    CHECK(std::filesystem::exists(stream));
    CHECK_FALSE(std::filesystem::exists(session_dir / "main.jsonl"));
    CHECK_FALSE(std::filesystem::exists(session_dir / "session.json"));
    // compact 管理入口的分派判据(RunCompactCommand/TryRunCompact 的
    // v3_main_writer() 判定)在默认态成立。
    CHECK(ledger->v3_main_writer() != nullptr);

    // 首两行:system(seq=1)+ session.started。
    const auto head = ReadLines(stream);
    REQUIRE(head.size() >= 2);
    CHECK(head[0].value("type", std::string()) == "message");
    CHECK(head[0]["message"].value("role", std::string()) == "system");
    CHECK(head[1].value("type", std::string()) == "event");
    CHECK(head[1].value("kind", std::string()) == "session.started");

    // 一轮 turn:user 入账入链、prepared 落账、assistant 定稿。
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("默认态的一问"));
        const std::string request_id =
            bridge->OnRequestPrepared(MakeRequest("你是 LubanCode,读写跑都走工具。",
                                                  {UserMessage("默认态的一问")}),
                                      PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("默认态的一答"), "end_turn",
                                          "resp-1"));
        bridge->OnUsageRecorded(request_id, api::Usage{}, /*reported_by_provider=*/false,
                                "resp-1");
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }
    const auto ledger_read = v3::ReadV3Ledger(stream);
    REQUIRE(ledger_read.has_value());
    bool saw_prepared = false;
    bool saw_user = false;
    bool saw_assistant = false;
    for (const auto& event : ledger_read->events) {
        if (event.kind == v3::EventKindV3::ModelRequestPrepared) {
            saw_prepared = true;
        }
    }
    for (const auto& message : ledger_read->messages) {
        const std::string role = message.message.value("role", std::string());
        saw_user = saw_user || role == "user";
        saw_assistant = saw_assistant || role == "assistant";
    }
    CHECK(saw_prepared);
    CHECK(saw_user);
    CHECK(saw_assistant);
    CHECK(ledger_read->context.chain.size() >= 3);  // system -> user -> assistant

    // 封口:CloseSession 落 session.ended。
    REQUIRE(ledger->CloseSession("exit").error_code.empty());
    const auto sealed = v3::ReadV3Ledger(stream);
    REQUIRE(sealed.has_value());
    bool ended = false;
    for (const auto& event : sealed->events) {
        ended = ended || event.kind == v3::EventKindV3::SessionEnded;
    }
    CHECK(ended);
}

TEST_CASE("默认-v3 冒烟: SessionManager::LaunchSession 同默认开 v3 场") {
    EnvUnset unset("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    const auto root = FreshRoot("manager-launch");
    lubancode::trajectory::SessionManagerOptions options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.251-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    lubancode::trajectory::SessionManager manager(options);
    auto active = manager.LaunchSession();
    REQUIRE(active.has_value());
    const std::string session_id = (*active)->session_id();
    const std::filesystem::path session_dir = (*active)->session_dir();
    // v3 布局:只有 <id>.jsonl,无 v2 的 session.json。
    CHECK(std::filesystem::exists(
        session_dir / platform::Utf8ToPath(session_id + ".jsonl")));
    CHECK_FALSE(std::filesystem::exists(session_dir / "session.json"));
    CHECK_FALSE(std::filesystem::exists(session_dir / "main.jsonl"));
}

// Resume 接入 v3 单 R1 回归钉:v3 场的 workspace_key() 曾只认 v2 main
//(恒空),/resume 的 Cwd 范围拿空 key 直接空手——盘上档案全在也 0/0。
// 本案钉三层:key 非空且等于身份钥匙;封口后重开(退场再进的截图场景)
// Cwd 查询列得上一场;All 范围同账。
TEST_CASE("默认-v3 冒烟: v3 场 workspace_key 非空,Cwd 列表列得上一场") {
    EnvUnset unset("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    const auto root = FreshRoot("workspace-key-cwd");
    const auto identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    const std::string first_id = [&] {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        // v3 场读面身份:不再只认 v2 main 的 scope。
        CHECK_FALSE(ledger->workspace_key().empty());
        CHECK(ledger->workspace_key() == identity.workspace_key);
        const std::string id = ledger->session_id();
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
        return id;
    }();

    // 截图场景:退出重进,同 workspace 裸开新场,/resume 默认 Cwd 范围。
    auto second = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(second.has_value());
    trajectory::SessionIndexQuery cwd_query;  // all_workspaces=false:填 workspace_key()
    const auto cwd_page = second->ListWorkspaceSessions(cwd_query);
    CHECK(cwd_page.total >= 1);
    bool saw_first = false;
    for (const auto& entry : cwd_page.entries) {
        saw_first = saw_first || entry.session_id == first_id;
    }
    CHECK(saw_first);

    // All 范围同一份账(不走 workspace_key,历来自成;对齐用)。
    trajectory::SessionIndexQuery all_query;
    all_query.all_workspaces = true;
    const auto all_page = second->ListWorkspaceSessions(all_query);
    CHECK(all_page.total >= cwd_page.total);
}
