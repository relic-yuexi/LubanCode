// T15-A(V3-GAP-09 P0,SessionV3 旧设计清理单 B1):v3 会话的删除准入。
// 旧 DeleteSessionDir 的封口门只扫 main.jsonl——对只有 sessions/<id>/
// <id>.jsonl 的 v3 场 journal_exists 恒假,未封口的运行档直穿 remove_all。
// 本册钉新门:
//   - 未封口 v3(无 session.ended)拒绝删除,目录及内容字节原样保留;
//   - 坏账(封口后追加垃圾行)拒绝删除(v3 主账验卷不过);
//   - 活锁在场拒绝(v2/v3 共用的锁门,v3 场补钉);
//   - 封口完好的 v3 可删,tombstone 带实际末行 hash;
//   - main.jsonl 与 <id>.jsonl 并存 = 格式歧义,拒绝;
//   - v2 未封口仍拒(老门回归,一字不动)。
// 全部用独立临时目录,不拿用户真实 Session 验删除。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
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
using namespace lubancode::trajectory;  // v3:: / DeleteSessionDir / ReadSessionTombstone
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
                     ("lubancode-v3-delete-gate-" + std::string(tag));
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

std::filesystem::path V3StreamOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir() /
           platform::Utf8ToPath(platform::PathToUtf8(ledger.session_dir().filename()) + ".jsonl");
}

// DeleteSessionDir 的 workspace_dir(= sessions/ 的父目录)。
std::filesystem::path WorkspaceDirOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir().parent_path().parent_path();
}

// 完整一轮(user -> prepared -> assistant -> turn 收口),账上有真对话。
void DriveTurn(TrajectorySessionLedger& ledger, const std::string& text) {
    auto bridge = ledger.NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage(text));
    const std::string request_id = bridge->OnRequestPrepared(
        MakeRequest("你是 LubanCode,读写跑都走工具。", {UserMessage(text)}), PreparedContext());
    REQUIRE_FALSE(request_id.empty());
    REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("收到。"), "end_turn", "resp-1"));
    bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
}

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// 会话目录全部文件的 (相对路径 -> 字节) 快照:断言"原样保留"用。
std::map<std::string, std::string> SnapshotDir(const std::filesystem::path& dir) {
    std::map<std::string, std::string> snap;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::error_code ec;
        const auto rel = std::filesystem::relative(entry.path(), dir, ec);
        snap[platform::PathToUtf8(rel)] = ReadFileBytes(entry.path());
    }
    return snap;
}

}  // namespace

// ---------------------------------------------------------------------------
// 拒绝面:未封口 / 坏账 / 活锁 / 格式歧义
// ---------------------------------------------------------------------------

TEST_CASE("v3 删除门: 未封口(无 session.ended)拒绝,目录字节原样") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("unsealed");
    std::filesystem::path stream;
    std::string session_id;
    std::map<std::string, std::string> before;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        DriveTurn(*ledger, "还没收尾的一轮");
        stream = V3StreamOf(*ledger);
        session_id = ledger->session_id();
        before = SnapshotDir(ledger->session_dir());
    }
    // ledger 已析构(锁随之释放),账未封口:旧门 journal_exists 恒假会直穿,
    // 新门按 session.ended 拒。
    const std::filesystem::path workspace_dir = stream.parent_path().parent_path();
    const auto outcome =
        trajectory::DeleteSessionDir(workspace_dir, session_id, "user_delete", 1759468800000LL);
    CHECK(outcome.error_code == "session.delete_unsealed");
    CHECK(SnapshotDir(stream.parent_path()) == before);  // 一字不动
    CHECK(std::filesystem::exists(stream));
}

TEST_CASE("v3 删除门: 坏账(封口后追加垃圾)拒绝删除") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("corrupt");
    std::filesystem::path stream;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        DriveTurn(*ledger, "收尾前先留一轮");
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
        stream = V3StreamOf(*ledger);
    }
    // 注入:封口完好后往尾巴追加半行垃圾(坏尾,验卷必挂)。
    {
        std::ofstream file(stream, std::ios::binary | std::ios::app);
        REQUIRE(file.is_open());
        file << "{\"seq\":99,\"kind\":\"session.started\"";  // 无换行、断 JSON
    }
    const std::string session_id = platform::PathToUtf8(stream.parent_path().filename());
    const auto before = SnapshotDir(stream.parent_path());
    const auto outcome = trajectory::DeleteSessionDir(stream.parent_path().parent_path(),
                                                      session_id, "user_delete",
                                                      1759468800000LL);
    CHECK(outcome.error_code == "session.delete_v3_unreadable");
    CHECK(SnapshotDir(stream.parent_path()) == before);
}

TEST_CASE("v3 删除门: 活锁在场拒绝(锁门 v3 场补钉)") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("live-lock");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    DriveTurn(*ledger, "活锁场的一轮");
    REQUIRE(ledger->CloseSession("exit").error_code.empty());
    // ledger 活着 = 独占锁在本进程手里(Inspect → Alive)。
    const auto outcome = trajectory::DeleteSessionDir(WorkspaceDirOf(*ledger), ledger->session_id(),
                                                      "user_delete", 1759468800000LL);
    CHECK(outcome.error_code == "session.delete_locked");
    CHECK(std::filesystem::exists(V3StreamOf(*ledger)));
}

TEST_CASE("v3 删除门: main.jsonl 与 v3 主账并存 = 格式歧义拒绝") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("ambiguous");
    std::filesystem::path stream;
    std::string session_id;
    std::filesystem::path workspace_dir;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        DriveTurn(*ledger, "歧义场的一轮");
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
        stream = V3StreamOf(*ledger);
        session_id = ledger->session_id();
        workspace_dir = WorkspaceDirOf(*ledger);
    }
    // 注入并存:同目录再放一份 main.jsonl(内容不拘,存在即歧义)。
    {
        std::ofstream file(stream.parent_path() / "main.jsonl", std::ios::binary);
        REQUIRE(file.is_open());
        file << "{\"event\":\"run.started\"}\n";
    }
    const auto before = SnapshotDir(stream.parent_path());
    const auto outcome =
        trajectory::DeleteSessionDir(workspace_dir, session_id, "user_delete", 1759468800000LL);
    CHECK(outcome.error_code == "session.delete_format_ambiguous");
    CHECK(SnapshotDir(stream.parent_path()) == before);
}

// ---------------------------------------------------------------------------
// 放行面:封口完好可删 + tombstone;v2 老门回归
// ---------------------------------------------------------------------------

TEST_CASE("v3 删除门: 封口完好可删,tombstone 带实际末行 hash") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("sealed-ok");
    std::filesystem::path stream;
    std::string session_id;
    std::filesystem::path workspace_dir;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        DriveTurn(*ledger, "封口场的一轮");
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
        stream = V3StreamOf(*ledger);
        session_id = ledger->session_id();
        workspace_dir = WorkspaceDirOf(*ledger);
    }
    // 末行 hash 的期望值:直接从账上取(与实现同一读取面)。
    const auto ledger_read = v3::ReadV3Ledger(stream);
    REQUIRE(ledger_read.has_value());
    std::string expected_last_hash;
    if (const auto last = ledger_read->LastEntry(); last.has_value()) {
        expected_last_hash = last->is_message
                                 ? ledger_read->messages[last->index].line_hash
                                 : ledger_read->events[last->index].line_hash;
    }
    REQUIRE_FALSE(expected_last_hash.empty());

    const auto outcome =
        trajectory::DeleteSessionDir(workspace_dir, session_id, "user_delete", 1759468800000LL);
    REQUIRE(outcome.ok());
    CHECK_FALSE(std::filesystem::exists(stream.parent_path()));
    const auto tombstone =
        trajectory::ReadSessionTombstone(workspace_dir / "tombstones", session_id);
    REQUIRE(tombstone.has_value());
    CHECK(tombstone->last_event_hash.has_value());
    CHECK(*tombstone->last_event_hash == expected_last_hash);
    CHECK(tombstone->reason == "user_delete");
}

TEST_CASE("v2 老门回归: 未封口 main.jsonl 场仍拒删(一字不动)") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("v2-unsealed");
    std::filesystem::path main_jsonl;
    std::string session_id;
    std::filesystem::path workspace_dir;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        DriveTurn(*ledger, "v2 未封口的一轮");
        main_jsonl = ledger->session_dir() / "main.jsonl";
        REQUIRE(std::filesystem::exists(main_jsonl));
        session_id = ledger->session_id();
        workspace_dir = WorkspaceDirOf(*ledger);
    }
    const auto outcome =
        trajectory::DeleteSessionDir(workspace_dir, session_id, "user_delete", 1759468800000LL);
    CHECK(outcome.error_code == "session.delete_unsealed");
    CHECK(std::filesystem::exists(main_jsonl));
}
