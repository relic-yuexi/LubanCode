// 应用Worker接入单 P3:持久受理—结果核对(幂等受理 + 重启查询 + 稳定
// 结果读取)的协议面验收册。
//
//   GAP-05 turn/start 幂等:同键同载荷重发回原受理(operationId/turnId
//         原值,duplicate=true,不重跑);同键异载荷 operation_conflict。
//   GAP-05 thread/start 幂等:同键同 cwd 重发回原场身份(duplicate=true,
//         不建第二场);同键异 cwd operation_conflict;requested 在而
//         completed 无(崩溃窄窗注入)回 session_create_unknown 不建场。
//   GAP-06 operation/read:活场终态对账;重启后按 clientOperationId 查回
//         final 与稳定正文(v3 投影);v2 场正文缺口如实报;账态注入(终
//         态行被抹)回 unknown 不冒充。
//   GAP-07 查询零副作用:operation/read 不触发模型、不重发 turn/completed。
//   故障注入:受理落盘失败(operations.jsonl 目录占位)拒收零执行;
//         终态行丢失回 unknown。
// 真拔电/真杀进程不在本册(与 test_session_service_durability 同口径:
// 账态注入 + 重开服务模拟重启面,进程级硬杀证据归后续真机验收)。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "app_server/connection.hpp"
#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "app_server/server.hpp"
#include "tools/path_utils.hpp"

using namespace lubancode;

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

// 按脚本吐事件的假后端(test_app_server_turn.cpp 同款)。
class SharedScriptBackend : public api::Backend {
public:
    explicit SharedScriptBackend(std::vector<std::vector<api::StreamEvent>>& scripts)
        : scripts_(scripts) {}

    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        if (index_ >= scripts_.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const api::StreamEvent& event : scripts_[index_]) {
            on_event(event);
        }
        ++index_;
        ++calls_;
        return {};
    }
    std::size_t calls_ = 0;  // 模型真实被叫了几次(零副作用断言的底)

private:
    std::vector<std::vector<api::StreamEvent>>& scripts_;
    std::size_t index_ = 0;
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "fake-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{10, 5, 0, 0, 0}},
    };
}

// 一台测试用 Server:假 backend 注入,workspaces 根指临时目录。重启模拟
// = 同一 workspaces 根再起一台(旧台先停场析构)。
struct TestHarness {
    std::vector<std::vector<api::StreamEvent>> scripts;

    explicit TestHarness(const std::string& sessions_dir) {
        app_server::ServerOptions options;
        options.workspaces_dir = sessions_dir + "/workspaces";
        options.cwd = "/test/cwd";
        options.outbox_capacity = 256;
        server = std::make_unique<app_server::Server>(
            std::move(options),
            [this]() -> std::unique_ptr<api::Backend> {
                auto backend = std::make_unique<SharedScriptBackend>(scripts);
                live_backend = backend.get();
                return backend;
            },
            nullptr);
        auto dispatcher = std::make_shared<app_server::Dispatcher>();
        server->AttachForTest(std::make_unique<app_server::StdioConnection>(
            std::move(dispatcher), [](const std::string&) {}, []() { return ""; }, 256));
    }

    // 同步口径驱动一轮(直驱 HandleTurnStart:受理 + 等收尾,回
    // turn/completed 的 params)。带幂等键。
    nlohmann::json Turn(const std::string& thread_id, const std::string& text,
                        const std::string& client_operation_id, std::string& error_code) {
        return server->HandleTurnStart(thread_id, text, {}, error_code, client_operation_id);
    }

    // 模型真实被叫了几次(零副作用断言的底;回合没起过 factory 就没被
    // 调,算 0)。
    std::size_t ModelCalls() const { return live_backend != nullptr ? live_backend->calls_ : 0; }

    SharedScriptBackend* live_backend = nullptr;
    std::unique_ptr<app_server::Server> server;
};

std::string MakeTempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return tools::PathToUtf8(dir);
}

// workspaces 树里按 sessionId 找场目录(布局 <root>/<workspace>/<id>/)。
std::filesystem::path SessionDirOf(const std::string& sessions_dir, const std::string& session_id) {
    const std::filesystem::path workspaces = tools::Utf8ToPath(sessions_dir) / "workspaces";
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(workspaces, ec)) {
        if (entry.is_directory() && entry.path().filename() == tools::Utf8ToPath(session_id)) {
            return entry.path();
        }
    }
    return std::filesystem::path();
}

// JSONL 读写(账态注入用)。
std::vector<nlohmann::json> ReadJsonl(const std::filesystem::path& path) {
    std::vector<nlohmann::json> lines;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return lines;
    std::string text;
    while (std::getline(in, text)) {
        if (text.empty()) continue;
        lines.push_back(nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false));
    }
    return lines;
}

void RewriteJsonl(const std::filesystem::path& path, const std::vector<nlohmann::json>& lines) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const nlohmann::json& line : lines) {
        out << line.dump() << "\n";
    }
}

// 台账上按 clientOperationId 抠 operationId(HandleTurnStart 同步口径回
// 的是 turn/completed params,受理字段在账上——以账对账)。
std::string OperationIdOf(const std::string& sessions_dir, const std::string& thread_id,
                          const std::string& client_operation_id) {
    const std::filesystem::path session_dir = SessionDirOf(sessions_dir, thread_id);
    REQUIRE(!session_dir.empty());
    for (const nlohmann::json& line : ReadJsonl(session_dir / "operations.jsonl")) {
        if (line.is_object() && line.contains("kind") && line["kind"] == "operation.accepted" &&
            line.contains("clientOperationId") &&
            line["clientOperationId"] == client_operation_id) {
            return line["operationId"].get<std::string>();
        }
    }
    return std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// GAP-05:turn/start 幂等(活场)
// ---------------------------------------------------------------------------

TEST_CASE("turn/start 幂等:同键同载荷回原受理不重跑,同键异载荷明报冲突") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const std::string sessions_dir = MakeTempDir("lubancode_test_op_idem_turn");
    TestHarness harness(sessions_dir);
    harness.scripts = {TextOnlyScript("第一轮的答复。")};

    std::string error_code;
    const nlohmann::json start =
        harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = start["threadId"];

    // 首发受理:同步口径回 turn/completed params(成功终态)。
    const nlohmann::json first = harness.Turn(thread_id, "问一句", "OP-TURN-1", error_code);
    REQUIRE(error_code.empty());
    CHECK(first["status"] == "success");
    const std::string operation_id = OperationIdOf(sessions_dir, thread_id, "OP-TURN-1");
    REQUIRE(!operation_id.empty());

    // 同键同载荷重发:回原受理(duplicate、原 operationId、原 turnId),
    // 不再跑一轮模型(backend 只被叫过一次)。
    const nlohmann::json retry = harness.Turn(thread_id, "问一句", "OP-TURN-1", error_code);
    REQUIRE(error_code.empty());
    CHECK(retry["duplicate"] == true);
    CHECK(retry["operationId"] == operation_id);
    CHECK(retry["turnId"] == first["turnId"]);

    // 同键异载荷:明报 operation_conflict,不另起 turn。
    const nlohmann::json conflict = harness.Turn(thread_id, "换一句", "OP-TURN-1", error_code);
    REQUIRE(error_code == "operation_conflict");
    CHECK(conflict.is_null());

    // 模型只跑了一轮(重发与冲突都不触发执行)。
    CHECK(harness.ModelCalls() == 1);

    std::string stop_error;
    harness.server->HandleThreadStop(thread_id, stop_error);
}

// ---------------------------------------------------------------------------
// GAP-05:thread/start 幂等(会话创建去重)
// ---------------------------------------------------------------------------

TEST_CASE("thread/start 幂等:同键同 cwd 回原场不建第二场,异 cwd 报冲突") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const std::string sessions_dir = MakeTempDir("lubancode_test_op_idem_thread");
    TestHarness harness(sessions_dir);

    const std::string cwd = (tools::Utf8ToPath(sessions_dir) / "ws").generic_string();
    std::error_code ec;
    std::filesystem::create_directories(tools::Utf8ToPath(cwd), ec);

    std::string error_code;
    nlohmann::json params;
    params["cwd"] = cwd;
    params["clientOperationId"] = "CREATE-1";
    const nlohmann::json first = harness.server->HandleThreadStart(params, error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = first["threadId"];
    CHECK(first.value("duplicate", false) == false);  // 首发:正常回执

    // 同键同 cwd 重发(回执丢失场景):回原场身份,不建第二场。
    const nlohmann::json retry = harness.server->HandleThreadStart(params, error_code);
    REQUIRE(error_code.empty());
    CHECK(retry["threadId"] == thread_id);
    CHECK(retry["duplicate"] == true);
    CHECK(retry["active"] == true);  // 原场还在本进程活着
    CHECK(harness.server->active_thread_count() == 1);

    // 同键异 cwd:载荷不同,operation_conflict,不建第二场。
    nlohmann::json other = params;
    other["cwd"] = (tools::Utf8ToPath(sessions_dir) / "ws2").generic_string();
    const nlohmann::json conflict = harness.server->HandleThreadStart(other, error_code);
    REQUIRE(error_code == "operation_conflict");
    CHECK(harness.server->active_thread_count() == 1);

    // 台账在盘上:requested + completed 两行(PowerLoss 档先意图后结果)。
    const std::filesystem::path session_dir = SessionDirOf(sessions_dir, thread_id);
    REQUIRE(!session_dir.empty());
    const std::filesystem::path ledger = session_dir.parent_path() / "session-creates.jsonl";
    REQUIRE(std::filesystem::exists(ledger));
    bool saw_requested = false;
    bool saw_completed = false;
    for (const nlohmann::json& line : ReadJsonl(ledger)) {
        if (!line.is_object() || !line.contains("kind")) continue;
        if (line["kind"] == "session.create.requested") saw_requested = true;
        if (line["kind"] == "session.create.completed") saw_completed = true;
    }
    CHECK(saw_requested);
    CHECK(saw_completed);

    std::string stop_error;
    harness.server->HandleThreadStop(thread_id, stop_error);
}

TEST_CASE("thread/start 幂等·崩溃窄窗:completed 行缺失时重发不建第二场") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const std::string sessions_dir = MakeTempDir("lubancode_test_op_idem_window");
    TestHarness harness(sessions_dir);

    const std::string cwd = (tools::Utf8ToPath(sessions_dir) / "ws").generic_string();
    std::error_code ec;
    std::filesystem::create_directories(tools::Utf8ToPath(cwd), ec);

    std::string error_code;
    nlohmann::json params;
    params["cwd"] = cwd;
    params["clientOperationId"] = "CREATE-W";
    const nlohmann::json first = harness.server->HandleThreadStart(params, error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = first["threadId"];

    // 账态注入:抹掉 completed 行,模拟"场已建成、completed 落账前崩溃"。
    const std::filesystem::path session_dir = SessionDirOf(sessions_dir, thread_id);
    REQUIRE(!session_dir.empty());
    const std::filesystem::path ledger = session_dir.parent_path() / "session-creates.jsonl";
    std::vector<nlohmann::json> kept;
    for (const nlohmann::json& line : ReadJsonl(ledger)) {
        if (line.is_object() && line.contains("kind") &&
            line["kind"] == "session.create.completed") {
            continue;
        }
        kept.push_back(line);
    }
    RewriteJsonl(ledger, kept);

    // 重发同键:session_create_unknown(意图在、结果无),不建第二场。
    const nlohmann::json retry = harness.server->HandleThreadStart(params, error_code);
    REQUIRE(error_code == "session_create_unknown");
    CHECK(harness.server->active_thread_count() == 1);  // 只有原场

    std::string stop_error;
    harness.server->HandleThreadStop(thread_id, stop_error);
}

// ---------------------------------------------------------------------------
// GAP-06/07:operation/read(活场 + 重启 + 账态注入)
// ---------------------------------------------------------------------------

TEST_CASE("operation/read 活场:final 对账——终态、引用、usage 如实") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const std::string sessions_dir = MakeTempDir("lubancode_test_op_read_live");
    TestHarness harness(sessions_dir);
    harness.scripts = {TextOnlyScript("活场的答复。")};

    std::string error_code;
    const nlohmann::json start =
        harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = start["threadId"];

    const nlohmann::json completed = harness.Turn(thread_id, "问", "OP-LIVE-1", error_code);
    REQUIRE(error_code.empty());
    const std::string operation_id = OperationIdOf(sessions_dir, thread_id, "OP-LIVE-1");
    REQUIRE(!operation_id.empty());

    // 按键查:final 终态与回执对账。
    std::string read_error;
    const nlohmann::json by_key =
        harness.server->HandleOperationRead(thread_id, "OP-LIVE-1", "", read_error);
    REQUIRE(read_error.empty());
    CHECK(by_key["status"] == "final");
    CHECK(by_key["operationId"] == operation_id);
    CHECK(by_key["clientOperationId"] == "OP-LIVE-1");
    CHECK(by_key["executionStatus"] == "success");
    CHECK(by_key["turnId"] == completed["turnId"]);
    CHECK(by_key["usageReported"] == true);
    CHECK(by_key["resultEnvelopePersisted"] == true);
    REQUIRE(by_key.contains("finalMessageRefs"));
    REQUIRE(by_key["finalMessageRefs"].is_array());
    REQUIRE(by_key["finalMessageRefs"].size() == 1);
    CHECK(by_key["finalMessageRefs"][0] == completed["finalMessageRefs"][0]);

    // 按号查:同一条事实。
    const nlohmann::json by_id =
        harness.server->HandleOperationRead(thread_id, "", operation_id, read_error);
    REQUIRE(read_error.empty());
    CHECK(by_id["status"] == "final");
    CHECK(by_id["operationId"] == operation_id);

    // 双键都给但指向不同操作:按账面事实报错,不猜。
    const nlohmann::json mismatch =
        harness.server->HandleOperationRead(thread_id, "OP-NOPE", operation_id, read_error);
    CHECK(read_error.find("operation_id_mismatch") != std::string::npos);

    // 查无此键:状态交代,不是协议错。
    read_error.clear();
    const nlohmann::json missing =
        harness.server->HandleOperationRead(thread_id, "OP-NOPE", "", read_error);
    REQUIRE(read_error.empty());
    CHECK(missing["status"] == "not_found");

    // v2 场正文缺口如实报(活场也是 v2 账)。
    CHECK(by_key["sourceFormat"] == "v2");
    CHECK_FALSE(by_key.contains("finalMessages"));

    std::string stop_error;
    harness.server->HandleThreadStop(thread_id, stop_error);
}

TEST_CASE("operation/read 重启后(v3):按原键找回终态与稳定正文,零重跑零重投") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const std::string sessions_dir = MakeTempDir("lubancode_test_op_read_v3");
    const std::string cwd = (tools::Utf8ToPath(sessions_dir) / "ws").generic_string();
    std::error_code ec;
    std::filesystem::create_directories(tools::Utf8ToPath(cwd), ec);
    const std::string model_text = "v3 场的最终答复。";

    std::string thread_id;
    std::string first_operation_id;
    {
        TestHarness harness(sessions_dir);
        harness.scripts = {TextOnlyScript(model_text)};

        std::string error_code;
        nlohmann::json params;
        params["cwd"] = cwd;
        params["clientOperationId"] = "CREATE-V3";
        const nlohmann::json start = harness.server->HandleThreadStart(params, error_code);
        REQUIRE(error_code.empty());
        thread_id = start["threadId"];

        const nlohmann::json completed = harness.Turn(thread_id, "问到底", "OP-V3-1", error_code);
        REQUIRE(error_code.empty());
        REQUIRE(completed["status"] == "success");
        first_operation_id = OperationIdOf(sessions_dir, thread_id, "OP-V3-1");
        REQUIRE(!first_operation_id.empty());

        // 正常收口再"重启"(进程退出封口)。
        std::string stop_error;
        harness.server->HandleThreadStop(thread_id, stop_error);
        CHECK(stop_error.empty());
    }

    // 重启:同一 workspaces 根起一台新服务(进程内模拟,账在盘上)。
    TestHarness revived(sessions_dir);
    std::string read_error;
    const nlohmann::json read =
        revived.server->HandleOperationRead(thread_id, "OP-V3-1", "", read_error);
    REQUIRE(read_error.empty());
    CHECK(read["status"] == "final");
    CHECK(read["operationId"] == first_operation_id);
    CHECK(read["executionStatus"] == "success");
    CHECK(read["sourceFormat"] == "v3");
    CHECK(read["resultEnvelopePersisted"] == true);

    // 稳定正文:finalMessages 带账上 messageId 与正文(跨重启仍可读,
    // 不重问模型——backend 零调用)。
    REQUIRE(read.contains("finalMessages"));
    REQUIRE(read["finalMessages"].is_array());
    REQUIRE(read["finalMessages"].size() == 1);
    const nlohmann::json& message = read["finalMessages"][0];
    CHECK(message["text"] == model_text);
    CHECK(message["messageId"].get<std::string>().find("msg") != std::string::npos);
    REQUIRE(read.contains("finalMessageRefs"));
    CHECK(message["ref"] == read["finalMessageRefs"][0]);

    // GAP-07 零副作用:查询没触发模型(backend_calls 恒 0)。
    CHECK(revived.ModelCalls() == 0);

    // 会话创建去重跨重启:同键 thread/start 找回原场身份,不建第二场,
    // 场不在新进程里(active=false 如实——续跑须显式恢复,1.x 面没有)。
    std::string create_error;
    nlohmann::json params;
    params["cwd"] = cwd;
    params["clientOperationId"] = "CREATE-V3";
    const nlohmann::json dedup = revived.server->HandleThreadStart(params, create_error);
    REQUIRE(create_error.empty());
    CHECK(dedup["threadId"] == thread_id);
    CHECK(dedup["duplicate"] == true);
    CHECK(dedup["active"] == false);
    CHECK(revived.server->active_thread_count() == 0);
}

TEST_CASE("operation/read 账态注入:终态行被抹,回 unknown 不冒充,查询不触发执行") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const std::string sessions_dir = MakeTempDir("lubancode_test_op_read_unknown");
    const std::string cwd = (tools::Utf8ToPath(sessions_dir) / "ws").generic_string();
    std::error_code ec;
    std::filesystem::create_directories(tools::Utf8ToPath(cwd), ec);

    std::string thread_id;
    {
        TestHarness harness(sessions_dir);
        harness.scripts = {TextOnlyScript("中断前的答复。")};
        std::string error_code;
        const nlohmann::json start =
            harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
        REQUIRE(error_code.empty());
        thread_id = start["threadId"];
        const nlohmann::json completed = harness.Turn(thread_id, "问", "OP-UNK-1", error_code);
        REQUIRE(error_code.empty());
        std::string stop_error;
        harness.server->HandleThreadStop(thread_id, stop_error);
    }

    // 账态注入:抹掉 operation.final 行,模拟"执行中断电、终态没落成"
    //(AW-14:缺终态行不是"未执行",也不是"成功")。
    const std::filesystem::path session_dir = SessionDirOf(sessions_dir, thread_id);
    REQUIRE(!session_dir.empty());
    const std::filesystem::path operations = session_dir / "operations.jsonl";
    std::vector<nlohmann::json> kept;
    for (const nlohmann::json& line : ReadJsonl(operations)) {
        if (line.is_object() && line.contains("kind") && line["kind"] == "operation.final") {
            continue;
        }
        kept.push_back(line);
    }
    RewriteJsonl(operations, kept);

    TestHarness revived(sessions_dir);
    std::string read_error;
    const nlohmann::json read =
        revived.server->HandleOperationRead(thread_id, "OP-UNK-1", "", read_error);
    REQUIRE(read_error.empty());
    CHECK(read["status"] == "unknown");
    CHECK(read["operationId"].get<std::string>().substr(0, 3) == "op-");
    // 缺口交代:已派发无终态,查询不触发执行(§9.2/§9.1)。
    REQUIRE(read["gaps"].is_array());
    CHECK(read["gaps"].size() == 1);
    CHECK(read["gaps"][0].get<std::string>().find("no_final_after_dispatch") !=
          std::string::npos);
    CHECK(revived.ModelCalls() == 0);
}

// ---------------------------------------------------------------------------
// 故障注入:受理落盘失败(AW-13)
// ---------------------------------------------------------------------------

TEST_CASE("受理落盘失败:账位被目录占位,turn/start 拒收零执行") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const std::string sessions_dir = MakeTempDir("lubancode_test_op_inject_append");
    TestHarness harness(sessions_dir);
    harness.scripts = {TextOnlyScript("不该出现的答复。")};

    std::string error_code;
    const nlohmann::json start =
        harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = start["threadId"];

    // 注入:operations.jsonl 的位置占成目录——OperationsFile 惰性开账,
    // 首笔 Append 的 JournalWriter 打不开,broken 即刻置位。
    const std::filesystem::path session_dir = SessionDirOf(sessions_dir, thread_id);
    REQUIRE(!session_dir.empty());
    std::error_code ec;
    std::filesystem::create_directory(session_dir / "operations.jsonl", ec);

    // 带键受理:拒收(不回 accepted、不跑模型)。
    const nlohmann::json rejected = harness.Turn(thread_id, "问", "OP-BROKEN-1", error_code);
    REQUIRE(error_code == "operation.append_failed");
    CHECK(harness.ModelCalls() == 0);
    CHECK(harness.server->active_thread_count() == 1);  // 场还在,回合没起

    // 同键重发:同样拒收(broken 传播——写盘失败停止受理,不回成功语义)。
    const nlohmann::json again = harness.Turn(thread_id, "问", "OP-BROKEN-1", error_code);
    REQUIRE(error_code == "operation.append_failed");
    CHECK(harness.ModelCalls() == 0);

    std::string stop_error;
    harness.server->HandleThreadStop(thread_id, stop_error);
}

// ---------------------------------------------------------------------------
// 协议参数面(schema)
// ---------------------------------------------------------------------------

TEST_CASE("clientOperationId 的参数口径:空串报错,operation/read 双键至少一枚") {
    const nlohmann::json turn_params{{"threadId", "t"}, {"text", "x"},
                                     {"clientOperationId", ""}};
    std::string thread_id, text, client_operation_id;
    std::vector<nlohmann::json> images;
    const app_server::ParamsCheck empty_key =
        app_server::CheckTurnStartParams(turn_params, thread_id, text, images,
                                         client_operation_id);
    CHECK_FALSE(empty_key.ok);
    CHECK(empty_key.message.find("不许为空") != std::string::npos);

    const nlohmann::json thread_params{{"clientOperationId", ""}};
    std::string create_key;
    CHECK_FALSE(app_server::CheckThreadStartParams(thread_params, create_key).ok);

    // operation/read:threadId 必填,双键至少一枚。
    std::string read_client, read_operation;
    CHECK_FALSE(app_server::CheckOperationReadParams(
                    nlohmann::json{{"clientOperationId", "k"}}, thread_id, read_client,
                    read_operation)
                    .ok);
    CHECK_FALSE(app_server::CheckOperationReadParams(
                    nlohmann::json{{"threadId", "t"}}, thread_id, read_client, read_operation)
                    .ok);
    const app_server::ParamsCheck ok = app_server::CheckOperationReadParams(
        nlohmann::json{{"threadId", "t"}, {"clientOperationId", "k"}}, thread_id, read_client,
        read_operation);
    CHECK(ok.ok);
    CHECK(read_client == "k");
    CHECK(read_operation.empty());
}
