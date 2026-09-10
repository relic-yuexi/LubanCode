// app-server 接 SessionService 测试册(AppServer 接入 Session v3 第一棒:
// 统一服务入口与身份)。协议 1.2 面一行不动的前提下,验三处接线:
//   1. thread/start 走服务开张(会话账布局与旧路一致);
//   2. turn/start 的输入先过服务接纳——operations.jsonl 先账后回执,
//      turn/completed 协议形状照旧;
//   3. typed 域命令走服务执行(命令轨迹包裹与旧路同源)。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "app_server/schema.hpp"
#include "app_server/server.hpp"

using namespace lubancode;

namespace {

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
        return {};
    }

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

std::string MakeTempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::u8string u8 = dir.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::vector<nlohmann::json> ReadJsonl(const std::filesystem::path& path) {
    std::vector<nlohmann::json> lines;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return lines;
    }
    std::string text;
    while (std::getline(in, text)) {
        if (text.empty()) {
            continue;
        }
        lines.push_back(nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false));
    }
    return lines;
}

// workspaces 树里找 thread 的会话目录(一场 thread 一枚 main.jsonl)。
std::optional<std::filesystem::path> FindSessionDir(const std::string& workspaces_dir) {
    const std::filesystem::path workspaces(
        std::filesystem::path(reinterpret_cast<const char8_t*>(workspaces_dir.c_str())));
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(workspaces, ec)) {
        if (entry.is_regular_file() && entry.path().filename() == "main.jsonl") {
            return entry.path().parent_path();
        }
    }
    return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// thread/start 的开张接线:会话账走 SessionService,布局与旧路一致
// ---------------------------------------------------------------------------

TEST_CASE("thread/start 走服务开张:v2 布局照旧,台账不空造") {
    const std::string sessions_dir = MakeTempDir("lubancode_test_app_server_service_start");
    app_server::ServerOptions options;
    options.workspaces_dir = sessions_dir + "/workspaces";
    options.cwd = "/test/cwd";
    app_server::Server server(std::move(options),
                              [] { return std::unique_ptr<api::Backend>(); }, nullptr);
    std::string error_code;
    const nlohmann::json started =
        server.HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    REQUIRE(started.contains("threadId"));
    const std::string thread_id = started["threadId"].get<std::string>();
    CHECK_FALSE(thread_id.empty());

    const auto session_dir = FindSessionDir(sessions_dir + "/workspaces");
    REQUIRE(session_dir.has_value());
    CHECK(std::filesystem::exists(*session_dir / "session.json"));
    // 没有输入就没有操作台账(先账后回执只对真输入起账)。
    CHECK_FALSE(std::filesystem::exists(*session_dir / "operations.jsonl"));

    const nlohmann::json stopped = server.HandleThreadStop(thread_id, error_code);
    CHECK(error_code.empty());
    CHECK(stopped.is_object());  // thread/stop 的 result 是空对象(schema 现行合同)
    CHECK(server.active_thread_count() == 0);
}

// ---------------------------------------------------------------------------
// turn/start 的输入接纳接线:先账后回执,协议回包形状照旧
// ---------------------------------------------------------------------------

TEST_CASE("turn/start 经服务接纳:operations 先账,协议回包一字不动") {
    const std::string sessions_dir = MakeTempDir("lubancode_test_app_server_service_turn");
    std::vector<std::vector<api::StreamEvent>> scripts = {TextOnlyScript("你好,服务。")};
    app_server::ServerOptions options;
    options.workspaces_dir = sessions_dir + "/workspaces";
    options.cwd = "/test/cwd";
    app_server::Server server(std::move(options),
                              [&scripts] { return std::make_unique<SharedScriptBackend>(scripts); },
                              nullptr);
    std::string error_code;
    const nlohmann::json started =
        server.HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = started["threadId"].get<std::string>();

    // 同步口径(等回合收尾):协议回包 = turn/completed params,形状与
    // 旧路一致(status/threadId/turnId/usage/stepsUsed)。
    const nlohmann::json completed =
        server.HandleTurnStart(thread_id, "打个招呼", {}, error_code);
    REQUIRE(error_code.empty());
    CHECK(completed["status"] == "success");
    CHECK(completed["threadId"] == thread_id);
    CHECK(completed["turnId"].get<std::string>().substr(0, 5) == "turn-");
    CHECK(completed["usage"]["inputTokens"] == 10);
    CHECK(completed["stepsUsed"] == 1);

    // 服务层先账:回合跑完,operations.jsonl 里已有这笔回合的接纳记录
    //(协议 1.2 没有幂等键,键位空;payload hash 可复算)。
    const auto session_dir = FindSessionDir(sessions_dir + "/workspaces");
    REQUIRE(session_dir.has_value());
    const auto lines = ReadJsonl(*session_dir / "operations.jsonl");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].value("kind", std::string()) == "operation.accepted");
    CHECK(lines[0].value("clientOperationId", std::string()).empty());
    CHECK(lines[0].value("operationId", std::string()) == "op-1");
    CHECK(lines[0].contains("payloadHash"));

    // 第二回合照走(每轮一接纳一消费,队列不积):协议回包形状照旧,
    // 台账记到 op-2。
    scripts.push_back(TextOnlyScript("第二轮。"));
    const nlohmann::json second =
        server.HandleTurnStart(thread_id, "再问一句", {}, error_code);
    REQUIRE(error_code.empty());
    CHECK(second["status"] == "success");
    const auto after_two = ReadJsonl(*session_dir / "operations.jsonl");
    REQUIRE(after_two.size() == 2);
    CHECK(after_two[1].value("operationId", std::string()) == "op-2");

    (void)server.HandleThreadStop(thread_id, error_code);
}

// ---------------------------------------------------------------------------
// typed 域命令的执行接线:服务执行,命令轨迹包裹同源
// ---------------------------------------------------------------------------

TEST_CASE("typed 域命令走服务执行:goal 未开给稳定禁用码,命令事件落账") {
    const std::string sessions_dir = MakeTempDir("lubancode_test_app_server_service_domain");
    app_server::ServerOptions options;
    options.workspaces_dir = sessions_dir + "/workspaces";
    options.cwd = "/test/cwd";
    options.features_goal = false;  // 缺省关:goal/* 回 goal.disabled
    app_server::Server server(std::move(options),
                              [] { return std::unique_ptr<api::Backend>(); }, nullptr);
    std::string error_code;
    const nlohmann::json started =
        server.HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = started["threadId"].get<std::string>();

    app_server::IncomingRequest request;
    request.id = 7;
    request.method = "goal/create";
    request.params = nlohmann::json{{"threadId", thread_id}, {"text", "定个小目标"}};
    bool errored = false;
    std::string out_code;
    std::string out_message;
    const nlohmann::json rejected =
        server.HandleTypedDomainCommand(request, errored, out_code, out_message);
    CHECK(errored);
    // feature 关:实例在(按 thread 各起一本)但没仓,稳定码
    // goal.store_unavailable + payload.disabled(goal_coordinator 现行合同;
    // goal.disabled 只在实例缺席时给)。
    CHECK(out_code == "goal.store_unavailable");
    CHECK(rejected.is_null());

    // 命令轨迹照落(服务里的 BeginCommand/EndCommand 与旧实现同源):
    // goal/create 的 requested 先 durable,failed 终态随后。
    const auto session_dir = FindSessionDir(sessions_dir + "/workspaces");
    REQUIRE(session_dir.has_value());
    bool saw_command_requested = false;
    bool saw_command_failed = false;
    for (const nlohmann::json& line : ReadJsonl(*session_dir / "main.jsonl")) {
        if (!line.is_object() || !line.contains("kind")) {
            continue;
        }
        const std::string kind = line["kind"].get<std::string>();
        if (kind == "control.command.requested") {
            saw_command_requested = true;
            REQUIRE(line.contains("payload"));
            CHECK(line["payload"].value("command_name", std::string()) == "goal/create");
        }
        if (kind == "control.command.failed") {
            saw_command_failed = true;
        }
    }
    CHECK(saw_command_requested);
    CHECK(saw_command_failed);

    (void)server.HandleThreadStop(thread_id, error_code);
}
